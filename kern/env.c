/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/elf.h>
#include <inc/vsyscall.h>
#include <inc/signal.h>

#include <kern/env.h>
#include <kern/kdebug.h>
#include <kern/macro.h>
#include <kern/monitor.h>
#include <kern/pmap.h>
#include <kern/pmap.h>
#include <kern/sched.h>
#include <kern/timer.h>
#include <kern/traceopt.h>
#include <kern/trap.h>
#include <kern/vsyscall.h>
#include <kern/syscall.h>

/* Currently active environment */
struct Env *curenv = NULL;

#ifdef CONFIG_KSPACE
/* All environments */
struct Env env_array[NENV];
struct Env *envs = env_array;
#else
/* All environments */
struct Env *envs = NULL;
#endif

/* Virtual syscall page address */
volatile int *vsys;

/* Free environment list
 * (linked by Env->env_link) */
static struct Env *env_free_list;


/* NOTE: Should be at least LOGNENV */
#define ENVGENSHIFT 12

/* Converts an envid to an env pointer.
 * If checkperm is set, the specified environment must be either the
 * current environment or an immediate child of the current environment.
 *
 * RETURNS
 *     0 on success, -E_BAD_ENV on error.
 *   On success, sets *env_store to the environment.
 *   On error, sets *env_store to NULL. */
int
envid2env(envid_t envid, struct Env **env_store, bool need_check_perm) {
    struct Env *env;

    /* If envid is zero, return the current environment. */
    if (!envid) {
        *env_store = curenv;
        return 0;
    }

    /* Look up the Env structure via the index part of the envid,
     * then check the env_id field in that struct Env
     * to ensure that the envid is not stale
     * (i.e., does not refer to a _previous_ environment
     * that used the same slot in the envs[] array). */
    env = &envs[ENVX(envid)];
    if (env->env_status == ENV_FREE || env->env_id != envid) {
        *env_store = NULL;
        return -E_BAD_ENV;
    }

    /* Check that the calling environment has legitimate permission
     * to manipulate the specified environment.
     * If checkperm is set, the specified environment
     * must be either the current environment
     * or an immediate child of the current environment. */
    if (need_check_perm && env != curenv && env->env_parent_id != curenv->env_id) {
        *env_store = NULL;
        return -E_BAD_ENV;
    }

    *env_store = env;
    return 0;
}

/* Mark all environments in 'envs' as free, set their env_ids to 0,
 * and insert them into the env_free_list.
 * Make sure the environments are in the free list in the same order
 * they are in the envs array (i.e., so that the first call to
 * env_alloc() returns envs[0]).
 */
void
env_init(void) {
    /* Allocate vsys array with kzalloc_region().
     * Don't forget about rounding.
     * kzalloc_region only works with current_space != NULL */
    // LAB 12: Your code here
    vsys = kzalloc_region(UVSYS_SIZE);
    memset((void *)vsys, 0, UVSYS_SIZE);
    /* Allocate envs array with kzalloc_region().
     * Don't forget about rounding.
     * kzalloc_region() only works with current_space != NULL */
    // LAB 8: Your code here
    envs = (struct Env *)kzalloc_region(NENV * sizeof(struct Env));
    memset(envs, 0, ROUNDUP(sizeof(struct Env) * NENV, PAGE_SIZE));

    /* Map envs to UENVS read-only,
     * but user-accessible (with PROT_USER_ set) */
    // LAB 8: Your code here
    map_region(current_space, UENVS, &kspace, (uintptr_t)envs, UENVS_SIZE, PROT_USER_ | PROT_R);
    map_region(current_space, UVSYS, &kspace, (uintptr_t)vsys, UVSYS_SIZE, PROT_USER_ | PROT_R);

    /* Set up envs array */

    // LAB 3: Your code here
    env_free_list = envs;

    for (int i = 0; i < NENV; i++) {
        envs[i].env_link = ((i == NENV - 1) ? NULL : &envs[i + 1]);
        envs[i].env_id = 0;
        envs[i].env_status = ENV_FREE;
    }
}

