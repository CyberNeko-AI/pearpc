/*
 *  PearPC
 *  jitc_mmu.S
 *
 *  AArch64 MMU assembly - TLB fast path for JITC
 *
 *  Copyright (C) 2026 Sebastian Biallas (sb@biallas.net)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 *  Memory access stubs called from JIT-generated code.
 *
 *  Each stub has an inline TLB fast path (~6 instructions for the
 *  common case). On TLB miss or page-crossing access, falls through
 *  to a C++ slow-path helper that does full EA->PA translation,
 *  fills the TLB, and performs the access.
 *
 *  Register conventions (JIT context):
 *    X20 = PPC_CPU_State pointer (callee-saved, always valid)
 *    X19 = JITC pointer (callee-saved, always valid)
 *
 *  Read stubs:
 *    IN:  w0 = effective address, w9 = pc_ofs
 *    OUT: w0 = value (host-endian, byte-swapped from BE storage)
 *         w0/x0 for dword
 *
 *  Write stubs:
 *    IN:  w0 = effective address, w9 = pc_ofs
 *         w1 = value (host-endian) / x1 for dword
 *    OUT: none
 *
 *  Scratch registers (clobbered): x0-x5, x9, x16, x17
 *  Preserved: x19-x28, x29, x30 (LR saved/restored in slow path)
 *
 *  All slow-path helpers return int: 0 = OK, nonzero = DSI exception.
 *  Read helpers store the result in cpu->temp (word) or
 *  cpu->temp/temp2 (dword). The return value is the sole signal for
 *  DSI — no exception_pending flag is involved.
 */

#ifndef PREFIX
#define PREFIX
#endif

#define GLUE(a, b) GLUE2(a, b)
#define GLUE2(a, b) a##b
#define EXTERN(sym) GLUE(PREFIX, sym)

.macro do_export name
    .globl \name
\name:
.endm

#include "jitc_common.h"

// Uncomment to disable TLB fast path (force all accesses through slow path)
//#define DISABLE_TLB 1

.macro TLB_MISS_BRANCH target
#ifdef DISABLE_TLB
    b       \target
#else
    b.ne    \target
#endif
.endm

.text

