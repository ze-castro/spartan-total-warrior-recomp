/* Xbox kernel HLE. Handlers are `uint32_t k_<ExportName>(CPU *c, const uint32_t *a)`, picked up by
 * name through the generated table (build/ktable.c). Unimplemented imports abort with their name
 * so the runtime grows by running the game and implementing what it asks for. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include "kernel.h"
#include "gametables.h"
#include "display.h"

#define GUEST_HEAP 0x00400000u        /* physical RAM is 64 MB; the XBE image ends below 4 MB */
#define RAM_END    0x04000000u
#define DATA_BASE  0x7E100000u
static uint32_t data_next = DATA_BASE;

/* Guest heap: a first-fit allocator over the console's 64 MB of RAM with a coalescing free list. Allocations made for the game (pool,
 * contiguous and virtual memory) are tracked so that the matching free returns the memory; guest_alloc() (kernel objects, stacks) is
 * permanent. Reused memory is zeroed: the old bump allocator always handed out fresh zero pages and the game may rely on it.
 * STW_HEAPLOG=1 prints the usage once a second. */
typedef struct { uint32_t addr, size; } Range;
static Range *freel;
static int nfree, capfree;
static uint32_t heap_top = RAM_END;                       /* beyond this, only when the 64 MB are really exhausted (warned) */
static uint64_t heap_used, heap_perm, heap_peak, heap_allocs, heap_frees;
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
#define HT_SIZE 131072
#define HT_EMPTY 0
#define HT_TOMB 1
static struct { uint32_t addr, size; } ht[HT_SIZE];       /* tracked allocations: address -> size (open addressing) */

static void heap_init(void) {
    if (freel) return;
    capfree = 1024;
    freel = malloc(capfree * sizeof *freel);
    freel[0] = (Range){ GUEST_HEAP, RAM_END - GUEST_HEAP };
    nfree = 1;
}
static void free_insert(uint32_t addr, uint32_t size) {
    int lo = 0, hi = nfree;
    while (lo < hi) { int m = (lo + hi) / 2; if (freel[m].addr < addr) lo = m + 1; else hi = m; }
    int i = lo;
    if (i > 0 && freel[i - 1].addr + freel[i - 1].size == addr) {            /* merge with the block before */
        freel[i - 1].size += size;
        if (i < nfree && freel[i - 1].addr + freel[i - 1].size == freel[i].addr) {   /* ... and the one after */
            freel[i - 1].size += freel[i].size;
            memmove(&freel[i], &freel[i + 1], (nfree - i - 1) * sizeof *freel);
            nfree--;
        }
        return;
    }
    if (i < nfree && addr + size == freel[i].addr) { freel[i].addr = addr; freel[i].size += size; return; }
    if (nfree == capfree) { capfree *= 2; freel = realloc(freel, capfree * sizeof *freel); }
    memmove(&freel[i + 1], &freel[i], (nfree - i) * sizeof *freel);
    freel[i] = (Range){ addr, size };
    nfree++;
}
static uint32_t heap_take(uint32_t size, uint32_t align) {            /* caller holds heap_lock */
    heap_init();
    if (!size) size = 1;
    if (align < 16) align = 16;
    for (int i = 0; i < nfree; i++) {
        uint32_t start = (freel[i].addr + align - 1) & ~(align - 1), end = freel[i].addr + freel[i].size;
        if (start < freel[i].addr || start + size > end || start + size < start) continue;
        uint32_t pre = start - freel[i].addr, post = end - (start + size), base = freel[i].addr;
        if (pre && post) {                                              /* split in two */
            freel[i].size = pre;
            if (nfree == capfree) { capfree *= 2; freel = realloc(freel, capfree * sizeof *freel); }
            memmove(&freel[i + 2], &freel[i + 1], (nfree - i - 1) * sizeof *freel);
            freel[i + 1] = (Range){ start + size, post };
            nfree++;
        } else if (pre) freel[i].size = pre;
        else if (post) { freel[i].addr = start + size; freel[i].size = post; }
        else { memmove(&freel[i], &freel[i + 1], (nfree - i - 1) * sizeof *freel); nfree--; }
        (void)base;
        memset(G + start, 0, size);
        return start;
    }
    static int warned;                                                  /* the 64 MB are used up: go beyond them like the old allocator did */
    if (!warned++) fprintf(stderr, "stw: guest heap exhausted (%llu MB in use, %llu MB permanent): allocating beyond the console's 64 MB\n",
                           (unsigned long long)(heap_used >> 20), (unsigned long long)(heap_perm >> 20));
    heap_top = (heap_top + align - 1) & ~(align - 1);
    uint32_t p = heap_top;
    heap_top += size;
    return p;
}
static void track_add(uint32_t addr, uint32_t size) {
    uint32_t h = (addr >> 4) * 2654435761u % HT_SIZE;
    while (ht[h].addr > HT_TOMB) h = (h + 1) % HT_SIZE;
    ht[h].addr = addr; ht[h].size = size;
}
static int track_find(uint32_t addr) {
    uint32_t h = (addr >> 4) * 2654435761u % HT_SIZE;
    for (int n = 0; n < HT_SIZE && ht[h].addr != HT_EMPTY; n++, h = (h + 1) % HT_SIZE) if (ht[h].addr == addr) return (int)h;
    return -1;
}

