/* Native IOKit input ABI. Keyboard codes are USB HID keyboard-page usages. */
#ifndef NATIVE_BOOT_INPUT_H
#define NATIVE_BOOT_INPUT_H
#include <stdint.h>

enum {
    kNativeInputConnection = 0,
    kNativeInputInformation = 0,
    kNativeInputRead = 1,
    kNativeInputVersion = 1,
    kNativeInputKeyboard = 1,
    kNativeInputPointer = 2,
    kNativeInputBatchSize = 32,
    kNativeInputQueueSize = 256,
};

typedef struct NativeInputEvent {
    uint64_t sequence;
    uint32_t type;
    uint32_t code; /* key usage, or pointer button bits: left=1, right=2, middle=4 */
    int32_t value; /* key down=1/up=0, or relative X */
    int32_t value2; /* pointer relative Y, positive down */
} NativeInputEvent;

typedef struct NativeInputInfo {
    uint32_t version;
    uint32_t capabilities; /* (1 << kNativeInputKeyboard) | (1 << kNativeInputPointer) */
    uint32_t queued;
    uint32_t capacity;
    uint64_t dropped;
} NativeInputInfo;

typedef struct NativeInputBatch {
    uint32_t version;
    uint32_t count;
    uint64_t dropped;
    NativeInputEvent events[kNativeInputBatchSize];
} NativeInputBatch;
#endif
