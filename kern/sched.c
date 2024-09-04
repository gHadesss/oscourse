#include <inc/assert.h>
#include <inc/x86.h>
#include <inc/string.h>
#include <kern/env.h>
#include <kern/monitor.h>
#include <kern/pmap.h>
#include <kern/traceopt.h>


struct Taskstate cpu_ts;
_Noreturn void sched_halt(void);

/* This function checks if specified env is stopped via sigwait and if so, looks up
 * for specified signals and if any exists, allows to continue execution. If not, continue waiting. 
 * Returned values: 
 * 0, if we may run this env; 
 * 1, if it is still waiting. */
bool
check4pending_sigwait(struct Env *env) {
    if (!env->env_sig_awaiting) {
        return 0;
    }

    /* Search the queue for specified sigs */
    size_t sig_idx = env->env_sig_queue_start;

    while (sig_idx != env->env_sig_queue_end) {
        int signo = env->env_sig_queue[sig_idx].qs_info.si_signo;

        if (SIGNAL_MASK(signo) & env->env_sig_awaiting) {
            break;
        }
        
        sig_idx = (sig_idx + 1) % SIG_QUEUE_SIZE;
    }

    if (sig_idx == env->env_sig_queue_end) {
        return 1;
    }

    /* Return found signal in sigwait fields, remove it from queue */
    struct QueuedSignal *qs = env->env_sig_queue + sig_idx;

    if (trace_signals) {
        cprintf("signals: removed signo %d from env %x's queue\n", qs->qs_info.si_signo, env->env_id);
    }

    if (env->env_sig_caught_ptr) {
        user_mem_assert(env, env->env_sig_caught_ptr, sizeof(*env->env_sig_caught_ptr), PROT_W | PROT_USER_);
        struct AddressSpace *old = switch_address_space(&env->address_space);
        set_wp(0);
        nosan_memcpy((void *)env->env_sig_caught_ptr, (void *)&qs->qs_info.si_signo, sizeof(qs->qs_info.si_signo));
        set_wp(1);
        switch_address_space(old);

        env->env_sig_caught_ptr = NULL;
    }

    env->env_sig_awaiting = 0;

    struct QueuedSignal *qs_end = env->env_sig_queue + env->env_sig_queue_end;
    
    if (qs < qs_end) {
        memmove(qs, qs + 1, sizeof(struct QueuedSignal) * (env->env_sig_queue_end - sig_idx - 1));
        env->env_sig_queue_end = (env->env_sig_queue_end - 1) % SIG_QUEUE_SIZE;
    }
    else {
        memmove(qs, qs + 1, sizeof(struct QueuedSignal) * (SIG_QUEUE_SIZE - sig_idx - 1));
        memmove(env->env_sig_queue + SIG_QUEUE_SIZE - 1, env->env_sig_queue, sizeof(struct QueuedSignal));
        
        if (env->env_sig_queue_end) {
            memmove(env->env_sig_queue, env->env_sig_queue + 1, sizeof(struct QueuedSignal) * (env->env_sig_queue_end - 1));
            env->env_sig_queue_end = (env->env_sig_queue_end - 1) % SIG_QUEUE_SIZE;
        } else {
            env->env_sig_queue_end = SIG_QUEUE_SIZE - 1;
        }
    }


    return 0;
}

/* Choose a user environment to run and run it */
_Noreturn void
sched_yield(void) {
    /* Implement simple round-robin scheduling.
     *
     * Search through 'envs' for an ENV_RUNNABLE environment in
     * circular fashion starting just after the env was
     * last running.  Switch to the first such environment found.
     *
     * If no envs are runnable, but the environment previously
     * running is still ENV_RUNNING, it's okay to
     * choose that environment.
     *
     * If there are no runnable environments,
     * simply drop through to the code
     * below to halt the cpu */

    // LAB 3: Your code here:
    int next_idx = curenv ? curenv - envs : 0;

    for (int i = 0; i < NENV + 1; i++) {
        next_idx = (next_idx + 1) % NENV;

        if (envs[next_idx].env_status != ENV_RUNNABLE && envs[next_idx].env_status != ENV_RUNNING) {
            continue;
        }

        /* you can't look up for sigcont here */
        if (envs[next_idx].env_sig_stopped) {
            continue;
        }

        /* If we are stopped via sigwait, we need to look for specified signals. Probably need to add special function? */
        if (check4pending_sigwait(&envs[next_idx])) {
            continue;
        }

        env_run(&envs[next_idx]);
    }

    cprintf("Halt\n");

    /* No runnable environments,
     * so just halt the cpu */
    sched_halt();
}

/* Halt this CPU when there is nothing to do. Wait until the
 * timer interrupt wakes it up. This function never returns */
_Noreturn void
sched_halt(void) {

    /* For debugging and testing purposes, if there are no runnable
     * environments in the system, then drop into the kernel monitor */
    int i;
    for (i = 0; i < NENV; i++)
        if (envs[i].env_status == ENV_RUNNABLE ||
            envs[i].env_status == ENV_RUNNING) break;
    if (i == NENV) {
        cprintf("No runnable environments in the system!\n");
        for (;;) monitor(NULL);
    }

    /* Mark that no environment is running on CPU */
    curenv = NULL;

    /* Reset stack pointer, enable interrupts and then halt */
    asm volatile(
            "movq $0, %%rbp\n"
            "movq %0, %%rsp\n"
            "pushq $0\n"
            "pushq $0\n"
            "sti\n"
            "hlt\n" ::"a"(cpu_ts.ts_rsp0));

    /* Unreachable */
    for (;;)
        ;
}
