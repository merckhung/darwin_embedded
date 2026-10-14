/* Translate real i8042 translated set-1 bytes; no emulated system calls. */
#ifndef NATIVE_PS2_DECODE_H
#define NATIVE_PS2_DECODE_H
#include "NativeBootInput.h"

typedef struct NativePS2Decoder {
    uint8_t extended, pause_index, mouse_index, mouse[3];
} NativePS2Decoder;

static inline uint8_t native_ps2_usage(uint8_t scan, int extended)
{
    if (extended) {
        switch (scan) {
        case 0x1c: return 88; case 0x1d: return 228; case 0x35: return 84;
        case 0x37: return 70; case 0x38: return 230; case 0x47: return 74;
        case 0x48: return 82; case 0x49: return 75; case 0x4b: return 80;
        case 0x4d: return 79; case 0x4f: return 77; case 0x50: return 81;
        case 0x51: return 78; case 0x52: return 73; case 0x53: return 76;
        case 0x5b: return 227; case 0x5c: return 231; case 0x5d: return 101;
        default: return 0; /* including PrintScreen's synthetic shift bytes */
        }
    }
    static const uint8_t usage[128] = {
        [0x01]=41,[0x02]=30,[0x03]=31,[0x04]=32,[0x05]=33,[0x06]=34,[0x07]=35,
        [0x08]=36,[0x09]=37,[0x0a]=38,[0x0b]=39,[0x0c]=45,[0x0d]=46,[0x0e]=42,
        [0x0f]=43,[0x10]=20,[0x11]=26,[0x12]=8,[0x13]=21,[0x14]=23,[0x15]=28,
        [0x16]=24,[0x17]=12,[0x18]=18,[0x19]=19,[0x1a]=47,[0x1b]=48,[0x1c]=40,
        [0x1d]=224,[0x1e]=4,[0x1f]=22,[0x20]=7,[0x21]=9,[0x22]=10,[0x23]=11,
        [0x24]=13,[0x25]=14,[0x26]=15,[0x27]=51,[0x28]=52,[0x29]=53,[0x2a]=225,
        [0x2b]=49,[0x2c]=29,[0x2d]=27,[0x2e]=6,[0x2f]=25,[0x30]=5,[0x31]=17,
        [0x32]=16,[0x33]=54,[0x34]=55,[0x35]=56,[0x36]=229,[0x37]=85,[0x38]=226,
        [0x39]=44,[0x3a]=57,[0x3b]=58,[0x3c]=59,[0x3d]=60,[0x3e]=61,[0x3f]=62,
        [0x40]=63,[0x41]=64,[0x42]=65,[0x43]=66,[0x44]=67,[0x45]=83,[0x46]=71,
        [0x47]=95,[0x48]=96,[0x49]=97,[0x4a]=86,[0x4b]=92,[0x4c]=93,[0x4d]=94,
        [0x4e]=87,[0x4f]=89,[0x50]=90,[0x51]=91,[0x52]=98,[0x53]=99,[0x56]=100,
        [0x57]=68,[0x58]=69,
    };
    return usage[scan];
}

/* Pause's set-1 sequence has no break. Return 2 for an immediate press/release. */
static inline int native_ps2_keyboard(NativePS2Decoder *state, uint8_t byte, NativeInputEvent *event)
{
    static const uint8_t pause[5] = {0x1d,0x45,0xe1,0x9d,0xc5};
    if (state->pause_index) {
        if (byte != pause[state->pause_index - 1]) { state->pause_index = 0; return 0; }
        if (++state->pause_index != 6) return 0;
        state->pause_index = 0;
        *event = (NativeInputEvent){0,kNativeInputKeyboard,72,1,0};
        return 2;
    }
    if (byte == 0xe1) { state->extended = 0; state->pause_index = 1; return 0; }
    if (byte == 0xe0) { state->extended = 1; return 0; }
    uint8_t usage = native_ps2_usage(byte & 0x7f, state->extended);
    state->extended = 0;
    if (!usage) return 0;
    *event = (NativeInputEvent){0,kNativeInputKeyboard,usage,!(byte & 0x80),0};
    return 1;
}

static inline int native_ps2_mouse(NativePS2Decoder *state, uint8_t byte, NativeInputEvent *event)
{
    if (!state->mouse_index && !(byte & 8)) return 0;
    state->mouse[state->mouse_index++] = byte;
    if (state->mouse_index != 3) return 0;
    state->mouse_index = 0;
    uint8_t flags = state->mouse[0];
    int32_t x = state->mouse[1] - ((flags & 0x10) ? 256 : 0);
    int32_t y = state->mouse[2] - ((flags & 0x20) ? 256 : 0);
    *event = (NativeInputEvent){0,kNativeInputPointer,(uint32_t)(flags & 7),
        (flags & 0x40) ? 0 : x, (flags & 0x80) ? 0 : -y};
    return 1;
}
#endif
