/* Kernel objects: handles, dispatcher objects (events, semaphores, timers, threads), waits,
 * critical sections and system threads.
 *
 * Dispatcher objects live in guest memory with the real KDISPATCHER_HEADER layout
 * (Type@0, Size@2, SignalState@4), so handle-based (Nt*) and pointer-based (Ke*) calls
 * operate on the same state. */
void gpu_sync_fences(void);
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include "nv2a.h"
#include "display.h"
#include "kernel.h"

enum { T_NOTIF_EVENT = 0, T_SYNC_EVENT = 1, T_MUTANT = 2, T_SEMAPHORE = 5, T_THREAD = 6,
       T_NOTIF_TIMER = 8, T_SYNC_TIMER = 9 };
enum { H_OBJECT = 1, H_FILE = 2 };
void kfile_close(uint32_t idx);
#define STATUS_SUCCESS 0u
#define STATUS_TIMEOUT 0x102u
#define STATUS_INVALID_HANDLE 0xC0000008u
#define STATUS_INVALID_PARAMETER 0xC000000Du

static pthread_mutex_t gil = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;     /* broadcast on any dispatcher state change */

volatile int gil_want;
void gil_init(void) { pthread_mutex_lock(&gil); }
/* A thread that must run (interrupts, timers) raises gil_want; running guest code notices at its next
 * loop back-edge or blocking call and steps aside until the requester is done. */
void gil_yield(void) {
    pthread_mutex_unlock(&gil);
    while (gil_want) usleep(20);
    pthread_mutex_lock(&gil);
}
/* Fairness between guest threads: a thread that loops without ever blocking would otherwise keep the
 * lock forever (mutexes are not fair), so loops give the lock up briefly every so often. */
void gil_timeslice(void) {
    pthread_mutex_unlock(&gil);
    usleep(200);
    pthread_mutex_lock(&gil);
}
static void gil_acquire_priority(void) {
    __atomic_add_fetch(&gil_want, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&gil);
    __atomic_sub_fetch(&gil_want, 1, __ATOMIC_SEQ_CST);
}
void gil_release(void) { pthread_mutex_unlock(&gil); }
void gil_lock_priority(void) { gil_acquire_priority(); }      /* for host threads that must touch guest state (audio completions) */

/* ---- handles ---- */
#define MAXH 4096
static struct { int type; uint32_t obj; } htab[MAXH];

uint32_t handle_new(int type, uint32_t obj) {
    for (int i = 1; i < MAXH; i++)
        if (!htab[i].type) { htab[i].type = type; htab[i].obj = obj; return (uint32_t)i * 4; }
    fprintf(stderr, "stw: out of handles\n");
    exit(1);
}
uint32_t handle_obj(uint32_t h) {
    uint32_t i = h / 4;
    return (h % 4 == 0 && i > 0 && i < MAXH && htab[i].type) ? htab[i].obj : 0;
}

/* ---- dispatcher objects ---- */
static void obj_init(uint32_t o, int type, int32_t state) {
    memset(G + o, 0, 16);
    ST8(o, type); ST8(o + 2, 4);
    ST32(o + 4, state);
}
static int32_t sig(uint32_t o) { return (int32_t)LD32(o + 4); }
static void set_sig(uint32_t o, int32_t v) { ST32(o + 4, v); }

/* Acquire side effects when a wait on o is satisfied. */
static void satisfy(uint32_t o) {
    switch (LD8(o)) {
    case T_SYNC_EVENT: case T_SYNC_TIMER: set_sig(o, 0); break;
    case T_SEMAPHORE: set_sig(o, sig(o) - 1); break;
    default: break;
    }
}

/* timeout: guest pointer to LARGE_INTEGER (100 ns; negative = relative), 0 = infinite */
static int64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec; }
static void deadline_of(uint32_t tptr, int *infinite, struct timespec *ts) {
    *infinite = 1;
    if (!tptr) return;
    int64_t v = (int64_t)(LD32(tptr) | ((uint64_t)LD32(tptr + 4) << 32));
    int64_t ns = v < 0 ? now_ns() + (-v) * 100 : (v - 116444736000000000ll) * 100;
    if (v == 0) ns = 0;
    *infinite = 0;
    ts->tv_sec = ns / 1000000000ll; ts->tv_nsec = ns % 1000000000ll;
}

