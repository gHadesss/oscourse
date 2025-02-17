/* Ping-pong a counter between two processes.
 * Only need to start one of these -- splits into two with fork. */

#include <inc/lib.h>
#include <inc/signal.h>

volatile sig_atomic_t value = 0;
volatile sig_atomic_t sender = 0;
volatile sig_atomic_t updated = 0;

static void
handler(int signo, siginfo_t * info, void * ctx) {
    assert(signo == SIGUSR1);
    value = info->si_value.sival_int;
    sender = info->si_pid;
    updated = 1;
}

void
umain(int argc, char **argv) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, NULL);

    envid_t who;

    if ((who = fork()) != 0) {
        /* get the ball rolling */
        cprintf("send 0 from %x to %x\n", sys_getenvid(), who);
        union sigval sv;
        sv.sival_int = 0;
        sigqueue(who, SIGUSR1, sv);
    }

    while (1) {
        while (!updated) {
            sys_yield();
        }
        updated = 0;
        cprintf("%x got %d from %x\n", sys_getenvid(), value, sender);
        if (value == 10) return;
        union sigval sv;
        sv.sival_int = value + 1;
        sigqueue(sender, SIGUSR1, sv);
        if (sv.sival_int == 10) return;
    }
}