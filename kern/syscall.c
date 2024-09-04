/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/console.h>
#include <kern/env.h>
#include <kern/kclock.h>
#include <kern/pmap.h>
#include <kern/sched.h>
#include <kern/syscall.h>
#include <kern/trap.h>
#include <kern/traceopt.h>

/* Print a string to the system console.
 * The string is exactly 'len' characters long.
 * Destroys the environment on memory errors. */
static int
sys_cputs(const char *s, size_t len) {
    // LAB 8: Your code here

    /* Check that the user has permission to read memory [s, s+len).
     * Destroy the environment if not. */
    user_mem_assert(curenv, s, len, PROT_R | PROT_USER_);

#ifdef SANITIZE_SHADOW_BASE
    platform_asan_unpoison((void *)s, len);
#endif

    for (size_t i = 0; i < len; i++) {
        cputchar(*(s + i));
    }

#ifdef SANITIZE_SHADOW_BASE
    platform_asan_poison((void *)s, len);
#endif

    return 0;
}

/* Read a character from the system console without blocking.
 * Returns the character, or 0 if there is no input waiting. */
static int
sys_cgetc(void) {
    // LAB 8: Your code here
    return cons_getc();
}

/* Returns the current environment's envid. */
static envid_t
sys_getenvid(void) {
    // LAB 8: Your code here
    return curenv->env_id;
}

/* Destroy a given environment (possibly the currently running environment).
 *
 *  Returns 0 on success, < 0 on error.  Errors are:
 *  -E_BAD_ENV if environment envid doesn't currently exist,
 *      or the caller doesn't have permission to change envid. */
static int
sys_env_destroy(envid_t envid) {
    // LAB 8: Your code here.
    struct Env *env = NULL;

    if (envid2env(envid, &env, false)) {
        return -E_BAD_ENV;
    }

#if 1 /* TIP: Use this snippet to log required for passing grade tests info */
    if (trace_envs) {
        cprintf(env == curenv ?
                        "[%08x] exiting gracefully\n" :
                        "[%08x] destroying %08x\n",
                curenv->env_id, env->env_id);
    }
#endif

    env_destroy(env);
    return 0;
}

/* Deschedule current environment and pick a different one to run. */
static void
sys_yield(void) {
    // LAB 9: Your code here
    sched_yield();
}

/* Allocate a new environment.
 * Returns envid of new environment, or < 0 on error.  Errors are:
 *  -E_NO_FREE_ENV if no free environment is available.
 *  -E_NO_MEM on memory exhaustion. */
static envid_t
sys_exofork(void) {
    /* Create the new environment with env_alloc(), from kern/env.c.
     * It should be left as env_alloc created it, except that
     * status is set to ENV_NOT_RUNNABLE, and the register set is copied
     * from the current environment -- but tweaked so sys_exofork
     * will appear to return 0. */

    // LAB 9: Your code here
    struct Env* new = NULL;
    int res = 0;

    if ((res = env_alloc(&new, curenv->env_id, ENV_TYPE_USER)) < 0) {
        return res;
    }

    new->env_status = ENV_NOT_RUNNABLE;
    new->env_tf = curenv->env_tf;
    new->env_tf.tf_regs.reg_rax = 0;

    return new->env_id;
}

/* Set envid's env_status to status, which must be ENV_RUNNABLE
 * or ENV_NOT_RUNNABLE.
 *
 * Returns 0 on success, < 0 on error.  Errors are:
 *  -E_BAD_ENV if environment envid doesn't currently exist,
 *      or the caller doesn't have permission to change envid.
 *  -E_INVAL if status is not a valid status for an environment. */
static int
sys_env_set_status(envid_t envid, int status) {
    /* Hint: Use the 'envid2env' function from kern/env.c to translate an
     * envid to a struct Env.
     * You should set envid2env's third argument to 1, which will
     * check whether the current environment has permission to set
     * envid's status. */

    // LAB 9: Your code here
    struct Env* new = NULL;

    if (envid2env(envid, &new, 1) < 0) {
        return -E_BAD_ENV;
    }

    if (status != ENV_RUNNABLE && status != ENV_NOT_RUNNABLE) {
        return -E_INVAL;
    }

    new->env_status = status;
    return 0;
}