/* ---- user APCs: queued per target thread (KTHREAD address), delivered at alertable waits ---- */
#define MAXAPC 256
static struct { uint32_t thread, routine, ctx, a1, a2; } apcs[MAXAPC];
static uint32_t cur_thread(CPU *c);
static int run_apcs(CPU *c) {
    int ran = 0;
    for (int i = 0; i < MAXAPC; i++) {
        if (!apcs[i].thread || apcs[i].thread != cur_thread(c)) continue;
        uint32_t args[3] = { apcs[i].ctx, apcs[i].a1, apcs[i].a2 }, routine = apcs[i].routine;
        apcs[i].thread = 0;
        if (routine) guest_call(c, routine, 3, args);          /* a null routine is just a wake-up */
        ran = 1;
    }
    return ran;
}
uint32_t k_NtQueueApcThread(CPU *c, const uint32_t *a) {   /* Thread, Routine, Context, Arg1, Arg2 */
    (void)c;
    uint32_t t = handle_obj(a[0]);
    if (!t) return STATUS_INVALID_HANDLE;
    for (int i = 0; i < MAXAPC; i++)
        if (!apcs[i].thread) {
            apcs[i].thread = t; apcs[i].routine = a[1]; apcs[i].ctx = a[2]; apcs[i].a1 = a[3]; apcs[i].a2 = a[4];
            pthread_cond_broadcast(&changed);
            return STATUS_SUCCESS;
        }
    return 0xC0000017u;
}

#define STATUS_USER_APC 0xC0u
/* STW_STALLDUMP aid: what each guest thread is blocked on (object header type/signal state, timeout pointer). */
static struct { uint32_t obj, tptr, esp, thread; int alertable, active; } wait_state[32];
static __thread int my_wait = -1;
void kobj_dump_waits(void) {
    for (int i = 0; i < 32; i++)
        if (wait_state[i].active)
            fprintf(stderr, "[wait] slot %d esp=%#x obj=%#x type=%u signal=%d tptr=%#x timeout=%#x/%#x alertable=%d\n", i, wait_state[i].esp, wait_state[i].obj,
                    LD8(wait_state[i].obj), (int)LD32(wait_state[i].obj + 4), wait_state[i].tptr, wait_state[i].tptr ? LD32(wait_state[i].tptr + 4) : 0,
                    wait_state[i].tptr ? LD32(wait_state[i].tptr) : 0, wait_state[i].alertable);
    for (int i = 0; i < 32; i++)
        if (wait_state[i].active) {                                  /* return addresses found on the waiting thread's stack (any word that points into the image) */
            fprintf(stderr, "[wait] slot %d stack:", i);
            for (uint32_t k = 0, found = 0; k < 400 && found < 24; k++) { uint32_t v = LD32(wait_state[i].esp + k * 4); if (v >= 0x11000 && v < 0x2d0000) { fprintf(stderr, " %x", v); found++; } }
            fprintf(stderr, "\n");
        }
}
static uint32_t wait_any_impl(CPU *c, const uint32_t *objs, int n, uint32_t tptr, int alertable);
static uint32_t wait_any(CPU *c, const uint32_t *objs, int n, uint32_t tptr, int alertable) {
    static int next;
    if (my_wait < 0) my_wait = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED) % 32;
    wait_state[my_wait].obj = objs[0]; wait_state[my_wait].tptr = tptr; wait_state[my_wait].esp = c->esp; wait_state[my_wait].alertable = alertable; wait_state[my_wait].active = 1;
    uint32_t r = wait_any_impl(c, objs, n, tptr, alertable);
    wait_state[my_wait].active = 0;
    return r;
}
static uint32_t wait_any_impl(CPU *c, const uint32_t *objs, int n, uint32_t tptr, int alertable) {
    int inf; struct timespec ts;
    deadline_of(tptr, &inf, &ts);
    int poll = tptr && !LD32(tptr) && !LD32(tptr + 4);
    for (;;) {
        if (alertable && run_apcs(c)) return STATUS_USER_APC;
        for (int i = 0; i < n; i++)
            if (sig(objs[i]) > 0) { satisfy(objs[i]); return STATUS_SUCCESS + i; }
        if (poll) return STATUS_TIMEOUT;
        int r = inf ? pthread_cond_wait(&changed, &gil) : pthread_cond_timedwait(&changed, &gil, &ts);
        if (r == ETIMEDOUT) {
            for (int i = 0; i < n; i++)
                if (sig(objs[i]) > 0) { satisfy(objs[i]); return STATUS_SUCCESS + i; }
            return STATUS_TIMEOUT;
        }
    }
}

