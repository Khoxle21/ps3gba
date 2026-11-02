// GBA Wireless Adapter (AGB-015): one adapter per player on its GBA's serial port, and a hub in
// this process that plays the radio between them. Written from the protocol documentation of
// afska/gba-link-connection (docs/wireless_adapter.md, MIT) and from what the games' own library
// sends and expects (Nintendo's librfu, as decompiled in pret's pokefirered): plan, findings and
// limits in docs/wireless-adapter.md.
//
// The serial side: the adapter is an SPI device in Normal 32-bit mode. Each transfer swaps one
// word each way, so the adapter's word for a transfer is decided by the previous one. After a
// reset (the SD line held high) the GBA and the adapter swap the letters of "NINTENDO"; then the
// GBA sends commands (0x9966LLCC and LL words) and the adapter answers (0x9966LL(CC+0x80) and LL
// words). After a "wait" command the adapter clocks the line instead, to tell the GBA that data
// came in, that it was disconnected or that nothing happened in time.
//
// The radio: every adapter in the process is in range of every other. A host's send reaches its
// clients at once and brings back what each client queued for it; nothing waits for another
// game's thread. Packets queue (a few per direction) instead of overwriting each other as on
// hardware, so games whose threads drift a frame apart lose nothing.
#include <stdio.h>
#include <string.h>

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/sio.h>

#include "rfu.h"

void log_printf(const char* format, ...) __attribute__((format(printf, 1, 2)));

bool rfu_trace;

#ifdef __PPU__
#include <ppu-lv2.h>
#include <sys/mutex.h>
static sys_mutex_t hub_mutex;

void rfu_init(void) {
	sys_mutex_attr_t attr;
	sysMutexAttrInitialize(attr);
	sysMutexCreate(&hub_mutex, &attr);
}

static void hub_lock(void) {
	sysMutexLock(hub_mutex, 0);
}

static void hub_unlock(void) {
	sysMutexUnlock(hub_mutex);
}
#else
#include <pthread.h>
static pthread_mutex_t hub_mutex = PTHREAD_MUTEX_INITIALIZER;

void rfu_init(void) {
}

static void hub_lock(void) {
	pthread_mutex_lock(&hub_mutex);
}

static void hub_unlock(void) {
	pthread_mutex_unlock(&hub_mutex);
}
#endif

#define WORD_IDLE 0x80000000u // what a side sends when it has nothing to say
#define HEADER 0x99660000u
#define FRAME_CYCLES 280896
#define POLL_CYCLES 16384 // ~1 ms: how often a waiting adapter listens to the radio
#define CLOCK_CYCLES 600  // the adapter clocking a word to the GBA (~36 us)

enum {
	CMD_RESET = 0x10,
	CMD_LINK_STATUS = 0x11,
	CMD_VERSION = 0x12,
	CMD_SYSTEM_STATUS = 0x13,
	CMD_SLOT_STATUS = 0x14,
	CMD_CONFIG_STATUS = 0x15,
	CMD_BROADCAST = 0x16,
	CMD_SETUP = 0x17,
	CMD_HOST_START = 0x19,
	CMD_HOST_POLL = 0x1a,
	CMD_HOST_END = 0x1b,
	CMD_SCAN_START = 0x1c,
	CMD_SCAN_POLL = 0x1d,
	CMD_SCAN_END = 0x1e,
	CMD_CONNECT = 0x1f,
	CMD_CONNECT_POLL = 0x20,
	CMD_CONNECT_END = 0x21,
	CMD_SEND = 0x24,
	CMD_SEND_WAIT = 0x25,
	CMD_RECEIVE = 0x26,
	CMD_WAIT = 0x27,
	CMD_DATA_READY = 0x28,
	CMD_DISCONNECTED = 0x29,
	CMD_DISCONNECT = 0x30,
	CMD_RECOVER_START = 0x32,
	CMD_RECOVER_POLL = 0x33,
	CMD_RECOVER_END = 0x34,
	CMD_WAIT_UNKNOWN = 0x35,
	CMD_RETRANSMIT_WAIT = 0x37,
	CMD_BYE = 0x3d,
};

