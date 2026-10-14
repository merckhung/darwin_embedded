extern void native_module_event(int);
__attribute__((constructor)) static void initialize(void) { native_module_event(1); }
__attribute__((destructor)) static void terminate(void) { native_module_event(4); }
int native_helper(int value) { return value * 2; }