static void sleep_ns(int64_t ns) {                      /* lets other guest threads run */
    pthread_mutex_unlock(&gil);
    struct timespec t = { ns / 1000000000ll, ns % 1000000000ll };
    nanosleep(&t, NULL);
    pthread_mutex_lock(&gil);
}

uint32_t k_KeWaitForSingleObject(CPU *c, const uint32_t *a) {   /* Object, Reason, Mode, Alertable, Timeout */
    return wait_any(c, &a[0], 1, a[4], a[3]);
}
static uint32_t nt_wait(CPU *c, uint32_t h, uint32_t alertable, uint32_t timeout) {
    uint32_t o = handle_obj(h);
    if (!o) return STATUS_INVALID_HANDLE;
    return wait_any(c, &o, 1, timeout, alertable);
}
uint32_t k_NtWaitForSingleObject(CPU *c, const uint32_t *a) {    /* Handle, Alertable, Timeout */
    return nt_wait(c, a[0], a[1], a[2]);
}
uint32_t k_NtWaitForSingleObjectEx(CPU *c, const uint32_t *a) {  /* Handle, WaitMode, Alertable, Timeout */
    return nt_wait(c, a[0], a[2], a[3]);
}
uint32_t k_KeDelayExecutionThread(CPU *c, const uint32_t *a) {  /* Mode, Alertable, Interval */
    if (getenv("STW_SLEEPTRACE")) { static int n; if (++n == 3) { fprintf(stderr, "stw: 3rd sleep:\n"); guest_backtrace(c); } }
    if (a[1] && run_apcs(c)) return STATUS_USER_APC;
    int64_t v = (int64_t)(LD32(a[2]) | ((uint64_t)LD32(a[2] + 4) << 32));
    sleep_ns(v < 0 ? (-v) * 100 : 0);
    if (a[1] && run_apcs(c)) return STATUS_USER_APC;
    return STATUS_SUCCESS;
}

/* ---- events ---- */
uint32_t k_NtCreateEvent(CPU *c, const uint32_t *a) {           /* Handle*, ObjectAttributes, Type, InitialState */
    (void)c;
    uint32_t o = guest_alloc(16, 16);
    obj_init(o, a[2] ? T_SYNC_EVENT : T_NOTIF_EVENT, a[3] != 0);
    ST32(a[0], handle_new(H_OBJECT, o));
    return STATUS_SUCCESS;
}
uint32_t k_NtSetEvent(CPU *c, const uint32_t *a) {              /* Handle, PreviousState* */
    (void)c;
    uint32_t o = handle_obj(a[0]);
    if (!o) return STATUS_INVALID_HANDLE;
    if (a[1]) ST32(a[1], sig(o));
    set_sig(o, 1);
    pthread_cond_broadcast(&changed);
    return STATUS_SUCCESS;
}
uint32_t k_KeSetEvent(CPU *c, const uint32_t *a) {              /* Event, Increment, Wait */
    (void)c;
    int32_t old = sig(a[0]);
    set_sig(a[0], 1);
    pthread_cond_broadcast(&changed);
    return (uint32_t)old;
}

/* ---- critical sections: Xbox RTL_CRITICAL_SECTION = KDISPATCHER_HEADER(16) + LockCount@0x10
 * (-1 free) + RecursionCount@0x14 + OwningThread@0x18. Contention waits on the global condvar. ---- */
