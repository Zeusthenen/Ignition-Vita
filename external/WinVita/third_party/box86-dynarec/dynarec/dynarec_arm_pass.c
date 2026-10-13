#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>

#include "debug.h"
#include "box86context.h"
#include "dynarec.h"
#include "emu/x86emu_private.h"
#include "emu/x86run_private.h"
#include "x86run.h"
#include "x86emu.h"
#include "box86stack.h"
#include "emu/x86run_private.h"
#include "x86trace.h"
#include "dynarec_arm.h"
#include "dynarec_arm_private.h"
#include "arm_printer.h"
#include "dynarec_arm_functions.h"
#include "dyn86_memintrin.h"   /* D2Vita : memcpy/memset natifs sans trap */
#include "dyn86_intrin.h"      /* D2Vita : intrinseques natives, table generique */
extern int dyn86_nopend;                 /* D2_NOPEND — contrat plus bas */
extern unsigned long dyn86_nopend_dropped, dyn86_nopend_kept, dyn86_nopend_seen;
#include "dynarec_arm_helper.h"
#include "custommem.h"
#include "elfloader.h"

#ifndef STEP
#error No STEP defined
#endif

/* ---- D2Vita : profil du code emis (-DD2_EMITPROF) ----------------------
 * Deux mecanismes distincts :
 *   1. l'ATTRIBUTION des octets, faite en PASSE 2 seulement (la passe qui
 *      mesure les tailles) — cout nul, une fois par bloc traduit ;
 *   2. le COMPTEUR D'EXECUTION, du vrai code ARM emis dans le prologue de
 *      chaque bloc, donc present dans les passes 2 ET 3 (pass2 le mesure,
 *      pass3 l'ecrit). Il doit etre IDENTIQUE dans les deux, d'ou MOV32_
 *      (forme a taille fixe) et une adresse gelee avant pass0.
 * Sans -DD2_EMITPROF, tout ci-dessous disparait : le binaire livre ne paie
 * ni instruction emise, ni lecture de knob. */
#ifdef D2_EMITPROF
#include "dyn86_emitprof.h"
#endif
#if defined(D2_EMITPROF) && STEP==2
#define D2EP_BEGIN()      d2ep_begin(dyn->d2ep_idx)
#define D2EP_SW(f)        d2ep_switch((f), dyn->arm_size)
#define D2EP_OPEN(a)      d2ep_openinst((const uint8_t*)DYN86_G2H(a), dyn->arm_size, (a))
#define D2EP_CLOSE(len)   d2ep_closeinst(dyn->arm_size, (int)(len))
#define D2EP_MARK(v)      int d2ep_m_##v = dyn->arm_size
#define D2EP_ACC(v,k)     d2ep_sub((k), dyn->arm_size - d2ep_m_##v, 1)
#define D2EP_END(x,n)     d2ep_end(dyn->d2ep_idx, dyn->arm_size, (int)(x), (int)(n))
#else
#define D2EP_BEGIN()      do{}while(0)
#define D2EP_SW(f)        do{}while(0)
#define D2EP_OPEN(a)      do{}while(0)
#define D2EP_CLOSE(len)   do{}while(0)
#define D2EP_MARK(v)      do{}while(0)
#define D2EP_ACC(v,k)     do{}while(0)
#define D2EP_END(x,n)     do{}while(0)
#endif

#if STEP == 0
#ifndef PROT_READ
#define PROT_READ 0x1
#endif
#endif