// --- The hub: what each adapter's radio does, shared by the emulation threads ---

#define QUEUE 8
#define MAX_PACKET 90 // a host sends up to 87 bytes at a time, a client 16

struct packet {
	uint8_t size;
	uint8_t data[MAX_PACKET];
};

struct queue {
	struct packet p[QUEUE];
	int head, count;
};

enum role { ROLE_IDLE, ROLE_HOST, ROLE_SCAN, ROLE_CONNECTING, ROLE_CLIENT };

// Guarded by hub_mutex, except stamp's unlocked reads.
static struct node {
	bool on; // plugged in
	enum role role;
	uint16_t id;
	uint16_t target;      // connecting: the host's id
	bool open;            // host: accepting clients
	uint32_t game[6];     // broadcast data
	uint32_t setup;
	int max_clients;
	int client[4];        // host: the node in each slot, or -1
	int host, slot;       // client
	struct queue rx;      // client: from the host
	struct queue tx;      // client: for the host, which collects one at each of its sends
	struct queue from[4]; // host: collected from each slot's client
	struct packet last;   // host: the last packet sent (to retransmit)
	bool sent;            // host: a send-and-wait went out: tell it once it waits
	uint8_t lost;         // client: 1 + the reason it lost its host, told once it waits
	uint8_t lost_slots;
	volatile uint32_t stamp; // bumped by every change that may end this adapter's wait
} nodes[RFU_MAX];

static uint16_t id_seed = 0x1d3b;

static void push(struct queue* q, const struct packet* p) {
	if (q->count == QUEUE) { // full: the oldest is lost, as the radio would lose it
		q->head = (q->head + 1) % QUEUE;
		--q->count;
	}
	q->p[(q->head + q->count++) % QUEUE] = *p;
}

static bool pop(struct queue* q, struct packet* p) {
	if (!q->count) {
		return false;
	}
	*p = q->p[q->head];
	q->head = (q->head + 1) % QUEUE;
	--q->count;
	return true;
}

// Adapters get a new random-looking id each time they host or connect, unique in range.
static uint16_t new_id(void) {
	for (;;) {
		id_seed = id_seed * 25173 + 13849;
		bool used = id_seed == 0;
		for (int i = 0; i < RFU_MAX; ++i) {
			used |= nodes[i].id == id_seed;
		}
		if (!used) {
			return id_seed;
		}
	}
}

static int free_slot(const struct node* n) {
	for (int s = 0; s < n->max_clients; ++s) {
		if (n->client[s] < 0) {
			return s;
		}
	}
	return 0xFF;
}

static int clients(const struct node* n) {
	int count = 0;
	for (int s = 0; s < 4; ++s) {
		count += n->client[s] >= 0;
	}
	return count;
}

// A client loses its host (reason 0: the host disconnected it, 1: the link was lost).
static void drop_client(int c, int reason) {
	struct node* n = &nodes[c];
	log_printf("rfu: P%d left P%d's room (%s)\n", c + 1, n->host + 1, reason ? "host gone" : "disconnected by the host");
	n->lost = 1 + reason;
	n->lost_slots = 1 << n->slot;
	n->role = ROLE_IDLE;
	n->id = 0;
	n->rx.count = n->tx.count = 0;
	++n->stamp;
}

// Back to idle. A host's clients lose it; a client frees its host's slot, so the host sees its
// signal drop and times it out, as if that GBA had been switched off.
static void leave(int i) {
	struct node* n = &nodes[i];
	if (n->role == ROLE_HOST) {
		for (int s = 0; s < 4; ++s) {
			if (n->client[s] >= 0) {
				drop_client(n->client[s], 1);
			}
			n->client[s] = -1;
			n->from[s].count = 0;
		}
	} else if (n->role == ROLE_CLIENT) {
		struct node* h = &nodes[n->host];
		log_printf("rfu: P%d left P%d's room\n", i + 1, n->host + 1);
		h->client[n->slot] = -1;
		h->from[n->slot].count = 0;
		++h->stamp;
	}
	n->role = ROLE_IDLE;
	n->open = false;
	n->id = 0;
	n->sent = false;
	n->lost = 0;
	n->rx.count = n->tx.count = 0;
}

