/* Decode USB HID boot reports into the native input ABI. */
#ifndef NATIVE_HID_DECODE_H
#define NATIVE_HID_DECODE_H
#include "NativeBootInput.h"

static inline unsigned native_hid_keyboard(uint8_t previous[8], const uint8_t *report,
    unsigned length, NativeInputEvent *events, unsigned capacity)
{
    if (!report || !events || length != 8 || capacity < 20 || report[1]) return 0;
    for (unsigned i = 2; i < 8; ++i) {
        if (report[i] && (report[i] < 4 || report[i] > 231)) return 0;
        for (unsigned j = 2; j < i; ++j) if (report[i] && report[i] == report[j]) return 0;
    }
    unsigned count = 0;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned bit = 1u << i;
        if ((report[0] ^ previous[0]) & bit)
            events[count++] = (NativeInputEvent){0,kNativeInputKeyboard,224u+i,!!(report[0] & bit),0};
    }
    for (unsigned i = 2; i < 8; ++i) {
        int still = 0;
        for (unsigned j = 2; j < 8; ++j) still |= previous[i] == report[j];
        if (previous[i] && !still) events[count++] = (NativeInputEvent){0,kNativeInputKeyboard,previous[i],0,0};
    }
    for (unsigned i = 2; i < 8; ++i) {
        int was = 0;
        for (unsigned j = 2; j < 8; ++j) was |= report[i] == previous[j];
        if (report[i] && !was) events[count++] = (NativeInputEvent){0,kNativeInputKeyboard,report[i],1,0};
    }
    for (unsigned i = 0; i < 8; ++i) previous[i] = report[i];
    return count;
}

static inline unsigned native_hid_mouse(uint8_t *previous, const uint8_t *report,
    unsigned length, NativeInputEvent *event)
{
    if (!report || !event || length < 3) return 0;
    uint32_t buttons = report[0] & 7;
    int32_t x = (int8_t)report[1], y = (int8_t)report[2];
    unsigned changed = buttons != *previous || x || y;
    *previous = buttons;
    if (changed) *event = (NativeInputEvent){0,kNativeInputPointer,buttons,x,y};
    return changed;
}
#endif
