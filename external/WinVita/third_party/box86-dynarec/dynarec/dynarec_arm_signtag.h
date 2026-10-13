#ifndef __DYNAREC_ARM_SIGNTAG_H_
#define __DYNAREC_ARM_SIGNTAG_H_
/* =========================================================================
 * D2Vita — « POINTEUR COMPLEMENTE, DISCRIMINE PAR LE SIGNE »
 * Interrupteur : D2_SIGNTAG=1 (dyn86_signtag). Defaut 0 = ancien code, a
 * l'instruction pres. Lu par le GENERATEUR a la traduction (patron
 * box86_dynarec_callret) : un seul binaire porte les deux jambes de l'A/B.
 *
 * --- CE QUE C'EST -------------------------------------------------------
 * La bibliotheque de listes chainees de Blizzard (Fog/Storm, presente dans
 * tout le monolithe 1.14d) range dans un meme champ 32 bits DEUX choses :
 *   - soit un DEPLACEMENT relatif, petit et POSITIF ;
 *   - soit un POINTEUR ABSOLU range COMPLEMENTE (`~p`).
 * Le discriminant est LE BIT DE SIGNE :
 *
 *     mov  esi, [eax+4]        ; le champ
 *     test esi, esi
 *     jge  .relatif            ; positif  -> deplacement
 *     not  esi                 ; negatif  -> ~p, on retrouve p
 *   .relatif:
 *     ...
 *     mov  [esi], edi          ; puis on DEREFERENCE
 *
 * C'est correct tant que tout pointeur invite est < 0x80000000 : alors
 * `~p >= 0x80000000`, donc NEGATIF. Des que la memoire invitee vit au-dessus
 * de 2 Gio (modele A L'IDENTITE sur Vita, D2LAYOUT=haut), `~p < 0x80000000`
 * devient POSITIF : la branche « deplacement » est prise, l'adresse fabriquee
 * est du bruit, et l'ecriture qui suit tombe n'importe ou. Ce n'est PAS un
 * validateur qui refuse : c'est un encodage qui se TROMPE en silence.
 *
 * --- CE QUE CET INTERRUPTEUR FAIT ---------------------------------------
 * Il remplace le discriminant « bit 31 » par « bit 30 » sur les sites
 * RECONNUS, et sur eux seuls :
 *
 *     avant :  tst rX, rX      ; branche si N == 0   (bit 31)
 *     apres :  tst rX, #0x40000000 ; branche si Z == 1 (bit 30 nul)
 *
 * Pourquoi le bit 30 separe encore les deux domaines : `~p` a son bit 30 a 1
 * exactement quand `p` l'a a 0, c'est-a-dire pour tout plan invite hors de
 * [0x40000000, 0x80000000) — ce qui couvre le plan haut (0x81000000+) COMME le
 * plan compact (0x00500000..0x11900000). Les deplacements, eux, sont des
 * tailles de champ ou des differences a l'interieur d'une meme structure :
 * tres petits, bit 30 a 0. Le predicat « bit 30 mis => pointeur complemente »
 * est donc equivalent au predicat d'origine SUR CES PLANS MEMOIRE.
 *
 * ⚠ CE N'EST PAS UNE CORRECTION GENERALE, ET IL FAUT LE DIRE.
 *  - Le predicat tombe des qu'un pointeur invite atterrit dans
 *    [0x40000000, 0x80000000) : le discriminant s'inverserait en silence.
 *  - Et surtout, il ne couvre QUE ce motif-la. Le meme champ est teste
 *    ailleurs sous d'autres formes — notamment un test TERNAIRE
 *    (`test rX,rX ; jle` : 0 = fin de liste, positif = deplacement, negatif =
 *    ~pointeur), ou un simple echange de bit ne suffit plus. C'est ce
 *    motif-la qui tue le plan haut, a 0x44f8e5 (docs/perf/identite_20260905.md).
 * L'interrupteur est donc un INSTRUMENT DE MESURE, pas un correctif :
 * rt_boot refuse de l'armer hors D2LAYOUT=haut.
 *
 * --- LES VERROUS DE SURETE ----------------------------------------------
 * (S1) MOTIF EXACT, sur les OCTETS x86 : l'instruction precedente est
 *      `TEST rX,rX` (0x85, mod=11, reg==rm), la courante est `JGE rel8`
 *      (0x7D) ou `JNS rel8` (0x79), et l'instruction QUI SUIT le Jcc (le
 *      chemin non pris) est `NOT rX` (0xF7 /2, mod=11) sur LE MEME registre.
 *      Aucun prefixe ne peut passer : les octets sont lus tels quels.
 * (S2) COUPLE EXCLUSIF (meme verrou que fastmmu, V3) : `pred_sz == 1 &&
 *      pred[0] == ninst-1`, pas de barriere, les deux instructions vivantes,
 *      `has_next`. Sans (S2) un saut pourrait atterrir sur le Jcc sans etre
 *      passe par le TEST, et le registre teste ne serait pas celui du motif.
 * (S3) EN PASSE 0, TOUJOURS REFUSE (pred[] n'existe pas encore). Le predicat
 *      est PUR : il ne lit que insts[] (fige apres pass0+updateNeed) et les
 *      octets x86, donc il rend le meme verdict aux passes 1, 2 et 3 — la
 *      condition pour que la taille mesuree en passe 2 soit celle emise en 3.
 * (S4) LES DRAPEAUX SONT POSES A L'IDENTIQUE. On garde le READFLAGS du Jcc et
 *      tout l'archivage differe du TEST : seule la SOURCE du dernier TST
 *      change. Un lecteur ulterieur de xFlags voit exactement ce qu'il
 *      voyait.
 * (S5) COMPTEURS. `vus` compte les motifs reconnus (verdict PUR, publie aussi
 *      par la jambe temoin), `reecrits` ceux effectivement changes. Une jambe
 *      armee qui publie reecrits=0 est un TEST VIDE.
 * ========================================================================= */

