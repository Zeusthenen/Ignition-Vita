#ifndef __DYNAREC_ARM_MMU_H_
#define __DYNAREC_ARM_MMU_H_
/* =========================================================================
 * D2Vita — fastmmu : SORTIR L'ADD DE BASE DU CHEMIN CRITIQUE
 *
 * Deux interrupteurs SEPARES, lus par le GENERATEUR a la traduction (patron
 * box86_dynarec_callret) : un seul binaire porte les deux
 * jambes de chaque A/B, et defaut 0 = code d'avant, a l'instruction pres.
 *
 *   D2_MMUFOLD=1   (dyn86_mmufold)   base pliee dans les adressages ABSOLUS
 *   D2_MMUSTACK=<masque> (dyn86_mmustack)  chaine de PUSH/POP r32 consecutifs
 *                  bit 1 = POP  (raccourcit la CHAINE DEPENDANTE)
 *                  bit 2 = PUSH (ne retire QUE des instructions)
 *                  3 = les deux. Les deux bits sont SEPARES exprès : ils ne
 *                  font pas la meme promesse, et l'histoire du projet dit que
 *                  « moins d'instructions » ne se paie pas forcement en img/s
 *                  (D2_CPSRJCC, retire depuis : -5,1 % de code emis pour 0 %).
 *
 * --- LE CONSTAT ----------------------------------------------------------
 *
 * Sur Vita la memoire invitee n'est PAS a l'identite (D2ARENA, membase =
 * 0x41000000), donc chaque dereferencement x86 devient :
 *
 *     add  rT, rInvite, #membase      <-- 1 MAILLON de la chaine dependante
 *     ldr  rD, [rT, #dep]             <-- 1 MAILLON
 *
 * Le profil du 05/09 chiffre ce poste a 12,6 % des octets emis. La pente
 * d'horloge du meme jour (ARM 444 -> 333 MHz : -25 % d'horloge pour -24 %
 * d'images, elasticite 0,97) dit que le processeur est borne par le CALCUL,
 * pas par la memoire. Et D2_CPSRJCC a montre que retirer des instructions
 * ALU INDEPENDANTES ne rend rien : sur un Cortex-A9 a double emission elles
 * se glissent dans des creneaux libres. Ce qui borne est donc la LONGUEUR DE
 * LA CHAINE DEPENDANTE — et l'ADD de base y est, entre le calcul d'adresse
 * et le chargement.
 *
 * --- LE RECENSEMENT (tools/mmu_census.py, 300 blocs chauds, 05/09) --------
 *
 * 3 068 156 166 ADD fastmmu ponderes par les entrees de bloc, repartis par
 * FORME (c'est la forme qui dit ce qu'on peut retirer, et par quel moyen) :
 *
 *   forme  part    transformation                        maillon retire
 *   REGn   61,5 %  aucune : LDR [Rb,Rx] n'accepte pas     0
 *                  de deplacement immediat en plus
 *   REG0   29,7 %  ldr rD,[xBase, rInvite]                1, MAIS exige un
 *                                                         REGISTRE DEDIE
 *   IDX     6,6 %  ldr rD,[rT, rIdx, lsl #s]              1, sans registre
 *   ABS     2,2 %  mov32 rT,#(disp+membase)  = MMUFOLD    1, sans registre
 *
 * Et, transversalement : PUSH/POP r32 pesent 39,5 % de tous les ADD fastmmu,
 * dont 12,8 points sont des RAFALES (le meme registre de travail est
 * recalcule a l'identique d'un PUSH/POP au suivant).
 *
 * ⚠️ REG0 (29,7 %) est le plus gros poste et il est BLOQUE : la convention
 * box86/ARM n'a AUCUN registre libre — r0 = emu, r1/r2/r3 = travail,
 * r4..r11 = les 8 registres x86, r12 = xFlags, r13 = pile hote, r14 = xEIP
 * et 4e registre de travail, r15 = PC. Reserver une base couterait un
 * registre de travail (refonte de GETEB/GETEW/CALL_) ou xFlags (le poste le
 * plus chaud du profil). Ce document ne le fait pas ; il le CHIFFRE.
 *
 * =========================================================================
 * 1. D2_MMUFOLD — la base pliee dans les adressages ABSOLUS
 * =========================================================================
 *
 * geted() materialise deja par un MOV32 la constante d'un [disp32] ou d'un
 * [reg*s+disp32]. La constante ET dyn86_membase sont connues a la traduction,
 * donc MOV32(ret, disp + dyn86_membase) rend l'ADD de MMU_ADDR inutile.
 *
 *   avant :  movw r2,#0x544c / movt r2,#0x1cd / add r2,r2,#membase / ldr r1,[r2]
 *   apres :  movw r2,#0x544c / movt r2,#0x141 /                      ldr r1,[r2]
 *
 * UN MAILLON DE MOINS, et le maillon retire est celui qui separait la
 * constante du chargement : apres pliage l'adresse est prete des le movt.
 *
 * LE GARDE-FOU DE LA DOUBLE TRADUCTION. Le piege documente en tete de
 * GETEDO/GETEDO2 est d'appliquer MMU_ADDR PAR-DESSUS une adresse deja
 * traduite : la base serait ajoutee deux fois. Le protocole est donc opt-in,
 * en deux temps :
 *   - MMUFOLD_REQ() est pose JUSTE avant l'appel a geted(), uniquement dans
 *     les macros GETED* qui appliquent MMU_ADDR ensuite ;
 *   - geted() CONSOMME la demande a son entree (qu'il plie ou non) et repond
 *     par dyn->mmufold_done ;
 *   - MMU_ADDR_ED() — la variante utilisee par ces macros — saute l'ADD quand
 *     la reponse est vraie.
 * Consequence : un site qui ne pose pas la demande ne peut pas plier. Les
 * ~270 appels directs a geted() dans les fichiers d'opcodes (SSE, x87, LOCK,
 * LEA) utilisent MMU_ADDR, pas MMU_ADDR_ED, et gardent le comportement
 * d'avant SANS exception a auditer.
 *
 * Jamais quand getfixonly est demande (la constante repart telle quelle vers
 * l'appelant, p.ex. LEA), jamais quand le deplacement part dans *fixaddress.
 * INERTE si dyn86_membase == 0 : sans arene il n'y a rien a plier — un banc
 * sans D2ARENA ne prouve donc RIEN sur ce knob.
 *
 * HISTORIQUE. Ce levier a existe, a ete valide au pixel, puis retire par
 * 46f4989 : il n'avait ete mesure que NOYE dans un lot de cinq a -3,7 % dont
 * le suspect n°1 etait le cache de sauts indirects (1802 vidages). Il est
 * retabli SEUL, sous son propre interrupteur, avec ses propres compteurs, et
 * SANS le cache de sauts.
 *
 * =========================================================================
 * 2. D2_MMUSTACK — la chaine de PUSH/POP r32
 * =========================================================================
 *
 * MMU_PUSH1(reg, scr) emet :
 *     add scr, xESP, #membase        ; scr = ESP + base
 *     str reg, [scr, #-4]!           ; ecrit en ESP-4, ET scr -= 4
 *     sub xESP, xESP, #4             ; ESP = ESP-4
 * A la sortie, scr == ESP_nouveau + membase : l'ADD du PUSH SUIVANT
 * recalculerait exactement ce que scr contient deja.
 *
 * MMU_POP1(reg, scr) emet :
 *     add scr, xESP, #membase        ; scr = ESP + base
 *     ldr reg, [scr]                 ; lit en ESP
 *     add xESP, xESP, #4             ; ESP = ESP+4
 * A la sortie, scr == ESP_nouveau + membase - 4 : le POP suivant lit en
 * [scr, #4], celui d'apres en [scr, #8], etc.
 *
 *   avant (3 pop d'affilee)          apres
 *     add r1,r8,#membase               add r1,r8,#membase
 *     ldr fp,[r1]                      ldr fp,[r1]
 *     add r8,r8,#4                     add r8,r8,#4
 *     add r1,r8,#membase               ldr sl,[r1,#4]
 *     ldr sl,[r1]                      add r8,r8,#4
 *     add r8,r8,#4                     ldr r7,[r1,#8]
 *     add r1,r8,#membase               add r8,r8,#4
 *     ldr r7,[r1]
 *     add r8,r8,#4
 *     9 instructions                   7 instructions
 *
 * --- CE QUE CHAQUE MOITIE PROMET, ET CE QU'ELLE NE PROMET PAS ------------
 *
 * POP (bit 1) — RACCOURCIT LA CHAINE. Avant, le k-ieme chargement d'une
 * rafale est a 2k-1 maillons de la tete : add xESP -> add scr -> ldr ->
 * add xESP -> add scr -> ldr ... Apres, les k chargements ne dependent plus
 * QUE du premier `add scr` : ils sont a UN maillon, et independants entre
 * eux ; les `add xESP` sortent completement du chemin des chargements.
 *   rafale de 3 POP : profondeur 5 -> profondeur 1.
 *
 * PUSH (bit 2) — NE RETIRE QUE DES INSTRUCTIONS. Le `str [scr,#-4]!` met a
 * jour scr lui-meme, donc le k-ieme rangement depend du (k-1)-ieme : la
 * profondeur reste k, exactement comme la chaine de `sub xESP` qu'elle
 * remplace. Une instruction de moins par maillon, PAS un maillon de moins.
 * C'est precisement le profil de D2_CPSRJCC, qui a rendu 0 % sur console :
 * ce bit est donc a mesurer SEPAREMENT, et a laisser eteint s'il ne rend
 * rien.
 *   (Une variante qui rendrait PUSH lui aussi de profondeur 1 — stores a
 *   deplacement constant sans reecriture, puis un seul `sub xESP,#4k` — a
 *   ete ecartee : elle laisse xESP perime pendant la rafale, ce qui n'est
 *   plus une transformation purement locale.)
 *
 * --- LES CINQ VERROUS DE SURETE ------------------------------------------
 *
 * (V1) OPT-IN PAR SITE. Seules les macros MMU_PUSH1_CH / MMU_POP1_CH — posees
 *      UNIQUEMENT sur PUSH r32 (0x50..0x57) et POP r32 (0x58..0x5F) —
 *      participent. Les ~40 autres appels a MMU_PUSH1/MMU_POP1 (PUSHA, POPA,
 *      PUSH ES, RET, prefixes 64/65/67...) sont inchanges a l'instruction
 *      pres et n'ont pas a etre audites.
 *
 * (V2) RIEN N'A ETE EMIS ENTRE LES DEUX. dyn->arm_size n'avance que par EMIT,
 *      et seulement aux passes 2 et 3. Memoriser arm_size juste apres un
 *      maillon et exiger l'egalite au maillon suivant est la preuve DIRECTE
 *      qu'aucun mot ARM n'a ete emis entre eux — donc que scr est intact.
 *      C'est plus fort qu'une enumeration de ce que la boucle d'arm_pass peut
 *      emettre entre deux instructions (fpu_purgecache, SMSTART, READFLAGS) :
 *      on ne raisonne pas sur la liste, on constate.
 *
 * (V3) PAS DE POINT D'ENTREE. (V2) ne dit rien d'un SAUT qui atterrirait sur
 *      le maillon : on entrerait avec scr indefini. D'ou, comme pour le
 *      signtag (S2), pred_sz == 1 && pred[0] == ninst-1 (unique predecesseur,
 *      celui d'avant) et x86.barrier == 0. En passe 0, pred[] n'existe pas
 *      encore et la chaine est donc TOUJOURS refusee — passe 0 n'emet rien,
 *      c'est sans consequence.
 *
 * (V4) MEME FAMILLE, MEME REGISTRE DE TRAVAIL. Un PUSH ne chaine jamais sur
 *      un POP (l'invariant de deplacement differe), et le registre memorise
 *      doit etre celui qu'on va utiliser. PUSH ESP (0x54) est EXCLU : il
 *      passe par un autre registre de travail (MOV_REG(x1,gd) puis
 *      MMU_PUSH1(x1,x2)). POP ESP (0x5C) est EXCLU : apres lui ESP vaut une
 *      valeur lue en memoire, l'invariant scr = ESP+base tombe.
 *
 * (V5) DEPLACEMENT BORNE. La chaine s'arrete a D2MMUST_MAXDELTA (128 octets,
 *      soit 32 POP), tres en deca des 4095 de LDR_IMM9. Au-dela, un nouveau
 *      maillon de tete est emis.
 *
 * Ce que le levier NE change PAS : les adresses lues et ecrites, a l'octet
 * pres ; la valeur finale de xESP ; l'ordre des acces memoire. C'est la meme
 * suite d'acces, avec moins d'instructions pour la calculer.
 * ========================================================================= */