/* Allocates and initializes a new environment.
 * On success, the new environment is stored in *newenv_store.
 *
 * Returns
 *     0 on success, < 0 on failure.
 * Errors
 *    -E_NO_FREE_ENV if all NENVS environments are allocated
 *    -E_NO_MEM on memory exhaustion
 */
int
env_alloc(struct Env **newenv_store, envid_t parent_id, enum EnvType type) {

    struct Env *env;
    if (!(env = env_free_list))
        return -E_NO_FREE_ENV;

    /* Allocate and set up the page directory for this environment. */
    int res = init_address_space(&env->address_space);
    if (res < 0) return res;

    /* Generate an env_id for this environment */
    int32_t generation = (env->env_id + (1 << ENVGENSHIFT)) & ~(NENV - 1);
    /* Don't create a negative env_id */
    if (generation <= 0) generation = 1 << ENVGENSHIFT;
    env->env_id = generation | (env - envs);

    /* Set the basic status variables */
    env->env_parent_id = parent_id;
#ifdef CONFIG_KSPACE
    env->env_type = ENV_TYPE_KERNEL;
#else
    env->env_type = type;
#endif
    env->env_status = ENV_RUNNABLE;
    env->env_runs = 0;

    /* Clear out all the saved register state,
     * to prevent the register values
     * of a prior environment inhabiting this Env structure
     * from "leaking" into our new environment */
    memset(&env->env_tf, 0, sizeof(env->env_tf));

    /* Set up appropriate initial values for the segment registers.
     * GD_UD is the user data (KD - kernel data) segment selector in the GDT, and
     * GD_UT is the user text (KT - kernel text) segment selector (see inc/memlayout.h).
     * The low 2 bits of each segment register contains the
     * Requestor Privilege Level (RPL); 3 means user mode, 0 - kernel mode.  When
     * we switch privilege levels, the hardware does various
     * checks involving the RPL and the Descriptor Privilege Level
     * (DPL) stored in the descriptors themselves */

#ifdef CONFIG_KSPACE
    env->env_tf.tf_ds = GD_KD;
    env->env_tf.tf_es = GD_KD;
    env->env_tf.tf_ss = GD_KD;
    env->env_tf.tf_cs = GD_KT;

    // LAB 3: Your code here:
    static uintptr_t stack_top = 0x2000000;
    env->env_tf.tf_rsp = stack_top - PAGE_SIZE * 2 * (env - envs);
#else
    env->env_tf.tf_ds = GD_UD | 3;
    env->env_tf.tf_es = GD_UD | 3;
    env->env_tf.tf_ss = GD_UD | 3;
    env->env_tf.tf_cs = GD_UT | 3;
    env->env_tf.tf_rsp = USER_STACK_TOP;
#endif

    /* For now init trapframe with IF set */
    env->env_tf.tf_rflags = FL_IF | (type == ENV_TYPE_FS ? FL_IOPL_3 : FL_IOPL_0);

    /* Clear the page fault handler until user installs one. */
    env->env_pgfault_upcall = 0;

    /* Also clear the IPC receiving flag. */
    env->env_ipc_recving = 0;

    /* Clear signal related fields in Env structure */
    /* LAB 13: Your code here: */
    memset(&env->env_sig_queue, 0, sizeof(env->env_sig_queue));

    env->env_sig_queue_start = 0;
    env->env_sig_queue_end = 0;

    env->env_sig_stopped = 0;
    env->env_sig_mask = 0;
    env->env_sig_awaiting = 0;
    env->env_sig_caught_ptr = NULL;

    /* Initialize default signal handlers in new Env */
    /* LAB 13: Your code here: */
    if (!parent_id) {
        memset(&env->env_sig_sa, 0, sizeof(env->env_sig_sa));

        env->env_sig_sa[SIGUSR1 - 1].sa_handler = SIG_IGN;
        env->env_sig_sa[SIGUSR2 - 1].sa_handler = SIG_IGN;
        env->env_sig_sa[SIGCHLD - 1].sa_handler = SIG_IGN;
        env->env_sig_sa[SIGCONT - 1].sa_handler = SIG_IGN;
    } else {
        struct Env *new = NULL;
        envid2env(parent_id, &new, 0);
        memcpy(&env->env_sig_sa, &new->env_sig_sa, sizeof(env->env_sig_sa));
        env->env_pgfault_upcall = new->env_pgfault_upcall;
    }
    
    /* Commit the allocation */
    env_free_list = env->env_link;
    *newenv_store = env;

    if (trace_envs) cprintf("[%08x] new env %08x\n", curenv ? curenv->env_id : 0, env->env_id);
    return 0;
}