uint32_t guest_alloc(uint32_t size, uint32_t align) {                    /* permanent */
    pthread_mutex_lock(&heap_lock);
    uint32_t p = heap_take(size, align);
    heap_perm += size;
    pthread_mutex_unlock(&heap_lock);
    return p;
}
static uint32_t alloc_tracked(uint32_t size, uint32_t align) {          /* freed by the game through ExFreePool / MmFreeContiguousMemory / NtFreeVirtualMemory */
    pthread_mutex_lock(&heap_lock);
    uint32_t p = heap_take(size, align);
    track_add(p, size);
    heap_used += size; heap_allocs++;
    if (heap_used + heap_perm > heap_peak) heap_peak = heap_used + heap_perm;
    pthread_mutex_unlock(&heap_lock);
    return p;
}
static int free_tracked(uint32_t addr) {                                 /* 1 if it was a tracked allocation */
    pthread_mutex_lock(&heap_lock);
    int i = addr ? track_find(addr) : -1;
    if (i >= 0) {
        uint32_t size = ht[i].size;
        ht[i].addr = HT_TOMB;
        heap_used -= size; heap_frees++;
        if (addr + size <= RAM_END) free_insert(addr, size);
    }
    pthread_mutex_unlock(&heap_lock);
    return i >= 0;
}
static uint32_t tracked_size(uint32_t addr) {
    pthread_mutex_lock(&heap_lock);
    int i = addr ? track_find(addr) : -1;
    uint32_t r = i >= 0 ? ht[i].size : 0;
    pthread_mutex_unlock(&heap_lock);
    return r;
}
void guest_heap_stats(unsigned long long *used, unsigned long long *perm, unsigned long long *peak, unsigned long long *allocs, unsigned long long *frees) {
    *used = heap_used; *perm = heap_perm; *peak = heap_peak; *allocs = heap_allocs; *frees = heap_frees;
}

/* Call guest code from a kernel handler (stdcall, args pushed right to left). */
uint32_t guest_call(CPU *c, uint32_t fn, int n, const uint32_t *args) {
    uint32_t sp = c->esp;
    for (int i = n - 1; i >= 0; i--) { c->esp -= 4; ST32(c->esp, args[i]); }
    c->esp -= 4; ST32(c->esp, 0);
    call_ind(c, fn);
    c->esp = sp;
    return c->eax;
}