/* Set the page fault upcall for 'envid' by modifying the corresponding struct
 * Env's 'env_pgfault_upcall' field.  When 'envid' causes a page fault, the
 * kernel will push a fault record onto the exception stack, then branch to
 * 'func'.
 *
 * Returns 0 on success, < 0 on error.  Errors are:
 *  -E_BAD_ENV if environment envid doesn't currently exist,
 *      or the caller doesn't have permission to change envid. */
static int
sys_env_set_pgfault_upcall(envid_t envid, void *func) {
    // LAB 9: Your code here:
    struct Env* new = NULL;

    if (envid2env(envid, &new, 1) < 0) {
        return -E_BAD_ENV;
    }

    new->env_pgfault_upcall = func;
    return 0;
}

/* Allocate a region of memory and map it at 'va' with permission
 * 'perm' in the address space of 'envid'.
 * The page's contents are set to 0.
 * If a page is already mapped at 'va', that page is unmapped as a
 * side effect.
 *
 * This call should work with or without ALLOC_ZERO/ALLOC_ONE flags
 * (set them if they are not already set)
 *
 * It allocates memory lazily so you need to use map_region
 * with PROT_LAZY and ALLOC_ONE/ALLOC_ZERO set.
 *
 * Don't forget to set PROT_USER_
 *
 * PROT_ALL is useful for validation.
 *
 * Return 0 on success, < 0 on error.  Errors are:
 *  -E_BAD_ENV if environment envid doesn't currently exist,
 *      or the caller doesn't have permission to change envid.
 *  -E_INVAL if va >= MAX_USER_ADDRESS, or va is not page-aligned.
 *  -E_INVAL if perm is inappropriate (see above).
 *  -E_NO_MEM if there's no memory to allocate the new page,
 *      or to allocate any necessary page tables. */
static int
sys_alloc_region(envid_t envid, uintptr_t addr, size_t size, int perm) {
    // LAB 9: Your code here:
    struct Env* new = NULL;

    if (envid2env(envid, &new, 1) < 0) {
        return -E_BAD_ENV;
    }

    if (addr >= MAX_USER_ADDRESS || addr & CLASS_MASK(0)) {
        return -E_INVAL;
    }

    if (!(perm & PROT_ALL)) {
        return -E_INVAL;
    }

    if (perm & ALLOC_ONE) {
        perm = perm & ~ALLOC_ZERO;
    } else {
        perm = perm | ALLOC_ZERO;
        perm = perm & ~ALLOC_ONE;
    }

    if (map_region(&new->address_space, addr, NULL, 0, size, perm | PROT_USER_ | PROT_LAZY) < 0) {
        return -E_NO_MEM;
    }

    return 0;
}

/* Map the region of memory at 'srcva' in srcenvid's address space
 * at 'dstva' in dstenvid's address space with permission 'perm'.
 * Perm has the same restrictions as in sys_alloc_region, except
 * that it also does not supprt ALLOC_ONE/ALLOC_ONE flags.
 *
 * You only need to check alignment of addresses, perm flags and
 * that addresses are a part of user space. Everything else is
 * already checked inside map_region().
 *
 * Return 0 on success, < 0 on error.  Errors are:
 *  -E_BAD_ENV if srcenvid and/or dstenvid doesn't currently exist,
 *      or the caller doesn't have permission to change one of them.
 *  -E_INVAL if srcva >= MAX_USER_ADDRESS or srcva is not page-aligned,
 *      or dstva >= MAX_USER_ADDRESS or dstva is not page-aligned.
 *  -E_INVAL is srcva is not mapped in srcenvid's address space.
 *  -E_INVAL if perm is inappropriate (see sys_page_alloc).
 *  -E_INVAL if (perm & PROT_W), but srcva is read-only in srcenvid's
 *      address space.
 *  -E_NO_MEM if there's no memory to allocate any necessary page tables. */

