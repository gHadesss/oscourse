/* User-level signal handler support with syscalls wrappers.
 * We register the assembly language wrapper in 
 * sigentry.S, which in turns calls the assigned C
 * function. */

#include <inc/signal.h>
#include <inc/lib.h>
#include <inc/env.h>
#include <kern/traceopt.h>

void _handle_signal(struct UTrapframe *utf, struct QueuedSignal *qs) {
    if (qs->qs_act.sa_handler == SIG_DFL) {
        // if (trace_signals)

        sys_env_destroy(CURENVID);
    } else if (qs->qs_act.sa_handler == SIG_IGN) {
        // if (trace_signals)
        
        return;
    }
    
    if (qs->qs_act.sa_flags & SA_SIGINFO) {
        // if (trace_signals)

        (qs->qs_act.sa_sigaction)(qs->qs_info.si_signo, &qs->qs_info, (void *)utf);
    } else {
        // if (trace_signals)

        (qs->qs_act.sa_handler)(qs->qs_info.si_signo);
    }
}

int
sigqueue(pid_t pid, int sig, const union sigval value) {
    return sys_sigqueue(pid, sig, value);
}

int 
sigwait(const sigset_t *set, int *sig) {
    return sys_sigwait(set, sig);
}

int
sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    return sys_sigaction(signum, act, oldact);
}

int
sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    return sys_sigprocmask(how, set, oldset);
}