#include <stdint.h>

/* Interrupteurs et compteurs : definis dans src/dynarec86/dyn86.c (unite
 * compilee UNE fois), declares aussi dans shim/debug.h. */
extern int dyn86_mmufold;
extern int dyn86_mmustack;
extern unsigned long dyn86_mmu_folds;       /* adressages absolus PLIES       */
extern unsigned long dyn86_mmu_sites;       /* sites GETED* passes par geted  */
extern unsigned long dyn86_mmu_foldable;    /* dont forme ABSOLUE = PLIABLES  */
                                            /* (verdict PUR, hors knob : la   */
                                            /* jambe temoin le publie aussi)  */
extern unsigned long dyn86_mmu_st_heads;    /* tetes de chaine PUSH/POP r32   */
extern unsigned long dyn86_mmu_st_links;    /* maillons chaines (ADD supprime)*/
extern unsigned long dyn86_mmu_st_pop;      /* dont POP  (1 MAILLON de moins) */
extern unsigned long dyn86_mmu_st_push;     /* dont PUSH (1 INSTRUCTION seule)*/
extern unsigned long dyn86_mmu_st_rej_pos;  /* refus : du code a ete emis     */
extern unsigned long dyn86_mmu_st_rej_pred; /* refus : cible de saut/barriere */
extern unsigned long dyn86_mmu_st_rej_kind; /* refus : autre famille/registre */