static int
sys_map_region(envid_t srcenvid, uintptr_t srcva,
               envid_t dstenvid, uintptr_t dstva, size_t size, int perm) {
    // LAB 9: Your code here
    struct Env* src = NULL;
    struct Env* dst = NULL;

    if (envid2env(srcenvid, &src, 1) < 0 || envid2env(dstenvid, &dst, 1) < 0) {
        return -E_BAD_ENV;
    }

    if (srcva >= MAX_USER_ADDRESS || srcva & CLASS_MASK(0)) {
        return -E_INVAL;
    }

    if (dstva >= MAX_USER_ADDRESS || dstva & CLASS_MASK(0)) {
        return -E_INVAL;
    }

    if (perm & ~PROT_ALL || perm & ALLOC_ZERO || perm & ALLOC_ONE) {
        return -E_INVAL;
    }

    if (map_region(&dst->address_space, dstva, &src->address_space, srcva, size, perm | PROT_USER_) < 0) {
        return -E_NO_MEM;
    }

    return 0;
}

/* Unmap the region of memory at 'va' in the address space of 'envid'.
 * If no page is mapped, the function silently succeeds.
 *
 * Return 0 on success, < 0 on error.  Errors are:
 *  -E_BAD_ENV if environment envid doesn't currently exist,
 *      or the caller doesn't have permission to change envid.
 *  -E_INVAL if va >= MAX_USER_ADDRESS, or va is not page-aligned. */
static int
sys_unmap_region(envid_t envid, uintptr_t va, size_t size) {
    /* Hint: This function is a wrapper around unmap_region(). */

    // LAB 9: Your code here
    struct Env* new = NULL;

    if (envid2env(envid, &new, 1) < 0) {
        return -E_BAD_ENV;
    }

    if (va >= MAX_USER_ADDRESS || va & CLASS_MASK(0)) {
        return -E_INVAL;
    }

    unmap_region(&new->address_space, va, size);
    return 0;
}

/* Map region of physical memory to the userspace address.
 * This is meant to be used by the userspace drivers, of which
 * the only one currently is the filesystem server.
 *
 * Return 0 on succeeds, < 0 on error. Erros are:
 *  -E_BAD_ENV if environment envid doesn't currently exist,
 *      or the caller doesn't have permission to change envid.
 *  -E_BAD_ENV if is not a filesystem driver (ENV_TYPE_FS).
 *  -E_INVAL if va >= MAX_USER_ADDRESS, or va is not page-aligned.
 *  -E_INVAL if pa is not page-aligned.
 *  -E_INVAL if size is not page-aligned.
 *  -E_INVAL if prem contains invalid flags
 *     (including PROT_SHARE, PROT_COMBINE or PROT_LAZY).
 *  -E_NO_MEM if address does not exist.
 *  -E_NO_ENT if address is already used. */
static int
sys_map_physical_region(uintptr_t pa, envid_t envid, uintptr_t va, size_t size, int perm) {
    // LAB 10: Your code here
    // TIP: Use map_physical_region() with (perm | PROT_USER_ | MAP_USER_MMIO)
    //      And don't forget to validate arguments as always.
    struct Env* new = NULL;

    if (envid2env(envid, &new, 1) < 0 || new->env_type != ENV_TYPE_FS) {
        return -E_BAD_ENV;
    }

    if ((va + size) >= MAX_USER_ADDRESS || va & CLASS_MASK(0) || pa & CLASS_MASK(0) || size & CLASS_MASK(0)) {
        return -E_INVAL;
    }

    if (perm & ~PROT_ALL) {
        return -E_INVAL;
    }

    return map_physical_region(&new->address_space, va, pa, size, perm | PROT_USER_ | MAP_USER_MMIO);
}

