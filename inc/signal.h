/* See COPYRIGHT for copyright information. */

#ifndef JOS_INC_SIGNAL_H
#define JOS_INC_SIGNAL_H

#include <inc/types.h>
// #include <inc/env.h>

/* You can assume that int is atomic, this assumption is true on all of the machines
 * that the GNU C Library supports and on all POSIX systems we know of (GNU docs).
 */
typedef int32_t sig_atomic_t;

/* Signal mask type */
typedef uint32_t sigset_t;

/* Environment number */
typedef int32_t pid_t;

#define SIGNAL_MASK(signo) ((uint32_t)(1U << (signo - 1)))
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

/* signal numbers with default action */
enum {
    /* This is needed due to entry through the _pgfault_upcall (I personally
     * couldn't come up with better idea how to change context to handler better). */
    // SIGOMITTED = 0, /* NONE! This is not actual signal, it should never be passed to syscalls */

    /* We do not need SIGRESERVED to be honest, just control signal values correctly and we will be happy. */

    SIGINT = 1,      /* term                                                                   */
    SIGKILL,         /* term (reminder: can't be caught unlike sigterm)                        */
    SIGPIPE,         /* term (write in pipe with no readers)                                   */
    SIGUSR1,         /* ign                                                                    */
    SIGUSR2,         /* ign                                                                    */
    SIGTERM,         /* term                                                                   */
    SIGCHLD,         /* ign                                                                    */
    SIGSTOP,         /* stop                                                                   */
    SIGCONT,         /* ign  (when not stopped, else continue execution)                       */
    NSIGNALS = 9
};

/* Type for data associated with a signal.  */
union sigval {
    int sival_int;
    void *sival_ptr;
};

/* Second argument of sa_sigaction() */
struct siginfo_t {
    int si_signo;           /* Signal number */
    int si_code;            /* Signal sending reason */
    pid_t si_pid;           /* Sending environment number */
    uint32_t padding;       /* For using in _pgfault_upcall check */
    void *si_addr;          /* Failing address */
    union sigval si_value;  /* Signal value */
} __attribute__((packed));

typedef struct siginfo_t siginfo_t;

/* Redefined signal handler */
struct sigaction {
    /* Signal handling function */
    union {
        void (* sa_handler)(int);
        void (* sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;       /* Blocked signals during current handling */
    unsigned int sa_flags;  /* Specified flags */
};

/* Supported flags in sa_flags field */
#define SA_NOCLDSTOP  0x00000001  /* Don't send SIGCHLD if child receives SIGSTOP */
#define SA_SIGINFO    0x00000004  /* Use sa_sigaction instead of sa_handler */
#define SA_NODEFER    0x40000000  /* May catch a signal inside a handler */
#define SA_RESETHAND  0x80000000  /* Restore signal handler to default after handling */

#define SA_NOMASK	  SA_NODEFER
#define SA_ONESHOT	  SA_RESETHAND
#define SA_ALL_FLAGS (SA_NOCLDSTOP | SA_SIGINFO | SA_NODEFER | SA_RESETHAND)

/* Specified action in sigprocmask */
#define SIG_BLOCK     0
#define SIG_UNBLOCK   1
#define SIG_SETMASK   2


#endif /* !JOS_INC_SIGNAL_H */