#include <stdint.h>

extern int dyn86_signtag;                 /* D2_SIGNTAG=1 */
extern unsigned long dyn86_signtag_seen;  /* motifs reconnus (verdict pur)  */
extern unsigned long dyn86_signtag_done;  /* motifs reecrits (bit 30)       */

typedef struct d2sign_s {
    int ok;    /* 1 = motif reconnu ET contexte sur */
    int seen;  /* 1 = motif d'octets reconnu (avant les verrous de contexte) */
    int reg;   /* numero de registre x86 (0..7) du motif */
} d2sign_t;

static inline d2sign_t d2sign_analyze(dynarec_arm_t* dyn, int ni)
{
    d2sign_t r; r.ok = 0; r.seen = 0; r.reg = -1;
#if STEP == 0
    (void)dyn; (void)ni;
    return r;
#else
    if(ni < 1 || ni >= dyn->size || !dyn->insts) return r;
    {
        const uint8_t* j = (const uint8_t*)DYN86_G2H(dyn->insts[ni].x86.addr);
        const uint8_t* t = (const uint8_t*)DYN86_G2H(dyn->insts[ni-1].x86.addr);
        const uint8_t* n;
        int reg;
        if(j[0] != 0x7D && j[0] != 0x79) return r;          /* JGE ib / JNS ib */
        if(t[0] != 0x85) return r;                          /* TEST Ed, Gd     */
        if((t[1] & 0xC0) != 0xC0) return r;                 /* mod = 11        */
        if(((t[1] >> 3) & 7) != (t[1] & 7)) return r;       /* reg == rm       */
        reg = t[1] & 7;
        /* chemin NON pris du Jcc = l'octet juste apres le rel8 */
        n = j + 2;
        if(n[0] != 0xF7) return r;                          /* grp3 Ed         */
        if(n[1] != (uint8_t)(0xD0 | reg)) return r;         /* /2 NOT, mod=11, meme registre */
        r.seen = 1; r.reg = reg;

        /* (S2) couple exclusif : rien ne peut arriver sur le Jcc autrement */
        if(!dyn->insts[ni-1].x86.alive || !dyn->insts[ni].x86.alive) return r;
        if(!dyn->insts[ni-1].x86.has_next)    return r;
        if(dyn->insts[ni-1].x86.barrier_next) return r;
        if(dyn->insts[ni].x86.barrier)        return r;
        if(dyn->insts[ni].pred_sz != 1 || !dyn->insts[ni].pred
           || dyn->insts[ni].pred[0] != ni-1) return r;

        r.ok = 1;
        return r;
    }
#endif
}

/* Comptage : SITES TRADUITS, en passe 3 uniquement (sinon x4). */
#if STEP == 3
#define D2SIGN_TALLY(s, done)                                            \
    do { if((s).seen) { ++dyn86_signtag_seen;                            \
                        if(done) ++dyn86_signtag_done; } } while(0)
#else
#define D2SIGN_TALLY(s, done) do{ (void)(s); (void)(done); }while(0)
#endif

#endif /* __DYNAREC_ARM_SIGNTAG_H_ */
