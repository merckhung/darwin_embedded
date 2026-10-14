__attribute__((weak)) int native_coalesced_value=17;
int *native_weak_a_address(void) {return &native_coalesced_value;}