/* Pass the original ELF image to binary/size and bind all the symbols within
 * its loaded address space specified by image_start/image_end.
 * Make sure you understand why you need to check that each binding
 * must be performed within the image_start/image_end range.
 */
static int
bind_functions(struct Env *env, uint8_t *binary, size_t size, uintptr_t image_start, uintptr_t image_end) {
    // LAB 3: Your code here:
    /* NOTE: find_function from kdebug.c should be used */
    struct Elf *elf_image = (struct Elf *)binary;
    struct Secthdr *sh = (struct Secthdr *)(binary + elf_image->e_shoff);
    char *sh_str = (char *)(binary + sh[elf_image->e_shstrndx].sh_offset);

    uint16_t symtndx = UINT16_MAX;
    uint16_t strtndx = UINT16_MAX;

    for (uint16_t i = 0; i < elf_image->e_shnum; i++) {
        if (sh[i].sh_type == ELF_SHT_SYMTAB) {
            symtndx = i;
        }

        if (sh[i].sh_type == ELF_SHT_STRTAB && !strncmp(&sh_str[sh[i].sh_name], ".strtab", 7)) {
            strtndx = i;
        }
    }

    if (strtndx == (uint16_t)-1 || strncmp(&sh_str[sh[strtndx].sh_name], ".strtab", 7)) {
        panic("bind_functions: can't find strt\n");
    }

    if (symtndx == (uint16_t)-1 || strncmp(&sh_str[sh[symtndx].sh_name], ".symtab", 7)) {
        panic("bind_functions: can't find symt\n");
    }

    struct Elf64_Sym *symt = (struct Elf64_Sym *)(binary + sh[symtndx].sh_offset);
    uintptr_t addr = 0;

    for (size_t i = 0; i < sh[symtndx].sh_size / sizeof(struct Elf64_Sym *); i++) {
        if (ELF64_ST_BIND(symt[i].st_info) == STB_GLOBAL && ELF64_ST_TYPE(symt[i].st_info) == STT_OBJECT) {
            addr = find_function((char *)(binary + sh[strtndx].sh_offset + symt[i].st_name));

            if (addr) {
                // uintptr_t *func_addr = (uintptr_t *)symt[i].st_value; causes UB because of unaligned 
                // *func_addr = addr;                                    memory access
                if ((uintptr_t)symt[i].st_value > image_end || (uintptr_t)symt[i].st_value < image_start) {
                    panic("bind_functions: symbol is out of binary address range\n");
                }

                uintptr_t *func_addr = (uintptr_t *)symt[i].st_value;
                memcpy((void *)func_addr, (void *)&addr, sizeof(addr));
            }
        }
    }

    return 0;
}

/* Set up the initial program binary, stack, and processor flags
 * for a user process.
 * This function is ONLY called during kernel initialization,
 * before running the first environment.
 *
 * This function loads all loadable segments from the ELF binary image
 * into the environment's user memory, starting at the appropriate
 * virtual addresses indicated in the ELF program header.
 * At the same time it clears to zero any portions of these segments
 * that are marked in the program header as being mapped
 * but not actually present in the ELF file - i.e., the program's bss section.
 *
 * All this is very similar to what our boot loader does, except the boot
 * loader also needs to read the code from disk.  Take a look at
 * LoaderPkg/Loader/Bootloader.c to get ideas.
 *
 * Finally, this function maps one page for the program's initial stack.
 *
 * load_icode returns -E_INVALID_EXE if it encounters problems.
 *  - How might load_icode fail?  What might be wrong with the given input?
 *
 * Hints:
 *   Load each program segment into memory
 *   at the address specified in the ELF section header.
 *   You should only load segments with ph->p_type == ELF_PROG_LOAD.
 *   Each segment's address can be found in ph->p_va
 *   and its size in memory can be found in ph->p_memsz.
 *   The ph->p_filesz bytes from the ELF binary, starting at
 *   'binary + ph->p_offset', should be copied to address
 *   ph->p_va.  Any remaining memory bytes should be cleared to zero.
 *   (The ELF header should have ph->p_filesz <= ph->p_memsz.)
 *
 *   All page protection bits should be user read/write for now.
 *   ELF segments are not necessarily page-aligned, but you can
 *   assume for this function that no two segments will touch
 *   the same page.
 *
 *   You must also do something with the program's entry point,
 *   to make sure that the environment starts executing there.
 *   What?  (See env_run() and env_pop_tf() below.) */
