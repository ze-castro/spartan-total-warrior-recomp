/* Native runtime entry: maps the guest address space, loads default.xbe, wires up the kernel
 * imports and runs the recompiled entry point.
 *
 * usage: stw [path/to/default.xbe]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include "kernel.h"
#include "paths.h"
#include "input.h"
#include "lib.h"
#include "nv2a.h"
#include "gametables.h"
#include "display.h"

uint8_t *G;

#define XBE_ENTRY_KEY 0xA8FC57ABu
#define XBE_THUNK_KEY 0x5B6D40B6u
#define PHYS_RAM   0x10000000u
#define STACK_TOP  0x7D000000u
#define STACK_SIZE 0x00100000u

static uint32_t u32(const uint8_t *p, uint32_t o) { uint32_t v; memcpy(&v, p + o, 4); return v; }

void rt_bad_indirect(CPU *c, uint32_t t) {
    if (t >= KSTUB_BASE && t < KSTUB_LIMIT) { kernel_dispatch(c, t); return; }
    if (lib_dispatch(c, t)) return;
    fprintf(stderr, "stw: call to unlifted/unknown guest address %#x (return address %#x)\n",
            t, LD32(c->esp));
    {   void *bt[12]; int n = backtrace(bt, 12); backtrace_symbols_fd(bt, n, 2); }     /* host frames show which lifted function made the call */
    fprintf(stderr, "stw: kernel thunk table @0x27d940:");
    for (uint32_t a = 0x27d940, z = 0; a < 0x27db20; a += 4) { if (!LD32(a)) { fprintf(stderr, " [%x]=0", a); z++; } }
    fprintf(stderr, "\n");
    guest_backtrace(c);
    if (rt_trace_fn == 2) rt_dump_rings();
    exit(1);
}

static void *dump_after(void *arg) {
    sleep((unsigned)(uintptr_t)arg);
    rt_dump_rings();
    fprintf(stderr, "PMC_INTR_0=%08x PMC_INTR_EN=%08x PBUS=%08x PFIFO_INTR_0=%08x PFIFO_INTR_EN=%08x PGRAPH_INTR=%08x PCRTC=%08x PTIMER=%08x\n",
            LD32(NV2A_BASE + 0x100), LD32(NV2A_BASE + 0x140), LD32(NV2A_BASE + 0x1100), LD32(NV2A_BASE + 0x2100),
            LD32(NV2A_BASE + 0x2140), LD32(NV2A_BASE + 0x400100), LD32(NV2A_BASE + 0x600100), LD32(NV2A_BASE + 0x9100));
    {   uint32_t dev = LD32(0x235528);
        fprintf(stderr, "device %#x: +0=%#x +24=%#x +28=%#x +2c=%#x +30=%#x(->%#x) +44=%#x +934=%#x PGRAPH_B10=%#x\n", dev, LD32(dev), LD32(dev + 0x24),
                LD32(dev + 0x28), LD32(dev + 0x2c), LD32(dev + 0x30), LD32(LD32(dev + 0x30)), LD32(dev + 0x44), LD32(dev + 0x934), LD32(NV2A_BASE + 0x400B10));
    }
    {   uint32_t put = LD32(NV2A_BASE + 0x800040);
        fprintf(stderr, "push buffer around PUT:");
        for (uint32_t a = put - 0x40; a < put + 0x20; a += 4) fprintf(stderr, "%s%08x", a == put ? " |" : " ", LD32(a));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "DMA_PUT=%08x DMA_GET=%08x\n", LD32(NV2A_BASE + 0x800040), LD32(NV2A_BASE + 0x800044));
    _exit(0);
}

static void fault_handler(int sig, siginfo_t *si, void *ctx) {
    (void)ctx;
    uintptr_t a = (uintptr_t)si->si_addr;
    fprintf(stderr, "stw: fatal signal %d, fault address %p (%s)\n", sig, si->si_addr,
            a >= (uintptr_t)G && a < (uintptr_t)G + (1ull << 32) ? "inside guest memory" : "host address");
    if (a >= (uintptr_t)G && a < (uintptr_t)G + (1ull << 32)) fprintf(stderr, "stw: guest address %#x\n", (uint32_t)(a - (uintptr_t)G));
    void *bt[24];
    int n = backtrace(bt, 24);
    backtrace_symbols_fd(bt, n, 2);
    _exit(139);
}

static uint32_t guest_entry;

/* The guest's first thread: runs the XBE entry point, which creates the game's own threads and returns. */
static void *guest_main(void *arg) {
    (void)arg;
    gil_init();
    kobj_start_services();
    nv2a_start();
    CPU c;
    memset(&c, 0, sizeof c);
    c.fcw = 0x27F;
    c.fs = kpcr_new(STACK_TOP, STACK_SIZE);
    c.esp = STACK_TOP - 16;
    ST32(c.esp, 0);                                           /* return address: entry never returns */
    fprintf(stderr, "stw: entry %#x, %d lifted functions\n", guest_entry, stw_tables->nfuncs);
    call_ind(&c, guest_entry);
    fprintf(stderr, "stw: entry thread finished; other threads keep running\n");
    gil_release();
    return NULL;                                              /* returning from a start routine ends the thread */
}

static void start_guest_thread(void) {
    pthread_t th;
    pthread_create(&th, NULL, guest_main, NULL);
    pthread_detach(th);
}

static int g_argc;
static char **g_argv;

