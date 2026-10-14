int native_coalesced_value=43;
__attribute__((weak)) int native_weak_marker=1;
int *native_strong_address(void) {return &native_coalesced_value;}