static int
load_icode(struct Env *env, uint8_t *binary, size_t size) {
    // LAB 3: Your code here
    // LAB 8: Your code here
    struct Elf *elf_image = (struct Elf *)binary;

    if (elf_image->e_magic != ELF_MAGIC) {
        cprintf("load_icode: file has magic %08X instead of %08X\n", elf_image->e_magic, ELF_MAGIC);
        return -E_INVALID_EXE;
    }

    if (elf_image->e_shentsize != sizeof(struct Secthdr)) {
        cprintf("load_icode: file has sections of %u bytes instead of %u\n", elf_image->e_shentsize,
            (uint32_t) sizeof(struct Secthdr));
        return -E_INVALID_EXE;
    }

    if (elf_image->e_shstrndx >= elf_image->e_shnum) {
        cprintf("load_icode: file string section has invalid index %u out of %u entries\n", elf_image->e_shstrndx, 
            elf_image->e_shnum);
        return -E_INVALID_EXE;
    }

    if (elf_image->e_phentsize != sizeof(struct Proghdr)) {
        cprintf("load_icode: file has program headers of %u bytes instead of %u\n", elf_image->e_phentsize, 
            (uint32_t) sizeof(struct Proghdr));
        return -E_INVALID_EXE;
    }

    switch_address_space(&env->address_space);
    struct Proghdr *phs = (struct Proghdr *)((uint64_t)binary + elf_image->e_phoff);
    uintptr_t image_start = UINTPTR_MAX, image_end = 0;

    for (uint16_t i = 0; i < elf_image->e_phnum; i++) {
        if (phs[i].p_type != ELF_PROG_LOAD) {
            continue;
        }

        if (phs[i].p_filesz > phs[i].p_memsz) {
            cprintf("load_icode: section %u has %lu filesz with %lu memsz\n", i, phs[i].p_filesz, phs[i].p_memsz);
            switch_address_space(&kspace);
            return -E_INVALID_EXE;
        }

        uintptr_t rounded_addr = ROUNDDOWN(phs[i].p_va, PAGE_SIZE);
        size_t rounded_size = ROUNDUP(phs[i].p_memsz, PAGE_SIZE);

        // memset((void *)(phs[i].p_va + phs[i].p_filesz), 0, (size_t)(phs[i].p_memsz - phs[i].p_filesz));

        if (map_region(current_space, rounded_addr, NULL, 0, rounded_size, PROT_RWX | PROT_USER_ | ALLOC_ZERO)) {
            cprintf("load_icode: failed to map region [%lx, %lx]\n", rounded_addr, rounded_addr + rounded_size - 1);
            switch_address_space(&kspace);
            return -E_INVALID_EXE;
        }

        memcpy((void *)phs[i].p_va, (void *)((uint64_t)binary + phs[i].p_offset), (size_t)phs[i].p_filesz);

        if (image_start > (uintptr_t)phs[i].p_va) {
            image_start = (uintptr_t)phs[i].p_va;
        }

        if (image_end < (uintptr_t)(phs[i].p_va + phs[i].p_filesz)) {
            image_end = (uintptr_t)(phs[i].p_va + phs[i].p_filesz);
        }
    }

    uintptr_t stack_addr = (uintptr_t)(USER_STACK_TOP - USER_STACK_SIZE);
    if (map_region(&env->address_space, stack_addr, NULL, 0, USER_STACK_SIZE, PROT_R | PROT_W | PROT_USER_ | ALLOC_ZERO)) {
        cprintf("load_icode: failed to map user stack\n");
        switch_address_space(&kspace);
        return -E_INVALID_EXE;
    }

    switch_address_space(&kspace);
    env->env_tf.tf_rip = elf_image->e_entry;
#ifdef CONFIG_KSPACE
    if (bind_functions(env, binary, size, image_start, image_end)) {
        panic("load_icode: bind_functions has failed\n");
    }
#endif

    /* NOTE: When merging origin/lab10 put this hunk at the end
     *       of the function, when user stack is already mapped. */
    if (env->env_type == ENV_TYPE_FS) {
        /* If we are about to start filesystem server we need to pass
         * information about PCIe MMIO region to it. */
        struct AddressSpace *as = switch_address_space(&env->address_space);
        env->env_tf.tf_rsp = make_fs_args((char *)env->env_tf.tf_rsp);
        switch_address_space(as);
    }

    return 0;
}

