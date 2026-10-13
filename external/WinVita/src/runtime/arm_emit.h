// src/runtime/arm_emit.h — minimal ARMv7-A (A32) machine-code emitter.
//
// Just enough encoders to translate the x86 subset our first D2Common
// functions use. Emits little-endian 32-bit words into a caller buffer.
// Registers are ARM r0..r15 (r13=sp, r14=lr, r15=pc).
//
// This is shared by the shipping Vita dynarec and the desktop differential
// harness (which cross-compiles the emitter's output and runs it under
// qemu-arm to compare against the Unicorn x86 oracle).
#pragma once
#include <cstdint>
#include <vector>

namespace d2rt {

enum ArmReg { A_R0, A_R1, A_R2, A_R3, A_R4, A_R5, A_R6, A_R7,
              A_R8, A_R9, A_R10, A_R11, A_R12, A_SP, A_LR, A_PC };

// ARM condition codes.
enum ArmCond { C_EQ=0, C_NE=1, C_CS=2, C_CC=3, C_MI=4, C_PL=5, C_VS=6, C_VC=7,
               C_HI=8, C_LS=9, C_GE=10, C_LT=11, C_GT=12, C_LE=13, C_AL=14 };

class ArmEmit {
public:
    std::vector<uint32_t> code;

    uint32_t pos() const { return (uint32_t)code.size(); }   // word index
    void word(uint32_t w) { code.push_back(w); }

    // ---- data processing (imm rotate) ----
    // Encode an 8-bit immediate rotated by an even amount, if possible.
    static bool enc_imm(uint32_t v, uint32_t& out) {
        for (int r = 0; r < 16; ++r) {
            uint32_t rot = (v << (r * 2)) | (v >> (32 - r * 2));
            if ((rot & ~0xFFu) == 0) { out = (r << 8) | (rot & 0xFF); return true; }
        }
        return false;
    }

    // MOV Rd, #imm (uses MOVW/MOVT for arbitrary 32-bit values).
    void mov_imm(ArmReg rd, uint32_t imm) {
        uint32_t enc;
        if (enc_imm(imm, enc)) { word(0xE3A00000 | (rd << 12) | enc); return; }
        // MOVW Rd, #imm16
        uint16_t lo = imm & 0xFFFF, hi = imm >> 16;
        word(0xE3000000 | ((lo & 0xF000) << 4) | (rd << 12) | (lo & 0xFFF));
        if (hi) word(0xE3400000 | ((hi & 0xF000) << 4) | (rd << 12) | (hi & 0xFFF)); // MOVT
    }
    void mov_reg(ArmReg rd, ArmReg rm) { word(0xE1A00000 | (rd << 12) | rm); }

    // ADD/SUB Rd, Rn, #imm  (imm must fit rotate encoding).
    bool add_imm(ArmReg rd, ArmReg rn, uint32_t imm) {
        uint32_t enc; if (!enc_imm(imm, enc)) return false;
        word(0xE2800000 | (rn << 16) | (rd << 12) | enc); return true;
    }
    bool sub_imm(ArmReg rd, ArmReg rn, uint32_t imm) {
        uint32_t enc; if (!enc_imm(imm, enc)) return false;
        word(0xE2400000 | (rn << 16) | (rd << 12) | enc); return true;
    }
    void add_reg(ArmReg rd, ArmReg rn, ArmReg rm) { word(0xE0800000 | (rn<<16)|(rd<<12)|rm); }
    void sub_reg(ArmReg rd, ArmReg rn, ArmReg rm) { word(0xE0400000 | (rn<<16)|(rd<<12)|rm); }
    void and_reg(ArmReg rd, ArmReg rn, ArmReg rm) { word(0xE0000000 | (rn<<16)|(rd<<12)|rm); }
    void orr_reg(ArmReg rd, ArmReg rn, ArmReg rm) { word(0xE1800000 | (rn<<16)|(rd<<12)|rm); }
    void eor_reg(ArmReg rd, ArmReg rn, ArmReg rm) { word(0xE0200000 | (rn<<16)|(rd<<12)|rm); }

    // ---- loads/stores, 32-bit, immediate offset (signed) ----
    void ldr(ArmReg rt, ArmReg rn, int32_t off) {
        uint32_t u = off >= 0 ? 1 : 0; uint32_t a = off >= 0 ? off : -off;
        word(0xE5100000 | (u << 23) | (rn << 16) | (rt << 12) | (a & 0xFFF));
    }
    void str(ArmReg rt, ArmReg rn, int32_t off) {
        uint32_t u = off >= 0 ? 1 : 0; uint32_t a = off >= 0 ? off : -off;
        word(0xE5000000 | (u << 23) | (rn << 16) | (rt << 12) | (a & 0xFFF));
    }
    // 8/16-bit variants (zero-extend loads) — byte:
    void ldrb(ArmReg rt, ArmReg rn, int32_t off) {
        uint32_t u = off >= 0 ? 1 : 0; uint32_t a = off >= 0 ? off : -off;
        word(0xE5500000 | (u << 23) | (rn << 16) | (rt << 12) | (a & 0xFFF));
    }
    void strb(ArmReg rt, ArmReg rn, int32_t off) {
        uint32_t u = off >= 0 ? 1 : 0; uint32_t a = off >= 0 ? off : -off;
        word(0xE5400000 | (u << 23) | (rn << 16) | (rt << 12) | (a & 0xFFF));
    }

    // ---- compares (set flags) ----
    void cmp_imm(ArmReg rn, uint32_t imm) {
        uint32_t enc; if (!enc_imm(imm, enc)) { mov_imm(A_R12, imm); cmp_reg(rn, A_R12); return; }
        word(0xE3500000 | (rn << 16) | enc);
    }
    void cmp_reg(ArmReg rn, ArmReg rm) { word(0xE1500000 | (rn << 16) | rm); }
    void tst_reg(ArmReg rn, ArmReg rm) { word(0xE1100000 | (rn << 16) | rm); }

    // ---- branches ----
    // Emit a conditional branch to word-index `target`; returns the word
    // index of the branch so it can be patched later if target unknown.
    uint32_t b_cond(ArmCond c, uint32_t target_word) {
        uint32_t here = pos();
        int32_t rel = (int32_t)target_word - (int32_t)here - 2;  // pipeline +8 = +2 words
        word(((uint32_t)c << 28) | 0x0A000000 | (rel & 0x00FFFFFF));
        return here;
    }
    void patch_b(uint32_t branch_word, uint32_t target_word) {
        int32_t rel = (int32_t)target_word - (int32_t)branch_word - 2;
        uint32_t w = code[branch_word];
        code[branch_word] = (w & 0xFF000000) | (rel & 0x00FFFFFF);
    }
    void bx_lr() { word(0xE12FFF1E); }
    void push(uint32_t reglist) { word(0xE92D0000 | (reglist & 0xFFFF)); }   // STMFD sp!
    void pop(uint32_t reglist)  { word(0xE8BD0000 | (reglist & 0xFFFF)); }   // LDMFD sp!
};

} // namespace d2rt