static uint32_t cur_thread(CPU *c) { return LD32(c->fs + 0x2C); }
uint32_t k_RtlInitializeCriticalSection(CPU *c, const uint32_t *a) {
    (void)c;
    obj_init(a[0], T_SYNC_EVENT, 0);
    ST32(a[0] + 8, a[0] + 8); ST32(a[0] + 12, a[0] + 8);
    ST32(a[0] + 0x10, 0xFFFFFFFFu); ST32(a[0] + 0x14, 0); ST32(a[0] + 0x18, 0);
    return 0;
}
uint32_t k_RtlEnterCriticalSection(CPU *c, const uint32_t *a) {
    uint32_t cs = a[0], me = cur_thread(c);
    while (LD32(cs + 0x18) && LD32(cs + 0x18) != me) pthread_cond_wait(&changed, &gil);
    ST32(cs + 0x18, me);
    ST32(cs + 0x14, LD32(cs + 0x14) + 1);
    ST32(cs + 0x10, LD32(cs + 0x10) + 1);
    return 0;
}
uint32_t k_RtlLeaveCriticalSection(CPU *c, const uint32_t *a) {
    (void)c;
    uint32_t cs = a[0];
    ST32(cs + 0x10, LD32(cs + 0x10) - 1);
    ST32(cs + 0x14, LD32(cs + 0x14) - 1);
    if (LD32(cs + 0x14) == 0) {
        ST32(cs + 0x18, 0);
        pthread_cond_broadcast(&changed);
    }
    return 0;
}

/* ---- timers ---- */
#define MAXT 256
static struct { uint32_t obj, dpc; int64_t due_ns, period_ns; int active; } timers[MAXT];

static int timer_slot(uint32_t obj, int create) {
    int free = -1;
    for (int i = 0; i < MAXT; i++) {
        if (timers[i].obj == obj) return i;
        if (!timers[i].obj && free < 0) free = i;
    }
    if (create && free >= 0) { timers[free].obj = obj; return free; }
    return -1;
}
static void timer_arm(uint32_t obj, uint32_t due_ptr, int64_t period_ms, uint32_t dpc) {
    int s = timer_slot(obj, 1);
    if (s < 0) { fprintf(stderr, "stw: out of timers\n"); exit(1); }
    int64_t v = (int64_t)(LD32(due_ptr) | ((uint64_t)LD32(due_ptr + 4) << 32));
    timers[s].due_ns = v < 0 ? now_ns() + (-v) * 100 : (v - 116444736000000000ll) * 100;
    timers[s].period_ns = period_ms * 1000000;
    timers[s].dpc = dpc;
    timers[s].active = 1;
    set_sig(obj, 0);
    pthread_cond_broadcast(&changed);
}
static uint32_t timer_cancel(uint32_t obj) {
    int s = timer_slot(obj, 0);
    uint32_t was = s >= 0 && timers[s].active;
    if (s >= 0) timers[s].active = 0;
    return was;
}
uint32_t k_KeInitializeTimerEx(CPU *c, const uint32_t *a) {     /* Timer, Type (0 notification, 1 sync) */
    (void)c;
    obj_init(a[0], a[1] ? T_SYNC_TIMER : T_NOTIF_TIMER, 0);
    return 0;
}
uint32_t k_KeSetTimer(CPU *c, const uint32_t *a) {              /* Timer, DueTime(LARGE_INTEGER by value), Dpc */
    (void)c;                                                      /* a[1],a[2] = DueTime lo/hi */
    uint32_t tmp = guest_alloc(8, 8);
    ST32(tmp, a[1]); ST32(tmp + 4, a[2]);
    uint32_t was = timer_cancel(a[0]);
    timer_arm(a[0], tmp, 0, a[3]);
    return was;
}
uint32_t k_KeCancelTimer(CPU *c, const uint32_t *a) { (void)c; return timer_cancel(a[0]); }
uint32_t k_NtCreateTimer(CPU *c, const uint32_t *a) {           /* Handle*, ObjectAttributes, Type */
    (void)c;
    uint32_t o = guest_alloc(16, 16);
    obj_init(o, a[2] ? T_SYNC_TIMER : T_NOTIF_TIMER, 0);
    ST32(a[0], handle_new(H_OBJECT, o));
    return STATUS_SUCCESS;
}
uint32_t k_NtSetTimerEx(CPU *c, const uint32_t *a) {            /* Handle, DueTime*, ApcRoutine, ..., Period(ms) */
    (void)c;
    uint32_t o = handle_obj(a[0]);
    if (!o) return STATUS_INVALID_HANDLE;
    timer_arm(o, a[1], (int32_t)a[5], 0);
    return STATUS_SUCCESS;
}
uint32_t k_NtCancelTimer(CPU *c, const uint32_t *a) {
    (void)c;
    uint32_t o = handle_obj(a[0]);
    if (!o) return STATUS_INVALID_HANDLE;
    uint32_t was = timer_cancel(o);
    if (a[1]) ST8(a[1], was);
    return STATUS_SUCCESS;
}

