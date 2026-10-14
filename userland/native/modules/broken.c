extern void native_intentionally_missing_import(void);
extern void native_module_event(int);
__attribute__((constructor)) static void initialize(void) {
    native_module_event(99);
    native_intentionally_missing_import();
}
