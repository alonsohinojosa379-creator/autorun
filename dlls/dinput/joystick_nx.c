/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winreg.h"
#include "winerror.h"
#include "hidusage.h"
#include "xinput.h"
#include "device_private.h"
#include "wine/unixlib.h"
#include "../xinput1_3/nx_pad.h"

static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
static BOOL nx_backend;
static const GUID instance_guid = {0x4e580000,0x4155,0x544f,{0x52,0x55,0x4e,0,0,0,0,1}};
static const WCHAR controller_name[] = L"Controller (Xbox 360 for Windows)";
static const WCHAR device_key[] = L"System\\CurrentControlSet\\Enum\\HID\\VID_045E&PID_028E&IG_00\\AUTORUN";

struct nx_joystick
{
    struct dinput_device base;
};

static const struct dinput_device_vtbl nx_joystick_vtbl;

static BOOL CALLBACK init_backend( INIT_ONCE *once, void *param, void **context )
{
    nx_backend = !__wine_init_unix_call();
    return TRUE;
}

BOOL nx_joystick_enabled(void)
{
    InitOnceExecuteOnce( &init_once, init_backend, NULL, NULL );
    return nx_backend;
}

static BOOL get_state( XINPUT_STATE *state )
{
    struct nx_xinput_state_params params = {0};
    if (WINE_UNIX_CALL(nx_xinput_get_state, &params) || !params.connected) return FALSE;
    if (state) *state = params.state;
    return TRUE;
}

static void register_controller( BOOL connected )
{
    static LONG registered = -1;
    static SRWLOCK lock = SRWLOCK_INIT;
    static const WCHAR class_guid[] = L"{745a17a0-74d3-11d0-b6fe-00a0c90f57da}";
    HKEY key;

    AcquireSRWLockExclusive( &lock );
    if (registered == connected) goto done;
    if (!connected)
    {
        RegDeleteKeyW( HKEY_LOCAL_MACHINE, device_key );
        registered = FALSE;
        goto done;
    }
    if (RegCreateKeyExW( HKEY_LOCAL_MACHINE, device_key, 0, NULL, REG_OPTION_VOLATILE,
                        KEY_SET_VALUE, NULL, &key, NULL )) goto done;
    /* WMI uses this identity to distinguish XInput pads from DirectInput-only devices. */
    RegSetValueExW( key, L"ClassGUID", 0, REG_SZ, (const BYTE *)class_guid, sizeof(class_guid) );
    RegSetValueExW( key, L"DeviceDesc", 0, REG_SZ, (const BYTE *)controller_name, sizeof(controller_name) );
    RegCloseKey( key );
    registered = TRUE;
done:
    ReleaseSRWLockExclusive( &lock );
}

HRESULT nx_joystick_enum_device( DWORD flags, DIDEVICEINSTANCEW *instance, DWORD version, int index )
{
    DIDEVICEINSTANCEW info = { .dwSize = instance->dwSize, .guidInstance = instance_guid };
    BOOL connected;

    if (index) return DIERR_DEVICENOTREG;
    connected = get_state( NULL );
    register_controller( connected );
    if (!connected || (flags & DIEDFL_FORCEFEEDBACK)) return S_FALSE;
    info.guidProduct = dinput_pidvid_guid;
    info.guidProduct.Data1 = MAKELONG(0x045e, 0x028e);
    info.dwDevType = DIDEVTYPE_HID | (version >= 0x0800 ? DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8)
                                                     : DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8));
    lstrcpyW( info.tszInstanceName, controller_name );
    lstrcpyW( info.tszProductName, controller_name );
    info.wUsagePage = HID_USAGE_PAGE_GENERIC;
    info.wUsage = HID_USAGE_GENERIC_GAMEPAD;
    memcpy( instance, &info, min(instance->dwSize, sizeof(info)) );
    return DI_OK;
}