/* ---- D2Vita : D2_NOPEND — l'archivage des drapeaux vivant par PESSIMISME ---
 *
 * LE CONSTAT (docs/perf/profil_code_emis_20260905.md §4, levier L4).
 * box86 force `need_after |= X_PEND` sur la DERNIERE instruction de chaque
 * bloc, pour qu'un bloc SUIVANT qui lirait les drapeaux puisse les
 * materialiser par UpdateFlags. Ce X_PEND remonte par updateNeed et fait
 * archiver `op1/op2/res/df` — 1 movw + 4 STR — a presque chaque ALU.
 * Mesure : 7,87 Go d'octets ARM executes en archivage, soit ~1,97 MILLIARD
 * d'instructions, pour 19,7 millions de materialisations. Environ CENT
 * ecritures d'archivage pour UNE lecture : ~99 % ne sont jamais relues.
 *
 * POURQUOI CE LEVIER N'EST PAS DE LA FAMILLE DES QUINZE ZEROS.
 * Tout ce qui a rendu 0 % sur cette console retirait des ALU INDEPENDANTES,
 * qui se glissent dans les creneaux libres de la double emission du
 * Cortex-A9 (D2_CPSRJCC : -5,1 % de code emis, 861 M d'instructions en moins,
 * 0 % d'images). Ici ce sont des ECRITURES MEMOIRE : elles occupent le port
 * load/store, celui dont les LDR du travail utile ont besoin. Le verdict de
 * CPSRJCC ne s'y applique pas — ce qui ne veut pas dire que celui-ci sera
 * different, seulement qu'il doit etre mesure.
 *
 * MODES (dyn86_nopend) :
 *   0 = garder (DEFAUT, comportement box86 a l'instruction pres)
 *   1 = ne pas forcer quand le bloc finit par un CALL. Aucune convention x86
 *       ne fait lire au callee les drapeaux de son appelant, et son prologue
 *       les detruit de toute facon.
 *   3 = NE JAMAIS FORCER. INCORRECT PAR CONSTRUCTION : un bloc suivant qui lit
 *       les drapeaux materialisera un etat perime. Ce mode n'existe QUE pour
 *       mesurer le PLAFOND du levier avant d'investir dans sa correction.
 *       Une jambe en mode 3 n'est comparable que si la CHARGE est la meme :
 *       des drapeaux faux changent les branchements, donc le travail. Toute
 *       mesure en mode 3 doit publier `ring:` (dessins et sommets par image) a
 *       cote des img/s, et etre jetee si la charge a bouge.
 */
static int d2_nopend_is_call(dynarec_arm_t* dyn, int ninst)
{
    if(ninst < 0 || ninst >= dyn->size) return 0;
    uintptr_t a = dyn->insts[ninst].x86.addr;
    if(!a) return 0;
    const uint8_t* p = (const uint8_t*)DYN86_G2H(a);
    for(int k = 0; k < 4; ++k) {          /* sauter les prefixes */
        uint8_t o = *p;
        if(o==0x66||o==0x67||o==0xF0||o==0xF2||o==0xF3||
           o==0x2E||o==0x36||o==0x3E||o==0x26||o==0x64||o==0x65) { ++p; continue; }
        break;
    }
    if(*p == 0xE8 || *p == 0x9A) return 1;                       /* call rel32/far */
    if(*p == 0xFF) { uint8_t r = (p[1] >> 3) & 7; return (r==2 || r==3); } /* call r/m */
    return 0;
}
/* Rend 1 s'il faut FORCER X_PEND (comportement d'origine). */
static int d2_nopend_keep(dynarec_arm_t* dyn, int ninst)
{
    ++dyn86_nopend_seen;                 /* appels, TOUS modes confondus */
    if(!dyn86_nopend) return 1;
    if(dyn86_nopend == 3) { ++dyn86_nopend_dropped; return 0; }
    if(dyn86_nopend == 1 && d2_nopend_is_call(dyn, ninst)) {
        ++dyn86_nopend_dropped; return 0; }
    ++dyn86_nopend_kept; return 1;
}