static void run_dpc(uint32_t dpc, uint32_t timer);
void kobj_start_interrupts(void);
static void *timer_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&gil);
    for (;;) {
        sleep_ns(1000000);                                       /* 1 ms granularity */
        int64_t t = now_ns();
        for (int i = 0; i < MAXT; i++) {
            if (!timers[i].active || timers[i].due_ns > t) continue;
            set_sig(timers[i].obj, 1);
            uint32_t dpc = timers[i].dpc, obj = timers[i].obj;
            if (timers[i].period_ns) timers[i].due_ns += timers[i].period_ns; else timers[i].active = 0;
            pthread_cond_broadcast(&changed);
            if (dpc) run_dpc(dpc, obj);
        }
    }
    return NULL;
}
void kobj_start_services(void) {
    kobj_start_interrupts();
    pthread_t th;
    pthread_create(&th, NULL, timer_thread, NULL);
    pthread_detach(th);
}

/* ---- threads ---- */
uint32_t kpcr_new(uint32_t stack_top, uint32_t stack_size) {
    uint32_t fs = guest_alloc(0x1000, 0x1000), kthread = fs + 0x800;
    ST32(fs + 0x0, 0xFFFFFFFFu);
    ST32(fs + 0x4, stack_top);
    ST32(fs + 0x8, stack_top - stack_size);
    ST32(fs + 0x1C, fs);
    ST32(fs + 0x20, fs + 0x28);                                  /* Prcb */
    ST32(fs + 0x2C, kthread);                                    /* Prcb.CurrentThread */
    obj_init(kthread, T_THREAD, 0);
    return fs;
}

typedef struct { uint32_t routine, ctx1, ctx2, stack_top, stack_size, fs; } ThreadStart;
static void *thread_main(void *p) {
    ThreadStart ts = *(ThreadStart *)p;
    free(p);
    pthread_mutex_lock(&gil);
    CPU c;
    memset(&c, 0, sizeof c);
    c.fcw = 0x27F;
    c.fs = ts.fs;
    c.esp = ts.stack_top - 16;
    ST32(c.esp, 0);
    ST32(c.esp + 4, ts.ctx1);                                    /* StartRoutine(ctx1, ctx2) */
    ST32(c.esp + 8, ts.ctx2);
    call_ind(&c, ts.routine);
    uint32_t kt = LD32(ts.fs + 0x2C);
    set_sig(kt, 1);                                              /* returning from the routine ends the thread */
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gil);
    return NULL;
}
uint32_t k_PsCreateSystemThreadEx(CPU *c, const uint32_t *a) {
    (void)c;   /* ThreadHandle*, ExtraSize, KernelStackSize, TlsSize, ThreadId*, Ctx1, Ctx2, Suspended, DebugStack, Routine */
    uint32_t ssize = a[2] ? a[2] : 0x10000;
    ssize = (ssize + 0xFFF) & ~0xFFFu;
    uint32_t stack = guest_alloc(ssize, 0x1000);
    ThreadStart *ts = malloc(sizeof *ts);
    *ts = (ThreadStart){ a[9], a[5], a[6], stack + ssize, ssize, kpcr_new(stack + ssize, ssize) };
    if (a[7]) { fprintf(stderr, "stw: suspended thread creation not supported yet\n"); exit(1); }
    if (a[0]) ST32(a[0], handle_new(H_OBJECT, LD32(ts->fs + 0x2C)));
    if (a[4]) ST32(a[4], ts->fs);
    pthread_t th;
    pthread_create(&th, NULL, thread_main, ts);
    pthread_detach(th);
    sleep_ns(0);                                                 /* let the new thread start */
    return STATUS_SUCCESS;
}
uint32_t k_PsTerminateSystemThread(CPU *c, const uint32_t *a) {
    (void)a;
    set_sig(cur_thread(c), 1);
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gil);
    pthread_exit(NULL);
}
uint32_t k_KeSetBasePriorityThread(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }

