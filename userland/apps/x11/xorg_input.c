/* Native Xorg input through the real NativeBootInput IOUserClient. */
#include <dix-config.h>
#include "NativeBootInput.h"
#include <NativeIOKit.h>
#include <NativeIOKitMIG.h>
#include <mach/mach_host.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include "inputstr.h"
#include "mi.h"
#include "mipointer.h"
#include "exevents.h"
#include "xkbsrv.h"
#include "xserver-properties.h"
#include "extinit.h"
#include "os.h"
#include <string.h>

static io_connect_t connection;
static DeviceIntPtr keyboard, pointer;
static OsTimerPtr poll_timer;
static ValuatorMask *motion;
static uint64_t sequence, dropped;
static uint8_t down[256], buttons;

static Bool open_input(void)
{
    mach_port_t host = mach_host_self(), main = MACH_PORT_NULL;
    if (host_get_io_main(host, &main) != KERN_SUCCESS) { mach_port_deallocate(mach_task_self(), host); return FALSE; }
    mach_port_deallocate(mach_task_self(), host);
    io_string_t match = "<dict><key>IOProviderClass</key><string>NativeBootInput</string></dict>";
    io_iterator_t iterator = MACH_PORT_NULL;
    kern_return_t matching = KERN_FAILURE;
    kern_return_t result = io_service_get_matching_services_ool(main, match, strlen(match) + 1, &matching, &iterator);
    mach_port_deallocate(mach_task_self(), main);
    if (result != KERN_SUCCESS || matching != KERN_SUCCESS) return FALSE;
    io_service_t service = IOIteratorNext(iterator), other = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if (other) IOObjectRelease(other);
    if (!service || other) { if (service) IOObjectRelease(service); return FALSE; }
    result = IOServiceOpen(service, mach_task_self(), kNativeInputConnection, &connection);
    IOObjectRelease(service);
    if (result != KERN_SUCCESS) return FALSE;
    NativeInputInfo info;
    size_t length = sizeof(info);
    result = IOConnectCallStructMethod(connection, kNativeInputInformation, NULL, 0, &info, &length);
    if (result != KERN_SUCCESS || length != sizeof(info) || info.version != kNativeInputVersion ||
        info.capabilities != 6 || info.capacity != kNativeInputQueueSize) {
        IOServiceClose(connection); connection = 0; return FALSE;
    }
    sequence = 0; dropped = info.dropped;
    memset(down, 0, sizeof(down)); buttons = 0;
    return TRUE;
}

static void keyboard_control(DeviceIntPtr device, KeybdCtrl *control)
{
    (void)device;
    // Key repeat and modifier state are applied by the original XKB layer.
    // Physical indicator output has no native driver operation yet.
    static Bool reported;
    if (control->leds && !reported) { ErrorF("Native input: physical keyboard LEDs are unsupported\n"); reported = TRUE; }
}
static void pointer_control(DeviceIntPtr device, PtrCtrl *control)
{
    (void)device; (void)control;
    // Original DIX pointer acceleration applies this control in software.
}
static int keyboard_proc(DeviceIntPtr device, int operation)
{
    switch (operation) {
    case DEVICE_INIT: {
        // HID usage + 8 is the X keycode. Native keycodes supply canonical XKB
        // key names, so the original layouts/types/compatibility stay usable.
        return InitKeyboardDeviceStruct(device, NULL, NULL, keyboard_control) ? Success : BadAlloc;
    }
    case DEVICE_ON: device->public.on = TRUE; return Success;
    case DEVICE_OFF: case DEVICE_CLOSE: device->public.on = FALSE; memset(down, 0, sizeof(down)); return Success;
    default: return BadValue;
    }
}
static int pointer_proc(DeviceIntPtr device, int operation)
{
    switch (operation) {
    case DEVICE_INIT: {
        CARD8 map[4] = {0,1,2,3};
        Atom labels[3] = {XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT), XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE),
                          XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT)};
        Atom axes[2] = {XIGetKnownProperty(AXIS_LABEL_PROP_REL_X), XIGetKnownProperty(AXIS_LABEL_PROP_REL_Y)};
        if (!InitPointerDeviceStruct((DevicePtr)device, map, 3, labels, pointer_control, GetMotionHistorySize(), 2, axes) ||
            !InitValuatorAxisStruct(device, 0, axes[0], NO_AXIS_LIMITS, NO_AXIS_LIMITS, 0, 0, 0, Relative) ||
            !InitValuatorAxisStruct(device, 1, axes[1], NO_AXIS_LIMITS, NO_AXIS_LIMITS, 0, 0, 0, Relative) ||
            !InitPointerAccelerationScheme(device, PtrAccelDefault)) return BadAlloc;
        return Success;
    }
    case DEVICE_ON: device->public.on = TRUE; return Success;
    case DEVICE_OFF: case DEVICE_CLOSE: device->public.on = FALSE; buttons = 0; return Success;
    default: return BadValue;
    }
}

