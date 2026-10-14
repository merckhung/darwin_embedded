extern void native_module_fork_hook(void);
static int initialized;
__attribute__((constructor)) static void initialize(void) {
    native_module_fork_hook();
    initialized = 83;
}
int native_forking(void) { return initialized; }
