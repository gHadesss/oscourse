#include <inc/lib.h>
#include <inc/signal.h>


union sigval sv;

volatile sig_atomic_t chldcntr = 0;

static void
handler_chld(int signo, siginfo_t * info, void * ctx) {
    assert(signo == SIGCHLD);
    ++chldcntr;
}

volatile sig_atomic_t depth = 0;
volatile sig_atomic_t reached_max_depth = 0;
volatile sig_atomic_t f = 0;

static void
handler_nodefer(int signo) {
    if (10 < depth) {
        reached_max_depth = 1;
        return;
    }
    // cprintf("handling nodefer, depth is %d\n", depth++);
    ++depth;
    sigqueue(0, signo, sv);
    // cprintf("sent non-first sigint %d\n", depth);
    sys_yield();
    --depth;
}


volatile sig_atomic_t intcount = 0;

static void
handler_defer(int signo) {
    assert(depth == 0);
    if (2 < intcount)
        return;
    ++depth;
    // cprintf("%d %d\n", sigqueue(0, signo, sv), intcount);
    sigqueue(0, signo, sv);
    sys_yield();
    --depth;
    ++intcount;
}


volatile sig_atomic_t termcount = 0;

static void
handler_term(int signo) {
    ++termcount;
}

void
umain(int argc, char **argv) {
    sv.sival_ptr = 0;
    int err;

    // test SA_RESETHAND
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler_chld;
    sa.sa_flags = SA_RESETHAND | SA_SIGINFO;
    err = sigaction(SIGCHLD, &sa, NULL);
    assert(err == 0);

    err = sigqueue(CURENVID, SIGCHLD, sv);
    assert(err == 0);
    err = sigqueue(CURENVID, SIGCHLD, sv);  // ign
    assert(err == 0);
    assert(chldcntr == 1);

    struct sigaction old_sa;
    memset(&sa, 0, sizeof(sa));
    err = sigaction(SIGCHLD, &sa, &old_sa);
    assert(err == 0);
    assert((old_sa.sa_flags & SA_SIGINFO) == 0);
    assert(old_sa.sa_handler == SIG_IGN);

    cprintf("\n---------------------\n\n");

    // test SA_NODEFER and nested signals
    sa.sa_handler = handler_nodefer;
    sa.sa_flags = SA_NODEFER;
    err = sigaction(SIGINT, &sa, &old_sa);
    assert(err == 0);
    err = sigqueue(CURENVID, SIGINT, sv);
    assert(err == 0);
    assert(depth == 0);
    assert(reached_max_depth);
    reached_max_depth = 0;

    cprintf("\n---------------------\n\n");

    // signal is blocked by default
    sa.sa_handler = handler_defer;
    sa.sa_flags = 0;
    err = sigaction(SIGINT, &sa, &old_sa);
    assert(err == 0);
    assert(old_sa.sa_handler == handler_nodefer);
    assert(old_sa.sa_flags == SA_NODEFER);
    err = sigqueue(CURENVID, SIGINT, sv);
    assert(err == 0);
    assert(depth == 0);
    assert(intcount == 3);
    assert(!reached_max_depth);

    cprintf("\n---------------------\n\n");

    // test sigwait removes only one signal
    sa.sa_handler = handler_term;
    err = sigaction(SIGTERM, &sa, NULL);
    assert(err == 0);
    sigset_t set = SIGNAL_MASK(SIGTERM);

    sigprocmask(SIG_BLOCK, &set, NULL);
    sigqueue(CURENVID, SIGTERM, sv);
    sigqueue(CURENVID, SIGTERM, sv);
    sigqueue(CURENVID, SIGTERM, sv);
    sigwait(&set, NULL);
    sigqueue(CURENVID, SIGTERM, sv);
    
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    sigqueue(CURENVID, SIGTERM, sv);
    sys_yield();
    assert(termcount == 4);

    cprintf("Test finished\n");
}