static void try_connect(int i) {
	struct node* n = &nodes[i];
	for (int h = 0; h < RFU_MAX; ++h) {
		struct node* host = &nodes[h];
		if (h == i || !host->on || host->role != ROLE_HOST || !host->open || host->id != n->target) {
			continue;
		}
		int s = free_slot(host);
		if (s == 0xFF) {
			return;
		}
		host->client[s] = i;
		host->from[s].count = 0;
		++host->stamp;
		n->role = ROLE_CLIENT;
		n->host = h;
		n->slot = s;
		n->id = new_id();
		n->lost = 0;
		n->rx.count = n->tx.count = 0;
		log_printf("rfu: P%d joined P%d's room (slot %d)\n", i + 1, h + 1, s);
		return;
	}
}

// Bytes are sent least significant first in each word.
static void unpack(struct packet* p, const uint32_t* words, int count, int size) {
	p->size = size < MAX_PACKET ? size : MAX_PACKET;
	for (int b = 0; b < p->size; ++b) {
		p->data[b] = b / 4 < count ? words[b / 4] >> (8 * (b % 4)) : 0;
	}
}

static int pack(uint32_t* words, const uint8_t* bytes, int size) {
	int count = (size + 3) / 4;
	memset(words, 0, count * sizeof(*words));
	for (int b = 0; b < size; ++b) {
		words[b / 4] |= (uint32_t) bytes[b] << (8 * (b % 4));
	}
	return count;
}

// A host's packet goes to every client, and each client's oldest queued packet comes back.
static void deliver(int i, const struct packet* p, bool wait) {
	struct node* n = &nodes[i];
	for (int s = 0; s < 4; ++s) {
		int c = n->client[s];
		if (c < 0) {
			continue;
		}
		struct packet back;
		push(&nodes[c].rx, p);
		if (pop(&nodes[c].tx, &back)) {
			push(&n->from[s], &back);
		}
		++nodes[c].stamp;
	}
	if (wait) {
		n->sent = true;
		++n->stamp;
	}
}

// A send: a header word (the byte count; a client's at bit 8 + 5 * its slot), then the bytes.
static void send(int i, const uint32_t* in, int count, bool wait) {
	struct node* n = &nodes[i];
	if (!count) {
		return;
	}
	int bytes = n->role == ROLE_HOST ? in[0] & 0x7F : in[0] >> (8 + 5 * n->slot) & 0x1F;
	// A header alone resends a few bytes: games send one so that their clients can talk.
	int room = count > 1 ? 4 * (count - 1) : 4;
	struct packet p;
	unpack(&p, in + 1, count - 1, bytes < room ? bytes : room);
	if (n->role == ROLE_HOST) {
		n->last = p;
		deliver(i, &p, wait);
	} else if (n->role == ROLE_CLIENT) {
		push(&n->tx, &p);
	}
}

// A host receives one packet per client, their byte counts in the header (5 bits per slot from
// bit 8) and their bytes end to end; a client receives the host's (byte count in bits 0-6).
static int receive(int i, uint32_t* out) {
	struct node* n = &nodes[i];
	uint8_t bytes[4 * 31 + MAX_PACKET];
	uint32_t header = 0;
	int size = 0;
	bool got = false;
	struct packet p;
	if (n->role == ROLE_HOST) {
		for (int s = 0; s < 4; ++s) {
			if (pop(&n->from[s], &p)) {
				int b = p.size < 31 ? p.size : 31;
				header |= (uint32_t) b << (8 + 5 * s);
				memcpy(bytes + size, p.data, b);
				size += b;
				got = true;
			}
		}
	} else if (n->role == ROLE_CLIENT && pop(&n->rx, &p)) {
		header = p.size;
		memcpy(bytes, p.data, p.size);
		size = p.size;
		got = true;
	}
	if (!got) {
		return 0;
	}
	out[0] = header;
	return 1 + pack(out + 1, bytes, size);
}