#define D2MMUST_PUSH        2       /* == le bit de D2_MMUSTACK */
#define D2MMUST_POP         1
#define D2MMUST_MAXDELTA    128

/* Comptage : SITES TRADUITS, en passe 3 uniquement (sinon compte x4). */
#if STEP == 3
#define D2MMUST_TALLY_KIND()   (++dyn86_mmu_st_rej_kind)
#define D2MMUST_TALLY_POS()    (++dyn86_mmu_st_rej_pos)
#define D2MMUST_TALLY_PRED()   (++dyn86_mmu_st_rej_pred)
#define D2MMUST_TALLY_HEAD()   (++dyn86_mmu_st_heads)
#define D2MMUST_TALLY_LINK(k)  do{ ++dyn86_mmu_st_links; \
        if((k)==D2MMUST_POP) ++dyn86_mmu_st_pop; else ++dyn86_mmu_st_push; }while(0)
#define D2MMU_TALLY_FOLD()     (++dyn86_mmu_folds)
#define D2MMU_TALLY_SITE()     (++dyn86_mmu_sites)
#define D2MMU_TALLY_ABLE()     (++dyn86_mmu_foldable)
#else
#define D2MMUST_TALLY_KIND()   ((void)0)
#define D2MMUST_TALLY_POS()    ((void)0)
#define D2MMUST_TALLY_PRED()   ((void)0)
#define D2MMUST_TALLY_HEAD()   ((void)0)
#define D2MMUST_TALLY_LINK(k)  ((void)0)
#define D2MMU_TALLY_FOLD()     ((void)0)
#define D2MMU_TALLY_SITE()     ((void)0)
#define D2MMU_TALLY_ABLE()     ((void)0)
#endif