/* Try to send 'value' to the target env 'envid'.
 * If srcva < MAX_USER_ADDRESS, then also send region currently mapped at 'srcva',
 * so that receiver gets mapping.
 *
 * The send fails with a return value of -E_IPC_NOT_RECV if the
 * target is not blocked, waiting for an IPC.
 *
 * The send also can fail for the other reasons listed below.
 *
 * Otherwise, the send succeeds, and the target's ipc fields are
 * updated as follows:
 *    env_ipc_recving is set to 0 to block future sends;
 *    env_ipc_maxsz is set to min of size and it's current vlaue;
 *    env_ipc_from is set to the sending envid;
 *    env_ipc_value is set to the 'value' parameter;
 *    env_ipc_perm is set to 'perm' if a page was transferred, 0 otherwise.
 * The target environment is marked runnable again, returning 0
 * from the paused sys_ipc_recv system call.  (Hint: does the
 * sys_ipc_recv function ever actually return?)
 *
 * If the sender wants to send a page but the receiver isn't asking for one,
 * then no page mapping is transferred, but no error occurs.
 * The ipc only happens when no errors occur.
 * Send region size is the minimum of sized specified in sys_ipc_try_send() and sys_ipc_recv()
 *
 * Returns 0 on success, < 0 on error.
 * Errors are:
 *  -E_BAD_ENV if environment envid doesn't currently exist.
 *      (No need to check permissions.)
 *  -E_IPC_NOT_RECV if envid is not currently blocked in sys_ipc_recv,
 *      or another environment managed to send first.
 *  -E_INVAL if srcva < MAX_USER_ADDRESS but srcva is not page-aligned.
 *  -E_INVAL if srcva < MAX_USER_ADDRESS and perm is inappropriate
 *      (see sys_page_alloc).
 *  -E_INVAL if srcva < MAX_USER_ADDRESS but srcva is not mapped in the caller's
 *      address space.
 *  -E_INVAL if (perm & PTE_W), but srcva is read-only in the
 *      current environment's address space.
 *  -E_NO_MEM if there's not enough memory to map srcva in envid's
 *      address space. */
static int
sys_ipc_try_send(envid_t envid, uint32_t value, uintptr_t srcva, size_t size, int perm) {
    // LAB 9: Your code here
    struct Env* dst = NULL;

    if (envid2env(envid, &dst, 0) < 0) {
        return -E_BAD_ENV;
    }

    if (!dst->env_ipc_recving) {
        return -E_IPC_NOT_RECV;
    }

    if (srcva < MAX_USER_ADDRESS && srcva & CLASS_MASK(0)) {
        return -E_INVAL;
    }

    if (srcva < MAX_USER_ADDRESS && dst->env_ipc_dstva < MAX_USER_ADDRESS) {
        // page alignment
        if (srcva & CLASS_MASK(0) || dst->env_ipc_dstva & CLASS_MASK(0)) {
            return -E_INVAL;
        }

        // perm check
        if (perm & ~PROT_ALL) {
            return -E_INVAL;
        }

        // mapping and write permission check
        // if ((perm & PROT_W) && user_mem_check(curenv, (void *)srcva, size, PROT_W) < 0) {
        //     return -E_INVAL;
        // }

        // trying to map region with min length to dstva
        size_t min = MIN(size, dst->env_ipc_maxsz);
        
        if (map_region(&dst->address_space, dst->env_ipc_dstva, &curenv->address_space, srcva, 
                min, perm | PROT_USER_) < 0) {
            return -E_NO_MEM;
        }
        
        dst->env_ipc_perm = perm;
        dst->env_ipc_maxsz = min;
    } else {
        dst->env_ipc_perm = 0;
    }

    dst->env_ipc_recving = 0;
    dst->env_ipc_from = curenv->env_id;
    dst->env_ipc_value = value;
    dst->env_status = ENV_RUNNABLE;

    return 0;
}