static CARD32 poll_input(OsTimerPtr timer, CARD32 now, void *argument)
{
    (void)timer; (void)now; (void)argument;
    for (unsigned read = 0; read < 16; ++read) {
        NativeInputBatch batch;
        size_t length = sizeof(batch);
        if (IOConnectCallStructMethod(connection, kNativeInputRead, NULL, 0, &batch, &length) != KERN_SUCCESS ||
            length != sizeof(batch) || batch.version != kNativeInputVersion || batch.count > kNativeInputBatchSize)
            FatalError("Xorg: native input read failed\n");
        if (batch.dropped != dropped) FatalError("Xorg: native input queue overflow; device state requires resynchronization\n");
        if (!batch.count) break;
        for (unsigned i = 0; i < batch.count; ++i) {
            NativeInputEvent *event = &batch.events[i];
            if (event->sequence <= sequence) FatalError("Xorg: invalid native input sequence\n");
            sequence = event->sequence;
            if (event->type == kNativeInputKeyboard) {
                if (event->code < 4 || event->code > 231 || (event->value != 0 && event->value != 1))
                    FatalError("Xorg: invalid native keyboard event\n");
                if (keyboard && keyboard->public.on && down[event->code] != event->value) {
                    down[event->code] = event->value;
                    QueueKeyboardEvents(keyboard, event->value ? KeyPress : KeyRelease, event->code + 8);
                }
            } else if (event->type == kNativeInputPointer) {
                if (event->code > 7) FatalError("Xorg: invalid native pointer buttons\n");
                if (!pointer || !pointer->public.on) continue;
                if (event->value || event->value2) {
                    valuator_mask_zero(motion); valuator_mask_set(motion, 0, event->value); valuator_mask_set(motion, 1, event->value2);
                    QueuePointerEvents(pointer, MotionNotify, 0, POINTER_RELATIVE | POINTER_ACCELERATE, motion);
                }
                static const uint8_t bits[3] = {1,4,2};
                for (unsigned button = 0; button < 3; ++button)
                    if ((event->code ^ buttons) & bits[button])
                        QueuePointerEvents(pointer, event->code & bits[button] ? ButtonPress : ButtonRelease, button + 1, 0, NULL);
                buttons = event->code;
            } else FatalError("Xorg: unknown native input event\n");
        }
    }
    mieqProcessInputEvents();
    return 5;
}

void InitInput(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!open_input()) FatalError("Xorg: privileged native keyboard/mouse service unavailable\n");
    // InitCoreDevices allocates master devices before this call, but their
    // keymaps initialize afterward. Set the native rules for masters/slaves
    // together, and let original XKB publish the actual _XKB_RULES_NAMES.
    XkbRMLVOSet rules;
    XkbInitRules(&rules, "nativehid", "pc105", "us", "", "");
    XkbSetRulesDflts(&rules);
    XkbFreeRMLVOSet(&rules, FALSE);
    if (!mieqInit() || !(motion = valuator_mask_new(2))) FatalError("Xorg: native input event storage unavailable\n");
    pointer = AddInputDevice(serverClient, pointer_proc, TRUE);
    keyboard = AddInputDevice(serverClient, keyboard_proc, TRUE);
    if (!pointer || !keyboard) FatalError("Xorg: native input devices unavailable\n");
    AssignTypeAndName(pointer, MakeAtom(XI_MOUSE, sizeof(XI_MOUSE)-1, TRUE), "Native Darwin mouse");
    AssignTypeAndName(keyboard, MakeAtom(XI_KEYBOARD, sizeof(XI_KEYBOARD)-1, TRUE), "Native Darwin keyboard");
    poll_timer = TimerSet(NULL, 0, 5, poll_input, NULL);
    if (!poll_timer) FatalError("Xorg: native input polling timer unavailable\n");
}
void ProcessInputEvents(void) { mieqProcessInputEvents(); }
void CloseInput(void)
{
    if (poll_timer) { TimerFree(poll_timer); poll_timer = NULL; }
    if (connection) { IOServiceClose(connection); connection = 0; }
    if (motion) valuator_mask_free(&motion);
    pointer = keyboard = NULL; mieqFini();
}
void ddxInputThreadInit(void) { /* This backend polls native RPCs on the server thread. */ }
void DDXRingBell(int volume, int pitch, int duration)
{
    (void)pitch; (void)duration;
    if (volume) ErrorF("Native input: audible bell is unsupported\n");
}
int SetDeviceMode(ClientPtr client, DeviceIntPtr device, int mode)
{ (void)client; (void)device; (void)mode; return BadMatch; }
int SetDeviceValuators(ClientPtr client, DeviceIntPtr device, int *values, int first, int count)
{ (void)client; (void)device; (void)values; (void)first; (void)count; return BadMatch; }
int ChangeDeviceControl(ClientPtr client, DeviceIntPtr device, xDeviceCtl *control)
{ (void)client; (void)device; (void)control; return BadMatch; }
int NewInputDeviceRequest(InputOption *options, InputAttributes *attributes, DeviceIntPtr *device)
{ (void)options; (void)attributes; *device = NULL; return BadValue; }
void DeleteInputDeviceRequest(DeviceIntPtr device)
{
    if (device == pointer) pointer = NULL;
    if (device == keyboard) keyboard = NULL;
    RemoveDevice(device, TRUE);
}
