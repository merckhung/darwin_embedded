#include <stdlib.h>
#include <errno.h>
#include "libelf.h"
#include "decl.h"

int _elf32_ehdr(Elf *elf, int inplace) { (void)elf; (void)inplace; return -1; }
int _elf32_phdr(Elf *elf, int inplace) { (void)elf; (void)inplace; return -1; }
int _elf32_shdr(Elf *elf, int inplace) { (void)elf; (void)inplace; return -1; }
int _elf32_cookscn(Elf_Scn *scn) { (void)scn; return -1; }

int _elf64_ehdr(Elf *elf, int inplace) { (void)elf; (void)inplace; return -1; }
int _elf64_phdr(Elf *elf, int inplace) { (void)elf; (void)inplace; return -1; }
int _elf64_shdr(Elf *elf, int inplace) { (void)elf; (void)inplace; return -1; }
int _elf64_cookscn(Elf_Scn *scn) { (void)scn; return -1; }

const char *elf_macho_str_off(size_t off) { (void)off; return NULL; }

#ifdef linux
int fls(int x) {
    return (x == 0 ? 0 : (int)(8 * sizeof(int) - __builtin_clz(x)));
}
#endif