/* Block until a value is ready.  Record that you want to receive
 * using the env_ipc_recving, env_ipc_maxsz and env_ipc_dstva fields of struct Env,
 * mark yourself not runnable, and then give up the CPU.
 *
 * If 'dstva' is < MAX_USER_ADDRESS, then you are willing to receive a page of data.
 * 'dstva' is the virtual address at which the sent page should be mapped.
 *
 * This function only returns on error, but the system call will eventually
 * return 0 on success.
 * Return < 0 on error.  Errors are:
 *  -E_INVAL if dstva < MAX_USER_ADDRESS but dstva is not page-aligned;
 *  -E_INVAL if dstva is valid and maxsize is 0,
 *  -E_INVAL if maxsize is not page aligned. */
static int
sys_ipc_recv(uintptr_t dstva, uintptr_t maxsize) {
    // LAB 9: Your code here
    if (maxsize & CLASS_MASK(0)) {
        return -E_INVAL;
    }

    if (dstva < MAX_USER_ADDRESS) {
        if (!maxsize || dstva & CLASS_MASK(0)) {
            return -E_INVAL;
        }

        curenv->env_ipc_dstva = dstva;
        curenv->env_ipc_maxsz = maxsize;
    }

    curenv->env_status = ENV_NOT_RUNNABLE;
    curenv->env_ipc_recving = 1;
    curenv->env_tf.tf_regs.reg_rax = 0;
    sched_yield();

    return 0;
}

/*
 * This function sets trapframe and is unsafe
 * so you need:
 *   -Check environment id to be valid and accessible
 *   -Check argument to be valid memory
 *   -Use nosan_memcpy to copy from usespace
 *   -Prevent privilege escalation by overriding segments
 *   -Only allow program to set safe flags in RFLAGS register
 *   -Force IF to be set in RFLAGS
 */
static int
sys_env_set_trapframe(envid_t envid, struct Trapframe *tf) {
    // LAB 11: Your code here
    struct Env *new = NULL;

    if (envid2env(envid, &new, 1) < 0) {
        return -E_BAD_ENV;
    }

    user_mem_assert(curenv, tf, sizeof(*tf), PROT_R | PROT_USER_);
    nosan_memcpy((void *)&new->env_tf, (void *)tf, sizeof(*tf));
    
    new->env_tf.tf_cs = GD_UT | 3;
    new->env_tf.tf_ds = GD_UD | 3;
    new->env_tf.tf_es = GD_UD | 3;
    new->env_tf.tf_ss = GD_UD | 3;

    new->env_tf.tf_rflags &= 0xFFF;
    new->env_tf.tf_rflags |= FL_IF;

    return 0;
}

/* Return date and time in UNIX timestamp format: seconds passed
 * from 1970-01-01 00:00:00 UTC. */
static int
sys_gettime(void) {
    // LAB 12: Your code here
    return gettime();
}

/*
 * This function return the difference between maximal
 * number of references of regions [addr, addr + size] and [addr2,addr2+size2]
 * if addr2 is less than MAX_USER_ADDRESS, or just
 * maximal number of references to [addr, addr + size]
 *
 * Use region_maxref() here.
 */
static int
sys_region_refs(uintptr_t addr, size_t size, uintptr_t addr2, uintptr_t size2) {
    // LAB 10: Your code here
    if (addr2 >= MAX_USER_ADDRESS) {
        return region_maxref(&curenv->address_space, addr, size);
    }

    return region_maxref(&curenv->address_space, addr, size) - region_maxref(&curenv->address_space, addr2, size2);
}

/* sigqueue system call: add sent signal to pid's queue, ignore it or 
 * destroy environment immediately.
 * 
 * Returns 0 on success, < 0 on error.
 * Errors are:
 *  -E_BAD_ENV if environment envid doesn't currently exist.
 *      (No need to check permissions.)
 *  -E_INVAL if there is no signal in JOS with such signo.
 */