//////////////////////////////////////////////////////////////////////////////////////////////
//
//  ppc_effective_to_physical_code
//
//  IN:  w0 = effective address
//       w1 = bytes to unwind (ignored on aarch64)
//  OUT: w0 = physical address
//
.p2align 4
do_export EXTERN(ppc_effective_to_physical_code)
    and     w1, w0, #0xFFF              // w1 = page offset
    ubfx    w2, w0, #12, #TLB_BITS      // w2 = TLB index
    and     w3, w0, #0xFFFFF000         // w3 = page tag
    add     x4, x20, #tlb_code_0_eff
    add     x5, x20, #tlb_code_0_phys
    ldr     w16, [x4, x2, lsl #2]
    ldr     x5, [x5, x2, lsl #3]        // guest physical page base (speculative)
    cmp     w3, w16
    TLB_MISS_BRANCH    .Lcode_slow
    add     w0, w5, w1                 // w0 = physical address
    ret

.Lcode_slow:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    mov     w1, w0
    mov     x0, x20
    bl      EXTERN(ppc_effective_to_physical_code_c)
    ldp     x29, x30, [sp], #16
    ret


//////////////////////////////////////////////////////////////////////////////////////////////
//
//  ppc_read_effective_byte_asm
//
//  IN:  w0 = effective address
//  OUT: w0 = byte value (zero-extended to 32 bits)
//
.p2align 4
do_export EXTERN(ppc_read_effective_byte_asm)
    // Byte reads never cross a page boundary
    ubfx    w2, w0, #12, #TLB_BITS      // TLB index
    and     w1, w0, #0xFFF              // page offset
    and     w3, w0, #0xFFFFF000         // page tag
    add     x4, x20, #tlb_data_0_eff
    add     x5, x20, #tlb_data_0_phys
    ldr     w16, [x4, x2, lsl #2]
    ldr     x5, [x5, x2, lsl #3]        // host page base (speculative)
    cmp     w3, w16
    TLB_MISS_BRANCH    .Lread_byte_slow
    ldrb    w0, [x5, x1]               // load byte (no swap needed)
    ret
.Lread_byte_slow:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    str     w9, [x20, #pc_ofs]
    ldr     w16, [x20, #current_code_base]
    add     w16, w16, w9
    str     w16, [x20, #pc]
    mov     w1, w0
    mov     x0, x20
    bl      EXTERN(ppc_read_effective_byte_slow)
    // Check return value: w0 != 0 means DSI
    cbnz    w0, .Ldsi_from_slow
    // Load result from cpu->temp
    ldr     w0, [x20, #temp]
    ldp     x29, x30, [sp], #16
    ret


//////////////////////////////////////////////////////////////////////////////////////////////
//
//  ppc_read_effective_half_z_asm
//
//  IN:  w0 = effective address
//  OUT: w0 = halfword value (zero-extended to 32 bits)
//
.p2align 4
do_export EXTERN(ppc_read_effective_half_z_asm)
    ubfx    w2, w0, #12, #TLB_BITS
    and     w1, w0, #0xFFF
    and     w3, w0, #0xFFFFF000
    add     x4, x20, #tlb_data_0_eff
    add     x5, x20, #tlb_data_0_phys
    ldr     w16, [x4, x2, lsl #2]
    ldr     x5, [x5, x2, lsl #3]
    cmp     w1, #0xFFE
    ccmp    w3, w16, #0, ls
    TLB_MISS_BRANCH    .Lread_half_z_slow
    ldrh    w0, [x5, x1]               // load 16-bit LE
    rev16   w0, w0                      // byte-swap to BE
    ret
.Lread_half_z_slow:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    str     w9, [x20, #pc_ofs]
    ldr     w16, [x20, #current_code_base]
    add     w16, w16, w9
    str     w16, [x20, #pc]
    mov     w1, w0
    mov     x0, x20
    bl      EXTERN(ppc_read_effective_half_z_slow)
    // Check return value: w0 != 0 means DSI
    cbnz    w0, .Ldsi_from_slow
    ldr     w0, [x20, #temp]
    ldp     x29, x30, [sp], #16
    ret


//////////////////////////////////////////////////////////////////////////////////////////////
//
//  ppc_read_effective_half_s_asm
//
//  IN:  w0 = effective address
//  OUT: w0 = halfword value (sign-extended to 32 bits)
//
.p2align 4
do_export EXTERN(ppc_read_effective_half_s_asm)
    ubfx    w2, w0, #12, #TLB_BITS
    and     w1, w0, #0xFFF
    and     w3, w0, #0xFFFFF000
    add     x4, x20, #tlb_data_0_eff
    add     x5, x20, #tlb_data_0_phys
    ldr     w16, [x4, x2, lsl #2]
    ldr     x5, [x5, x2, lsl #3]
    cmp     w1, #0xFFE
    ccmp    w3, w16, #0, ls
    TLB_MISS_BRANCH    .Lread_half_s_slow
    ldrh    w0, [x5, x1]               // load 16-bit LE
    rev16   w0, w0                      // byte-swap to BE
    sxth    w0, w0                      // sign-extend 16 -> 32
    ret
.Lread_half_s_slow:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    str     w9, [x20, #pc_ofs]
    ldr     w16, [x20, #current_code_base]
    add     w16, w16, w9
    str     w16, [x20, #pc]
    mov     w1, w0
    mov     x0, x20
    bl      EXTERN(ppc_read_effective_half_z_slow)
    // Check return value: w0 != 0 means DSI
    cbnz    w0, .Ldsi_from_slow
    ldr     w0, [x20, #temp]
    sxth    w0, w0                      // sign-extend result
    ldp     x29, x30, [sp], #16
    ret


//////////////////////////////////////////////////////////////////////////////////////////////
//
//  ppc_read_effective_word_asm
//
//  IN:  w0 = effective address
//  OUT: w0 = word value (host-endian)
//
.p2align 4
do_export EXTERN(ppc_read_effective_word_asm)
    ubfx    w2, w0, #12, #TLB_BITS
    and     w1, w0, #0xFFF
    and     w3, w0, #0xFFFFF000
    add     x4, x20, #tlb_data_0_eff
    add     x5, x20, #tlb_data_0_phys
    ldr     w16, [x4, x2, lsl #2]
    ldr     x5, [x5, x2, lsl #3]
    cmp     w1, #0xFFC
    ccmp    w3, w16, #0, ls
    TLB_MISS_BRANCH    .Lread_word_slow
    ldr     w0, [x5, x1]
    rev     w0, w0                      // BE -> LE
    ret
.Lread_word_slow:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    str     w9, [x20, #pc_ofs]
    ldr     w16, [x20, #current_code_base]
    add     w16, w16, w9
    str     w16, [x20, #pc]
    mov     w1, w0
    mov     x0, x20
    bl      EXTERN(ppc_read_effective_word_slow)
    // Check return value: w0 != 0 means DSI
    cbnz    w0, .Ldsi_from_slow
    ldr     w0, [x20, #temp]
    ldp     x29, x30, [sp], #16
    ret


//////////////////////////////////////////////////////////////////////////////////////////////
//
//  ppc_read_effective_dword_asm
//
//  IN:  w0 = effective address
//  OUT: x0 = doubleword value (host-endian)
//
.p2align 4
do_export EXTERN(ppc_read_effective_dword_asm)
    ubfx    w2, w0, #12, #TLB_BITS
    and     w1, w0, #0xFFF
    and     w3, w0, #0xFFFFF000
    add     x4, x20, #tlb_data_0_eff
    add     x5, x20, #tlb_data_0_phys
    ldr     w16, [x4, x2, lsl #2]
    ldr     x5, [x5, x2, lsl #3]
    cmp     w1, #0xFF8
    ccmp    w3, w16, #0, ls
    TLB_MISS_BRANCH    .Lread_dword_slow
    ldr     x0, [x5, x1]
    rev     x0, x0                      // BE -> LE (64-bit)
    ret
.Lread_dword_slow:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    str     w9, [x20, #pc_ofs]
    ldr     w16, [x20, #current_code_base]
    add     w16, w16, w9
    str     w16, [x20, #pc]
    mov     w1, w0
    mov     x0, x20
    bl      EXTERN(ppc_read_effective_dword_slow)
    // Check return value: w0 != 0 means DSI
    cbnz    w0, .Ldsi_from_slow
    // Load 64-bit result from cpu->temp (hi32) and cpu->temp2 (lo32)
    ldr     w16, [x20, #temp]
    ldr     w17, [x20, #temp2]
    orr     x0, x17, x16, lsl #32
    ldp     x29, x30, [sp], #16
    ret


//////////////////////////////////////////////////////////////////////////////////////////////
//
//  ppc_write_effective_byte_asm
//
//  IN:  w0 = effective address, w1 = byte value
//
.p2align 4
do_export EXTERN(ppc_write_effective_byte_asm)
    ubfx    w3, w0, #12, #TLB_BITS
    and     w2, w0, #0xFFF
    and     w4, w0, #0xFFFFF000
    add     x5, x20, #tlb_data_8_eff
    add     x16, x20, #tlb_data_8_phys
    ldr     w17, [x5, x3, lsl #2]
    ldr     x16, [x16, x3, lsl #3]
    cmp     w4, w17
    TLB_MISS_BRANCH    .Lwrite_byte_slow
    strb    w1, [x16, x2]               // store byte (no swap needed)
    ret
.Lwrite_byte_slow:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    str     w9, [x20, #pc_ofs]
    ldr     w16, [x20, #current_code_base]
    add     w16, w16, w9
    str     w16, [x20, #pc]
    mov     w2, w1                      // w2 = data
    mov     w1, w0                      // w1 = ea
    mov     x0, x20                     // x0 = cpu
    bl      EXTERN(ppc_write_effective_byte_slow)
    // Check return value: w0 != 0 means DSI
    cbnz    w0, .Ldsi_from_slow
    ldp     x29, x30, [sp], #16
    ret


//////////////////////////////////////////////////////////////////////////////////////////////
//
//  ppc_write_effective_half_asm
//
//  IN:  w0 = effective address, w1 = halfword value (host-endian)
//
.p2align 4
do_export EXTERN(ppc_write_effective_half_asm)
    ubfx    w3, w0, #12, #TLB_BITS
    and     w2, w0, #0xFFF
    and     w4, w0, #0xFFFFF000
    add     x5, x20, #tlb_data_8_eff
    add     x16, x20, #tlb_data_8_phys
    ldr     w17, [x5, x3, lsl #2]
    ldr     x16, [x16, x3, lsl #3]
    cmp     w2, #0xFFE
    ccmp    w4, w17, #0, ls
    TLB_MISS_BRANCH    .Lwrite_half_slow
    rev16   w17, w1                     // byte-swap LE -> BE
    strh    w17, [x16, x2]
    ret
.Lwrite_half_slow:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    str     w9, [x20, #pc_ofs]
    ldr     w16, [x20, #current_code_base]
    add     w16, w16, w9
    str     w16, [x20, #pc]
    mov     w2, w1
    mov     w1, w0
    mov     x0, x20
    bl      EXTERN(ppc_write_effective_half_slow)
    // Check return value: w0 != 0 means DSI
    cbnz    w0, .Ldsi_from_slow
    ldp     x29, x30, [sp], #16
    ret


//////////////////////////////////////////////////////////////////////////////////////////////
//
//  ppc_write_effective_word_asm
//
//  IN:  w0 = effective address, w1 = word value (host-endian)
//
.p2align 4
do_export EXTERN(ppc_write_effective_word_asm)
    ubfx    w3, w0, #12, #TLB_BITS
    and     w2, w0, #0xFFF
    and     w4, w0, #0xFFFFF000
    add     x5, x20, #tlb_data_8_eff
    add     x16, x20, #tlb_data_8_phys
    ldr     w17, [x5, x3, lsl #2]
    ldr     x16, [x16, x3, lsl #3]
    cmp     w2, #0xFFC
    ccmp    w4, w17, #0, ls
    TLB_MISS_BRANCH    .Lwrite_word_slow
    rev     w17, w1                     // byte-swap LE -> BE
    str     w17, [x16, x2]
    ret
.Lwrite_word_slow:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    str     w9, [x20, #pc_ofs]
    ldr     w16, [x20, #current_code_base]
    add     w16, w16, w9
    str     w16, [x20, #pc]
    mov     w2, w1
    mov     w1, w0
    mov     x0, x20
    bl      EXTERN(ppc_write_effective_word_slow)
    // Check return value: w0 != 0 means DSI
    cbnz    w0, .Ldsi_from_slow
    ldp     x29, x30, [sp], #16
    ret


//////////////////////////////////////////////////////////////////////////////////////////////
//
//  ppc_write_effective_dword_asm
//
//  IN:  w0 = effective address, x1 = doubleword value (host-endian)
//
.p2align 4
do_export EXTERN(ppc_write_effective_dword_asm)
    ubfx    w3, w0, #12, #TLB_BITS
    and     w2, w0, #0xFFF
    and     w4, w0, #0xFFFFF000
    add     x5, x20, #tlb_data_8_eff
    add     x16, x20, #tlb_data_8_phys
    ldr     w17, [x5, x3, lsl #2]
    ldr     x16, [x16, x3, lsl #3]
    cmp     w2, #0xFF8
    ccmp    w4, w17, #0, ls
    TLB_MISS_BRANCH    .Lwrite_dword_slow
    rev     x17, x1                     // byte-swap LE -> BE (64-bit)
    str     x17, [x16, x2]
    ret
.Lwrite_dword_slow:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    str     w9, [x20, #pc_ofs]
    ldr     w16, [x20, #current_code_base]
    add     w16, w16, w9
    str     w16, [x20, #pc]
    mov     x2, x1                      // x2 = data (64-bit)
    mov     w1, w0                      // w1 = ea
    mov     x0, x20                     // x0 = cpu
    bl      EXTERN(ppc_write_effective_dword_slow)
    // Check return value: w0 != 0 means DSI
    cbnz    w0, .Ldsi_from_slow
    ldp     x29, x30, [sp], #16
    ret


//////////////////////////////////////////////////////////////////////////////////////////////
//
//  String load/store and cache stubs (not yet implemented)
//
//  ppc_opc_dcbz_asm
//
//  IN:  w0 = EA (already cache-line aligned: EA & ~0x1F)
//       w9 = pc_ofs
//
//  Zeroes 32 bytes at the aligned address.  Since the 32-byte block is
//  always within a single 4 KB page, one TLB lookup suffices.
//
.p2align 4
do_export EXTERN(ppc_opc_dcbz_asm)
    ubfx    w3, w0, #12, #TLB_BITS
    and     w2, w0, #0xFFF              // page offset
    and     w4, w0, #0xFFFFF000
    add     x5, x20, #tlb_data_8_eff
    add     x16, x20, #tlb_data_8_phys
    ldr     w17, [x5, x3, lsl #2]
    ldr     x16, [x16, x3, lsl #3]
    cmp     w4, w17
    TLB_MISS_BRANCH    .Ldcbz_slow
    // TLB hit — get host pointer and zero 32 bytes with two STP of XZR
    add     x16, x16, x2                 // host_ptr + page_offset
    stp     xzr, xzr, [x16]
    stp     xzr, xzr, [x16, #16]
    ret
.Ldcbz_slow:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    str     w9, [x20, #pc_ofs]
    ldr     w16, [x20, #current_code_base]
    add     w16, w16, w9
    str     w16, [x20, #pc]
    mov     w1, w0                      // w1 = ea (aligned)
    mov     x0, x20                     // x0 = cpu
    bl      EXTERN(ppc_opc_dcbz_slow)
    cbnz    w0, .Ldsi_from_slow
    ldp     x29, x30, [sp], #16
    ret


.p2align 4
do_export EXTERN(ppc_opc_stswi_asm)
    ret

.p2align 4
do_export EXTERN(ppc_opc_lswi_asm)
    ret

.p2align 4
do_export EXTERN(ppc_opc_icbi_asm)
    ret

//////////////////////////////////////////////////////////////////////////////////////////////
//
//  DSI exception from slow path
//
//  Called when a TLB slow-path function returns nonzero (DSI).
//  ppc_exception() already set DAR, DSISR, SRR0/SRR1, MSR, npc.
//  Clean up the slow-path stack frame and dispatch to npc.
//
.Ldsi_from_slow:
    // Clean up the slow-path stack frame (stp x29, x30 was pushed)
    ldp     x29, x30, [sp], #16
    // Dispatch to npc (exception vector).
    ldr     w0, [x20, #npc]
    b       EXTERN(ppc_new_pc_asm)