static void disconnect(int i, uint32_t slots) {
	struct node* n = &nodes[i];
	if (n->role == ROLE_HOST) {
		for (int s = 0; s < 4; ++s) {
			if (slots >> s & 1 && n->client[s] >= 0) {
				drop_client(n->client[s], 0);
				n->client[s] = -1;
				n->from[s].count = 0;
			}
		}
	} else if (n->role == ROLE_CLIENT && slots >> n->slot & 1) {
		leave(i);
	}
}

// Each client: its id, its slot at bits 16-23.
static int list_clients(const struct node* n, uint32_t* out) {
	int words = 0;
	if (n->role == ROLE_HOST) {
		for (int s = 0; s < 4; ++s) {
			if (n->client[s] >= 0) {
				out[words++] = nodes[n->client[s]].id | s << 16;
			}
		}
	}
	return words;
}

// Bits 0-15: id, 16-23: a client's slot (one bit), 24-31: idle, host with a closed or open
// room, searching, connecting, connected.
static uint32_t system_status(const struct node* n) {
	switch (n->role) {
	case ROLE_HOST:
		return (n->open ? 2u : 1u) << 24 | n->id;
	case ROLE_SCAN:
		return 3u << 24;
	case ROLE_CONNECTING:
		return 4u << 24;
	case ROLE_CLIENT:
		return 5u << 24 | 1u << (16 + n->slot) | n->id;
	default:
		return 0;
	}
}

// Runs a command against the hub (locked): its reply's words in out, or minus an error code
// (1: not now, 2: unknown command).
static int execute(struct rfu* rfu, uint32_t* out) {
	int i = rfu->node;
	struct node* n = &nodes[i];
	const uint32_t* in = rfu->req;
	int count = rfu->req_got < 32 ? rfu->req_got : 32;
	int words = 0;
	switch (rfu->cmd) {
	case CMD_RESET:
		leave(i);
		memset(n->game, 0, sizeof(n->game));
		n->setup = 0;
		n->max_clients = 4;
		rfu->timeout = 0;
		break;
	case CMD_LINK_STATUS: // signal strength per slot, a byte each
		out[0] = 0;
		if (n->role == ROLE_HOST) {
			for (int s = 0; s < 4; ++s) {
				out[0] |= n->client[s] >= 0 ? 0xFFu << (8 * s) : 0;
			}
		} else if (n->role == ROLE_CLIENT) {
			out[0] = 0xFFu << (8 * n->slot);
		}
		words = 1;
		break;
	case CMD_VERSION:
		out[words++] = 0x00830117; // a real adapter's
		break;
	case CMD_SYSTEM_STATUS:
		out[words++] = system_status(n);
		break;
	case CMD_SLOT_STATUS:
		out[words++] = n->role == ROLE_HOST && n->open ? free_slot(n) : 0xFF;
		words += list_clients(n, out + words);
		break;
	case CMD_CONFIG_STATUS:
		memcpy(out, n->role == ROLE_HOST ? n->game : (uint32_t[6]) { 0 }, sizeof(n->game));
		words = 6;
		if (n->role == ROLE_HOST) {
			out[words++] = n->setup;
		}
		out[words++] = 257;
		break;
	case CMD_BROADCAST:
		memset(n->game, 0, sizeof(n->game));
		memcpy(n->game, in, (count < 6 ? count : 6) * sizeof(*in));
		break;
	case CMD_SETUP: // bits 0-7: wait timeout in frames, 16-17: room for 4, 3, 2 or 1 clients
		if (count) {
			n->setup = in[0];
			n->max_clients = 4 - (in[0] >> 16 & 3);
			rfu->timeout = in[0];
		}
		break;
	case CMD_HOST_START:
		if (n->role != ROLE_HOST) {
			leave(i);
			n->role = ROLE_HOST;
			n->id = new_id();
			for (int s = 0; s < 4; ++s) {
				n->client[s] = -1;
				n->from[s].count = 0;
			}
		}
		n->open = true;
		break;
	case CMD_HOST_POLL:
		words = list_clients(n, out);
		break;
	case CMD_HOST_END:
		words = list_clients(n, out);
		n->open = false;
		if (n->role == ROLE_HOST && !clients(n)) {
			n->role = ROLE_IDLE;
			n->id = 0;
		}
		break;
	case CMD_SCAN_START:
		leave(i);
		n->role = ROLE_SCAN;
		break;
	case CMD_SCAN_POLL:
	case CMD_SCAN_END: // each open room: its id and next free slot, then its broadcast data
		for (int h = 0; h < RFU_MAX && words < 4 * 7; ++h) {
			const struct node* host = &nodes[h];
			if (h != i && host->on && host->role == ROLE_HOST && host->open) {
				out[words++] = host->id | free_slot(host) << 16;
				memcpy(out + words, host->game, sizeof(host->game));
				words += 6;
			}
		}
		if (rfu->cmd == CMD_SCAN_END && n->role == ROLE_SCAN) {
			n->role = ROLE_IDLE;
		}
		break;
	case CMD_CONNECT:
		leave(i);
		n->role = ROLE_CONNECTING;
		n->target = count ? in[0] : 0;
		try_connect(i);
		break;
	case CMD_CONNECT_POLL:
	case CMD_CONNECT_END: // our new id and slot once connected, else status 1 at bits 24-31
		if (n->role == ROLE_CONNECTING) {
			try_connect(i); // the room may have opened again (games keep polling a while)
		}
		out[words++] = n->role == ROLE_CLIENT ? (uint32_t) (n->id | n->slot << 16) : 1u << 24;
		if (rfu->cmd == CMD_CONNECT_END && n->role == ROLE_CONNECTING) {
			n->role = ROLE_IDLE;
		}
		break;
	case CMD_SEND:
	case CMD_SEND_WAIT:
		send(i, in, count, rfu->cmd == CMD_SEND_WAIT);
		break;
	case CMD_RETRANSMIT_WAIT:
		if (n->role == ROLE_HOST) {
			deliver(i, &n->last, true);
		}
		break;
	case CMD_RECEIVE:
		words = receive(i, out);
		break;
	case CMD_WAIT:
	case CMD_WAIT_UNKNOWN:
		break;
	case CMD_DISCONNECT:
		disconnect(i, count ? in[0] : 0);
		break;
	case CMD_RECOVER_START:
		break;
	case CMD_RECOVER_POLL:
	case CMD_RECOVER_END:
		out[words++] = 1; // failed: a client only loses its host when that one is gone
		break;
	case CMD_BYE:
		leave(i);
		break;
	default:
		return -2;
	}
	return words;
}

// What a waiting adapter tells its GBA (hub locked): 0x29 and the lost slot and reason once a
// client lost its host; 0x28 when data came in (a host: also right after its send-and-wait,
// every client having received it).
static int wait_event(struct rfu* rfu, uint32_t* event) {
	struct node* n = &nodes[rfu->node];
	if (n->lost) {
		event[0] = HEADER | 1 << 8 | CMD_DISCONNECTED;
		event[1] = n->lost_slots | (n->lost - 1) << 8;
		n->lost = 0;
		return 2;
	}
	bool ready = false;
	if (n->role == ROLE_HOST) {
		ready = n->sent;
		for (int s = 0; s < 4; ++s) {
			ready |= n->from[s].count > 0;
		}
	} else if (n->role == ROLE_CLIENT) {
		ready = n->rx.count > 0;
	}
	if (!ready) {
		return 0;
	}
	n->sent = false;
	event[0] = HEADER | CMD_DATA_READY;
	return 1;
}

void rfu_describe(int node, char* out, size_t size) {
	hub_lock();
	const struct node* n = &nodes[node];
	switch (n->role) {
	case ROLE_HOST:
		snprintf(out, size, "host%s, %d clients", n->open ? " (open)" : "", clients(n));
		break;
	case ROLE_CLIENT:
		snprintf(out, size, "client of P%d", n->host + 1);
		break;
	case ROLE_SCAN:
		snprintf(out, size, "searching");
		break;
	case ROLE_CONNECTING:
		snprintf(out, size, "connecting");
		break;
	default:
		snprintf(out, size, n->on ? "idle" : "unplugged");
		break;
	}
	hub_unlock();
}