/* Heuristic backtrace: stack words that point just after a call instruction. */
void guest_backtrace(CPU *c) {
    fprintf(stderr, "stw: guest stack (return addresses):");
    int n = 0;
    for (uint32_t sp = c->esp; sp < c->esp + 0x1000 && n < 24; sp += 4) {
        uint32_t v = LD32(sp);
        if (v < 0x11000 || v > 0x1FFFFF) continue;
        if (LD8(v - 5) == 0xE8 || LD8(v - 6) == 0xFF || LD8(v - 2) == 0xFF || LD8(v - 3) == 0xFF) {
            fprintf(stderr, " %#x", v);
            n++;
        }
    }
    fprintf(stderr, "\n");
}

static const char *gstr(uint32_t a) { return (const char *)(G + a); }

static uint32_t tick_addr;
static void *tick_thread(void *arg) {
    (void)arg;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        ST32(tick_addr, (uint32_t)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000));
        usleep(1000);
    }
    return NULL;
}

/* ---- data exports ---- */
uint32_t kernel_data_addr(const char *name) {
    uint32_t a = data_next;
    data_next += 0x400;
    if (!strcmp(name, "XboxKrnlVersion")) {            /* Major, Minor, Build, Qfe/flags */
        ST16(a, 1); ST16(a + 2, 0); ST16(a + 4, 5849); ST16(a + 6, 1);
    } else if (!strcmp(name, "KeTickCount")) {
        pthread_t th;
        tick_addr = a;
        pthread_create(&th, NULL, tick_thread, NULL);
        pthread_detach(th);
    } else if (!strcmp(name, "XeImageFileName")) {     /* ANSI_STRING -> "\Device\Harddisk0\Partition1\default.xbe" */
        static const char n[] = "\\Device\\CdRom0\\default.xbe";
        memcpy(G + a + 16, n, sizeof n);
        ST16(a, sizeof n - 1); ST16(a + 2, sizeof n); ST32(a + 4, a + 16);
    }
    return a;
}

void kernel_init(void) {}

/* ---- dispatch ---- */
void kernel_dispatch(CPU *c, uint32_t stub) {
    const KExport *e = &stw_tables->kexports[(stub - KSTUB_BASE) / 4];
    uint32_t a[16], sp = c->esp;
    int nw = 0;
    if (e->fastcall) { a[nw++] = c->ecx; a[nw++] = c->edx; }
    for (int i = 0; nw < 16; i++) a[nw++] = LD32(sp + 4 + 4 * i);
    static int trace = -1;
    if (trace < 0) trace = getenv("STW_TRACE") != NULL;
    if (trace) fprintf(stderr, "[k] %s(%#x, %#x, %#x, %#x) from %#x\n", e->name, a[0], a[1], a[2], a[3], LD32(sp));
    if (!e->fn) {
        fprintf(stderr, "stw: unimplemented kernel call %s (ordinal %u) from %#x args %#x %#x %#x %#x\n",
                e->name, e->ordinal, LD32(sp), a[0], a[1], a[2], a[3]);
        guest_backtrace(c);
        exit(1);
    }
    uint32_t r = e->fn(c, a);
    if (trace) fprintf(stderr, "[k]   %s => %#x\n", e->name, r);
    int pop = e->fastcall ? (e->argbytes > 8 ? e->argbytes - 8 : 0) : (e->argbytes > 0 ? e->argbytes : 0);
    c->esp = sp + 4 + pop;
    c->eax = r;
}

/* ---- handlers: process / debug ---- */
uint32_t k_HalReturnToFirmware(CPU *c, const uint32_t *a) {
    (void)c;
    fprintf(stderr, "stw: HalReturnToFirmware(%u)\n", a[0]);
    guest_backtrace(c);
    exit(a[0] == 0 ? 0 : 1);
}