static int
sys_sigqueue(pid_t pid, int sig, const union sigval value) {
    if (sig < SIGINT || sig > NSIGNALS) {
        return -E_INVAL;
    }

    struct Env *new = NULL;
    int err = 0;
    /* We probably want to send signals not only to our children but 
     * to some other envs */
    if (envid2env(pid, &new, 0)) {
        return -E_BAD_ENV;
    }

    /* Handle special signals first */
    if (sig == SIGKILL) {
        err = sys_env_destroy(pid);

        if (trace_signals) {
            cprintf("signals: sent signal SIGKILL from %x to %x\n", curenv->env_id, pid);
        }

        goto signal_sent;
    }

    if (sig == SIGSTOP) {
        new->env_sig_stopped = 1;
        struct Env *penv = NULL;

        if (envid2env(new->env_parent_id, &penv, false)) {
            return -E_BAD_ENV;
        }

        if (!(penv->env_sig_sa[SIGCHLD - 1].sa_flags & SA_NOCLDSTOP)) {
            sys_sigqueue(new->env_parent_id, SIGCHLD, (const union sigval)0);
        }

        if (trace_signals) {
            cprintf("signals: sent signal SIGSTOP from %x to %x\n", curenv->env_id, pid);
        }

        goto signal_sent;
    }

    if (sig == SIGCONT && new->env_sig_stopped) {
        new->env_sig_stopped = 0;
        struct Env *penv = NULL;

        if (envid2env(new->env_parent_id, &penv, false)) {
            return -E_BAD_ENV;
        }

        if (!(penv->env_sig_sa[SIGCHLD - 1].sa_flags & SA_NOCLDSTOP)) {
            sys_sigqueue(new->env_parent_id, SIGCHLD, (const union sigval)0);
        }

        if (trace_signals) {
            cprintf("signals: sent signal SIGCONT from %x to %x\n", curenv->env_id, pid);
        }

        goto signal_sent;
    }

    struct sigaction *sa = new->env_sig_sa + sig - 1;

    if (!new->env_pgfault_upcall) {
        /* Don't need to enqueue a signal with default handler, 
         * but we should do SIG_DFL or SIG_IGN anyway. */
        if (sa->sa_handler == SIG_DFL) {
            err = sys_env_destroy(pid);

            if (trace_signals) {
                cprintf("signals: sent SIG_DFL signal %d from %x to %x, upcall not set\n", sig, curenv->env_id, pid);
            }

            goto signal_sent;
        } else if (sa->sa_handler == SIG_IGN) {
            if (trace_signals) {
                cprintf("signals: sent SIG_IGN signal %d from %x to %x, upcall not set\n", sig, curenv->env_id, pid);
            }

            goto signal_sent;
        }
    }

    /* Find slot in circular queue */
    size_t new_end = (new->env_sig_queue_end + 1) % SIG_QUEUE_SIZE;
    
    if (new_end == new->env_sig_queue_start) {
        /* Queue is full, try again later */
        return -E_AGAIN;
    }

    /* Fill signal info (inplace) */ 
    struct QueuedSignal *qs = new->env_sig_queue + new->env_sig_queue_end;
    new->env_sig_queue_end = new_end;

    memcpy(&(qs->qs_act), sa, sizeof(struct sigaction));
    qs->qs_info.si_signo = sig;
    qs->qs_info.si_code = 0;
    qs->qs_info.si_pid = sys_getenvid();
    qs->qs_info.padding = 0xffffffff;
    qs->qs_info.si_addr = 0;
    qs->qs_info.si_value = value;

    if (sa->sa_flags & SA_RESETHAND) {
        sa->sa_handler = ((sig == SIGCHLD) || (sig == SIGUSR1) || (sig == SIGUSR2) || (sig == SIGCONT)) ? SIG_IGN : SIG_DFL;
        sa->sa_flags &= ~SA_SIGINFO;
    }

    if (trace_signals) {
        cprintf("signals: sent signal %d from %x to %x\n", sig, curenv->env_id, pid);
    }
    
signal_sent:
    return err;
}