// --- The adapter: the serial side, on its player's emulation thread ---

static struct mTiming* timing_of(struct rfu* rfu) {
	return &rfu->d.p->p->timing;
}

static void stop_waiting(struct rfu* rfu) {
	rfu->waiting = false;
	rfu->event_size = 0;
	mTimingDeschedule(timing_of(rfu), &rfu->poll);
}

static void start_waiting(struct rfu* rfu) {
	rfu->waiting = true;
	rfu->event_size = 0;
	rfu->wait_start = mTimingCurrentTime(timing_of(rfu));
	rfu->seen = nodes[rfu->node].stamp - 1; // look at once
	mTimingDeschedule(timing_of(rfu), &rfu->poll);
	mTimingSchedule(timing_of(rfu), &rfu->poll, CLOCK_CYCLES);
}

static void run_command(struct rfu* rfu) {
	hub_lock();
	int words = execute(rfu, rfu->resp + 1);
	hub_unlock();
	if (words < 0) {
		rfu->resp[0] = HEADER | 1 << 8 | 0xEE;
		rfu->resp[1] = -words;
		rfu->resp_size = 2;
	} else {
		rfu->resp[0] = HEADER | words << 8 | (uint8_t) (rfu->cmd + 0x80);
		rfu->resp_size = 1 + words;
	}
	if (rfu_trace) {
		log_printf("rfu P%d: %02x", rfu->node + 1, rfu->cmd);
		for (int w = 0; w < rfu->req_got && w < 32; ++w) {
			log_printf(" %08x", rfu->req[w]);
		}
		log_printf(" ->");
		for (int w = 0; w < rfu->resp_size; ++w) {
			log_printf(" %08x", rfu->resp[w]);
		}
		log_printf("\n");
	}
	rfu->resp_sent = 0;
	rfu->next = rfu->resp[0];
}

// The adapter's side of the NINTENDO exchange: each half-word pair goes out once the GBA echoed
// the previous one (the games' own library runs the same steps on its side), then the
// adapter's id 0x8001; the GBA's 0x8001 back ends it.
static void login(struct rfu* rfu, uint32_t word) {
	static const uint16_t nintendo[4] = { 0x494e, 0x544e, 0x4e45, 0x4f44 };
	uint16_t echo = word >> 16, own = word;
	if (echo == rfu->login_recv) {
		if (rfu->login_count == 4) {
			rfu->logged_in = true;
			rfu->req_size = -1;
			rfu->resp_sent = -1;
			rfu->next = WORD_IDLE;
			if (rfu_trace) {
				log_printf("rfu P%d: logged in\n", rfu->node + 1);
			}
			return;
		}
		if (rfu->login_recv == (uint16_t) ~rfu->login_send && own == (uint16_t) ~rfu->login_recv) {
			++rfu->login_count;
		}
	} else {
		rfu->login_count = 0;
	}
	rfu->login_send = rfu->login_count < 4 ? nintendo[rfu->login_count] : 0x8001;
	rfu->login_recv = ~own;
	rfu->next = (uint32_t) rfu->login_send << 16 | rfu->login_recv;
}