/* Allocates a new env with env_alloc, loads the named elf
 * binary into it with load_icode, and sets its env_type.
 * This function is ONLY called during kernel initialization,
 * before running the first user-mode environment.
 * The new env's parent ID is set to 0.
 */
void
env_create(uint8_t *binary, size_t size, enum EnvType type) {
    // LAB 3: Your code here
    // LAB 8: Your code here
    // LAB 10: Your code here
    struct Env *newenv = NULL;
    int res = env_alloc(&newenv, 0, type);
    
    if (res) {
        panic("env_alloc: can't allocate env, %i", res);
    }

    res = load_icode(newenv, binary, size);

    if (res) {
        panic("env_alloc: can't load binary, %i", res);
    }

    newenv->binary = binary;
    newenv->env_type = type;

    if (type == ENV_TYPE_FS) {
        newenv->env_tf.tf_rflags |= FL_IOPL_3;
    } else {
        newenv->env_tf.tf_rflags &= ~FL_IOPL_MASK;
    }
}


/* Frees env and all memory it uses */
void
env_free(struct Env *env) {

    /* Note the environment's demise. */
    if (trace_envs) cprintf("[%08x] free env %08x\n", curenv ? curenv->env_id : 0, env->env_id);

#ifndef CONFIG_KSPACE
    /* If freeing the current environment, switch to kern_pgdir
     * before freeing the page directory, just in case the page
     * gets reused. */
    if (&env->address_space == current_space)
        switch_address_space(&kspace);

    static_assert(MAX_USER_ADDRESS % HUGE_PAGE_SIZE == 0, "Misaligned MAX_USER_ADDRESS");
    release_address_space(&env->address_space);
#endif

    /* Return the environment to the free list */
    env->env_status = ENV_FREE;
    env->env_link = env_free_list;
    env_free_list = env;
}

/* Frees environment env
 *
 * If env was the current one, then runs a new environment
 * (and does not return to the caller)
 */
void
env_destroy(struct Env *env) {
    /* If env is currently running on other CPUs, we change its state to
     * ENV_DYING. A zombie environment will be freed the next time
     * it traps to the kernel. */

    // LAB 3: Your code here
    // LAB 10: Your code here
    // LAB 13: Your code here
    if (env->env_parent_id) {
        syscall(SYS_sigqueue, env->env_parent_id, SIGCHLD, 0, 0, 0, 0);
    }

    env->env_status = ENV_DYING;
    env_free(env);

    if (env == curenv) {
        sched_yield();
    }

    /* Reset in_page_fault flags in case *current* environment
     * is getting destroyed after performing invalid memory access. */
    // LAB 8: Your code here
    in_page_fault = 0;
}

#ifdef CONFIG_KSPACE
void
csys_exit(void) {
    if (!curenv) panic("curenv = NULL");
    env_destroy(curenv);
}

void
csys_yield(struct Trapframe *tf) {
    memcpy(&curenv->env_tf, tf, sizeof(struct Trapframe));
    sched_yield();
}
#endif

/* Restores the register values in the Trapframe with the 'ret' instruction.
 * This exits the kernel and starts executing some environment's code.
 *
 * This function does not return.
 */