/* sigwait system call: suspend environment execution untill specified in set
 * signals are caught, write caught signo in *sig.
 * 
 * Returns 0 on success, < 0 on error.
 * Errors are:
 *  -E_INVAL if mask is incorrect.
 */
static int
sys_sigwait(const sigset_t *set, int *sig) {
    user_mem_assert(curenv, set, sizeof(set), PROT_R | PROT_USER_);
    
    if (sig) {
        user_mem_assert(curenv, sig, sizeof(sig), PROT_R | PROT_W | PROT_USER_);
    }
    
    sigset_t tmp_set;
    nosan_memcpy(&tmp_set, (void *)set, sizeof(sigset_t));

    sigset_t all = ~0U;
    
    all &= ~SIGNAL_MASK(SIGSTOP);
    all &= ~SIGNAL_MASK(SIGCONT);
    all &= ~SIGNAL_MASK(SIGKILL);

    /* Non-catchable signals */
    if (tmp_set & ~all) {
        return -E_INVAL;
    }

    /* Mask isn't set */
    if (!(tmp_set & all)) {
        return -E_INVAL;
    }

    curenv->env_sig_awaiting = tmp_set;
    curenv->env_tf.tf_regs.reg_rax = 0;
    
    if (sig) {
        curenv->env_sig_caught_ptr = sig;
    }

    if (trace_signals) {
        cprintf("signals: env %x: will wait for signals 0x%x\n", curenv->env_id, tmp_set);
    }
    
    sched_yield();
    return 0;
}

/* sigaction system call: examine and change a signal action. 
 * If act is non-null, the new action for signal signum is installed from act. 
 * If oldact is non-null, the previous action is saved in oldact.
 * 
 * Returns 0 on success, < 0 on error.
 * Errors are:
 *  -E_INVAL if sig is incorrect;
 *  -E_INVAL if flags specified in act are incorrect.
 */
static int
sys_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    if (signum < SIGINT || signum > NSIGNALS) {
        return -E_INVAL;
    }

    /* It's not allowed to handle special signals */
    if (signum == SIGKILL || signum == SIGSTOP || signum == SIGCONT) {
        return -E_INVAL;
    }

    if (oldact) {
        user_mem_assert(curenv, oldact, sizeof(struct sigaction), PROT_R | PROT_W | PROT_USER_);
        nosan_memcpy((void *)oldact, (void *)&curenv->env_sig_sa[signum - 1], sizeof(struct sigaction));
        
        if (trace_signals) {
            cprintf("signals: env %x asked old sigaction of signal %d to oldact %p\n", curenv->env_id, signum, oldact);
        }
    }

    if (!act) {
        return 0;
    }

    struct sigaction sa_tmp;
    user_mem_assert(curenv, act, sizeof(struct sigaction), PROT_R | PROT_USER_);
    nosan_memcpy((void *)&sa_tmp, (void *)act, sizeof(struct sigaction));

    /* Incorrect flags check */
    if (sa_tmp.sa_flags & ~SA_ALL_FLAGS) {
        return -E_INVAL;
    }

    memcpy((void *)&curenv->env_sig_sa[signum - 1], (void *)&sa_tmp, sizeof(struct sigaction));

    if (trace_signals) {
        cprintf("signals: env %x changed sigaction of signal %d to act %p\n", curenv->env_id, signum, act);
    }

    return 0;
}

/* sigprocmask system call: 
 * 
 */