/* Le maillon `ninst` peut-il chainer sur le precedent ? Rend le deplacement a
 * appliquer (>=0) ou -1 = « tete de chaine, emettre l'ADD ».
 * PURE au sens ou elle ne lit que dyn : le meme verdict sort aux passes 2 et
 * 3 tant que ces deux passes emettent la meme chose — et un ecart de taille
 * pass2/pass3 est de toute facon deja detecte et annule le bloc
 * (dynarec_arm.c). */
static inline int d2mmust_link(dynarec_arm_t* dyn, int ninst, int kind, int scr)
{
#if STEP == 0
    (void)dyn; (void)ninst; (void)kind; (void)scr;
    return -1;                      /* pred[] n'existe pas encore : jamais */
#else
    if(!(dyn86_mmustack & kind) || !dyn86_membase) return -1;
    if(ninst < 1 || !dyn->insts) return -1;
    if(dyn->mmust_ninst != ninst-1) { return -1; }
    if(dyn->mmust_kind != kind || dyn->mmust_scr != scr) {
        D2MMUST_TALLY_KIND();
        return -1;
    }
    if(dyn->mmust_pos != dyn->arm_size) {   /* (V2) */
        D2MMUST_TALLY_POS();
        return -1;
    }
    if(dyn->insts[ninst].pred_sz != 1 || !dyn->insts[ninst].pred
       || dyn->insts[ninst].pred[0] != ninst-1
       || dyn->insts[ninst].x86.barrier
       || !dyn->insts[ninst].x86.alive
       || !dyn->insts[ninst-1].x86.alive
       || !dyn->insts[ninst-1].x86.has_next
       || dyn->insts[ninst-1].x86.barrier_next) {   /* (V3) */
        D2MMUST_TALLY_PRED();
        return -1;
    }
    if(dyn->mmust_delta > D2MMUST_MAXDELTA) return -1;   /* (V5) */
    return dyn->mmust_delta;
#endif
}

/* Etat de chaine : remis a neuf au debut de CHAQUE passe (arm_pass). */
#define D2MMUST_RESET(d)  do{ (d)->mmust_ninst = -1; (d)->mmust_pos = -1; \
                              (d)->mmust_kind = 0; (d)->mmust_scr = -1;   \
                              (d)->mmust_delta = 0; }while(0)

#endif //__DYNAREC_ARM_MMU_H_
