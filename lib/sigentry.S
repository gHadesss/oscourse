#include <inc/mmu.h>
#include <inc/memlayout.h>
#include <inc/trap.h>
#include <kern/macro.h>

# Signal upcall entrypoint.

#  (end of user stack)
#  8 bytes of alignment
#  utf_rsp
#  utf_rflags
#  utf_rip
#  utf_regs.reg_rax
#  ...
#  utf_regs.reg_r15
#  utf_err (signo probably? maybe useless)
#  utf_fault_va (always zero, trap handling legacy) 
#  old sigmask (4 bytes + 4 bytes alignment)
#  QueuedSignal (48 bytes) <-- %rsp

.text
.globl _sighdlr_upcall
_sighdlr_upcall:
    # Call the C signal handler.
    movq  %rsp, %rdi # passing the function argument in rdi
    addq  $56, %rdi
    movq  %rsp, %rsi # second argument in rsi is QueuedSignal *qs

    movabs $_handle_signal, %rax
    call  *%rax

    # Now the C signal handler has returned and you must return
    # to the trap time state.
    # Push trap-time %eip onto the trap-time stack.

    addq  $48, %rsp # ignore QueuedSignal
    movq  $2, %rdi  # store SIG_SETMASK macro in rdi
    leaq  4(%rsp), %rsi # store &env_sig_mask in rsi
    xor   %rdx, %rdx # rdx is NULL

    movabs $sys_sigprocmask, %rax
    call  *%rax # so that we can receive signals again
    addq  $8, %rsp

    # LAB 13: Your code here
    addq $16, %rsp

    # Restore the trap-time registers.  After you do this, you
    # can no longer modify any general-purpose registers (use POPA macro).
    # LAB 13: Your code here
    POPA

    pushq %rdx
    pushq %rax

    # 32 utf_rsp     ->   -----
    # 24 utf_rflags  -> utf_rsp
    # 16 utf_rip     -> utf_rflags
    #  8 rdx         ->    ---
    #  0 rax         ->    ---

    movq 32(%rsp), %rax
    subq $8, %rax
    movq 16(%rsp), %rdx
    movq %rdx, (%rax)

    movq 24(%rsp), %rdx
    movq %rdx, 16(%rsp)
    movq %rax, 24(%rsp)

    popq %rax
    popq %rdx

    # Restore rflags from the stack.  After you do this, you can
    # no longer use arithmetic operations or anything else that
    # modifies rflags.
    # LAB 13: Your code here
    popfq

    # Switch back to the adjusted trap-time stack.
    # LAB 13: Your code here
    popq %rsp

    # Return to execute interrupted user code.
    ret