// Can a PS3 app run code it wrote at runtime (needed for a dynarec)?
// One probe per launch: executing non-executable memory kills the app, so every step is logged
// first and the next launch marks an unfinished probe as crashed and moves on.
// Results: /dev_hdd0/tmp/jitprobe.txt. Driven by tools/jitprobe.sh.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <ppu-lv2.h>
#include <sys/memory.h>

#define OUT "/dev_hdd0/jitprobe.txt" // not tmp: RPCS3 empties /dev_hdd0/tmp when it starts
#define ALLOW_POKE "/dev_hdd0/jitprobe_allow_poke"
#define NPROBES 8

#ifdef IMPORTS
#include <audio/audio.h>
#include <io/pad.h>
#include <net/net.h>
#include <net/netctl.h>
#include <rsx/gcm_sys.h>
#include <sysmodule/sysmodule.h>
#include <sysutil/sysutil.h>
// ps3gba's imported libraries, referenced but never called: IMPORTS=1 checks whether importing
// them keeps an app from starting on HEN.
__attribute__((used)) static const void* imports[] = {
#ifdef IMP_audio
	(const void*) audioInit,
#endif
#ifdef IMP_io
	(const void*) ioPadInit,
#endif
#ifdef IMP_net
	(const void*) netInitialize,
#endif
#ifdef IMP_netctl
	(const void*) netCtlInit,
#endif
#ifdef IMP_gcm
	(const void*) gcmGetConfiguration,
#endif
#ifdef IMP_sysmodule
	(const void*) sysModuleLoad,
#endif
#ifdef IMP_sysutil
	(const void*) sysUtilRegisterCallback,
#endif
	NULL,
};
#endif

#ifdef PROC_PARAM
#include <sys/process.h>
// ps3gba's process parameters (1 MB main stack, 1 MB malloc pages): PROC_PARAM=1 checks whether
// they keep an app from starting on HEN.
SYS_PROCESS_PARAM(1001, 0x100000);
#endif

// Size of the code cave in .text (build with CAVE_MB=n to try a big JIT code cache).
#ifdef CAVE_MB
#define CAVE_BYTES (CAVE_MB * 1024 * 1024)
#else
#define CAVE_BYTES 4096
#endif
#define STR_(x) #x
#define STR(x) STR_(x)

// li r3,42 ; blr
static const u32 CODE[2] = {0x3860002A, 0x4E800020};

// A page of blr inside .text (the only memory we know is executable), then zeros up to CAVE_BYTES.
__asm__(".section .text\n.balign 4096\n.globl jit_cave\njit_cave:\n.rept 1024\nblr\n.endr\n"
	".space " STR(CAVE_BYTES) " - 4096\n.previous\n");
extern char jit_cave[];

static char bss_buf[4096] __attribute__((aligned(4096)));

static void logf_(const char *fmt, ...)
{
	FILE *f = fopen(OUT, "a");
	if (!f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f); // closed per line so a crash right after loses nothing
}

static s32 page_attr(u64 addr, void *out) { lv2syscall2(351, addr, (u64)out); return_to_user_prog(s32); }
static u64 lv2_peek(u64 addr) { lv2syscall1(6, addr); return_to_user_prog(u64); }
static s32 lv2_poke(u64 addr, u64 val) { lv2syscall2(7, addr, val); return_to_user_prog(s32); }

static void flush_code(void *p, size_t n)
{
	uintptr_t a, end = (uintptr_t)p + n;
	for (a = (uintptr_t)p & ~127UL; a < end; a += 128) __asm__ volatile("dcbst 0,%0" ::"r"(a) : "memory");
	__asm__ volatile("sync" ::: "memory");
	for (a = (uintptr_t)p & ~127UL; a < end; a += 128) __asm__ volatile("icbi 0,%0" ::"r"(a) : "memory");
	__asm__ volatile("isync" ::: "memory");
}

// Raw jump (no ELFv1 function descriptor); returns r3.
static s64 call_code(void *p)
{
	s64 r;
	__asm__ volatile("mtctr %1\n\tbctrl\n\tmr %0,3"
		: "=r"(r) : "r"(p)
		: "r0", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12",
		  "ctr", "lr", "xer", "cr0", "cr1", "cr5", "cr6", "cr7", "memory");
	return r;
}