uintptr_t arm_pass(dynarec_arm_t* dyn, uintptr_t addr)
{
    int ok = 1;
    int ninst = 0;
    int j32;
    uintptr_t ip = addr;
    uintptr_t init_addr = addr;
    MAYUSE(init_addr);
    int need_epilog = 1;
    // Clean up (because there are multiple passes)
    dyn->f.pending = 0;
    dyn->f.dfnone = 0;
    /* D2Vita (dynarec_arm_mmu.h) : etat fastmmu remis a neuf a CHAQUE passe.
     * La chaine PUSH/POP ne doit jamais survivre d'une passe a l'autre, et la
     * demande de pliage ne doit jamais etre heritee. */
    D2MMUST_RESET(dyn);
    dyn->mmufold_req = 0;
    dyn->mmufold_done = 0;
    dyn->forward = 0;
    dyn->forward_to = 0;
    dyn->forward_size = 0;
    dyn->forward_ninst = 0;
    #if STEP == 0
    memset(&dyn->insts[ninst], 0, sizeof(instruction_arm_t));
    #endif
    fpu_reset(dyn);
    int reset_n = -1;
    int stopblock = 2+(FindElfAddress(my_context, addr)?0:1); // if block is in elf_memory, it can be extended with bligblocks==2, else it needs 3    // ok, go now
    INIT;
#ifdef D2_EMITPROF
    D2EP_BEGIN();
    /* Compteur d'executions du bloc, 64 bits, incremente A CHAQUE ENTREE.
     * Place AVANT la sequence de budget : le BGT de celle-ci enjambe un
     * nombre FIXE d'instructions, il ne doit rien voir de nouveau entre lui
     * et sa cible. x1/x2 sont des registres de travail du traducteur (aucun
     * registre x86 ne leur est associe), libres a l'entree du bloc — la
     * sequence de budget se sert deja de x1 juste apres.
     * Non atomique : sous plusieurs fils on peut PERDRE des increments, on
     * n'en invente jamais. Le banc de profil est mono-coeur. */
    if(dyn->d2ep_idx >= 0) {
        D2EP_MARK(probe);
        uintptr_t d2ep_pc = (uintptr_t)&d2ep_blocks[dyn->d2ep_idx].exec;
        MOV32_(x1, d2ep_pc);
        LDR_IMM9(x2, x1, 0);
        ADDS_IMM8(x2, x2, 1);
        STR_IMM9(x2, x1, 0);
        LDR_IMM9(x2, x1, 4);
        ADC_IMM8(x2, x2, 0);
        STR_IMM9(x2, x1, 4);
        D2EP_ACC(probe, D2EP_S_PROBE);
    }
    D2EP_SW(D2EP_ORPHAN);
#endif
    // ---- D2Vita block-entry preemption budget (all passes) -----------------
    // 4-insn fast path: LDR/SUBS/STR the budget in the emu, skip while > 0.
    // Expiry path (10 insns): ip = block start (nothing of the block ran yet,
    // exactly resumable), dyn86_bbreak = 2, quit = 1, exit via arm_epilog.
    // This replaces the LinkNext-funnel quantum: blocks direct-link under the
    // scheduler, killing the arm_next round-trip on every block transition.
    // Internal loops inside one block still bypass the check — the exact same
    // residual as the old funnel (documented in cpu_box86.cpp).
    // D2_BUDGETTAIL=1 (box86_dynarec_budgettail) : le chemin d'expiration
    // quitte le prologue et va se poser en QUEUE de bloc. MEME CODE, autre
    // placement. Ce que ca change : en disposition historique, l'entree d'un
    // bloc traine 4 instructions utiles PUIS 10 instructions mortes que le BGT
    // enjambe — le premier vrai code du bloc commence 56 octets plus loin, soit
    // environ deux lignes d'I-cache chargees pour rien A CHAQUE ENTREE. Le
    // profil du code emis chiffre ce chemin a 9,1 % de TOUS les octets ARM
    // produits par le traducteur. Avec ~15 000 blocs et une entree de bloc deja
    // facturee ~283 cycles, c'est le meme genre d'effet que callret (+6,8 %
    // console / 0 % qemu) : NE RIEN ATTENDRE DE QEMU ici, l'emulation ne
    // modelise pas le cache.
    // Defaut 0 = disposition historique, a l'instruction pres.
    {
        D2EP_MARK(prolog);
        extern void arm_epilog(void);
        _Static_assert(offsetof(x86emu_t, dyn86_budget) < 4096,
                       "budget must be LDR_IMM9-addressable from xEmu");
        LDR_IMM9(x1, xEmu, offsetof(x86emu_t, dyn86_budget));
        SUBS_IMM8(x1, x1, 1);
        STR_IMM9(x1, xEmu, offsetof(x86emu_t, dyn86_budget));
        if(box86_dynarec_budgettail) {
            // Saut INVERSE (LE au lieu de GT) vers la queue du bloc. La cible
            // est une MARQUE au sens de MARK/GETMARK : elle est mesuree a la
            // passe precedente (pass2) et relue ici a l'emission (pass3). En
            // pass2 la marque vaut encore 0, donc l'offset calcule est faux —
            // sans consequence : Bcond fait UNE instruction quel que soit
            // l'offset, donc la TAILLE mesuree est exacte, et c'est la seule
            // chose que pass2 produise. Un ecart de taille pass2/pass3 est de
            // toute facon deja detecte et annule le bloc (dynarec_arm.c).
            j32 = dyn->budget_tail - (dyn->arm_size + 8);
            Bcond(cLE, j32);
        } else {
            EMIT(0xCA000000 | 9);       // BGT +10 insns (skip the expiry path)
            // arm_epilog's stm writes emu->ip FROM THE xEIP REGISTER (stale at
            // block entry) — so load the resume address into xEIP, not memory.
            // C10 (deep review): MOV32 is VARIABLE length — 1 insn when ~imm32
            // fits an imm8 (MVN form), else 2. The BGT above hardcodes a
            // 10-instruction expiry path, so a short encoding would make the
            // skip land INSIDE the block's first real instruction. MOV32_ is
            // the fixed-size (always 2) form: 2+1+1+1+1+2+1 = 9 insns + the pad
            // NOP = exactly 10, always.
            MOV32_(xEIP, init_addr);
            MOVW(x1, 2);
            STR_IMM9(x1, xEmu, offsetof(x86emu_t, dyn86_bbreak));
            MOVW(x1, 1);
            STR_IMM9(x1, xEmu, offsetof(x86emu_t, quit));
            MOV32_(x2, (uintptr_t)arm_epilog);
            BX(x2);
            EMIT(0xE1A00000);           // pad NOP: keep the 10-insn expiry length
        }
        D2EP_ACC(prolog, D2EP_S_PROLOG);
    }
    // ---- D2Vita : memcpy/memset du CRT servis SANS TRAP (D2_MEMINTRIN) -----
    // Contrat complet, mesure et garde-fous : src/dynarec86/dyn86_memintrin.h.
    // Le bloc qui COMMENCE a l'entree de memcpy/memset appelle un helper C
    // NATIF (comme div32/imul8 de box86 : pas de trap, pas de repartiteur).
    // Le helper rend 0 => on tombe dans le corps traduit d'origine, intact.
    // Defaut (dyn86_memintrin == 0) : PAS UNE INSTRUCTION emise ici.
    if(dyn86_memintrin
       && ((dyn86_mi_cpy_va && addr == dyn86_mi_cpy_va)
        || (dyn86_mi_set_va && addr == dyn86_mi_set_va))) {
        int is_set = (dyn86_mi_set_va && addr == dyn86_mi_set_va);
        // r0 = xEmu (invariant du dynarec), r1 = ESP invite.
        MOV_REG(x1, xESP);
        // CALL_ : scratch x3 pour l'adresse, valeur de retour rangee dans x1.
        // saveflags=1 => xFlags est sauve/relu autour de l'appel.
        CALL_(is_set ? (void*)dyn86_mi_set : (void*)dyn86_mi_copy, x1, 0);
        CMPS_IMM8(x1, 0);
        // Marque « corps traduit » : mesuree en pass2, relue en pass3 (meme
        // mecanisme que budget_tail). En pass2 la valeur est encore celle du
        // bloc precedent, donc l'offset calcule est faux — sans consequence :
        // Bcond fait UNE instruction quel que soit l'offset, et la TAILLE est
        // la seule chose que pass2 produise.
        j32 = dyn->memintrin_mark - (dyn->arm_size + 8);
        Bcond(cEQ, j32);                                  // 0 = repli fidele
        // Servi : EAX = dst, ecrit par le helper dans emu->regs[_AX].
        LDR_IMM9(xEAX, xEmu, offsetof(x86emu_t, regs[_AX]));
        ret_to_epilog(dyn, ninst);                        // le RET normal
        dyn->memintrin_mark = dyn->arm_size;
    }
    // ---- D2Vita : INTRINSEQUES NATIVES generiques (dyn86_intrin.h) ---------
    // Meme mecanisme que memintrin ci-dessus, mais TABULAIRE : n'importe
    // quelle fonction invitee dont on a prouve au desassemblage qu'aucun saut
    // n'entre dans son corps peut etre servie par un helper natif, sans trap.
    // C'est ce qui rouvre les cibles refusees le 06/09 au motif que « le trap
    // coute plus que le corps » (dessin_glide_20260906.md:143).
    // dyn86_intrin_find() est PUR (table figee avant la 1re traduction), donc
    // passes 2 et 3 emettent exactement la meme chose.
    // Eteint (dyn86_intrin_on == 0) : PAS UNE INSTRUCTION emise ici.
    else if(dyn86_intrin_on) {
        const dyn86_intrin_t* ie = dyn86_intrin_find(addr);
        if(ie) {
            // ⚡ Ranger les registres d'ENTREE dans emu : ils vivent dans
            // r4-r11 et ne descendent dans emu qu'a l'epilogue. Sans cela le
            // helper lit une valeur PERIMEE (cf. dyn86_intrin.h « ENTREES »).
            if(ie->inmask & DYN86_IR_AX) STR_IMM9(xEAX, xEmu, offsetof(x86emu_t, regs[_AX]));
            if(ie->inmask & DYN86_IR_CX) STR_IMM9(xECX, xEmu, offsetof(x86emu_t, regs[_CX]));
            if(ie->inmask & DYN86_IR_DX) STR_IMM9(xEDX, xEmu, offsetof(x86emu_t, regs[_DX]));
            if(ie->inmask & DYN86_IR_BX) STR_IMM9(xEBX, xEmu, offsetof(x86emu_t, regs[_BX]));
            if(ie->inmask & DYN86_IR_BP) STR_IMM9(xEBP, xEmu, offsetof(x86emu_t, regs[_BP]));
            if(ie->inmask & DYN86_IR_SI) STR_IMM9(xESI, xEmu, offsetof(x86emu_t, regs[_SI]));
            if(ie->inmask & DYN86_IR_DI) STR_IMM9(xEDI, xEmu, offsetof(x86emu_t, regs[_DI]));
            MOV_REG(x1, xESP);                 // r0 = xEmu (invariant), r1 = ESP invite
            CALL_((void*)ie->fn, x1, 0);
            CMPS_IMM8(x1, 0);
            // Meme marque que memintrin : pass2 la mesure, pass3 la relit. En
            // pass2 la valeur est encore celle du bloc precedent, donc l'offset
            // est faux — sans consequence, Bcond fait UNE instruction quel que
            // soit l'offset et la TAILLE est tout ce que pass2 produit.
            j32 = dyn->memintrin_mark - (dyn->arm_size + 8);
            Bcond(cEQ, j32);                   // 0 = repli sur le corps traduit
            // Ne recharger QUE les registres declares : tout le reste garde sa
            // valeur d'entree (conservateur ; cf. dyn86_intrin.h « REGISTRES »).
            if(ie->regmask & DYN86_IR_AX) LDR_IMM9(xEAX, xEmu, offsetof(x86emu_t, regs[_AX]));
            if(ie->regmask & DYN86_IR_CX) LDR_IMM9(xECX, xEmu, offsetof(x86emu_t, regs[_CX]));
            if(ie->regmask & DYN86_IR_DX) LDR_IMM9(xEDX, xEmu, offsetof(x86emu_t, regs[_DX]));
            if(ie->regmask & DYN86_IR_BX) LDR_IMM9(xEBX, xEmu, offsetof(x86emu_t, regs[_BX]));
            if(ie->regmask & DYN86_IR_BP) LDR_IMM9(xEBP, xEmu, offsetof(x86emu_t, regs[_BP]));
            if(ie->regmask & DYN86_IR_SI) LDR_IMM9(xESI, xEmu, offsetof(x86emu_t, regs[_SI]));
            if(ie->regmask & DYN86_IR_DI) LDR_IMM9(xEDI, xEmu, offsetof(x86emu_t, regs[_DI]));
            // stdcall : le RET depile lui-meme ses arguments.
            if(ie->retn) retn_to_epilog(dyn, ninst, ie->retn);
            else         ret_to_epilog(dyn, ninst);
            dyn->memintrin_mark = dyn->arm_size;
        }
    }
    #if STEP == 0
    uintptr_t cur_page = (addr)&~box86_pagesize;
    #endif
    while(ok) {
        #if STEP == 0
        if(cur_page != (addr)&~box86_pagesize) {
            cur_page = (addr)&~box86_pagesize;
            if(!(getProtection(addr)&PROT_READ)) {
                need_epilog = 1;
                break;
            }
        }
        #endif
        ip = addr;
        D2EP_OPEN(ip);
        fpu_propagate_stack(dyn, ninst);
        if (reset_n!=-1) {
            if(reset_n==-2) {
                MESSAGE(LOG_DEBUG, "Reset Caches to zero\n");
                dyn->f.dfnone = 0;
                dyn->f.pending = 0;
                fpu_reset(dyn);
            } else {
                fpu_reset_cache(dyn, ninst, reset_n);
                dyn->f = dyn->insts[reset_n].f_exit;
                if(dyn->insts[ninst].x86.barrier&BARRIER_FLOAT) {
                    MESSAGE(LOG_DEBUG, "Apply Barrier Float\n");
                    fpu_reset(dyn);
                }
                if(dyn->insts[ninst].x86.barrier&BARRIER_FLAGS) {
                    MESSAGE(LOG_DEBUG, "Apply Barrier Flags\n");
                    dyn->f.dfnone = 0;
                    dyn->f.pending = 0;
                }
            }
            reset_n = -1;
        }
        NEW_INST;
        #if STEP == 0
        if(ninst && dyn->insts[ninst-1].x86.barrier_next) {
            BARRIER(dyn->insts[ninst-1].x86.barrier_next);
        }
        #endif
        if(!ninst) {
            GOTEST(x1, x2);
        }
        if(dyn->insts[ninst].pred_sz>1) {SMSTART();}
        fpu_reset_scratch(dyn);
        if((dyn->insts[ninst].x86.need_before&~X_PEND) && !dyn->insts[ninst].pred_sz) {
            READFLAGS(dyn->insts[ninst].x86.need_before&~X_PEND);
        }
        if(box86_dynarec_test)  {
            MESSAGE(LOG_DUMP, "TEST INIT  ----\n");
            fpu_reflectcache(dyn, ninst, x1, x2, x3);
            MOV32(x1, ip);
            STM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP));
            STR_IMM9(x1, xEmu, offsetof(x86emu_t, ip));
            MOVW(x2, 1);
            CALL(x86test_init, -1, 0);
            fpu_unreflectcache(dyn, ninst, x1, x2, x3);
            MESSAGE(LOG_DUMP, "----------\n");
        }
