/* Native entry points from Apple's matching libsyscall archive. */
#ifndef NATIVE_DYLD_CALLS_H
#define NATIVE_DYLD_CALLS_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
extern int __open_nocancel(const char *, int, int);
extern int __close_nocancel(int);
extern long __read_nocancel(int, void *, size_t);
extern long __write_nocancel(int, const void *, size_t);
extern long __pread_nocancel(int, void *, size_t, int64_t);
extern int64_t __lseek(int, int64_t, int);
extern int __fcntl_nocancel(int, int, void *);
extern void *__mmap(void *, size_t, int, int, int, int64_t);
extern int __munmap(void *, size_t);
extern int __mprotect(void *, size_t, int);
extern int __sysctl(const int *, unsigned, void *, size_t *, const void *, size_t);
extern int __getpid(void);
extern int csops(int, unsigned, void *, size_t);
extern int issetugid(void);
extern uint32_t thread_self_trap(void);
extern int swtch(void);
extern int *__error(void);
extern void _thread_set_tsd_base(void *);
extern _Noreturn void __exit(int);
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
void __bzero(void *, size_t);
#endif