static void log_attr(int n, const char *name, void *p)
{
	u32 a[8] = {0};
	s32 ret = page_attr((u64)(uintptr_t)p, a);
	logf_("%d attr %-6s %p ret=%08x %08x %08x %08x %08x %08x %08x\n", n, name, p, (u32)ret,
		a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
}

static void exec_buffer(int n, const char *name, void *buf)
{
	if (!buf) { logf_("%d end %s allocation failed\n", n, name); return; }
	log_attr(n, name, buf);
	memcpy(buf, CODE, sizeof CODE);
	flush_code(buf, sizeof CODE);
	logf_("%d calling %s %p\n", n, name, buf);
	s64 r = call_code(buf);
	logf_("%d end %s returned %lld: %s\n", n, name, (long long)r, r == 42 ? "EXECUTABLE" : "wrong value");
}

static void *sys_alloc(size_t size, u64 flags)
{
	sys_mem_addr_t a = 0;
	s32 ret = sysMemoryAllocate(size, flags, &a);
	return ret ? NULL : (void *)(uintptr_t)a;
}

// Write the code into .text through the CFW/HEN lv2 poke syscall. Only if the kernel reads our
// page the same way we do (peek check), so a mismatched address can't hit kernel memory.
static void poke_text(int n)
{
	FILE *f = fopen(ALLOW_POKE, "r");
	if (!f) { logf_("%d end poke SKIPPED (no %s)\n", n, ALLOW_POKE); return; }
	fclose(f);
	u64 addr = (u64)(uintptr_t)jit_cave, want = 0x4E8000204E800020ULL;
	log_attr(n, "text", jit_cave);
	u64 seen = lv2_peek(addr);
	logf_("%d peek %016llx = %016llx\n", n, (unsigned long long)addr, (unsigned long long)seen);
	if (seen != want) { logf_("%d end poke ABORTED: kernel sees different bytes\n", n); return; }
	logf_("%d poking\n", n);
	s32 ret = lv2_poke(addr, ((u64)CODE[0] << 32) | CODE[1]);
	logf_("%d poke ret=%08x, now %016llx\n", n, (u32)ret, (unsigned long long)*(volatile u64 *)jit_cave);
	flush_code(jit_cave, sizeof CODE);
	logf_("%d calling text\n", n);
	s64 r = call_code(jit_cave);
	logf_("%d end poke returned %lld: %s\n", n, (long long)r, r == 42 ? "EXECUTABLE" : "wrong value");
}

// Code at the end and the middle of a big cave runs, and rewriting it runs the new code (a JIT
// rewrites its cache; an emulator that caches decoded code would still return the old value).
static void cave_end(int n)
{
	static const u32 CODE2[2] = {0x3860002B, 0x4E800020}; // li r3,43 ; blr
	char *spots[2] = {jit_cave + CAVE_BYTES / 2, jit_cave + CAVE_BYTES - sizeof CODE};
	logf_("%d cave %p size %d bytes\n", n, jit_cave, (int)CAVE_BYTES);
	for (int i = 0; i < 2; ++i) {
		char *p = spots[i];
		memcpy(p, CODE, sizeof CODE);
		flush_code(p, sizeof CODE);
		logf_("%d calling %p\n", n, p);
		s64 a = call_code(p);
		memcpy(p, CODE2, sizeof CODE2);
		flush_code(p, sizeof CODE2);
		s64 b = call_code(p);
		logf_("%d %p returned %lld then %lld after rewrite: %s\n", n, p, (long long)a, (long long)b,
			a == 42 && b == 43 ? "OK" : a == 42 && b == 42 ? "STALE (rewrite not seen)" : "WRONG");
	}
	logf_("%d end cave\n", n);
}

static void run_probe(int n)
{
	int local;
	switch (n) {
	case 0: // attributes only, safe
		log_attr(n, "text", jit_cave);
		log_attr(n, "bss", bss_buf);
		log_attr(n, "stack", &local);
		log_attr(n, "heap", memalign(4096, 4096));
		log_attr(n, "mem64k", sys_alloc(0x10000, SYS_MEMORY_PAGE_SIZE_64K));
		log_attr(n, "mem1m", sys_alloc(0x100000, SYS_MEMORY_PAGE_SIZE_1M));
		logf_("%d end attributes\n", n);
		break;
	case 1: exec_buffer(n, "heap", memalign(4096, 4096)); break;
	case 2: exec_buffer(n, "bss", bss_buf); break;
	case 3: exec_buffer(n, "mem64k", sys_alloc(0x10000, SYS_MEMORY_PAGE_SIZE_64K)); break;
	case 4: exec_buffer(n, "mem1m", sys_alloc(0x100000, SYS_MEMORY_PAGE_SIZE_1M)); break;
	case 5: exec_buffer(n, "text", jit_cave); break; // .text reports read-write: try a plain write
	case 6: poke_text(n); break;
	case 7: cave_end(n); break;
	}
}

int main(void)
{
	// Find where the previous launches stopped.
	int started = -1, ended = -1, n;
	char line[256], word[16];
	FILE *f = fopen(OUT, "r");
	if (f) {
		while (fgets(line, sizeof line, f))
			if (sscanf(line, "%d %15s", &n, word) == 2) {
				if (!strcmp(word, "start")) started = n;
				if (!strcmp(word, "end")) ended = n;
			}
		fclose(f);
	}
	if (started > ended) logf_("%d end CRASHED (app died during the probe)\n", started);
	int next = started + 1;
	if (next >= NPROBES) { logf_("done\n"); return 0; }
	logf_("%d start\n", next);
	run_probe(next);
	if (next + 1 >= NPROBES) logf_("done\n");
	return 0;
}