_Noreturn void
env_pop_tf(struct Trapframe *tf) {
    asm volatile(
            "movq %0, %%rsp\n"
            "movq 0(%%rsp), %%r15\n"
            "movq 8(%%rsp), %%r14\n"
            "movq 16(%%rsp), %%r13\n"
            "movq 24(%%rsp), %%r12\n"
            "movq 32(%%rsp), %%r11\n"
            "movq 40(%%rsp), %%r10\n"
            "movq 48(%%rsp), %%r9\n"
            "movq 56(%%rsp), %%r8\n"
            "movq 64(%%rsp), %%rsi\n"
            "movq 72(%%rsp), %%rdi\n"
            "movq 80(%%rsp), %%rbp\n"
            "movq 88(%%rsp), %%rdx\n"
            "movq 96(%%rsp), %%rcx\n"
            "movq 104(%%rsp), %%rbx\n"
            "movq 112(%%rsp), %%rax\n"
            "movw 120(%%rsp), %%es\n"
            "movw 128(%%rsp), %%ds\n"
            "addq $152,%%rsp\n" /* skip tf_trapno and tf_errcode */
            "iretq" ::"g"(tf)
            : "memory");

    /* Mostly to placate the compiler */
    panic("Reached unrecheble\n");
}

/* This function returns -1 if passed environment doesn't have pending signals
 * to handle, and index in env_sig_queue array if we have signal to handle. */
int env_check4pending_sigs(struct Env *env) {
    /* The first case: queue is empty. */
    if (env->env_sig_queue_start == env->env_sig_queue_end) {
        return -1;
    }

    int sig_idx = env->env_sig_queue_start;

    /* The second case: there are probably blocked signals in queue, so we need to check it 
     * for non-blocked signals presence in it and return index of the first non-blocked. */
    while (sig_idx != env->env_sig_queue_end) {
        struct QueuedSignal *qs = &env->env_sig_queue[sig_idx];
        
        if (env->env_sig_mask & SIGNAL_MASK(qs->qs_info.si_signo)) {
            sig_idx = (sig_idx + 1) % SIG_QUEUE_SIZE;
        } else {
            env->env_sig_queue_start = (env->env_sig_queue_start + 1) % SIG_QUEUE_SIZE;
            return sig_idx;
        }
    }

    return -1;
}

/* Context switch from curenv to env.
 * This function does not return.
 *
 * Step 1: If this is a context switch (a new environment is running):
 *       1. Set the current environment (if any) back to
 *          ENV_RUNNABLE if it is ENV_RUNNING (think about
 *          what other states it can be in),
 *       2. Set 'curenv' to the new environment,
 *       3. Set its status to ENV_RUNNING,
 *       4. Update its 'env_runs' counter,
 * Step 2: Use env_pop_tf() to restore the environment's
 *       registers and starting execution of process.

 * Hints:
 *    If this is the first call to env_run, curenv is NULL.
 *
 *    This function loads the new environment's state from
 *    env->env_tf.  Go back through the code you wrote above
 *    and make sure you have set the relevant parts of
 *    env->env_tf to sensible values.
 */
_Noreturn void
env_run(struct Env *env) {
    assert(env);

    if (trace_envs_more) {
        const char *state[] = {"FREE", "DYING", "RUNNABLE", "RUNNING", "NOT_RUNNABLE"};
        if (curenv) cprintf("[%08X] env stopped: %s\n", curenv->env_id, state[curenv->env_status]);
        cprintf("[%08X] env started: %s\n", env->env_id, state[env->env_status]);
    }

    // LAB 3: Your code here
    // LAB 8: Your code here
    if (curenv) {
        if (curenv->env_status == ENV_RUNNING) {
            curenv->env_status = ENV_RUNNABLE;
        }
    }

    curenv = env;
    curenv->env_status = ENV_RUNNING;
    curenv->env_runs++;

    int possible_signo = env_check4pending_sigs(curenv);

    if (possible_signo != -1) {
        signal_handler(&curenv->env_tf, &curenv->env_sig_queue[possible_signo]); // no return
    }

    switch_address_space(&curenv->address_space);
    env_pop_tf(&curenv->env_tf);

    panic("Reached unrecheable\n");
}