uint32_t k_NtClose(CPU *c, const uint32_t *a) {
    (void)c;
    uint32_t i = a[0] / 4;
    if (a[0] % 4 || i == 0 || i >= MAXH || !htab[i].type) return STATUS_INVALID_HANDLE;
    if (htab[i].type == H_FILE) kfile_close(htab[i].obj);
    htab[i].type = 0;
    return STATUS_SUCCESS;
}

/* DPC: KDPC { Type@0 Number@1 Importance@2 ListEntry@4 DeferredRoutine@C DeferredContext@10 SystemArg1@14 SystemArg2@18 } */
uint32_t k_KeInitializeDpc(CPU *c, const uint32_t *a) {         /* Dpc, Routine, Context */
    (void)c;
    memset(G + a[0], 0, 0x1C);
    ST8(a[0], 19);
    ST32(a[0] + 0xC, a[1]); ST32(a[0] + 0x10, a[2]);
    return 0;
}
static void run_dpc(uint32_t dpc, uint32_t timer) {
    (void)timer;
    static CPU c; static int init;
    if (!init) {
        memset(&c, 0, sizeof c); c.fcw = 0x27F;
        uint32_t st = guest_alloc(0x10000, 0x1000);
        c.fs = kpcr_new(st + 0x10000, 0x10000);
        c.esp = st + 0x10000 - 16;
        init = 1;
    }
    uint32_t sp = c.esp;
    ST32(sp - 4, LD32(dpc + 0x18)); ST32(sp - 8, LD32(dpc + 0x14));
    ST32(sp - 12, LD32(dpc + 0x10)); ST32(sp - 16, dpc); ST32(sp - 20, 0);
    c.esp = sp - 20;
    if (getenv("STW_INTTRACE")) fprintf(stderr, "[int] run dpc %#x routine %#x ctx %#x\n", dpc, LD32(dpc + 0xC), LD32(dpc + 0x10));
    call_ind(&c, LD32(dpc + 0xC));                               /* routine(Dpc, Context, Arg1, Arg2) */
    c.esp = sp;
}

/* ---- interrupts and DPCs ----
 * KINTERRUPT (guest memory, our layout): +0 ServiceRoutine, +4 ServiceContext, +8 Vector, +12 Connected.
 * The only interrupt source that exists is the GPU's vertical blank at 60 Hz. */
#define MAXINT 16
static uint32_t interrupts[MAXINT];

uint32_t k_KeInitializeInterrupt(CPU *c, const uint32_t *a) {   /* Interrupt, Routine, Context, Vector, Irql, Mode, Share */
    (void)c;
    ST32(a[0], a[1]); ST32(a[0] + 4, a[2]); ST32(a[0] + 8, a[3]); ST32(a[0] + 12, 0);
    return 0;
}
uint32_t k_KeConnectInterrupt(CPU *c, const uint32_t *a) {
    (void)c;
    ST32(a[0] + 12, 1);
    for (int i = 0; i < MAXINT; i++) if (!interrupts[i] || interrupts[i] == a[0]) { interrupts[i] = a[0]; return 1; }
    return 0;
}
uint32_t k_KeDisconnectInterrupt(CPU *c, const uint32_t *a) {
    (void)c;
    ST32(a[0] + 12, 0);
    for (int i = 0; i < MAXINT; i++) if (interrupts[i] == a[0]) interrupts[i] = 0;
    return 1;
}

#define MAXDPC 64
static uint32_t dpcq[MAXDPC];
uint32_t k_KeInsertQueueDpc(CPU *c, const uint32_t *a) {        /* Dpc, SystemArgument1, SystemArgument2 */
    (void)c;
    for (int i = 0; i < MAXDPC; i++) if (dpcq[i] == a[0]) return 0;       /* already queued */
    for (int i = 0; i < MAXDPC; i++)
        if (!dpcq[i]) { dpcq[i] = a[0]; ST32(a[0] + 0x14, a[1]); ST32(a[0] + 0x18, a[2]); return 1; }
    return 0;
}
uint32_t k_KeRemoveQueueDpc(CPU *c, const uint32_t *a) {
    (void)c;
    for (int i = 0; i < MAXDPC; i++) if (dpcq[i] == a[0]) { dpcq[i] = 0; return 1; }
    return 0;
}
static void run_dpc_queue(void) {
    for (int i = 0; i < MAXDPC; i++) {
        uint32_t d = dpcq[i];
        if (!d) continue;
        dpcq[i] = 0;
        run_dpc(d, 0);
    }
}

