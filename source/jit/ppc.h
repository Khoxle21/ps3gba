// PowerPC instruction encoders for the JIT (32-bit forms of the 64-bit PPE ISA).
#ifndef JIT_PPC_H
#define JIT_PPC_H

#include <stdint.h>

// CR bit index of field f: LT, GT, EQ, SO.
#define CR_LT(f) ((f) * 4 + 0)
#define CR_GT(f) ((f) * 4 + 1)
#define CR_EQ(f) ((f) * 4 + 2)
#define CR_SO(f) ((f) * 4 + 3)

// bc BO values. The "unlikely" forms set the at=10 static hint (predict not taken).
#define BO_TRUE 12
#define BO_FALSE 4
#define BO_TRUE_UNLIKELY 14
#define BO_FALSE_UNLIKELY 6
#define BO_ALWAYS 20

#define D_FORM(op, d, a, imm) (((uint32_t) (op) << 26) | ((d) << 21) | ((a) << 16) | ((uint32_t) (imm) & 0xFFFF))
#define X_FORM(d, a, b, xo) ((31u << 26) | ((d) << 21) | ((a) << 16) | ((b) << 11) | ((xo) << 1))

// Careful: addi, addis and D-form/indexed loads and stores read rA = r0 as the constant 0.
#define P_ADDI(d, a, imm) D_FORM(14, d, a, imm)
#define P_LI(d, imm) P_ADDI(d, 0, imm)
#define P_ADDIS(d, a, imm) D_FORM(15, d, a, imm)
#define P_LIS(d, imm) P_ADDIS(d, 0, imm)
#define P_ORI(a, s, imm) D_FORM(24, s, a, imm)
#define P_ORIS(a, s, imm) D_FORM(25, s, a, imm)
#define P_XORI(a, s, imm) D_FORM(26, s, a, imm)
#define P_XORIS(a, s, imm) D_FORM(27, s, a, imm)
#define P_ANDI_(a, s, imm) D_FORM(28, s, a, imm)
#define P_ANDIS_(a, s, imm) D_FORM(29, s, a, imm)
#define P_CMPWI(f, a, imm) D_FORM(11, (f) << 2, a, imm)
#define P_CMPLWI(f, a, imm) D_FORM(10, (f) << 2, a, imm)
#define P_SUBFIC(d, a, imm) D_FORM(8, d, a, imm)
#define P_NOP P_ORI(0, 0, 0)

#define P_LWZ(d, off, a) D_FORM(32, d, a, off)
#define P_LBZ(d, off, a) D_FORM(34, d, a, off)
#define P_LHZ(d, off, a) D_FORM(40, d, a, off)
#define P_STW(s, off, a) D_FORM(36, s, a, off)
#define P_STB(s, off, a) D_FORM(38, s, a, off)
#define P_STH(s, off, a) D_FORM(44, s, a, off)
#define P_LD(d, off, a) D_FORM(58, d, a, (off) & ~3)
#define P_STD(s, off, a) D_FORM(62, s, a, (off) & ~3)
#define P_STDU(s, off, a) (D_FORM(62, s, a, (off) & ~3) | 1)

#define P_CMPW(f, a, b) X_FORM((f) << 2, a, b, 0)
#define P_CMPLW(f, a, b) X_FORM((f) << 2, a, b, 32)
#define P_ADD(d, a, b) X_FORM(d, a, b, 266)
#define P_SUBF(d, a, b) X_FORM(d, a, b, 40) // d = b - a
#define P_NEG(d, a) X_FORM(d, a, 0, 104)
#define P_MULLW(d, a, b) X_FORM(d, a, b, 235)
#define P_MULHW(d, a, b) X_FORM(d, a, b, 75)
#define P_MULHWU(d, a, b) X_FORM(d, a, b, 11)
#define P_ADDC(d, a, b) X_FORM(d, a, b, 10)
#define P_ADDE(d, a, b) X_FORM(d, a, b, 138)
#define P_SUBFC(d, a, b) X_FORM(d, a, b, 8)
#define P_SUBFE(d, a, b) X_FORM(d, a, b, 136)
#define P_ADDZE(d, a) X_FORM(d, a, 0, 202)
#define P_AND(a, s, b) X_FORM(s, a, b, 28)
#define P_ANDC(a, s, b) X_FORM(s, a, b, 60)
#define P_OR(a, s, b) X_FORM(s, a, b, 444)
#define P_MR(a, s) P_OR(a, s, s)
#define P_ORC(a, s, b) X_FORM(s, a, b, 412)
#define P_XOR(a, s, b) X_FORM(s, a, b, 316)
#define P_NOR(a, s, b) X_FORM(s, a, b, 124)
#define P_NOT(a, s) P_NOR(a, s, s)
#define P_EQV(a, s, b) X_FORM(s, a, b, 284)
#define P_SLW(a, s, b) X_FORM(s, a, b, 24)
#define P_SRW(a, s, b) X_FORM(s, a, b, 536)
#define P_SRAW(a, s, b) X_FORM(s, a, b, 792)
#define P_SRAWI(a, s, sh) X_FORM(s, a, sh, 824)
#define P_CNTLZW(a, s) X_FORM(s, a, 0, 26)
#define P_EXTSB(a, s) X_FORM(s, a, 0, 954)
#define P_EXTSH(a, s) X_FORM(s, a, 0, 922)
#define P_LWZX(d, a, b) X_FORM(d, a, b, 23)
#define P_LBZX(d, a, b) X_FORM(d, a, b, 87)
#define P_LHZX(d, a, b) X_FORM(d, a, b, 279)
#define P_STWX(s, a, b) X_FORM(s, a, b, 151)
#define P_STBX(s, a, b) X_FORM(s, a, b, 215)
#define P_STHX(s, a, b) X_FORM(s, a, b, 407)
#define P_LWBRX(d, a, b) X_FORM(d, a, b, 534)
#define P_LHBRX(d, a, b) X_FORM(d, a, b, 790)
#define P_STWBRX(s, a, b) X_FORM(s, a, b, 662)
#define P_STHBRX(s, a, b) X_FORM(s, a, b, 918)
#define P_DCBST(a, b) X_FORM(0, a, b, 54)
#define P_ICBI(a, b) X_FORM(0, a, b, 982)
#define P_SYNC 0x7C0004AC
#define P_ISYNC 0x4C00012C