uint32_t k_DbgPrint(CPU *c, const uint32_t *a) {      /* cdecl, printf-style over guest stack words */
    (void)c;
    const char *fmt = gstr(a[0]);
    int ai = 1;
    char out[1024];
    size_t o = 0;
    for (const char *p = fmt; *p && o < sizeof out - 32; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        char spec[16];
        int n = 0;
        spec[n++] = *p++;
        while (*p && strchr("-+ #0123456789.l", *p) && n < 12) spec[n++] = *p++;
        char t = *p;
        spec[n++] = t; spec[n] = 0;
        if (t == 's') o += snprintf(out + o, sizeof out - o, spec, gstr(a[ai++]));
        else if (t == 'c') o += snprintf(out + o, sizeof out - o, spec, (int)a[ai++]);
        else if (t == '%') out[o++] = '%';
        else if (strchr("fgeE", t)) {
            uint64_t q = a[ai] | ((uint64_t)a[ai + 1] << 32); double d; memcpy(&d, &q, 8); ai += 2;
            o += snprintf(out + o, sizeof out - o, spec, d);
        } else o += snprintf(out + o, sizeof out - o, spec, a[ai++]);
    }
    out[o] = 0;
    fprintf(stderr, "[DbgPrint] %s", out);
    return 0;
}

/* ---- memory ---- */
uint32_t k_ExAllocatePoolWithTag(CPU *c, const uint32_t *a) { (void)c; return alloc_tracked((a[0] + 15) & ~15u, 16); }
uint32_t k_ExQueryPoolBlockSize(CPU *c, const uint32_t *a) { (void)c; return tracked_size(a[0]); }
uint32_t k_ExFreePool(CPU *c, const uint32_t *a) { (void)c; free_tracked(a[0]); return 0; }
uint32_t k_MmAllocateContiguousMemory(CPU *c, const uint32_t *a) { (void)c; return 0x80000000u | alloc_tracked(a[0], 4096); }
uint32_t k_MmAllocateContiguousMemoryEx(CPU *c, const uint32_t *a) {
    (void)c;                                           /* size, low, high, alignment, protect */
    return 0x80000000u | alloc_tracked(a[0], a[3] > 4096 ? a[3] : 4096);
}
uint32_t k_MmFreeContiguousMemory(CPU *c, const uint32_t *a) { (void)c; free_tracked(a[0] & 0x0FFFFFFFu); return 0; }

/* ---- irql (single guest CPU model: the irql lives in the KPCR) ---- */
uint32_t k_KfRaiseIrql(CPU *c, const uint32_t *a) { (void)c; uint32_t o = LD8(FS_BASE + 0x24); ST8(FS_BASE + 0x24, a[0]); return o; }
uint32_t k_KfLowerIrql(CPU *c, const uint32_t *a) { (void)c; ST8(FS_BASE + 0x24, a[0]); return 0; }
uint32_t k_KeRaiseIrqlToDpcLevel(CPU *c, const uint32_t *a) { (void)c; (void)a; uint32_t o = LD8(FS_BASE + 0x24); ST8(FS_BASE + 0x24, 2); return o; }

/* ---- time ---- */
uint32_t k_KeQuerySystemTime(CPU *c, const uint32_t *a) {
    (void)c;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    uint64_t ft = ((uint64_t)t.tv_sec + 11644473600ull) * 10000000ull + t.tv_nsec / 100;
    ST32(a[0], (uint32_t)ft); ST32(a[0] + 4, (uint32_t)(ft >> 32));
    return 0;
}

