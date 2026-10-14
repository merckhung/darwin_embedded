#include "ps2_decode.h"
#include "hid_decode.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    NativePS2Decoder state = {0};
    NativeInputEvent event = {0};
    assert(native_ps2_keyboard(&state, 0x1e, &event) == 1 && event.code == 4 && event.value == 1);
    assert(native_ps2_keyboard(&state, 0x9e, &event) == 1 && event.code == 4 && event.value == 0);
    assert(!native_ps2_keyboard(&state, 0xe0, &event));
    assert(native_ps2_keyboard(&state, 0x4b, &event) == 1 && event.code == 80 && event.value == 1);
    assert(!native_ps2_keyboard(&state, 0xe0, &event));
    assert(native_ps2_keyboard(&state, 0xcb, &event) == 1 && event.code == 80 && event.value == 0);
    assert(!native_ps2_keyboard(&state, 0xe0, &event));
    assert(!native_ps2_keyboard(&state, 0x2a, &event)); /* fake PrintScreen shift */
    assert(!native_ps2_keyboard(&state, 0xfa, &event)); /* command ACK */
    const uint8_t pause[] = {0xe1,0x1d,0x45,0xe1,0x9d,0xc5};
    for (unsigned i = 0; i < sizeof(pause) - 1; ++i) assert(!native_ps2_keyboard(&state, pause[i], &event));
    assert(native_ps2_keyboard(&state, pause[5], &event) == 2 && event.code == 72 && event.value == 1);
    assert(!native_ps2_keyboard(&state, 0xe1, &event));
    assert(!native_ps2_keyboard(&state, 0x20, &event)); /* malformed Pause discarded */
    assert(native_ps2_keyboard(&state, 0x30, &event) == 1 && event.code == 5);
    assert(!native_ps2_mouse(&state, 0x00, &event)); /* loss of packet synchronization */
    assert(!native_ps2_mouse(&state, 0x29, &event));
    assert(!native_ps2_mouse(&state, 37, &event));
    assert(native_ps2_mouse(&state, 243, &event) == 1 && event.type == kNativeInputPointer &&
        event.code == 1 && event.value == 37 && event.value2 == 13);
    assert(!native_ps2_mouse(&state, 0x1c, &event));
    assert(!native_ps2_mouse(&state, 0xff, &event));
    assert(native_ps2_mouse(&state, 0x80, &event) == 1 && event.code == 4 && event.value == -1 && event.value2 == -128);
    assert(!native_ps2_mouse(&state, 0xcd, &event));
    assert(!native_ps2_mouse(&state, 0xff, &event));
    assert(native_ps2_mouse(&state, 0xff, &event) == 1 && event.code == 5 && !event.value && !event.value2);
    uint8_t previous[8] = {0};
    NativeInputEvent events[24];
    const uint8_t shifted_a[8] = {2,0,4};
    assert(native_hid_keyboard(previous, shifted_a, 8, events, 24) == 2);
    assert(events[0].code == 225 && events[0].value == 1 && events[1].code == 4 && events[1].value == 1);
    assert(!native_hid_keyboard(previous, shifted_a, 8, events, 24));
    const uint8_t rollover[8] = {0,0,1,1,1,1,1,1};
    const uint8_t duplicate[8] = {0,0,5,5};
    assert(!native_hid_keyboard(previous, rollover, 8, events, 24));
    assert(!native_hid_keyboard(previous, duplicate, 8, events, 24));
    assert(!native_hid_keyboard(previous, shifted_a, 7, events, 24));
    assert(memcmp(previous, shifted_a, 8) == 0); /* invalid reports retain real prior state */
    const uint8_t b[8] = {0,0,5};
    assert(native_hid_keyboard(previous, b, 8, events, 24) == 3);
    assert(events[0].code == 225 && !events[0].value && events[1].code == 4 && !events[1].value &&
        events[2].code == 5 && events[2].value == 1);
    const uint8_t old_six[8] = {0,0,4,5,6,7,8,9};
    const uint8_t new_six[8] = {255,0,40,41,42,43,44,45};
    memcpy(previous, old_six, 8);
    assert(!native_hid_keyboard(previous, new_six, 8, events, 19));
    assert(memcmp(previous, old_six, 8) == 0);
    assert(native_hid_keyboard(previous, new_six, 8, events, 20) == 20); /* eight modifiers + six up + six down */
    uint8_t buttons = 0;
    const uint8_t mouse[3] = {1,129,127};
    assert(native_hid_mouse(&buttons, mouse, 3, &event) == 1 && event.code == 1 && event.value == -127 && event.value2 == 127);
    const uint8_t stationary[3] = {1,0,0};
    assert(!native_hid_mouse(&buttons, stationary, 3, &event));
    assert(!native_hid_mouse(&buttons, stationary, 2, &event) && buttons == 1);
    puts("PASS PS/2 and HID keys, modifiers, rollover/malformed reports, bounded transitions and signed mouse vectors");
    return 0;
}