static HRESULT nx_enum_objects( IDirectInputDevice8W *iface, const DIPROPHEADER *filter,
                                DWORD flags, enum_object_callback callback, void *context )
{
    static const GUID *axes[] = { &GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis };
    struct dinput_device *impl = CONTAINING_RECORD( iface, struct dinput_device, IDirectInputDevice8W_iface );
    DIDEVICEOBJECTINSTANCEW object = { .dwSize = sizeof(object) };
    unsigned int i;

    for (i = 0; i < 16; i++)
    {
        object.guidType = i < 5 ? *axes[i] : i == 5 ? GUID_POV : GUID_Button;
        object.dwOfs = i < 6 ? i * sizeof(LONG) : 24 + i - 6;
        object.dwType = i < 5 ? DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(i)
                             : i == 5 ? DIDFT_POV : DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i - 6);
        object.dwFlags = i < 5 ? DIDOI_ASPECTPOSITION : 0;
        object.wUsagePage = i < 6 ? HID_USAGE_PAGE_GENERIC : HID_USAGE_PAGE_BUTTON;
        object.wUsage = i < 5 ? HID_USAGE_GENERIC_X + i : i == 5 ? HID_USAGE_GENERIC_HATSWITCH : i - 5;
        swprintf( object.tszName, ARRAY_SIZE(object.tszName), i < 5 ? L"Axis %u" : i == 5 ? L"POV" : L"Button %u",
                  i < 5 ? i : i - 6 );
        if (flags != DIDFT_ALL && !(flags & DIDFT_GETTYPE(object.dwType))) continue;
        if (filter->dwHow == DIPH_BYOFFSET && filter->dwObj != object.dwOfs) continue;
        if (filter->dwHow == DIPH_BYID && (filter->dwObj & 0xffffff) != object.dwType) continue;
        if (filter->dwHow == DIPH_BYUSAGE && filter->dwObj != (DWORD)MAKELONG(object.wUsage, object.wUsagePage)) continue;
        if (callback( impl, i, NULL, &object, context ) == DIENUM_STOP) return DIENUM_STOP;
    }
    return DIENUM_CONTINUE;
}

static LONG scale_axis( LONG value, const struct object_properties *props )
{
    LONG minimum = props->range_min == DIPROPRANGE_NOMIN ? 0 : props->range_min;
    LONG maximum = props->range_max == DIPROPRANGE_NOMAX ? 65535 : props->range_max;
    LONGLONG center = ((LONGLONG)minimum + maximum + 1) / 2;
    LONG magnitude = value < 0 ? -value : value;
    LONG extent = value < 0 ? 32768 : 32767;
    LONG deadzone = extent * props->deadzone / 10000;
    LONG saturation = extent * props->saturation / 10000;

    if (props->calibration_mode == DIPROPCALIBRATIONMODE_RAW) return value;
    if (magnitude <= deadzone) return center;
    if (magnitude >= saturation) return value < 0 ? minimum : maximum;
    return center + (magnitude - deadzone) * ((value < 0 ? minimum : maximum) - center)
                    / (saturation - deadzone);
}

static HRESULT nx_poll( IDirectInputDevice8W *iface )
{
    static const WORD buttons[] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB};
    static const LONG pov[] = {-1, 0, 18000, -1, 27000, 31500, 22500, -1, 9000, 4500, 13500, -1, -1, -1, -1, -1};
    struct dinput_device *impl = CONTAINING_RECORD( iface, struct dinput_device, IDirectInputDevice8W_iface );
    XINPUT_STATE state;
    LONG values[6];
    DWORD timestamp = GetTickCount(), sequence;
    unsigned int i;
    BOOL changed = FALSE;

    if (!get_state( &state ))
    {
        IDirectInputDevice8_Unacquire( iface );
        return DIERR_INPUTLOST;
    }
    values[0] = state.Gamepad.sThumbLX;
    values[1] = -1 - state.Gamepad.sThumbLY;
    values[2] = (state.Gamepad.bLeftTrigger - state.Gamepad.bRightTrigger) * 32767 / 255;
    values[3] = state.Gamepad.sThumbRX;
    values[4] = -1 - state.Gamepad.sThumbRY;
    values[5] = pov[state.Gamepad.wButtons & 15];

    EnterCriticalSection( &impl->crit );
    if (impl->status != STATUS_ACQUIRED)
    {
        LeaveCriticalSection( &impl->crit );
        return DIERR_NOTACQUIRED;
    }
    sequence = InterlockedIncrement( (LONG *)&impl->dinput->evsequence );
    for (i = 0; i < 16; i++)
    {
        DWORD value = i < 5 ? scale_axis( values[i], impl->object_properties + i )
                           : i == 5 ? values[5] : (state.Gamepad.wButtons & buttons[i - 6]) ? 0x80 : 0;
        DWORD offset = i < 6 ? i * sizeof(LONG) : 24 + i - 6;
        if (i < 6)
        {
            DWORD previous;
            memcpy( &previous, impl->device_state + offset, sizeof(previous) );
            if (previous == value) continue;
            memcpy( impl->device_state + offset, &value, sizeof(value) );
        }
        else
        {
            if (impl->device_state[offset] == value) continue;
            impl->device_state[offset] = value;
        }
        queue_event( iface, i, value, timestamp, sequence );
        changed = TRUE;
    }
    if (changed && impl->hEvent) SetEvent( impl->hEvent );
    LeaveCriticalSection( &impl->crit );
    return DI_OK;
}

