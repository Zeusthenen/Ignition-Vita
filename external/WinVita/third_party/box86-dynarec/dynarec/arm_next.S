//arm update linker table for dynarec
//called with pointer to emu as 1st parameter
//and address of table to as 2nd parameter
//ip is at r12

.text
.align 4

.extern LinkNext

.global arm_next
    .4byte  0   // NULL pointer before arm64_next, for getDB
arm_next:
    // emu is r0, IP address is r1
    push    {r0, r1, r12, r14}
    // DIAG reg-sync (live regs for LinkNext/eiptrap dumps) only when a diag
    // knob armed it at boot (dyn86_diag_regs <- D2_EIPTRAP/D2_XFERTRACE).
    // Nominal cost: 3 insns instead of an unconditional 10-word STM + 40
    // bytes of D$ traffic on every arm_next transition. r2/r3 are scratch,
    // r4-r12/r14 are untouched by the push above, r0 still holds emu.
    ldr     r2, =dyn86_diag_regs
    ldr     r2, [r2]
    cmp     r2, #0
    stmne   r0, {r4-r12,r14}
    // call the function
    bl      LinkNext
    // return offset is jump address
    mov     r3, r0
    pop     {r0, r1, r12, r14}
    bx      r3
.ltorg