static int 
sys_sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    if (oldset) {
        user_mem_assert(curenv, oldset, sizeof(sigset_t), PROT_R | PROT_W | PROT_USER_);
        nosan_memcpy((void *)oldset, (void *)&curenv->env_sig_mask, sizeof(sigset_t));
    }

    if (!set) {
        return 0;
    }

    sigset_t tmp_set;
    user_mem_assert(curenv, set, sizeof(sigset_t), PROT_R | PROT_USER_);
    nosan_memcpy((void *)&tmp_set, (void *)set, sizeof(sigset_t));

    sigset_t allowed_mask = ~((sigset_t)(~0U) >> NSIGNALS << NSIGNALS);
    allowed_mask &= (~SIGNAL_MASK(SIGKILL) | ~SIGNAL_MASK(SIGSTOP) | ~SIGNAL_MASK(SIGCONT));
    tmp_set &= allowed_mask;

    sigset_t new_mask = curenv->env_sig_mask;

    switch (how) {
    case SIG_BLOCK:
        new_mask |= tmp_set;
        break;
    case SIG_UNBLOCK:
        new_mask &= ~tmp_set;
        break;
    case SIG_SETMASK:
        new_mask = tmp_set;
        break;
    default:
        return -E_INVAL;
    }

    if (trace_signals) {
        cprintf("signals: env %x changed mask from 0x%x to 0x%x\n", curenv->env_id, curenv->env_sig_mask, new_mask);
    }

    curenv->env_sig_mask = new_mask;
    return 0;
}

/* Dispatches to the correct kernel function, passing the arguments. */
uintptr_t
syscall(uintptr_t syscallno, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6) {
    /* Call the function corresponding to the 'syscallno' parameter.
     * Return any appropriate return value. */

    // LAB 8: Your code here
    // LAB 9: Your code here
    // LAB 10: Your code here
    // LAB 11: Your code here
    // LAB 12: Your code here
    // LAB 13: Your code here
    switch(syscallno) {
    case SYS_cputs:
        return sys_cputs((const char *)a1, (size_t)a2);
    case SYS_cgetc:
        return sys_cgetc();
    case SYS_getenvid:
        return sys_getenvid();
    case SYS_env_destroy:
        return sys_env_destroy((envid_t)a1);
    case SYS_alloc_region:
        return sys_alloc_region((envid_t)a1, (uintptr_t)a2, (size_t)a3, (int)a4);
    case SYS_map_region:
        return sys_map_region((envid_t)a1, (uintptr_t)a2, (envid_t)a3, (uintptr_t)a4, (size_t)a5, (int)a6);
    case SYS_map_physical_region:
        return sys_map_physical_region((uintptr_t)a1, (envid_t)a2, (uintptr_t)a3, (size_t)a4, (int)a5);
    case SYS_unmap_region:
        return sys_unmap_region((envid_t)a1, (uintptr_t)a2, (size_t)a3);
    case SYS_region_refs:
        return sys_region_refs((uintptr_t)a1, (size_t)a2, (uintptr_t)a3, (uintptr_t)a4);
    case SYS_exofork:
        return sys_exofork();
    case SYS_env_set_status:
        return sys_env_set_status((envid_t)a1, (int)a2);
    case SYS_env_set_trapframe:
        return sys_env_set_trapframe((envid_t)a1, (struct Trapframe *)a2);
    case SYS_env_set_pgfault_upcall:
        return sys_env_set_pgfault_upcall((envid_t)a1, (void*)a2);
    case SYS_yield:
        sys_yield();
        return 0;
    case SYS_ipc_try_send:
        return sys_ipc_try_send((envid_t)a1, (uint32_t)a2, (uintptr_t)a3, (size_t)a4, (int)a5);
    case SYS_ipc_recv:
        return sys_ipc_recv((uintptr_t)a1, (uintptr_t)a2);
    case SYS_gettime:
        return sys_gettime();
    case SYS_sigqueue:
        return sys_sigqueue((pid_t)a1, (int)a2, (const union sigval)(void *)a3);
    case SYS_sigwait:
        return sys_sigwait((const sigset_t *)a1, (int *)a2);
    case SYS_sigaction:
        return sys_sigaction((int)a1, (const struct sigaction *)a2, (struct sigaction *)a3);
    case SYS_sigprocmask:
        return sys_sigprocmask((int)a1, (const sigset_t *)a2, (sigset_t *)a3);
    default:
        return -E_NO_SYS;
    }
}