#ifdef HAVE_TRACE
        else if(my_context->dec && box86_dynarec_trace) {
        if((trace_end == 0) 
            || ((ip >= trace_start) && (ip < trace_end)))  {
                MESSAGE(LOG_DUMP, "TRACE ----\n");
                fpu_reflectcache(dyn, ninst, x1, x2, x3);
                MOV32(x1, ip);
                STM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP));
                STR_IMM9(x1, xEmu, offsetof(x86emu_t, ip));
                MOVW(x2, 1);
                CALL(PrintTrace, -1, 0);
                fpu_unreflectcache(dyn, ninst, x1, x2, x3);
                MESSAGE(LOG_DUMP, "----------\n");
            }
        }
#endif

        addr = dynarec00(dyn, addr, ip, ninst, &ok, &need_epilog);

        INST_EPILOG;
        D2EP_CLOSE(addr - ip);

        int next = ninst+1;
        #if STEP > 0
        if(!dyn->insts[ninst].x86.has_next && dyn->insts[ninst].x86.jmp && dyn->insts[ninst].x86.jmp_insts!=-1)
            next = dyn->insts[ninst].x86.jmp_insts;
        if(dyn->insts[ninst].x86.has_next && dyn->insts[next].x86.barrier) {
            if(dyn->insts[next].x86.barrier&BARRIER_FLOAT)
                fpu_purgecache(dyn, ninst, 0, x1, x2, x3);
            if(dyn->insts[next].x86.barrier&BARRIER_FLAGS) {
                dyn->f.pending = 0;
                dyn->f.dfnone = 0;
            }
        }
        #endif
        #ifndef PROT_READ
        #define PROT_READ 1
        #endif
        #if STEP != 0
        if(!ok && !need_epilog && (addr < (dyn->start+dyn->isize))) {
            ok = 1;
            // we use the 1st predecessor here
            int ii = ninst+1;
            if(ii<dyn->size && !dyn->insts[ii].x86.alive) {
                while(ii<dyn->size && !dyn->insts[ii].x86.alive) {
                    // may need to skip opcodes to advance
                    ++ninst;
                    NEW_INST;
                    MESSAGE(LOG_DEBUG, "Skipping unused opcode\n");
                    INST_NAME("Skipped opcode");
                    INST_EPILOG;
                    addr += dyn->insts[ii].x86.size;
                    ++ii;
                }
            }
            if((dyn->insts[ii].x86.barrier&BARRIER_FULL)==BARRIER_FULL)
                reset_n = -2;    // hack to say Barrier!
            else {
                reset_n = getNominalPred(dyn, ii);  // may get -1 if no predecessor are availble
                if(reset_n==-1) {
                    reset_n = -2;
                    MESSAGE(LOG_DEBUG, "Warning, Reset Caches mark not found\n");
                }
            }
        }
        #else
        // check if block need to be stopped, because it's a 00 00 opcode (unreadeable is already checked earlier)
        if((ok>0) && !dyn->forward && !(*(uint8_t*)DYN86_G2H(addr)) && !(*(uint8_t*)DYN86_G2H(addr+1))) {
            if(box86_dynarec_dump) dynarec_log(LOG_NONE, "Stopping block at %p reason: %s\n", (void*)addr, "Next opcode is 00 00");
            ok = 0;
            need_epilog = 1;
        }
        if(dyn->forward) {
            if(dyn->forward_to == addr && !need_epilog && ok>=0) {
                // we made it!
                reset_n = get_first_jump(dyn, addr);
                if(box86_dynarec_dump) dynarec_log(LOG_NONE, "Forward extend block for %d bytes %s%p -> %p (ninst %d - %d)\n", dyn->forward_to-dyn->forward, dyn->insts[dyn->forward_ninst].x86.has_callret?"(opt. call) ":"", (void*)dyn->forward, (void*)dyn->forward_to, reset_n, ninst);
                if(dyn->insts[dyn->forward_ninst].x86.has_callret && !dyn->insts[dyn->forward_ninst].x86.has_next)
                    dyn->insts[dyn->forward_ninst].x86.has_next = 1;  // this block actually continue
                dyn->forward = 0;
                dyn->forward_to = 0;
                dyn->forward_size = 0;
                dyn->forward_ninst = 0;
                ok = 1; // in case it was 0
            } else if ((dyn->forward_to < addr) || ok<=0) {
                // something when wrong! rollback
                if(box86_dynarec_dump) dynarec_log(LOG_NONE, "Could not forward extend block for %d bytes %p -> %p\n", dyn->forward_to-dyn->forward, (void*)dyn->forward, (void*)dyn->forward_to);
                ok = 0;
                dyn->size = dyn->forward_size;
                ninst = dyn->forward_ninst;
                addr = dyn->forward;
                dyn->forward = 0;
                dyn->forward_to = 0;
                dyn->forward_size = 0;
                dyn->forward_ninst = 0;
            }
            // else just continue
        } else if(!ok && !need_epilog && box86_dynarec_bigblock && (getProtection(addr+3)&~PROT_READ))
            if(*(uint32_t*)DYN86_G2H(addr)!=0) {   // check if need to continue (but is next 4 bytes are 0, stop)
                uintptr_t next = get_closest_next(dyn, addr);
                if(next && (
                    (((next-addr)<15) && is_nops(dyn, addr, next-addr)) 
                    /*||(((next-addr)<30) && is_instructions(dyn, addr, next-addr))*/ ))
                {
                    ok = 1;
                    if(dyn->insts[ninst].x86.has_callret && !dyn->insts[ninst].x86.has_next) {
                        dyn->insts[ninst].x86.has_next = 1;  // this block actually continue
                    } else {
                        // need to find back that instruction to copy the caches, as previous version cannot be used anymore
                        // and pred table is not ready yet
                        reset_n = get_first_jump(dyn, next);
                    }
                    if(box86_dynarec_dump) dynarec_log(LOG_NONE, "Extend block %p, %s%p -> %p (ninst=%d, jump from %d)\n", dyn, dyn->insts[ninst].x86.has_callret?"(opt. call) ":"", (void*)addr, (void*)next, ninst, dyn->insts[ninst].x86.has_callret?ninst:reset_n);
                } else if(next && (next-addr)<box86_dynarec_forward && (getProtection(next)&PROT_READ)/*box86_dynarec_bigblock>=stopblock*/) {
                    if(!((box86_dynarec_bigblock<stopblock) && !isJumpTableDefault((void*)next))) {
                        if(dyn->forward) {
                            if(next<dyn->forward_to)
                                dyn->forward_to = next;
                            reset_n = -2;
                            ok = 1;
                        } else {
                            dyn->forward = addr;
                            dyn->forward_to = next;
                            dyn->forward_size = dyn->size;
                            dyn->forward_ninst = ninst;
                            reset_n = -2;
                            ok = 1;
                        }
                    }
                }
            }
        #endif
        if(ok<0)  {
            ok = 0; need_epilog=1; 
            #if STEP == 0
            if(ninst) {
                --ninst;
                if(!dyn->insts[ninst].x86.barrier) {
                    BARRIER(BARRIER_FLOAT);
                }
                if(d2_nopend_keep(dyn, ninst))
                    dyn->insts[ninst].x86.need_after |= X_PEND;
                ++ninst;
            }
            if(dyn->forward) {
                // stopping too soon
                dyn->size = dyn->forward_size;
                ninst = dyn->forward_ninst+1;
                addr = dyn->forward;
                dyn->forward = 0;
                dyn->forward_to = 0;
                dyn->forward_size = 0;
                dyn->forward_ninst = 0;
            }
            #endif
        }
        if((ok>0) && dyn->insts[ninst].x86.has_callret)
            reset_n = -2;
        ++ninst;
        #if STEP == 0
        memset(&dyn->insts[ninst], 0, sizeof(instruction_arm_t));
        if((ok>0) && (((box86_dynarec_bigblock<stopblock) && !isJumpTableDefault((void*)addr)) 
            || (addr>=box86_nodynarec_start && addr<box86_nodynarec_end)))
        #else
        if((ok>0) && (ninst==dyn->size))
        #endif
        {
            #if STEP == 0
            if(dyn->forward) {
                // stopping too soon
                dyn->size = dyn->forward_size;
                ninst = dyn->forward_ninst+1;
                addr = dyn->forward;
                dyn->forward = 0;
                dyn->forward_to = 0;
                dyn->forward_size = 0;
                dyn->forward_ninst = 0;
            }
            #endif
            int j32;
            MAYUSE(j32);
            MESSAGE(LOG_DEBUG, "Stopping block %p (%d / %d)\n",(void*)init_addr, ninst, dyn->size);
            if(!box86_dynarec_dump && addr>=box86_nodynarec_start && addr<box86_nodynarec_end)
                dynarec_log(LOG_INFO, "Stopping block in no-dynarec zone\n");
            --ninst;
            if(!dyn->insts[ninst].x86.barrier) {
                BARRIER(BARRIER_FLOAT);
            }
            #if STEP == 0
            if(d2_nopend_keep(dyn, ninst))
                dyn->insts[ninst].x86.need_after |= X_PEND;
            #endif
            ++ninst;
            NOTEST(x1, x3);
            fpu_purgecache(dyn, ninst, 0, x1, x2, x3);
            jump_to_next(dyn, addr, 0, ninst);
            ok=0; need_epilog=0;
        }
    }
    if(need_epilog) {
        NOTEST(x1, x3);
        fpu_purgecache(dyn, ninst, 0, x1, x2, x3);
        jump_to_epilog(dyn, ip, 0, ninst);  // no linker here, it's an unknow instruction
    }
    // D2_BUDGETTAIL=1 : la QUEUE du bloc — le chemin d'expiration du budget,
    // deplace ici depuis le prologue. Aucun chemin ne TOMBE dedans : le bloc se
    // termine juste au-dessus par jump_to_next ou jump_to_epilog, tous deux
    // inconditionnels. On n'y arrive que par le BLE du prologue.
    // Contenu IDENTIQUE a la version en ligne (le NOP de bourrage en moins : il
    // n'existait que pour tenir la longueur fixe de 10 qu'exigeait le BGT).
    // La marque est posee AVANT l'emission, donc elle vise bien la premiere
    // instruction de la queue ; elle sera relue par le prologue de la passe
    // suivante.
    if(box86_dynarec_budgettail) {
        extern void arm_epilog(void);
        dyn->budget_tail = dyn->arm_size;
        MOV32_(xEIP, init_addr);
        MOVW(x1, 2);
        STR_IMM9(x1, xEmu, offsetof(x86emu_t, dyn86_bbreak));
        MOVW(x1, 1);
        STR_IMM9(x1, xEmu, offsetof(x86emu_t, quit));
        MOV32_(x2, (uintptr_t)arm_epilog);
        BX(x2);
    }
    FINI;
    D2EP_END(dyn->isize, dyn->size);
    MESSAGE(LOG_DUMP, "---- END OF BLOCK ---- (%d)\n", dyn->size);
    return addr;
}