// A transfer completed: the GBA sent `word` (clocked by the GBA unless `ours`).
static void receive_word(struct rfu* rfu, uint32_t word, bool ours) {
	if (!rfu->logged_in) {
		login(rfu, word);
		return;
	}
	if (rfu->waiting && ours) {
		// We clocked event[event_sent], or after the last one the idle word that brings the GBA's ack.
		if (++rfu->event_sent < rfu->event_size) {
			rfu->next = rfu->event[rfu->event_sent];
		} else if (rfu->event_sent == rfu->event_size) {
			rfu->next = WORD_IDLE;
		} else {
			stop_waiting(rfu);
			rfu->next = WORD_IDLE;
		}
		return;
	}
	if (rfu->waiting) {
		stop_waiting(rfu); // the GBA clocks again: it gave up waiting
	}
	if (rfu->resp_sent >= 0) { // replying
		if ((word >> 16) == (HEADER >> 16)) { // the GBA started over
			rfu->resp_sent = -1;
			rfu->req_size = -1;
		} else if (++rfu->resp_sent < rfu->resp_size) {
			rfu->next = rfu->resp[rfu->resp_sent];
			return;
		} else {
			rfu->resp_sent = -1;
			rfu->req_size = -1;
			rfu->next = WORD_IDLE;
			uint8_t ack = rfu->resp[0];
			if (ack == (uint8_t) (CMD_SEND_WAIT + 0x80) || ack == (uint8_t) (CMD_WAIT + 0x80) ||
			    ack == (uint8_t) (CMD_WAIT_UNKNOWN + 0x80) || ack == (uint8_t) (CMD_RETRANSMIT_WAIT + 0x80)) {
				start_waiting(rfu); // the GBA hands the clock over
			}
			return;
		}
	}
	rfu->next = WORD_IDLE;
	if (rfu->req_size < 0) { // a command's header
		if ((word >> 16) != (HEADER >> 16)) {
			return;
		}
		rfu->cmd = word;
		rfu->req_size = word >> 8 & 0xFF;
		rfu->req_got = 0;
	} else {
		if (rfu->req_got < 32) {
			rfu->req[rfu->req_got] = word;
		}
		++rfu->req_got;
	}
	if (rfu->req_got == rfu->req_size) {
		run_command(rfu);
	}
}

static void transfer_done(struct mTiming* timing, void* context, uint32_t late) {
	(void) timing;
	struct rfu* rfu = context;
	uint32_t out = rfu->next;
	rfu->busy = false;
	receive_word(rfu, rfu->gba, !rfu->gba_clocks);
	GBASIONormal32FinishTransfer(rfu->d.p, out, late);
}

static bool tells(const struct rfu* rfu) {
	return rfu->waiting && rfu->event_size;
}

// While waiting: anything to tell the GBA? Then clock it out as soon as the GBA is ready.
static void poll(struct mTiming* timing, void* context, uint32_t late) {
	(void) late;
	struct rfu* rfu = context;
	if (!rfu->waiting || rfu->event_size) {
		return;
	}
	if (nodes[rfu->node].stamp != rfu->seen) {
		hub_lock();
		rfu->seen = nodes[rfu->node].stamp;
		rfu->event_size = wait_event(rfu, rfu->event);
		hub_unlock();
	}
	if (!rfu->event_size && rfu->timeout &&
	    (uint32_t) (mTimingCurrentTime(timing) - rfu->wait_start) >= (uint32_t) rfu->timeout * FRAME_CYCLES) {
		rfu->event[0] = HEADER | CMD_WAIT;
		rfu->event_size = 1;
	}
	if (!rfu->event_size) {
		mTimingSchedule(timing, &rfu->poll, POLL_CYCLES);
		return;
	}
	if (rfu_trace) {
		log_printf("rfu P%d: tells %08x\n", rfu->node + 1, rfu->event[0]);
	}
	rfu->event_sent = 0;
	rfu->next = rfu->event[0];
	if (rfu->busy && !mTimingIsScheduled(timing, &rfu->transfer)) {
		rfu->gba_clocks = false;
		mTimingSchedule(timing, &rfu->transfer, CLOCK_CYCLES);
	}
}

static void reset_adapter(struct rfu* rfu) {
	mTimingDeschedule(timing_of(rfu), &rfu->transfer);
	mTimingDeschedule(timing_of(rfu), &rfu->poll);
	rfu->busy = false;
	rfu->next = 0;
	rfu->logged_in = false;
	rfu->login_send = rfu->login_recv = 0;
	rfu->login_count = 0;
	rfu->req_size = -1;
	rfu->resp_sent = -1;
	rfu->waiting = false;
	rfu->event_size = 0;
	rfu->timeout = 0;
	hub_lock();
	leave(rfu->node);
	hub_unlock();
}

