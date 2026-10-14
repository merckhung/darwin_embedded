extern void native_module_event(int);
static int initialized;
__attribute__((constructor)) static void initialize(void) {
    native_module_event(5);
    initialized = 71;
}
int native_slow(void) { return initialized; }
