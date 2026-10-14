#ifndef _CRT_EXTERNS_H_
#define _CRT_EXTERNS_H_

#ifdef __cplusplus
extern "C" {
#endif

extern char ***_NSGetArgv(void);
extern int *_NSGetArgc(void);
extern char ***_NSGetEnviron(void);
extern char **_NSGetProgname(void);
extern void *_NSGetMachExecuteHeader(void);

#ifdef __cplusplus
}
#endif

#endif
