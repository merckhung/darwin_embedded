extern void native_module_event(int);
extern void native_module_close_hook(void);
extern int native_helper(int);
static int initialized;
__attribute__((constructor)) static void initialize(void) {
    initialized = 11;
    native_module_event(2);
}
__attribute__((destructor)) static void terminate(void) {
    native_module_close_hook();
    native_module_event(3);
}
int native_module(int value) { return native_helper(value) + initialized; }
__asm__(".globl _native_absolute_zero\n.set _native_absolute_zero, 0\n");