static bool rfu_init_driver(struct GBASIODriver* driver) {
	struct rfu* rfu = (struct rfu*) driver;
	hub_lock();
	struct node* n = &nodes[rfu->node];
	memset(n, 0, sizeof(*n));
	n->on = true;
	n->max_clients = 4;
	for (int s = 0; s < 4; ++s) {
		n->client[s] = -1;
	}
	hub_unlock();
	reset_adapter(rfu);
	return true;
}

static void rfu_deinit(struct GBASIODriver* driver) {
	struct rfu* rfu = (struct rfu*) driver;
	reset_adapter(rfu);
	hub_lock();
	nodes[rfu->node].on = false;
	hub_unlock();
}

static void rfu_reset(struct GBASIODriver* driver) {
	reset_adapter((struct rfu*) driver);
}

static bool rfu_handles_mode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	(void) driver;
	return mode == GBA_SIO_NORMAL_32;
}

static int rfu_connected(struct GBASIODriver* driver) {
	(void) driver;
	return 1;
}

// mGBA's own transfers only in the other modes: in Normal 32-bit mode we time them ourselves
// (writeSIOCNT), since the clock can come from either side.
static bool rfu_start(struct GBASIODriver* driver) {
	return driver->p->mode != GBA_SIO_NORMAL_32;
}

// Holding SD high resets the adapter (the games do it, in GPIO mode, before the login).
static uint16_t rfu_write_rcnt(struct GBASIODriver* driver, uint16_t value) {
	struct rfu* rfu = (struct rfu*) driver;
	bool sd = (value & 0xC000) == 0x8000 && (value & 0x22) == 0x22;
	if (sd && !rfu->sd) {
		reset_adapter(rfu);
		if (rfu_trace) {
			log_printf("rfu P%d: reset\n", rfu->node + 1);
		}
	}
	rfu->sd = sd;
	return value;
}

static uint16_t rfu_write_siocnt(struct GBASIODriver* driver, uint16_t value) {
	struct rfu* rfu = (struct rfu*) driver;
	struct GBA* gba = driver->p->p;
	bool internal = value & 1, so = value & 8;
	// SI answers the handshake between words at once: opposite to SO while the GBA clocks,
	// following SO while the adapter does.
	if (internal != so) {
		value |= 4;
	} else {
		value &= ~4;
	}
	if (!(value & 0x80)) { // stopped
		if (rfu->busy) {
			mTimingDeschedule(&gba->timing, &rfu->transfer);
			rfu->busy = false;
		}
		return value;
	}
	if (!rfu->busy) {
		rfu->busy = true;
		rfu->gba = gba->memory.io[GBA_REG(SIODATA32_LO)] | (uint32_t) gba->memory.io[GBA_REG(SIODATA32_HI)] << 16;
	}
	if (mTimingIsScheduled(&gba->timing, &rfu->transfer)) {
		return value;
	}
	if (internal) { // the GBA clocks it (also once it switches to its own clock mid-transfer)
		rfu->gba_clocks = true;
		mTimingSchedule(&gba->timing, &rfu->transfer, GBASIOTransferCycles(GBA_SIO_NORMAL_32, value, 0));
	} else if (tells(rfu)) {
		rfu->gba_clocks = false;
		mTimingSchedule(&gba->timing, &rfu->transfer, CLOCK_CYCLES);
	}
	return value;
}

void rfu_create(struct rfu* rfu, int node) {
	memset(rfu, 0, sizeof(*rfu));
	rfu->node = node;
	rfu->d.init = rfu_init_driver;
	rfu->d.deinit = rfu_deinit;
	rfu->d.reset = rfu_reset;
	rfu->d.handlesMode = rfu_handles_mode;
	rfu->d.connectedDevices = rfu_connected;
	rfu->d.start = rfu_start;
	rfu->d.writeRCNT = rfu_write_rcnt;
	rfu->d.writeSIOCNT = rfu_write_siocnt;
	rfu->transfer = (struct mTimingEvent) { .context = rfu, .name = "RFU transfer", .callback = transfer_done, .priority = 0x80 };
	rfu->poll = (struct mTimingEvent) { .context = rfu, .name = "RFU poll", .callback = poll, .priority = 0x81 };
}