/* Reads default.xbe, builds the guest address space and patches the kernel imports. Runs after the launcher has settled which disc to use. */
static int load_game_image(int argc, char **argv) {
    if (argc <= 1) {
        char why[300];
        if (!stw_game_valid(stw_game_root(), why, sizeof why)) { fprintf(stderr, "stw: no usable game files in %s: %s\n", stw_game_root(), why); return 2; }
    }
    char defpath[1200];
    snprintf(defpath, sizeof defpath, "%s/default.xbe", stw_game_root());
    const char *path = argc > 1 ? argv[1] : defpath;
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *x = malloc(n);
    if (fread(x, 1, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); return 2; }
    fclose(f);
    if (memcmp(x, "XBEH", 4)) { fprintf(stderr, "%s: not an XBE\n", path); return 2; }

    G = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (G == MAP_FAILED) { perror("mmap"); return 2; }
    {   /* Physical RAM: [0, 256 MB) is backed by one shared object mapped twice, at 0 and at 0x80000000,
         * like the Xbox kernel's identity + contiguous-memory windows (virtual 0x80000000+p == physical p). */
        char name[64];
        snprintf(name, sizeof name, "/stw_ram_%d", (int)getpid());
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0 || (shm_unlink(name), ftruncate(fd, PHYS_RAM))) { perror("shm"); return 2; }
        if (mmap(G, PHYS_RAM, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED ||
            mmap(G + 0x80000000u, PHYS_RAM, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED ||
            /* the write-combined alias D3D hands out for locked surfaces: 64 MB at 0xF0000000 */
            mmap(G + 0xF0000000u, 0x04000000u, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
            perror("mmap ram"); return 2;
        }
        close(fd);
    }

    {   /* the translated game code: linked in (developer build) or loaded from the library prepared for this disc (the app) */
        stw_use_static_tables();
        if (!stw_tables) {
            char lib[1300], why[400];
            stw_lifted_path(lib, sizeof lib);
            if (stw_load_game_lib(lib, why, sizeof why)) { fprintf(stderr, "stw: cannot load the translated game code (%s): %s\n", lib, why); return 2; }
        }
    }
    uint32_t base = u32(x, 0x104), nsec = u32(x, 0x11C), sec_tbl = u32(x, 0x120) - base;
    uint32_t entry = u32(x, 0x128) ^ XBE_ENTRY_KEY, thunk = u32(x, 0x158) ^ XBE_THUNK_KEY;
    memcpy(G + base, x, u32(x, 0x108));                         /* XBE headers sit at the image base */
    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t flags = u32(x, sec_tbl + i * 56), va = u32(x, sec_tbl + i * 56 + 4);
        uint32_t ro = u32(x, sec_tbl + i * 56 + 12), rs = u32(x, sec_tbl + i * 56 + 16);
        (void)flags;
        memcpy(G + va, x + ro, rs);
    }
    free(x);
    {   /* The game's aspect ratio: its device setup loads it from this word (runtime/data/patches.txt redirects the load). 16:9 makes the game build
         * its projection and culling for a wider view; the picture is still 640x480 and the presenter stretches it (anamorphic, like widescreen TVs). */
        int wide = getenv("STW_WIDE") ? atoi(getenv("STW_WIDE")) != 0 : stw_settings_get().widescreen;
        float ar = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
        memcpy(G + 0x7E0F0000u, &ar, 4);
    }

    kernel_init();
    for (uint32_t a = thunk;; a += 4) {                       /* patch kernel import thunks */
        uint32_t v = LD32(a);
        if (!v) break;
        uint32_t ord = v & 0x7FFFFFFFu;
        int found = 0;
        for (int k = 0; stw_tables->kexports[k].ordinal; k++) {
            if (stw_tables->kexports[k].ordinal != ord) continue;
            ST32(a, stw_tables->kexports[k].is_data ? kernel_data_addr(stw_tables->kexports[k].name) : KSTUB_BASE + 4u * k);
            found = 1;
            break;
        }
        if (!found) { fprintf(stderr, "stw: import of unknown kernel ordinal %u\n", ord); return 2; }
    }

    if (getenv("STW_FNTRACE")) rt_trace_fn = strcmp(getenv("STW_FNTRACE"), "ring") ? 1 : 2;
    if (getenv("STW_DUMPAFTER")) {                            /* debugging aid: dump the rings after N seconds and exit */
        pthread_t th;
        pthread_create(&th, NULL, dump_after, (void *)(uintptr_t)atoi(getenv("STW_DUMPAFTER")));
        pthread_detach(th);
    }
    if (getenv("STW_PROTECT")) {                              /* debugging aid: make a guest page read-only; a write faults with a backtrace */
        uint32_t pa = (uint32_t)strtoul(getenv("STW_PROTECT"), NULL, 16) & ~0x3FFFu;
        if (mprotect(G + pa, 0x4000, PROT_READ)) perror("mprotect");
    }
    guest_entry = entry;
    return 0;
}

static void start_windowed(void) {                            /* called by the window code once the launcher is done */
    if (load_game_image(g_argc, g_argv)) _exit(2);
    start_guest_thread();
}

int main(int argc, char **argv) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    g_argc = argc; g_argv = argv;
    if (strcmp(stw_resources_dir(), ".") && !getenv("STW_NOLOG") && !isatty(2)) {   /* started from Finder: keep the log where a bug report can find it */
        char lp[1200];
        stw_path(lp, sizeof lp, "stw.log");
        if (!freopen(lp, "w", stderr)) perror("log");
        setvbuf(stderr, NULL, _IOLBF, 0);
    }
    if (getenv("STW_HEADLESS")) {                             /* no window: the guest runs on this thread */
        if (load_game_image(argc, argv)) return 2;
        guest_main(NULL);
        pthread_exit(NULL);
    }
    display_main_loop(start_windowed);                        /* Cocoa needs the main thread; never returns */
    return 0;
}
