    .syntax unified
    .arm
    .equ UEP_WORD_VA, 0xc094810c
    .equ ANCHOR1_VA, 0xc028a6f4
    .equ ANCHOR2_VA, 0xc028aadc

    .section .init.text,"ax",%progbits
    .global init_module
    .type init_module,%function
init_module:
    push {r4-r8, lr}
    ldr ip, =ANCHOR1_VA
    ldm ip, {r1,r2,r3,r4}
    ldr r0, =exp1
    ldm r0, {r5,r6,r7,r8}
    cmp r1, r5
    cmpeq r2, r6
    cmpeq r3, r7
    cmpeq r4, r8
    bne fail
    ldr ip, =ANCHOR2_VA
    ldm ip, {r1,r2,r3,r4}
    ldr r0, =exp2
    ldm r0, {r5,r6,r7,r8}
    cmp r1, r5
    cmpeq r2, r6
    cmpeq r3, r7
    cmpeq r4, r8
    bne fail
    ldr ip, =UEP_WORD_VA
    ldr r1, [ip]           @ old value
    mov r2, #0
    str r2, [ip]           @ relax enforcement
    ldr r3, [ip]           @ readback
    ldr r0, =fmt_done
    bl printk
    b done
fail:
    ldr r0, =fmt_fail
    bl printk
done:
    mvn r0, #18
    pop {r4-r8, pc}

    .section .exit.text,"ax",%progbits
    .global cleanup_module
cleanup_module:
    bx lr

    .section .rodata,"a",%progbits
fmt_done:
    .asciz "uepclear6: cleared old=%08lx now=%08lx\n"
fmt_fail:
    .asciz "uepclear6: ABORT anchor mismatch, no write\n"
.align 2
exp1:
    .word 0xe3083108, 0xe34c3094, 0xe3a02000, 0xe593a004
exp2:
    .word 0xe3083108, 0xe34c3094, 0xe55b7072, 0xe5933004
