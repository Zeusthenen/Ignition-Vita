#define INIT    
#define FINI        \
    if(ninst)       \
        addInst(dyn->instsize, &dyn->insts_size, dyn->insts[ninst].x86.size, dyn->insts[ninst].size/4); \
    addInst(dyn->instsize, &dyn->insts_size, 0, 0);
#define EMIT(A)                                         \
    do{                                                 \
        if(box86_dynarec_dump) print_opcode(dyn, ninst, (uint32_t)(A)); \
        /* D2Vita (audit T12 §12.4 entree 4) — CETTE BORNE ETAIT MORTE.       \
         * Le test portait sur dyn->next, mis a NULL en dynarec_arm.c:551      \
         * (« no need for next and jmps anymore ») AVANT que pass3 ne tourne.  \
         * Sur ARM32 la borne valait donc (uintptr_t)0 - 4 = 0xFFFFFFFC : le   \
         * test etait une tautologie et CHAQUE EMIT ecrivait sans aucune       \
         * limite. Une divergence pass2/pass3 debordait droit dans la marque   \
         * d'allocation du bloc voisin.                                        \
         * Le champ manifestement voulu est jmp_next, affecte juste avant      \
         * pass3 (dynarec_arm.c:605) et jusqu'ici MORT (deux occurrences dans  \
         * tout l'arbre : son affectation et sa declaration).                  \
         * Il vaut next+sizeof(void*), donc jmp_next-sizeof(void*) == next ==  \
         * p+arm_size : exactement la fin de la zone de code allouee.          \
         * Consequence : une divergence fait desormais SUPPRIMER l'emission de \
         * trop au lieu de deborder — et le detecteur de taille en aval        \
         * (dynarec_arm.c:650) annule le bloc. */                              \
        if((uintptr_t)dyn->block<(uintptr_t)dyn->jmp_next-sizeof(void*))\
            *(uint32_t*)(dyn->block) = (uint32_t)(A);   \
        dyn->block += 4; dyn->arm_size += 4;            \
        dyn->insts[ninst].size2 += 4;                   \
    }while(0)

#define MESSAGE(A, ...)  if(box86_dynarec_dump) dynarec_log(LOG_NONE, __VA_ARGS__)
#define NEW_INST        \
    if(ninst)                                                   \
        addInst(dyn->instsize, &dyn->insts_size, dyn->insts[ninst-1].x86.size, dyn->insts[ninst-1].size/4);
#define INST_EPILOG     
#define INST_NAME(name) inst_name_pass3(dyn, ninst, name)
