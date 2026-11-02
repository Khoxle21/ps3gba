#ifndef RFU_H
#define RFU_H

#include <stdbool.h>
#include <stdint.h>

#include <mgba/core/timing.h>
#include <mgba/gba/interface.h>

// Adapters in range of each other: one per player.
#define RFU_MAX 4

// One player's GBA Wireless Adapter (AGB-015), an SPI device on its GBA's serial port:
// core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, &rfu->d) plugs it in, NULL unplugs it. Only
// that player's emulation thread calls into it (through mGBA); the adapters reach each other
// through a shared hub (the radio), so one game's thread never waits for another's.
struct rfu {
	struct GBASIODriver d;
	int node; // place in the hub, 0 to RFU_MAX - 1 (the player)
	struct mTimingEvent transfer, poll;
	bool busy;       // the GBA started a transfer that hasn't completed
	bool gba_clocks; // in it: the GBA's clock, else the adapter's
	bool sd;         // the reset line is held high
	uint32_t gba;    // the word the GBA sends in it
	uint32_t next;   // the word the adapter sends in it (decided by the previous one)
	// Login: the "NINTENDO" exchange that follows a reset.
	bool logged_in;
	uint16_t login_send, login_recv;
	int login_count;
	// A command from the GBA (req_size < 0: waiting for one), then the adapter's reply: ack
	// header and data (resp_sent < 0: not replying).
	uint8_t cmd;
	int req_size, req_got;
	uint32_t req[32];
	int resp_size, resp_sent;
	uint32_t resp[40];
	// After a "wait" command the adapter is the clock master until it has told the GBA
	// something: data arrived, a disconnection or a timeout.
	bool waiting;
	int event_size, event_sent; // event_size 0: nothing to tell yet
	uint32_t event[2];
	uint32_t wait_start, seen;
	uint8_t timeout; // frames, from Setup (0: none)
};

void rfu_init(void); // once, before any adapter is plugged in
void rfu_create(struct rfu* rfu, int node);
// Hub state for the log: "host 2 clients", "client of P1", "idle"...
void rfu_describe(int node, char* out, size_t size);
extern bool rfu_trace; // log every command and reply (desktop harness)

#endif
