#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "initguid.h"
#include "wine/unixlib.h"

static NTSTATUS mock_unix_call( unsigned int code, void *args );
NTSTATUS WINAPI __wine_init_unix_call(void);
static int mock_swprintf( wchar_t *buffer, size_t count, const wchar_t *format, ... );
#undef WINE_UNIX_CALL
#define WINE_UNIX_CALL(code, args) mock_unix_call(code, args)
#define swprintf mock_swprintf
#include "../../dlls/dinput/joystick_nx.c"
#undef swprintf

static XINPUT_STATE pad_state;
static unsigned int events, axes, buttons, povs;
static BOOL connected = TRUE;
static NTSTATUS mock_unix_call( unsigned int code, void *args )
{
    struct nx_xinput_state_params *params = args;
    assert(code == nx_xinput_get_state && params->index == 0);
    params->connected = connected;
    params->state = pad_state;
    return STATUS_SUCCESS;
}
static int mock_swprintf( wchar_t *buffer, size_t count, const wchar_t *format, ... )
{ assert(count); buffer[0] = 0; (void)format; return 0; }
void WINAPI EnterCriticalSection( CRITICAL_SECTION *section ) { (void)section; }
void WINAPI LeaveCriticalSection( CRITICAL_SECTION *section ) { (void)section; }
DWORD WINAPI GetTickCount(void) { return 1; }
BOOL WINAPI SetEvent( HANDLE event ) { (void)event; return TRUE; }
void queue_event( IDirectInputDevice8W *iface, int index, DWORD value, DWORD time, DWORD sequence )
{ (void)iface; (void)value; (void)time; (void)sequence; assert(index < 16); events++; }

static HRESULT WINAPI unacquire( IDirectInputDevice8W *iface )
{
    struct dinput_device *device = CONTAINING_RECORD(iface, struct dinput_device, IDirectInputDevice8W_iface);
    device->status = STATUS_UNACQUIRED;
    return DI_OK;
}
static BOOL enum_object( struct dinput_device *device, UINT index, struct hid_value_caps *caps,
                         const DIDEVICEOBJECTINSTANCEW *object, void *context )
{
    unsigned int *count = context;
    (void)device;
    assert(!caps && index < 16);
    assert(object->dwOfs < DEVICE_STATE_MAX_SIZE);
    if (object->dwType & DIDFT_AXIS) axes++;
    if (object->dwType & DIDFT_BUTTON) buttons++;
    if (object->dwType & DIDFT_POV) povs++;
    (*count)++;
    return DIENUM_CONTINUE;
}

int main(void)
{
    static const IDirectInputDevice8WVtbl vtable = {.Unacquire = unacquire};
    struct object_properties props[16] = {{0}};
    struct dinput input = {0};
    struct dinput_device device = {.dinput = &input, .object_properties = props, .status = STATUS_ACQUIRED};
    DIPROPHEADER filter = {.dwSize = sizeof(filter), .dwHeaderSize = sizeof(filter), .dwHow = DIPH_DEVICE};
    IDirectInputDevice8W *iface = &device.IDirectInputDevice8W_iface;
    unsigned int count = 0, i;
    LONG previous, value;
    LONG state_values[6];

    iface->lpVtbl = &vtable;
    for (i = 0; i < 5; i++)
    {
        props[i].range_min = 0;
        props[i].range_max = 65535;
        props[i].saturation = 10000;
    }
    assert(nx_enum_objects(iface, &filter, DIDFT_ALL, enum_object, &count) == DIENUM_CONTINUE);
    assert(count == 16 && axes == 5 && buttons == 10 && povs == 1);
    filter.dwHow = DIPH_BYID;
    filter.dwObj = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(9);
    count = 0;
    nx_enum_objects(iface, &filter, DIDFT_ALL, enum_object, &count);
    assert(count == 1);
    assert(scale_axis(-32768, props) == 0);
    assert(scale_axis(32767, props) == 65535);
    assert(scale_axis(0, props) == 32768);
    props[0].deadzone = 2000;
    props[0].saturation = 8000;
    assert(scale_axis(-1000, props) == 32768 && scale_axis(1000, props) == 32768);
    previous = -1;
    for (value = -32768; value <= 32767; value++)
    {
        LONG scaled = scale_axis(value, props);
        assert(scaled >= previous && scaled <= 65535);
        previous = scaled;
    }
    props[0].deadzone = 10000;
    assert(scale_axis(32767, props) == 32768);
    props[0].deadzone = 0;
    props[0].saturation = 10000;

    pad_state.Gamepad.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT;
    pad_state.Gamepad.sThumbLX = 32767;
    pad_state.Gamepad.sThumbLY = 32767;
    pad_state.Gamepad.bLeftTrigger = 255;
    assert(nx_poll(iface) == DI_OK);
    memcpy(state_values, device.device_state, sizeof(state_values));
    assert(state_values[0] == 65535);
    assert(state_values[1] == 0);
    assert(state_values[2] == 65535);
    assert(state_values[5] == 4500);
    assert(device.device_state[24] == 0x80 && !device.device_state[25]);
    events = 0;
    assert(nx_poll(iface) == DI_OK && !events);
    connected = FALSE;
    assert(nx_poll(iface) == DIERR_INPUTLOST && device.status == STATUS_UNACQUIRED);
    puts("DirectInput controller tests passed");
}