#define P_RLWINM(a, s, sh, mb, me) ((21u << 26) | ((s) << 21) | ((a) << 16) | ((sh) << 11) | ((mb) << 6) | ((me) << 1))
#define P_RLWIMI(a, s, sh, mb, me) ((20u << 26) | ((s) << 21) | ((a) << 16) | ((sh) << 11) | ((mb) << 6) | ((me) << 1))
#define P_RLWNM(a, s, b, mb, me) ((23u << 26) | ((s) << 21) | ((a) << 16) | ((b) << 11) | ((mb) << 6) | ((me) << 1))
#define P_ROTLWI(a, s, sh) P_RLWINM(a, s, sh, 0, 31)
#define P_SLWI(a, s, n) P_RLWINM(a, s, n, 0, 31 - (n))
#define P_SRWI(a, s, n) P_RLWINM(a, s, (32 - (n)) & 31, n, 31)
// 64-bit shifts and insert (the code-bit test of multiple stores).
#define P_SLD_(a, s, b) (X_FORM(s, a, b, 27) | 1) // record form: cr0
#define P_SRD(a, s, b) X_FORM(s, a, b, 539)
#define P_RLDIMI(a, s, sh, mb) ((30u << 26) | ((s) << 21) | ((a) << 16) | (((sh) & 31) << 11) | \
	((((mb) & 31) << 1 | (mb) >> 5) << 5) | (3 << 2) | ((sh) >> 5 << 1))
#define P_CLRLDI32(a, s) ((30u << 26) | ((s) << 21) | ((a) << 16) | (0 << 11) | (32 << 5 & 0x7C0) | (1 << 5)) // rldicl a,s,0,32

#define P_MFLR(d) (0x7C0802A6 | ((d) << 21))
#define P_MTLR(s) (0x7C0803A6 | ((s) << 21))
#define P_MTCTR(s) (0x7C0903A6 | ((s) << 21))
#define P_MFCR(d) (0x7C000026 | ((d) << 21))
#define P_MTCRF(mask, s) (0x7C000120 | ((s) << 21) | ((mask) << 12))
#define P_BCTR 0x4E800420
#define P_BCTRL 0x4E800421
#define P_BLR 0x4E800020

#define CR_OP(bt, ba, bb, xo) ((19u << 26) | ((bt) << 21) | ((ba) << 16) | ((bb) << 11) | ((xo) << 1))
#define P_CRAND(bt, ba, bb) CR_OP(bt, ba, bb, 257)
#define P_CRANDC(bt, ba, bb) CR_OP(bt, ba, bb, 129)
#define P_CROR(bt, ba, bb) CR_OP(bt, ba, bb, 449)
#define P_CRORC(bt, ba, bb) CR_OP(bt, ba, bb, 417)
#define P_CRXOR(bt, ba, bb) CR_OP(bt, ba, bb, 193)
#define P_CREQV(bt, ba, bb) CR_OP(bt, ba, bb, 289)
#define P_CRNOR(bt, ba, bb) CR_OP(bt, ba, bb, 33)
#define P_CRNOT(bt, ba) P_CRNOR(bt, ba, ba)
#define P_CRMOVE(bt, ba) P_CROR(bt, ba, ba)

// Relative branches; displacement in bytes from the branch instruction.
#define P_B(disp) ((18u << 26) | ((uint32_t) (disp) & 0x03FFFFFC))
#define P_BL(disp) (P_B(disp) | 1)
#define P_BC(bo, bi, disp) ((16u << 26) | ((bo) << 21) | ((bi) << 16) | ((uint32_t) (disp) & 0xFFFC))

#endif
