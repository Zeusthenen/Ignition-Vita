/* Deferred-flags materializer for the extracted Box86 dynarec.
 * UpdateFlags() below is copied VERBATIM from Box86 src/emu/x86run_private.c
 * Box86 is (c) ptitSeb, MIT license — see third_party/box86-dynarec/LICENSE
 */
#include <stdint.h>

#include "debug.h"
#include "regs.h"
#include "emu/x86emu_private.h"
#include "emu/x86run_private.h"

#define PARITY(x)   (((emu->x86emu_parity_tab[(x) / 32] >> ((x) % 32)) & 1) == 0)
#define XOR2(x) 	(((x) ^ ((x)>>1)) & 0x1)

#ifdef D2_EMITPROF
#include "dyn86_emitprof.h"
#endif

void UpdateFlags(x86emu_t *emu)
{
#ifdef D2_EMITPROF
    /* EXACT counter (not sampled): UpdateFlags is plain C, so incrementing
     * here is trivial and can't lie. Two counters, because the first alone
     * would mislead: many calls land on `d_none` and compute nothing, yet
     * still pay for the exit from translated code (stack, flag save, BLX). */
    ++d2ep_run_updateflags;
    if(emu->df != d_none) ++d2ep_run_updateflags_work;
#endif
    uint32_t cc;
    uint32_t lo, hi;
    uint32_t bc;
    uint32_t cnt;

    switch(emu->df) {
        case d_none:
            return;
        case d_add8:
            CONDITIONAL_SET_FLAG(emu->res & 0x100, F_CF);
            CONDITIONAL_SET_FLAG((emu->res & 0xff) == 0, F_ZF);
            CONDITIONAL_SET_FLAG(emu->res & 0x80, F_SF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);

            cc = (emu->op1 & emu->op2) | ((~emu->res) & (emu->op1 | emu->op2));
            CONDITIONAL_SET_FLAG(XOR2(cc >> 6), F_OF);
            CONDITIONAL_SET_FLAG(cc & 0x8, F_AF);
            break;
        case d_add16:
            CONDITIONAL_SET_FLAG(emu->res & 0x10000, F_CF);
            CONDITIONAL_SET_FLAG((emu->res & 0xffff) == 0, F_ZF);
            CONDITIONAL_SET_FLAG(emu->res & 0x8000, F_SF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            cc = (emu->op1 & emu->op2) | ((~emu->res) & (emu->op1 | emu->op2));
            CONDITIONAL_SET_FLAG(XOR2(cc >> 14), F_OF);
            CONDITIONAL_SET_FLAG(cc & 0x8, F_AF);
            break;
        case d_add32:
            lo = (emu->op2 & 0xFFFF) + (emu->op1 & 0xFFFF);
            hi = (lo >> 16) + (emu->op2 >> 16) + (emu->op1 >> 16);
            CONDITIONAL_SET_FLAG(hi & 0x10000, F_CF);
            CONDITIONAL_SET_FLAG(!emu->res, F_ZF);
            CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            cc = (emu->op1 & emu->op2) | ((~emu->res) & (emu->op1 | emu->op2));
            CONDITIONAL_SET_FLAG(XOR2(cc >> 30), F_OF);
            CONDITIONAL_SET_FLAG(cc & 0x8, F_AF);
            break;
        case d_and8:
            CLEAR_FLAG(F_OF);
            CLEAR_FLAG(F_CF);
            CLEAR_FLAG(F_AF);
            CONDITIONAL_SET_FLAG(emu->res & 0x80, F_SF);
            CONDITIONAL_SET_FLAG(emu->res == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            break;
        case d_and16:
            CLEAR_FLAG(F_OF);
            CLEAR_FLAG(F_CF);
            CLEAR_FLAG(F_AF);
            CONDITIONAL_SET_FLAG(emu->res & 0x8000, F_SF);
            CONDITIONAL_SET_FLAG(emu->res == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            break;
        case d_and32:
            CLEAR_FLAG(F_OF);
            CLEAR_FLAG(F_CF);
            CLEAR_FLAG(F_AF);
            CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
            CONDITIONAL_SET_FLAG(emu->res == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            break;
        case d_dec8:
            CONDITIONAL_SET_FLAG(emu->res & 0x80, F_SF);
            CONDITIONAL_SET_FLAG((emu->res & 0xff) == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            bc = (emu->res & (~emu->op1 | 1)) | (~emu->op1 & 1);
            CONDITIONAL_SET_FLAG(XOR2(bc >> 6), F_OF);
            CONDITIONAL_SET_FLAG(bc & 0x8, F_AF);
            break;
        case d_dec16:
            CONDITIONAL_SET_FLAG(emu->res & 0x8000, F_SF);
            CONDITIONAL_SET_FLAG((emu->res & 0xffff) == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            bc = (emu->res & (~emu->op1 | 1)) | (~emu->op1 & 1);
            CONDITIONAL_SET_FLAG(XOR2(bc >> 14), F_OF);
            CONDITIONAL_SET_FLAG(bc & 0x8, F_AF);
            break;
        case d_dec32:
            CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
            CONDITIONAL_SET_FLAG(!emu->res, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            bc = (emu->res & (~emu->op1 | 1)) | (~emu->op1 & 1);
            CONDITIONAL_SET_FLAG(XOR2(bc >> 30), F_OF);
            CONDITIONAL_SET_FLAG(bc & 0x8, F_AF);
            break;
        case d_inc8:
            CONDITIONAL_SET_FLAG((emu->res & 0xff) == 0, F_ZF);
            CONDITIONAL_SET_FLAG(emu->res & 0x80, F_SF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            cc = ((1 & emu->op1) | (~emu->res)) & (1 | emu->op1);
            CONDITIONAL_SET_FLAG(XOR2(cc >> 6), F_OF);
            CONDITIONAL_SET_FLAG(cc & 0x8, F_AF);
            break;
        case d_inc16:
            CONDITIONAL_SET_FLAG((emu->res & 0xffff) == 0, F_ZF);
            CONDITIONAL_SET_FLAG(emu->res & 0x8000, F_SF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            cc = (1 & emu->op1) | ((~emu->res) & (1 | emu->op1));
            CONDITIONAL_SET_FLAG(XOR2(cc >> 14), F_OF);
            CONDITIONAL_SET_FLAG(cc & 0x8, F_AF);
            break;
        case d_inc32:
            CONDITIONAL_SET_FLAG(!emu->res, F_ZF);
            CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            cc = (1 & emu->op1) | ((~emu->res) & (1 | emu->op1));
            CONDITIONAL_SET_FLAG(XOR2(cc >> 30), F_OF);
            CONDITIONAL_SET_FLAG(cc & 0x8, F_AF);
            break;
        case d_imul8:
            lo = emu->res & 0xff;
            hi = (emu->res>>8)&0xff;
            if (((lo & 0x80) == 0 && hi == 0x00) ||
                ((lo & 0x80) != 0 && hi == 0xFF)) {
                CLEAR_FLAG(F_CF);
                CLEAR_FLAG(F_OF);
            } else {
                SET_FLAG(F_CF);
                SET_FLAG(F_OF);
            }
            CONDITIONAL_SET_FLAG(PARITY(lo & 0xff), F_PF);
            break;
        case d_imul16:
            lo = (uint16_t)emu->res;
            hi = (uint16_t)(emu->res >> 16);
            if (((lo & 0x8000) == 0 && hi == 0x00) ||
                ((lo & 0x8000) != 0 && hi == 0xFFFF)) {
                CLEAR_FLAG(F_CF);
                CLEAR_FLAG(F_OF);
            } else {
                SET_FLAG(F_CF);
                SET_FLAG(F_OF);
            }
            CONDITIONAL_SET_FLAG(PARITY(lo & 0xff), F_PF);
            break;
        case d_imul32:
            if (((emu->res & 0x80000000) == 0 && emu->op1 == 0x00) ||
                ((emu->res & 0x80000000) != 0 && emu->op1 == 0xFFFFFFFF)) {
                CLEAR_FLAG(F_CF);
                CLEAR_FLAG(F_OF);
            } else {
                SET_FLAG(F_CF);
                SET_FLAG(F_OF);
            }
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            break;
        case d_mul8:
            lo = emu->res & 0xff;
            hi = (emu->res>>8)&0xff;
            if (hi == 0) {
                CLEAR_FLAG(F_CF);
                CLEAR_FLAG(F_OF);
            } else {
                SET_FLAG(F_CF);
                SET_FLAG(F_OF);
            }
            CONDITIONAL_SET_FLAG(PARITY(lo & 0xff), F_PF);
            break;
        case d_mul16:
            lo = (uint16_t)emu->res;
            hi = (uint16_t)(emu->res >> 16);
            if (hi == 0) {
                CLEAR_FLAG(F_CF);
                CLEAR_FLAG(F_OF);
            } else {
                SET_FLAG(F_CF);
                SET_FLAG(F_OF);
            }
            CONDITIONAL_SET_FLAG(PARITY(lo & 0xff), F_PF);
            break;
        case d_mul32:
            if (emu->op1 == 0) {
                CLEAR_FLAG(F_CF);
                CLEAR_FLAG(F_OF);
            } else {
                SET_FLAG(F_CF);
                SET_FLAG(F_OF);
            }
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            break;
        case d_or8:
            CLEAR_FLAG(F_OF);
            CLEAR_FLAG(F_CF);
            CLEAR_FLAG(F_AF);
            CONDITIONAL_SET_FLAG(emu->res & 0x80, F_SF);
            CONDITIONAL_SET_FLAG(emu->res == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            break;
        case d_or16:
            CLEAR_FLAG(F_OF);
            CLEAR_FLAG(F_CF);
            CLEAR_FLAG(F_AF);
            CONDITIONAL_SET_FLAG(emu->res & 0x8000, F_SF);
            CONDITIONAL_SET_FLAG(emu->res == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            break;
        case d_or32:
            CLEAR_FLAG(F_OF);
            CLEAR_FLAG(F_CF);
            CLEAR_FLAG(F_AF);
            CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
            CONDITIONAL_SET_FLAG(emu->res == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            break;
        case d_neg8:
            CONDITIONAL_SET_FLAG(emu->op1 != 0, F_CF);
            CONDITIONAL_SET_FLAG((emu->res & 0xff) == 0, F_ZF);
            CONDITIONAL_SET_FLAG(emu->res & 0x80, F_SF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            bc = emu->res | emu->op1;
            CONDITIONAL_SET_FLAG(XOR2(bc >> 6), F_OF);
            CONDITIONAL_SET_FLAG(bc & 0x8, F_AF);
            break;
        case d_neg16:
            CONDITIONAL_SET_FLAG(emu->op1 != 0, F_CF);
            CONDITIONAL_SET_FLAG((emu->res & 0xffff) == 0, F_ZF);
            CONDITIONAL_SET_FLAG(emu->res & 0x8000, F_SF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            bc = emu->res | emu->op1;
            CONDITIONAL_SET_FLAG(XOR2(bc >> 14), F_OF);
            CONDITIONAL_SET_FLAG(bc & 0x8, F_AF);
            break;
        case d_neg32:
            CONDITIONAL_SET_FLAG(emu->op1 != 0, F_CF);
            CONDITIONAL_SET_FLAG(!emu->res, F_ZF);
            CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            bc = emu->res | emu->op1;
            CONDITIONAL_SET_FLAG(XOR2(bc >> 30), F_OF);
            CONDITIONAL_SET_FLAG(bc & 0x8, F_AF);
            break;
        case d_rcl8:
            cc = (emu->op1 >> (8 - emu->op2)) & 0x1;
            CONDITIONAL_SET_FLAG(cc, F_CF);
            CONDITIONAL_SET_FLAG(emu->op2 == 1 && XOR2(cc + ((emu->res >> 6) & 0x2)), F_OF);
            break;
        case d_rcl16:
            cc = (emu->op1 >> (16 - emu->op2)) & 0x1;
            CONDITIONAL_SET_FLAG(cc, F_CF);
    		CONDITIONAL_SET_FLAG(emu->op2 == 1 && XOR2(cc + ((emu->res >> 14) & 0x2)), F_OF);
            break;
        case d_rcl32:
            cc = (emu->op1 >> (32 - emu->op2)) & 0x1;
            CONDITIONAL_SET_FLAG(cc, F_CF);
		    CONDITIONAL_SET_FLAG(emu->op2 == 1 && XOR2(cc + ((emu->res >> 30) & 0x2)), F_OF);
            break;
        case d_rcr8:
            cnt = emu->op2 % 9;
            if (cnt == 1) {
                CONDITIONAL_SET_FLAG(emu->op1 & 0x1, F_CF);
    			cc = ACCESS_FLAG(F_CF) != 0;
                CONDITIONAL_SET_FLAG(XOR2(cc + ((emu->op1 >> 6) & 0x2)), F_OF);
            } else {
                CONDITIONAL_SET_FLAG((emu->op1 >> (cnt - 1)) & 0x1, F_CF);
            }
            break;
        case d_rcr16:
            cnt = emu->op2 % 17;
            if (cnt == 1) {
                CONDITIONAL_SET_FLAG(emu->op1 & 0x1, F_CF);
    			cc = ACCESS_FLAG(F_CF) != 0;
                CONDITIONAL_SET_FLAG(XOR2(cc + ((emu->op1 >> 14) & 0x2)), F_OF);
            } else {
                CONDITIONAL_SET_FLAG((emu->op1 >> (cnt - 1)) & 0x1, F_CF);
            }
            break;
        case d_rcr32:
            cnt = emu->op2 % 33;
            if (cnt == 1) {
                CONDITIONAL_SET_FLAG(emu->op1 & 0x1, F_CF);
    			cc = ACCESS_FLAG(F_CF) != 0;
                CONDITIONAL_SET_FLAG(XOR2(cc + ((emu->op1 >> 30) & 0x2)), F_OF);
            } else {
                CONDITIONAL_SET_FLAG((emu->op1 >> (cnt - 1)) & 0x1, F_CF);
            }
            break;
        case d_shl8:
            if (emu->op2 < 8) {
                cnt = emu->op2 % 8;
                if (cnt > 0) {
                    cc = emu->op1 & (1 << (8 - cnt));
                    CONDITIONAL_SET_FLAG(cc, F_CF);
                    CONDITIONAL_SET_FLAG((emu->res & 0xff) == 0, F_ZF);
                    CONDITIONAL_SET_FLAG(emu->res & 0x80, F_SF);
                    CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
                }
                if (cnt == 1) {
                    CONDITIONAL_SET_FLAG((((emu->res & 0x80) == 0x80) ^(ACCESS_FLAG(F_CF) != 0)), F_OF);
                } else {
                    CLEAR_FLAG(F_OF);
                }
            } else {
                CONDITIONAL_SET_FLAG((emu->op1 << (emu->op2-1)) & 0x80, F_CF);
                CLEAR_FLAG(F_OF);
                CLEAR_FLAG(F_SF);
                SET_FLAG(F_PF);
                SET_FLAG(F_ZF);
            }
            break;
        case d_shl16:
            if (emu->op2 < 16) {
                cnt = emu->op2 % 16;
                if (cnt > 0) {
                    cc = emu->op1 & (1 << (16 - cnt));
                    CONDITIONAL_SET_FLAG(cc, F_CF);
                    CONDITIONAL_SET_FLAG((emu->res & 0xffff) == 0, F_ZF);
                    CONDITIONAL_SET_FLAG(emu->res & 0x8000, F_SF);
                    CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
                }
                if (cnt == 1) {
                    CONDITIONAL_SET_FLAG(((!!(emu->res & 0x8000)) ^(ACCESS_FLAG(F_CF) != 0)), F_OF);
                } else {
                    CLEAR_FLAG(F_OF);
                }
            } else {
                CONDITIONAL_SET_FLAG((emu->op1 << (emu->op2-1)) & 0x8000, F_CF);
                CLEAR_FLAG(F_OF);
                CLEAR_FLAG(F_SF);
                SET_FLAG(F_PF);
                SET_FLAG(F_ZF);
            }
            break;
        case d_shl32:
            if (emu->op2 > 0) {
                cc = emu->op1 & (1 << (32 - emu->op2));
                CONDITIONAL_SET_FLAG(cc, F_CF);
                CONDITIONAL_SET_FLAG(!emu->res, F_ZF);
                CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
                CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            }
            if (emu->op2 == 1) {
                CONDITIONAL_SET_FLAG(((!!(emu->res & 0x80000000)) ^
                                        (ACCESS_FLAG(F_CF) != 0)), F_OF);
            } else {
                CLEAR_FLAG(F_OF);
            }
            break;
        case d_sar8:
            if (emu->op2 < 8) {
                if(emu->op2) {
                    cc = emu->op1 & (1 << (emu->op2 - 1));
                    CONDITIONAL_SET_FLAG(cc, F_CF);
                    CONDITIONAL_SET_FLAG((emu->res & 0xff) == 0, F_ZF);
                    CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
                    CONDITIONAL_SET_FLAG(emu->res & 0x80, F_SF);
                    if(emu->op2==1)
                        CLEAR_FLAG(F_OF);
                }
            } else {
                if (emu->op1&0x80) {
                    SET_FLAG(F_CF);
                    CLEAR_FLAG(F_ZF);
                    SET_FLAG(F_SF);
                    SET_FLAG(F_PF);
                } else {
                    CLEAR_FLAG(F_CF);
                    SET_FLAG(F_ZF);
                    CLEAR_FLAG(F_SF);
                    SET_FLAG(F_PF);
                }
            }
            break;
        case d_sar16:
            if (emu->op2 < 16) {
                if(emu->op2) {
                    cc = emu->op1 & (1 << (emu->op2 - 1));
                    CONDITIONAL_SET_FLAG(cc, F_CF);
                    CONDITIONAL_SET_FLAG((emu->res & 0xffff) == 0, F_ZF);
                    CONDITIONAL_SET_FLAG(emu->res & 0x8000, F_SF);
                    CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
                    if(emu->op2==1)
                        CLEAR_FLAG(F_OF);
                }
            } else {
                if (emu->op1&0x8000) {
                    SET_FLAG(F_CF);
                    CLEAR_FLAG(F_ZF);
                    SET_FLAG(F_SF);
                    SET_FLAG(F_PF);
                } else {
                    CLEAR_FLAG(F_CF);
                    SET_FLAG(F_ZF);
                    CLEAR_FLAG(F_SF);
                    SET_FLAG(F_PF);
                }
            }
            break;
        case d_sar32:
            if(emu->op2) {
                cc = emu->op1 & (1 << (emu->op2 - 1));
                CONDITIONAL_SET_FLAG(cc, F_CF);
                CONDITIONAL_SET_FLAG(emu->res == 0, F_ZF);
                CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
                CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
                if(emu->op2==1)
                    CLEAR_FLAG(F_OF);
            }
            break;
        case d_shr8:
            if (emu->op2 < 8) {
                cnt = emu->op2 % 8;
                if (cnt > 0) {
                    cc = emu->op1 & (1 << (cnt - 1));
                    CONDITIONAL_SET_FLAG(cc, F_CF);
                    CONDITIONAL_SET_FLAG((emu->res & 0xff) == 0, F_ZF);
                    CONDITIONAL_SET_FLAG(emu->res & 0x80, F_SF);
                    CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
                }
                if (cnt == 1) {
                    CONDITIONAL_SET_FLAG(emu->op1 & 0x80, F_OF);
                }
            } else {
                CONDITIONAL_SET_FLAG((emu->op1 >> (emu->op2-1)) & 0x1, F_CF);
                CLEAR_FLAG(F_SF);
                SET_FLAG(F_PF);
                SET_FLAG(F_ZF);
            }
            break;
        case d_shr16:
            if (emu->op2 < 16) {
                cnt = emu->op2 % 16;
                if (cnt > 0) {
                    cc = emu->op1 & (1 << (cnt - 1));
                    CONDITIONAL_SET_FLAG(cc, F_CF);
                    CONDITIONAL_SET_FLAG((emu->res & 0xffff) == 0, F_ZF);
                    CONDITIONAL_SET_FLAG(emu->res & 0x8000, F_SF);
                    CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
                }
                if (cnt == 1) {
                    CONDITIONAL_SET_FLAG(emu->op1 & 0x8000, F_OF);
                }
            } else {
                CLEAR_FLAG(F_CF);
                SET_FLAG(F_ZF);
                CLEAR_FLAG(F_SF);
                SET_FLAG(F_PF);
            }
            break;
        case d_shr32:
            cnt = emu->op2;
            if (cnt > 0) {
                cc = emu->op1 & (1 << (cnt - 1));
                CONDITIONAL_SET_FLAG(cc, F_CF);
                CONDITIONAL_SET_FLAG(!emu->res, F_ZF);
                CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
                CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            }
            if (cnt == 1) {
                CONDITIONAL_SET_FLAG(emu->op1 & 0x80000000, F_OF);
            }
            break;
        case d_shrd32:
            cnt = emu->op2;
            if (cnt > 0) {
                cc = emu->op1 & (1 << (cnt - 1));
                CONDITIONAL_SET_FLAG(cc, F_CF);
                CONDITIONAL_SET_FLAG(!emu->res, F_ZF);
                CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
                CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
                if (cnt == 1) {
                    CONDITIONAL_SET_FLAG((emu->op1 ^ emu->res) & 0x80000000, F_OF);
                }
            }
            break;
        case d_shld32:
            cnt = emu->op2;
            if (cnt > 0) {
                cc = emu->op1 & (1 << (32 - cnt));
                CONDITIONAL_SET_FLAG(cc, F_CF);
                CONDITIONAL_SET_FLAG(!emu->res, F_ZF);
                CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
                CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
                if (cnt == 1) {
                    CONDITIONAL_SET_FLAG((emu->op1 ^ emu->res) & 0x80000000, F_OF);
                } else {
                    CLEAR_FLAG(F_OF);
                }
            }
            break;
        case d_sub8:
            CONDITIONAL_SET_FLAG(emu->res & 0x80, F_SF);
            CONDITIONAL_SET_FLAG((emu->res & 0xff) == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            bc = (emu->res & ((~emu->op1) | emu->op2)) | ((~emu->op1) & emu->op2);
            CONDITIONAL_SET_FLAG(bc & 0x80, F_CF);
            CONDITIONAL_SET_FLAG(XOR2(bc >> 6), F_OF);
            CONDITIONAL_SET_FLAG(bc & 0x8, F_AF);
            break;
        case d_sub16:
            CONDITIONAL_SET_FLAG(emu->res & 0x8000, F_SF);
            CONDITIONAL_SET_FLAG((emu->res & 0xffff) == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            bc = (emu->res & ((~emu->op1) | emu->op2)) | ((~emu->op1) & emu->op2);
            CONDITIONAL_SET_FLAG(bc & 0x8000, F_CF);
            CONDITIONAL_SET_FLAG(XOR2(bc >> 14), F_OF);
            CONDITIONAL_SET_FLAG(bc & 0x8, F_AF);
            break;
        case d_sub32:
            CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
            CONDITIONAL_SET_FLAG(!emu->res, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            bc = (emu->res & ((~emu->op1) | emu->op2)) | ((~emu->op1) & emu->op2);
            CONDITIONAL_SET_FLAG(bc & 0x80000000, F_CF);
            CONDITIONAL_SET_FLAG(XOR2(bc >> 30), F_OF);
            CONDITIONAL_SET_FLAG(bc & 0x8, F_AF);
            break;
        case d_xor8:
            CLEAR_FLAG(F_OF);
            CONDITIONAL_SET_FLAG(emu->res & 0x80, F_SF);
            CONDITIONAL_SET_FLAG(emu->res == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            CLEAR_FLAG(F_CF);
            CLEAR_FLAG(F_AF);
            break;
        case d_xor16:
            CLEAR_FLAG(F_OF);
            CONDITIONAL_SET_FLAG(emu->res & 0x8000, F_SF);
            CONDITIONAL_SET_FLAG(emu->res == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            CLEAR_FLAG(F_CF);
            CLEAR_FLAG(F_AF);
            break;
        case d_xor32:
            CLEAR_FLAG(F_OF);
            CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
            CONDITIONAL_SET_FLAG(emu->res == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            CLEAR_FLAG(F_CF);
            CLEAR_FLAG(F_AF);
            break;
        case d_cmp8:
            CLEAR_FLAG(F_CF);
            CONDITIONAL_SET_FLAG(emu->res & 0x80, F_SF);
            CONDITIONAL_SET_FLAG((emu->res & 0xff) == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            bc = (emu->res & (~emu->op1 | emu->op2)) | (~emu->op1 & emu->op2);
            CONDITIONAL_SET_FLAG(bc & 0x80, F_CF);
            CONDITIONAL_SET_FLAG(XOR2(bc >> 6), F_OF);
            CONDITIONAL_SET_FLAG(bc & 0x8, F_AF);
            break;
        case d_cmp16:
            CONDITIONAL_SET_FLAG(emu->res & 0x8000, F_SF);
            CONDITIONAL_SET_FLAG((emu->res & 0xffff) == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            bc = (emu->res & (~emu->op1 | emu->op2)) | (~emu->op1 & emu->op2);
            CONDITIONAL_SET_FLAG(bc & 0x8000, F_CF);
            CONDITIONAL_SET_FLAG(XOR2(bc >> 14), F_OF);
            CONDITIONAL_SET_FLAG(bc & 0x8, F_AF);
            break;
        case d_cmp32:
        	CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
        	CONDITIONAL_SET_FLAG(!emu->res, F_ZF);
        	CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
        	bc = (emu->res & (~emu->op1 | emu->op2)) | (~emu->op1 & emu->op2);
        	CONDITIONAL_SET_FLAG(bc & 0x80000000, F_CF);
        	CONDITIONAL_SET_FLAG(XOR2(bc >> 30), F_OF);
        	CONDITIONAL_SET_FLAG(bc & 0x8, F_AF);
            break;
        case d_tst8:
        	CLEAR_FLAG(F_OF);
        	CONDITIONAL_SET_FLAG(emu->res & 0x80, F_SF);
        	CONDITIONAL_SET_FLAG(emu->res == 0, F_ZF);
        	CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
        	CLEAR_FLAG(F_CF);
            break;
        case d_tst16:
            CLEAR_FLAG(F_OF);
            CONDITIONAL_SET_FLAG(emu->res & 0x8000, F_SF);
            CONDITIONAL_SET_FLAG(emu->res == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            CLEAR_FLAG(F_CF);
            break;
        case d_tst32:
        	CLEAR_FLAG(F_OF);
        	CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
        	CONDITIONAL_SET_FLAG(emu->res == 0, F_ZF);
        	CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
        	CLEAR_FLAG(F_CF);
            break;
        case d_adc8:
            CONDITIONAL_SET_FLAG(emu->res & 0x100, F_CF);
            CONDITIONAL_SET_FLAG((emu->res & 0xff) == 0, F_ZF);
            CONDITIONAL_SET_FLAG(emu->res & 0x80, F_SF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            cc = (emu->op1 & emu->op2) | ((~emu->res) & (emu->op1 | emu->op2));
            CONDITIONAL_SET_FLAG(XOR2(cc >> 6), F_OF);
            CONDITIONAL_SET_FLAG(cc & 0x8, F_AF);
            break;
        case d_adc16:
            CONDITIONAL_SET_FLAG(emu->res & 0x10000, F_CF);
            CONDITIONAL_SET_FLAG((emu->res & 0xffff) == 0, F_ZF);
            CONDITIONAL_SET_FLAG(emu->res & 0x8000, F_SF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            cc = (emu->op1 & emu->op2) | ((~emu->res) & (emu->op1 | emu->op2));
            CONDITIONAL_SET_FLAG(XOR2(cc >> 14), F_OF);
            CONDITIONAL_SET_FLAG(cc & 0x8, F_AF);
            break;
        case d_adc32:
            if(emu->res == (emu->op1+emu->op2)) {
                lo = (emu->op1 & 0xFFFF) + (emu->op2 & 0xFFFF);
            } else {
                lo = 1 + (emu->op1 & 0xFFFF) + (emu->op2 & 0xFFFF);
            }
            hi = (lo >> 16) + (emu->op1 >> 16) + (emu->op2 >> 16);
            CONDITIONAL_SET_FLAG(hi & 0x10000, F_CF);
            CONDITIONAL_SET_FLAG(!emu->res, F_ZF);
            CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            cc = (emu->op2 & emu->op1) | ((~emu->res) & (emu->op2 | emu->op1));
            CONDITIONAL_SET_FLAG(XOR2(cc >> 30), F_OF);
            CONDITIONAL_SET_FLAG(cc & 0x8, F_AF);
            break;
        case d_sbb8:
            CONDITIONAL_SET_FLAG(emu->res & 0x80, F_SF);
            CONDITIONAL_SET_FLAG((emu->res & 0xff) == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            bc = (emu->res & (~emu->op1 | emu->op2)) | (~emu->op1 & emu->op2);
            CONDITIONAL_SET_FLAG(bc & 0x80, F_CF);
            CONDITIONAL_SET_FLAG(XOR2(bc >> 6), F_OF);
            CONDITIONAL_SET_FLAG(bc & 0x8, F_AF);
            break;
        case d_sbb16:
            CONDITIONAL_SET_FLAG(emu->res & 0x8000, F_SF);
            CONDITIONAL_SET_FLAG((emu->res & 0xffff) == 0, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            bc = (emu->res & (~emu->op1 | emu->op2)) | (~emu->op1 & emu->op2);
            CONDITIONAL_SET_FLAG(bc & 0x8000, F_CF);
            CONDITIONAL_SET_FLAG(XOR2(bc >> 14), F_OF);
            CONDITIONAL_SET_FLAG(bc & 0x8, F_AF);
            break;
        case d_sbb32:
            CONDITIONAL_SET_FLAG(emu->res & 0x80000000, F_SF);
            CONDITIONAL_SET_FLAG(!emu->res, F_ZF);
            CONDITIONAL_SET_FLAG(PARITY(emu->res & 0xff), F_PF);
            bc = (emu->res & (~emu->op1 | emu->op2)) | (~emu->op1 & emu->op2);
            CONDITIONAL_SET_FLAG(bc & 0x80000000, F_CF);
            CONDITIONAL_SET_FLAG(XOR2(bc >> 30), F_OF);
            CONDITIONAL_SET_FLAG(bc & 0x8, F_AF);
            break;
        case d_rol8:
            if(emu->op2 == 1) {
                CONDITIONAL_SET_FLAG((emu->res + (emu->res >> 7)) & 1, F_OF);
            }
        	CONDITIONAL_SET_FLAG(emu->res & 0x1, F_CF);
            break;
        case d_rol16:
            if(emu->op2 == 1) {
                CONDITIONAL_SET_FLAG((emu->res + (emu->res >> 15)) & 1, F_OF);
            }
        	CONDITIONAL_SET_FLAG(emu->res & 0x1, F_CF);
            break;
        case d_rol32:
            if(emu->op2 == 1) {
                CONDITIONAL_SET_FLAG((emu->res + (emu->res >> 31)) & 1, F_OF);
            }
        	CONDITIONAL_SET_FLAG(emu->res & 0x1, F_CF);
            break;
        case d_ror8:
            if(emu->op2 == 1) {
                CONDITIONAL_SET_FLAG(XOR2(emu->res >> 6), F_OF);
            }
            CONDITIONAL_SET_FLAG(emu->res & (1 << 7), F_CF);
            break;
        case d_ror16:
            if(emu->op2 == 1) {
                CONDITIONAL_SET_FLAG(XOR2(emu->res >> 14), F_OF);
            }
            CONDITIONAL_SET_FLAG(emu->res & (1 << 15), F_CF);
            break;
        case d_ror32:
            if(emu->op2 == 1) {
                CONDITIONAL_SET_FLAG(XOR2(emu->res >> 30), F_OF);
            }
            CONDITIONAL_SET_FLAG(emu->res & (1 << 31), F_CF);
            break;

        case d_unknown:
            printf_log(LOG_NONE, "Box86: %p trying to evaluate Unknown defered Flags\n", (void*)R_EIP);
            break;
    }
    RESET_FLAGS(emu);
}
