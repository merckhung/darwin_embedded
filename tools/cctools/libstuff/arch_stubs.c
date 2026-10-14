#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stuff/bool.h>
#include <stuff/bytesex.h>

void *llvm_create_disasm(const char *TripleName, void *DisInfo, int TagType, void *GetOpInfo, void *SymbolLookUp) { return NULL; }
void llvm_disasm_dispose(void *DC) {}
int llvm_disasm_instruction(void *DC, uint8_t *Bytes, uint64_t Size, uint64_t Pc, char *OutString, size_t OutStringSize) { return 0; }
const char *llvm_disasm_version_string(void) { return "none"; }
void llvm_disasm_set_options(void *DC, uint64_t Options) {}

void *objc_getClass(const char *name) { return NULL; }
void *objc_getMetaClass(const char *name) { return NULL; }

