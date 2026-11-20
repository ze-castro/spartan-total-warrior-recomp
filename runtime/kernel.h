/* Xbox kernel HLE: export table and handler interface. */
#ifndef STW_KERNEL_H
#define STW_KERNEL_H
#include <stdint.h>
#include "rt.h"

/* args[] holds the call's arguments in order (fastcall: ecx, edx first, then the stack ones). */
typedef uint32_t (*KHandler)(CPU *c, const uint32_t *args);

typedef struct {
    uint32_t ordinal;
    const char *name;
    int fastcall;
    int argbytes;          /* stdcall: bytes the callee pops; fastcall: total incl. the 8 in registers; -1 = cdecl */
    int is_data;
    KHandler fn;           /* NULL when unimplemented (and always for data exports) */
} KExport;

extern const KExport KEXPORTS[];
#define KSTUB_BASE 0x7E000000u         /* guest addresses that function imports are patched to */
#define KSTUB_LIMIT (KSTUB_BASE + 0x1000u)

uint32_t kernel_data_addr(const char *name);       /* guest address of a data export's storage */
void kernel_dispatch(CPU *c, uint32_t stub);        /* called by call_ind for stub addresses */
void kernel_init(void);

/* guest memory helpers */
uint32_t guest_alloc(uint32_t size, uint32_t align);
void guest_backtrace(CPU *c);
uint32_t guest_call(CPU *c, uint32_t fn, int nargs, const uint32_t *args);   /* stdcall into guest code */  /* bump allocator for the guest heap */

/* ---- kobj.c: threads, handles, dispatcher objects ----
 * Guest threads are host threads serialized by one global lock that is dropped only while a thread
 * blocks (wait/sleep/yield), which models the Xbox's single CPU without preemption. */
void gil_release(void);
void gil_lock_priority(void);                         /* take the guest lock from a host thread, ahead of running guest code */
void gil_init(void);                                  /* the calling (main) thread takes the lock */
uint32_t kpcr_new(uint32_t stack_top, uint32_t stack_size);   /* allocate a KPCR+KTHREAD; returns fs base */
void kobj_start_services(void);                       /* timer thread */
uint32_t kobj_cur_thread(CPU *c);
int kobj_queue_apc(uint32_t thread, uint32_t routine, uint32_t ctx, uint32_t a1, uint32_t a2);
void kobj_signal_handle(uint32_t handle);
uint32_t handle_new(int type, uint32_t obj);
uint32_t handle_obj(uint32_t h);                      /* guest address of the dispatcher object, 0 if invalid */
#endif
