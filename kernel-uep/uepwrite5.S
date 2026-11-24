    .syntax unified
    .arm
    .equ UEP_WORD_VA, 0xc095e4d4
    .equ ANCHOR1_VA, 0xc029ca04
    .equ ANCHOR2_VA, 0xc029cca4

    .section .init.text,"ax",%progbits
    .global init_module
    .type init_module,%function
init_module:
    push {r4-r8, lr}

    @ validate anchor1 (full 16 bytes) before touching anything else
    ldr ip, =ANCHOR1_VA
    ldm ip, {r1,r2,r3,r4}
    ldr r0, =exp1
    ldm r0, {r5,r6,r7,r8}
    cmp r1, r5
    cmpeq r2, r6
    cmpeq r3, r7
    cmpeq r4, r8
    bne fail

    @ validate anchor2 (first 16 of 36 bytes)
    ldr ip, =ANCHOR2_VA
    ldm ip, {r1,r2,r3,r4}
    ldr r0, =exp2
    ldm r0, {r5,r6,r7,r8}
    cmp r1, r5
    cmpeq r2, r6
    cmpeq r3, r7
    cmpeq r4, r8
    bne fail

    @ anchors verified for this mapping: change the word in RAM only
    ldr ip, =UEP_WORD_VA
    ldr r1, [ip]           @ old value
    mov r2, #NEWVALUE
    str r2, [ip]
    ldr r3, [ip]           @ readback
    ldr r0, =fmt_done
    bl printk
    b done
fail:
    ldr r0, =fmt_fail
    bl printk
done:
    mvn r0, #18            @ -ENODEV: never stays resident
    pop {r4-r8, pc}

    .section .exit.text,"ax",%progbits
    .global cleanup_module
    .type cleanup_module,%function
cleanup_module:
    bx lr

    .section .rodata,"a",%progbits
fmt_done:
    .asciz "codex-uep5: changed old=%08lx now=%08lx\n"
fmt_fail:
    .asciz "codex-uep5: ABORT anchor mismatch, no write\n"
.align 2
exp1:
    .word 0xe30e34d0, 0xe34c3095, 0xe3a02000, 0xe593a004
exp2:
    .word 0xe30e34d0, 0xe3580000, 0xe34c3095, 0xe593a004
