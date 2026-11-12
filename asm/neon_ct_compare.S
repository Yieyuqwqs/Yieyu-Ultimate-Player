// [12] ARM64 NEON：常数时间比对（微秒级硬指令）。
// 用途：卡密/授权令牌的等长比对。要求：
//   ① 不许提前 return（时间必须与"第几位不同"无关）
//   ② 不许依赖编译器重排（纯汇编，只用寄存器）
//   ③ 长度不等直接返回 0，且不等长本身也不泄露前缀信息
//
// uint64_t yieyu_ct_equal(const uint8_t* a, const uint8_t* b, uint64_t n)
//   x0=a, x1=b, x2=n  →  x0 = 1(equal) / 0(not)

    .text
    .arch armv8-a
    .globl yieyu_ct_equal
    .type  yieyu_ct_equal, %function
    .balign 64
yieyu_ct_equal:
    mov     x3, #0                  // 累积差异
    mov     x4, #0                  // 偏移
    cbz     x2, .Leq_true           // n==0：视为相等（调用方自行判空）
.Lloop32:
    cmp     x4, x2
    b.hs    .Ltail
    sub     x5, x2, x4
    cmp     x5, #32
    b.lo    .Ltail
    add     x6, x0, x4              // LDP 只吃立即数偏移/前后索引，不能写 [x0,x4]
    add     x7, x1, x4
    ldp     q0, q1, [x6]
    ldp     q2, q3, [x7]
    eor     v0.16b, v0.16b, v2.16b
    eor     v1.16b, v1.16b, v3.16b
    orr     v0.16b, v0.16b, v1.16b
    uaddlv  h0, v0.16b              // 256 位差异折叠成标量
    umov    w5, v0.h[0]
    orr     x3, x3, x5
    add     x4, x4, #32
    b       .Lloop32
.Ltail:
    cmp     x4, x2
    b.hs    .Ldone
    ldrb    w5, [x0, x4]
    ldrb    w6, [x1, x4]
    eor     w5, w5, w6
    orr     x3, x3, x5
    add     x4, x4, #1
    b       .Ltail
.Ldone:
    cmp     x3, #0
    cset    x0, eq
    ret
.Leq_true:
    mov     x0, #1
    ret
    .size yieyu_ct_equal, .-yieyu_ct_equal
    .section .note.GNU-stack,"",%progbits