uint64_t rt_rdtsc(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return ((uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec) * 733 / 1000;
}

/* ---- memory manager ---- */
uint32_t k_MmQueryAllocationSize(CPU *c, const uint32_t *a) {
    (void)c;
    return tracked_size(a[0] & 0x0FFFFFFFu);
}
uint32_t k_MmPersistContiguousMemory(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
uint32_t k_MmGetPhysicalAddress(CPU *c, const uint32_t *a) { (void)c; return a[0] & 0x0FFFFFFFu; }   /* VA 0x80000000+p == PA p */
uint32_t k_MmSetAddressProtect(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
uint32_t k_MmQueryAddressProtect(CPU *c, const uint32_t *a) { (void)c; (void)a; return 4; /* PAGE_READWRITE */ }
uint32_t k_MmLockUnlockBufferPages(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
uint32_t k_MmLockUnlockPhysicalPage(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
uint32_t k_MmClaimGpuInstanceMemory(CPU *c, const uint32_t *a) {   /* NumberOfBytes*, NumberOfPaddingBytes* */
    (void)c;
    uint32_t want = a[0] ? LD32(a[0]) : 0;
    if (a[0] && want == 0xFFFFFFFFu) ST32(a[0], 0x14000);          /* query: NV2A instance memory size */
    if (a[1]) ST32(a[1], 0);
    return 0x80000000u | (guest_alloc(0x20000, 0x1000) + 0x20000 - 0x14000);
}
uint32_t k_MmQueryStatistics(CPU *c, const uint32_t *a) {
    (void)c;                       /* MM_STATISTICS: Length, TotalPhysicalPages, AvailablePages, ... (64 MB console) */
    memset(G + a[0] + 4, 0, 0x20);
    ST32(a[0] + 4, 0x4000); ST32(a[0] + 8, 0x2000);
    return 0;
}
uint32_t k_NtAllocateVirtualMemory(CPU *c, const uint32_t *a) {   /* Base*, ZeroBits, Size*, Type, Protect */
    (void)c;
    uint32_t size = (LD32(a[2]) + 0xFFF) & ~0xFFFu, want = LD32(a[0]);
    uint32_t p = want ? want : alloc_tracked(size, 0x1000);
    ST32(a[0], p); ST32(a[2], size);
    return 0;
}
uint32_t k_NtFreeVirtualMemory(CPU *c, const uint32_t *a) {     /* Base*, Size*, FreeType: MEM_RELEASE (0x8000) returns the allocation; MEM_DECOMMIT keeps it */
    (void)c;
    if (a[2] & 0x8000u) free_tracked(LD32(a[0]));
    return 0;
}
uint32_t k_NtQueryVirtualMemory(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0xC0000001u; }

/* ---- hal / interrupts / dpc (no real hardware behind them) ---- */
uint32_t k_HalRegisterShutdownNotification(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
uint32_t k_HalGetInterruptVector(CPU *c, const uint32_t *a) { (void)c; return 0x30 + a[0]; }
uint32_t k_HalReadWritePCISpace(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
uint32_t k_HalIsResetOrShutdownPending(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
uint32_t k_KeSynchronizeExecution(CPU *c, const uint32_t *a) {   /* Interrupt, Routine, Context */
    return guest_call(c, a[1], 1, &a[2]);
}
uint32_t k_KeStallExecutionProcessor(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
uint32_t k_KeSaveFloatingPointState(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
uint32_t k_KeRestoreFloatingPointState(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
uint32_t k_KeBugCheck(CPU *c, const uint32_t *a) {
    (void)c;
    fprintf(stderr, "stw: KeBugCheck(%#x) from %#x\n", a[0], LD32(c->esp));
    guest_backtrace(c);
    exit(1);
}
uint32_t k_AvGetSavedDataAddress(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
uint32_t k_AvSetSavedDataAddress(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
uint32_t k_AvSendTVEncoderOption(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
/* AvSetDisplayMode(RegisterBase, Step, Mode, Format, Pitch, FrameBuffer): the kernel would program the video
 * encoder and CRTC here; we only record what is being scanned out (see display.c). */
uint32_t k_AvSetDisplayMode(CPU *c, const uint32_t *a) {
    (void)c;
    if (getenv("STW_TRACE")) fprintf(stderr, "[av] SetDisplayMode mode=%#x format=%#x pitch=%#x fb=%#x\n", a[2], a[3], a[4], a[5]);
    display_set_mode(a[2], a[3], a[4], a[5]);
    return 0;
}

/* EEPROM settings: (ValueIndex, Type*, Value*, ValueLength, ResultLength*) -> defaults of a
 * factory-fresh PAL console, English. */
uint32_t k_ExQueryNonVolatileSetting(CPU *c, const uint32_t *a) {
    (void)c;
    uint32_t v = 0;
    switch (a[0]) {
    case 7: v = 1; break;                 /* XC_LANGUAGE: English */
    case 8: v = 0x00010000; break;        /* XC_VIDEO_FLAGS: 16:9 off, PAL60 off */
    default: break;
    }
    if (a[1]) ST32(a[1], 4);              /* REG_DWORD */
    if (a[2] && a[3] >= 4) ST32(a[2], v);
    if (a[4]) ST32(a[4], 4);
    return 0;
}

/* ---- rtl ---- */
uint32_t k_RtlInitAnsiString(CPU *c, const uint32_t *a) {
    (void)c;
    uint32_t n = a[1] ? (uint32_t)strlen(gstr(a[1])) : 0;
    ST16(a[0], n); ST16(a[0] + 2, a[1] ? n + 1 : 0); ST32(a[0] + 4, a[1]);
    return 0;
}
uint32_t k_RtlEqualString(CPU *c, const uint32_t *a) {            /* A*, B*, CaseInsensitive */
    (void)c;
    uint32_t la = LD16(a[0]), lb = LD16(a[1]);
    if (la != lb) return 0;
    const char *x = gstr(LD32(a[0] + 4)), *y = gstr(LD32(a[1] + 4));
    return a[2] ? strncasecmp(x, y, la) == 0 : memcmp(x, y, la) == 0;
}
uint32_t k_RtlCompareMemoryUlong(CPU *c, const uint32_t *a) {     /* Source, Length, Pattern */
    (void)c;
    uint32_t i = 0;
    for (; i + 4 <= a[1] && LD32(a[0] + i) == a[2]; i += 4) {}
    return i;
}
uint32_t k_RtlNtStatusToDosError(CPU *c, const uint32_t *a) {
    (void)c;
    switch (a[0]) {
    case 0: return 0;
    case 0xC000000Fu: case 0xC0000034u: return 2;      /* file / object name not found */
    case 0xC000003Au: return 3;                         /* path not found */
    case 0xC0000022u: return 5;                         /* access denied */
    case 0xC0000011u: return 38;                        /* end of file */
    case 0xC0000035u: return 183;                       /* object name collision: ERROR_ALREADY_EXISTS */
    case 0x80000006u: return 18;   /* no more files */
    case 0xC0000008u: return 6;                         /* invalid handle */
    case 0xC0000004u: return 24;                        /* info length mismatch: ERROR_BAD_LENGTH */
    case 0x80000005u: return 234;                       /* buffer overflow: ERROR_MORE_DATA */
    case 0xC0000101u: return 145;                       /* directory not empty */
    case 0xC0000103u: return 267;                       /* not a directory */
    case 0xC000007Fu: return 112;                       /* disk full */
    case 0xC0000002u: return 120;                       /* not implemented */
    default: return 317;
    }
}
/* ---- time: FILETIME ticks (100 ns since 1601-01-01) <-> TIME_FIELDS { SHORT Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday } ---- */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {          /* days since 1970-01-01 (Hinnant) */
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
uint32_t k_RtlTimeToTimeFields(CPU *c, const uint32_t *a) {        /* PLARGE_INTEGER Time, PTIME_FIELDS TimeFields */
    (void)c;
    uint64_t t = LD32(a[0]) | ((uint64_t)LD32(a[0] + 4) << 32);
    uint64_t ms_total = t / 10000, days = ms_total / 86400000ull, ms_day = ms_total % 86400000ull;
    int64_t z = (int64_t)days - 134774 + 719468;                    /* 1601-01-01 is 134774 days before 1970-01-01 */
    int64_t era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    ST16(a[1], (uint16_t)y); ST16(a[1] + 2, (uint16_t)m); ST16(a[1] + 4, (uint16_t)d);
    ST16(a[1] + 6, (uint16_t)(ms_day / 3600000)); ST16(a[1] + 8, (uint16_t)(ms_day / 60000 % 60));
    ST16(a[1] + 10, (uint16_t)(ms_day / 1000 % 60)); ST16(a[1] + 12, (uint16_t)(ms_day % 1000));
    ST16(a[1] + 14, (uint16_t)((days + 1) % 7));                    /* 1601-01-01 was a Monday; 0 = Sunday */
    return 0;
}
uint32_t k_RtlTimeFieldsToTime(CPU *c, const uint32_t *a) {        /* PTIME_FIELDS, PLARGE_INTEGER -> BOOLEAN */
    (void)c;
    int64_t y = (int16_t)LD16(a[0]), m = (int16_t)LD16(a[0] + 2), d = (int16_t)LD16(a[0] + 4), h = (int16_t)LD16(a[0] + 6);
    int64_t mi = (int16_t)LD16(a[0] + 8), s = (int16_t)LD16(a[0] + 10), ms = (int16_t)LD16(a[0] + 12);
    if (y < 1601 || m < 1 || m > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59 || ms < 0 || ms > 999) return 0;
    uint64_t days = (uint64_t)(days_from_civil(y, m, d) + 134774);
    uint64_t t = (((days * 86400ull + (uint64_t)(h * 3600 + mi * 60 + s)) * 1000ull) + (uint64_t)ms) * 10000ull;
    ST32(a[1], (uint32_t)t); ST32(a[1] + 4, (uint32_t)(t >> 32));
    return 1;
}

/* ---- crypto: SHA-1 and HMAC-SHA1, as the XDK's XcSHA* / XcHMAC (the game signs and checks save data with them) ---- */
uint32_t k_XcSHAInit(CPU *c, const uint32_t *a) {                  /* UCHAR *pbSHAContext (116 bytes; CC_SHA1_CTX fits) */
    (void)c;
    CC_SHA1_Init((CC_SHA1_CTX *)(G + a[0]));
    return 0;
}
uint32_t k_XcSHAUpdate(CPU *c, const uint32_t *a) {                /* ctx, input, length */
    (void)c;
    CC_SHA1_Update((CC_SHA1_CTX *)(G + a[0]), G + a[1], a[2]);
    return 0;
}
uint32_t k_XcSHAFinal(CPU *c, const uint32_t *a) {                 /* ctx, digest (20 bytes) */
    (void)c;
    CC_SHA1_Final(G + a[1], (CC_SHA1_CTX *)(G + a[0]));
    return 0;
}
uint32_t k_XcHMAC(CPU *c, const uint32_t *a) {                     /* key, keylen, input1, len1, input2, len2, digest: HMAC-SHA1(key, input1 || input2) */
    (void)c;
    CCHmacContext h;
    CCHmacInit(&h, kCCHmacAlgSHA1, G + a[0], a[1]);
    if (a[2] && a[3]) CCHmacUpdate(&h, G + a[2], a[3]);
    if (a[4] && a[5]) CCHmacUpdate(&h, G + a[4], a[5]);
    CCHmacFinal(&h, G + a[6]);
    return 0;
}
uint32_t k_RtlRaiseException(CPU *c, const uint32_t *a) {
    (void)c;
    fprintf(stderr, "stw: RtlRaiseException code %#x from %#x\n", LD32(a[0]), LD32(c->esp));
    guest_backtrace(c);
    exit(1);
}

/* ---- objects ---- */
uint32_t k_ObReferenceObjectByHandle(CPU *c, const uint32_t *a) {   /* Handle, ObjectType, Object** */
    (void)c;
    uint32_t o = handle_obj(a[0]);
    if (!o) return 0xC0000008u;
    ST32(a[2], o);
    return 0;
}
uint32_t k_ObfDereferenceObject(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
uint32_t k_XeLoadSection(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }     /* all sections are resident */
uint32_t k_XeUnloadSection(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }

uint32_t k_FscSetCacheSize(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }   /* file cache size hint */

/* ---- port I/O: only PCI config mechanism #1 (0xCF8/0xCFC) has any model; the rest reads as 0/ones ---- */
static uint32_t pci_addr;
static int port_trace = -1;
uint32_t rt_in(int size, uint16_t port) {
    if (port_trace < 0) port_trace = getenv("STW_PORTTRACE") != NULL;
    uint32_t v = size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFu;
    if (port == 0xCF8) v = pci_addr;
    else if (port >= 0xCFC && port < 0xD00) v = 0xFFFFFFFFu;            /* no device answers config reads yet */
    if (port_trace) fprintf(stderr, "[port] in%d %#x -> %#x\n", size * 8, port, v);
    return v;
}
void rt_out(int size, uint16_t port, uint32_t value) {
    if (port_trace < 0) port_trace = getenv("STW_PORTTRACE") != NULL;
    if (port == 0xCF8) pci_addr = value;
    if (port_trace) fprintf(stderr, "[port] out%d %#x <- %#x\n", size * 8, port, value);
}

int rt_trace_fn;                                       /* 1 = print every entry, 2 = ring buffer per thread */
#define RING 64
typedef struct { uint32_t fn[RING]; uint32_t n; uint32_t esp; } Ring;
static __thread Ring *my_ring;
static Ring *rings[32];
static int nrings;
void rt_trace_enter(CPU *c, uint32_t fn) {
    static uint32_t only; static int init;
    if (!init) { init = 1; only = getenv("STW_TRACEFN") ? (uint32_t)strtoul(getenv("STW_TRACEFN"), NULL, 16) : 0; }
    if (only && fn == only) {                              /* STW_TRACEFN=<addr>: full arguments of one function */
        fprintf(stderr, "[fn] %#x ret=%#x args:", fn, LD32(c->esp));
        for (int i = 0; i < 10; i++) fprintf(stderr, " %#x", LD32(c->esp + 4 + 4 * i));
        fprintf(stderr, "\n");
    }
    if (rt_trace_fn == 1) {
        fprintf(stderr, "[fn] %#x esp=%#x ecx=%#x edx=%#x args=%#x %#x %#x\n", fn, c->esp, c->ecx, c->edx, LD32(c->esp + 4), LD32(c->esp + 8), LD32(c->esp + 12));
        return;
    }
    if (!my_ring) {
        my_ring = calloc(1, sizeof(Ring));
        if (nrings < 32) rings[nrings++] = my_ring;
    }
    if (my_ring->n && my_ring->fn[(my_ring->n - 1) % RING] == fn) return;       /* collapse tight repeats */
    my_ring->fn[my_ring->n++ % RING] = fn;
    my_ring->esp = c->esp;
}
void rt_dump_rings(void) {
    for (int i = 0; i < nrings; i++) {
        Ring *r = rings[i];
        fprintf(stderr, "thread %d (esp %#x), most recent function entries last:", i, r->esp);
        for (uint32_t k = r->n > RING ? r->n - RING : 0; k < r->n; k++) fprintf(stderr, " %x", r->fn[k % RING]);
        fprintf(stderr, "\n  stack:");
        int n = 0;                                          /* return addresses on the thread's stack, innermost first */
        for (uint32_t sp = r->esp; sp < r->esp + 0x2000 && n < 40; sp += 4) {
            uint32_t v = LD32(sp);
            if (v < 0x11000 || v > 0x1FFFFF) continue;
            if (LD8(v - 5) == 0xE8 || LD8(v - 6) == 0xFF || LD8(v - 2) == 0xFF || LD8(v - 3) == 0xFF) { fprintf(stderr, " %x", v); n++; }
        }
        fprintf(stderr, "\n");
    }
}

__thread uint64_t rt_poll_count;
/* Watchdog: one thread has gone around loops 300 million times (a few seconds): probably a spin on a
 * hardware/other-thread condition that never becomes true. Say where. */
void rt_poll_report(CPU *c, const char *fn) {
    fprintf(stderr, "stw: possible stuck poll loop in %s (esp %#x, eax=%#x ecx=%#x edx=%#x esi=%#x edi=%#x ebx=%#x ebp=%#x)\n",
            fn, c->esp, c->eax, c->ecx, c->edx, c->esi, c->edi, c->ebx, c->ebp);
}