static CPU *isr_cpu(void) {
    static CPU c; static int init;
    if (!init) {
        memset(&c, 0, sizeof c); c.fcw = 0x27F;
        uint32_t st = guest_alloc(0x10000, 0x1000);
        c.fs = kpcr_new(st + 0x10000, 0x10000);
        c.esp = st + 0x10000 - 16;
        init = 1;
    }
    return &c;
}

static pthread_mutex_t int_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t int_cv = PTHREAD_COND_INITIALIZER;
static int int_poked;
/* The GPU raised an interrupt: wake the interrupt thread now instead of waiting for its next 1 ms tick. */
void kobj_poke_interrupt(void) {
    pthread_mutex_lock(&int_mu);
    int_poked = 1;
    pthread_cond_signal(&int_cv);
    pthread_mutex_unlock(&int_mu);
}

static void *interrupt_thread(void *arg) {
    (void)arg;
    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);
    for (;;) {
        {   struct timespec to;                                  /* sleep until poked, at most 1 ms (vblank timekeeping) */
            clock_gettime(CLOCK_REALTIME, &to);
            to.tv_nsec += 1000000; if (to.tv_nsec >= 1000000000) { to.tv_sec++; to.tv_nsec -= 1000000000; }
            pthread_mutex_lock(&int_mu);
            if (!int_poked) pthread_cond_timedwait(&int_cv, &int_mu, &to);
            int_poked = 0;
            pthread_mutex_unlock(&int_mu);
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long us = (now.tv_sec - last.tv_sec) * 1000000L + (now.tv_nsec - last.tv_nsec) / 1000;
        static long vb_us; if (!vb_us) vb_us = getenv("STW_VBHZ") ? 1000000L / atoi(getenv("STW_VBHZ")) : 16667;   /* STW_VBHZ=240: benchmark aid, vblank faster than 60 Hz */
        int vblank = us >= vb_us;
        if (vblank) last = now;
        /* Cheap unlocked peek: only take the guest lock when something can actually fire. */
        int pending = LD32(NV2A_BASE + 0x100) != 0 && (LD32(NV2A_BASE + 0x140) & 1);
        if (!vblank && !pending) continue;
        gpu_sync_fences();                                         /* the ISR/DPCs must see every fence the GPU has been given */
        gil_acquire_priority();
        if (vblank && (LD32(NV2A_BASE + 0x600140) & 1)) {          /* PCRTC vertical blank, if enabled */
            ST32RAW(NV2A_BASE + 0x600100, LD32(NV2A_BASE + 0x600100) | 1);
            nv2a_update_pmc();
        }
        if (LD32(NV2A_BASE + 0x100) != 0 && (LD32(NV2A_BASE + 0x140) & 1)) {
            for (int i = 0; i < MAXINT; i++) {
                uint32_t in = interrupts[i];
                if (!in) continue;
                CPU *c = isr_cpu();
                uint32_t sp = c->esp, args[2] = { in, LD32(in + 4) };
                guest_call(c, LD32(in), 2, args);
                c->esp = sp;
            }
            run_dpc_queue();
        }
        if (vblank) display_vblank();
        pthread_mutex_unlock(&gil);
    }
    return NULL;
}
void kobj_start_interrupts(void) {
    pthread_t th;
    pthread_create(&th, NULL, interrupt_thread, NULL);
    pthread_detach(th);
}

/* ---- helpers for asynchronous I/O completion (kfile.c) ---- */
uint32_t kobj_cur_thread(CPU *c) { return cur_thread(c); }
int kobj_queue_apc(uint32_t thread, uint32_t routine, uint32_t ctx, uint32_t a1, uint32_t a2) {
    for (int i = 0; i < MAXAPC; i++)
        if (!apcs[i].thread) {
            apcs[i].thread = thread; apcs[i].routine = routine; apcs[i].ctx = ctx; apcs[i].a1 = a1; apcs[i].a2 = a2;
            pthread_cond_broadcast(&changed);
            return 1;
        }
    return 0;
}
void kobj_signal_handle(uint32_t handle) {
    uint32_t o = handle_obj(handle);
    if (o) { ST32(o + 4, 1); pthread_cond_broadcast(&changed); }
}