static HRESULT nx_acquire( IDirectInputDevice8W *iface )
{
    return get_state( NULL ) ? DI_OK : DIERR_INPUTLOST;
}

static HRESULT nx_unacquire( IDirectInputDevice8W *iface )
{
    return DI_OK;
}

static HRESULT nx_get_property( IDirectInputDevice8W *iface, DWORD property, DIPROPHEADER *header,
                                const DIDEVICEOBJECTINSTANCEW *object )
{
    switch (property)
    {
    case (DWORD_PTR)DIPROP_VIDPID:
        ((DIPROPDWORD *)header)->dwData = MAKELONG(0x045e, 0x028e);
        return DI_OK;
    case (DWORD_PTR)DIPROP_JOYSTICKID:
        ((DIPROPDWORD *)header)->dwData = 0;
        return DI_OK;
    case (DWORD_PTR)DIPROP_INSTANCENAME:
    case (DWORD_PTR)DIPROP_PRODUCTNAME:
        lstrcpyW( ((DIPROPSTRING *)header)->wsz, controller_name );
        return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}

static const struct dinput_device_vtbl nx_joystick_vtbl =
{
    .poll = nx_poll,
    .acquire = nx_acquire,
    .unacquire = nx_unacquire,
    .enum_objects = nx_enum_objects,
    .get_property = nx_get_property,
};

HRESULT nx_joystick_create_device( struct dinput *dinput, const GUID *guid, IDirectInputDevice8W **out )
{
    struct nx_joystick *impl;
    HRESULT hr;
    unsigned int i;

    if (!IsEqualGUID( guid, &instance_guid ) && !IsEqualGUID( guid, &GUID_Joystick )) return DIERR_DEVICENOTREG;
    if (!get_state( NULL )) return DIERR_DEVICENOTREG;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return DIERR_OUTOFMEMORY;
    dinput_device_init( &impl->base, &nx_joystick_vtbl, &instance_guid, dinput );
    nx_joystick_enum_device( 0, &impl->base.instance, dinput->dwVersion, 0 );
    impl->base.caps.dwDevType = impl->base.instance.dwDevType;
    impl->base.caps.dwFlags = DIDC_ATTACHED | DIDC_EMULATED | DIDC_POLLEDDEVICE | DIDC_POLLEDDATAFORMAT;
    if (FAILED(hr = dinput_device_init_device_format( &impl->base.IDirectInputDevice8W_iface )))
    {
        IDirectInputDevice8_Release( &impl->base.IDirectInputDevice8W_iface );
        return hr;
    }
    for (i = 0; i < 5; i++)
    {
        impl->base.object_properties[i].logical_min = -32768;
        impl->base.object_properties[i].logical_max = 32767;
        impl->base.object_properties[i].physical_min = -32768;
        impl->base.object_properties[i].physical_max = 32767;
        impl->base.object_properties[i].saturation = 10000;
    }
    *out = &impl->base.IDirectInputDevice8W_iface;
    return DI_OK;
}
