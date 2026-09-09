/* lmw/stmw regression: a successful TLB miss precedes a cross-page DSI.
 * The handler records SRR0 and repairs it before rfi so a broken JIT
 * reports a failed check instead of jumping into unrelated memory.
 */
    .text
    .globl _start
    .set check_id, 0

.macro CHECK reg, value
    .set check_id, check_id + 1
    cmpwi   \reg, \value
    beq     1f
    li      3, check_id
    .long   0x00333304
1:
.endm

.macro EXPECT_FAULT label
    li      0, 0
    stw     0, 0xf00(0)
    bl      2f
2:
    mflr    3
    addi    3, 3, (\label - 2b)
    stw     3, 0xf14(0)
.endm

.macro CHECK_FAULT flags
    lwz     3, 0xf00(0)
    CHECK   3, 1
    lwz     3, 0xf0c(0)
    lwz     4, 0xf14(0)
    cmpw    3, 4
    beq     3f
    li      3, 100
    .long   0x00333304
3:
    lwz     3, 0xf08(0)
    srwi    3, 3, 24
    CHECK   3, \flags
.endm

_start:
    li      0, 0
    stw     0, 0xf14(0)
    bl      .Lhandler_addr
.Lhandler_addr:
    mflr    5
    addi    5, 5, (.Lhandler - .Lhandler_addr)
    li      6, 0x300
    li      7, (.Lhandler_end - .Lhandler) / 4
    mtctr   7
.Lcopy:
    lwz     0, 0(5)
    stw     0, 0(6)
    addi    5, 5, 4
    addi    6, 6, 4
    bdnz    .Lcopy
    li      6, 0x300
    icbi    0, 6
    sync
    isync

    /* Prime only the first page, then invalidate its TLB entry.
     * stmw's third word faults after a successful slow-path call. */
    lis     8, 0x51
    ori     8, 8, 0xff8
    lwz     3, 0(8)
    tlbie   8
    EXPECT_FAULT .Lstore
    li      28, 0x1122
    li      29, 0x3344
    li      30, 0x5566
    li      31, 0x7788
.Lstore:
    stmw    28, 0(8)
    CHECK_FAULT 0x42
    lwz     3, 0(8)
    CHECK   3, 0x1122
    lwz     3, 4(8)
    CHECK   3, 0x3344
    lwz     3, 8(8)
    CHECK   3, 0x5566
    lwz     3, 12(8)
    CHECK   3, 0x7788

    /* Use another pair of pages for lmw. Map/write the first page;
     * the next page is still unmapped and contains zeroed RAM. */
    li      0, 0
    stw     0, 0xf14(0)
    lis     8, 0x52
    ori     8, 8, 0xff8
    stw     28, 0(8)
    stw     29, 4(8)
    tlbie   8
    EXPECT_FAULT .Lload
    li      28, -1
    li      29, -1
    li      30, -1
    li      31, -1
.Lload:
    lmw     28, 0(8)
    CHECK_FAULT 0x40
    CHECK   28, 0x1122
    CHECK   29, 0x3344
    CHECK   30, 0
    CHECK   31, 0

    li      3, 0
    .long   0x00333304

/* The PROM uses VSID=0 for these addresses. Install an identity PTE
 * in the primary PTEG at SDR1=0x300003, preserving the faulting GPRs.
 * CR is scratch: the tested instructions do not depend on its input. */
.Lhandler:
    stw     0, 0xf20(0)
    mtsprg  0, 3
    mtsprg  1, 4
    lwz     0, 0xf00(0)
    addi    0, 0, 1
    stw     0, 0xf00(0)
    mfspr   0, 18
    stw     0, 0xf08(0)
    mfsrr0  0
    stw     0, 0xf0c(0)
    lwz     0, 0xf14(0)
    cmpwi   0, 0
    beq     .Lkeep_pc
    mtsrr0  0
.Lkeep_pc:
    mfdar   3
    srwi    4, 3, 12
    andi.   4, 4, 0x1fff
    slwi    4, 4, 6
    addis   4, 4, 0x30
    srwi    0, 3, 22
    andi.   0, 0, 0x3f
    oris    0, 0, 0x8000
    stw     0, 0(4)
    rlwinm  0, 3, 0, 0, 19
    ori     0, 0, 0xc2
    stw     0, 4(4)
    sync
    tlbie   3
    sync
    lwz     0, 0xf20(0)
    mfsprg  3, 0
    mfsprg  4, 1
    rfi
.Lhandler_end:
