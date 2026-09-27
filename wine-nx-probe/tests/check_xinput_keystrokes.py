from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/xinput1_3/main.c').read_text()
start = source.index('static const int JS_STATE_OFF')
end = source.index('DWORD WINAPI DECLSPEC_HOTPATCH XInputGetCapabilities(', start)
fixture = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "xinput.h"
#define TRACE(...) ((void)0)
static XINPUT_STATE state;
static BOOL connected = TRUE;
static DWORD xinput_get_state(DWORD index, XINPUT_STATE *out)
{
    if (index || !connected) return ERROR_DEVICE_NOT_CONNECTED;
    *out = state;
    return ERROR_SUCCESS;
}
void WINAPI AcquireSRWLockExclusive(SRWLOCK *lock) { (void)lock; }
void WINAPI ReleaseSRWLockExclusive(SRWLOCK *lock) { (void)lock; }
'''
checks = r'''
static void expect(WORD key, WORD flags)
{
    XINPUT_KEYSTROKE event = {0};
    assert(XInputGetKeystroke(XUSER_INDEX_ANY, 0, &event) == ERROR_SUCCESS);
    assert(event.VirtualKey == key && event.Flags == flags && !event.UserIndex);
    assert(!event.Unicode && !event.HidCode);
}
int main(void)
{
    XINPUT_KEYSTROKE event;
    assert(XInputGetKeystroke(0, 0, &event) == ERROR_EMPTY);
    assert(XInputGetKeystroke(4, 0, &event) == ERROR_BAD_ARGUMENTS);
    assert(XInputGetKeystroke(1, 0, &event) == ERROR_DEVICE_NOT_CONNECTED);
    state.Gamepad.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START;
    expect(VK_PAD_START, XINPUT_KEYSTROKE_KEYDOWN);
    expect(VK_PAD_A, XINPUT_KEYSTROKE_KEYDOWN);
    assert(XInputGetKeystroke(0, 0, &event) == ERROR_EMPTY);
    state.Gamepad.wButtons = 0;
    expect(VK_PAD_START, XINPUT_KEYSTROKE_KEYUP);
    expect(VK_PAD_A, XINPUT_KEYSTROKE_KEYUP);
    state.Gamepad.bLeftTrigger = 255;
    expect(VK_PAD_LTRIGGER, XINPUT_KEYSTROKE_KEYDOWN);
    state.Gamepad.bLeftTrigger = 0;
    expect(VK_PAD_LTRIGGER, XINPUT_KEYSTROKE_KEYUP);
    state.Gamepad.sThumbLX = 32767;
    expect(VK_PAD_LTHUMB_RIGHT, XINPUT_KEYSTROKE_KEYDOWN);
    state.Gamepad.sThumbLX = -32768;
    expect(VK_PAD_LTHUMB_RIGHT, XINPUT_KEYSTROKE_KEYUP);
    expect(VK_PAD_LTHUMB_LEFT, XINPUT_KEYSTROKE_KEYDOWN);
    state.Gamepad.sThumbLX = 0;
    expect(VK_PAD_LTHUMB_LEFT, XINPUT_KEYSTROKE_KEYUP);
    assert(XInputGetKeystroke(0, 0, &event) == ERROR_EMPTY);
    connected = FALSE;
    assert(XInputGetKeystroke(0, 0, &event) == ERROR_DEVICE_NOT_CONNECTED);
    assert(XInputGetKeystroke(XUSER_INDEX_ANY, 0, &event) == ERROR_EMPTY);
    puts("XInput keystroke tests passed");
}
'''

with tempfile.TemporaryDirectory(prefix='autorun-xinput-') as directory:
    path = Path(directory)
    test = path / 'keystrokes.c'
    test.write_text(fixture + source[start:end] + checks)
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-sign-compare',
                    '-D__WINESRC__', '-D_WIN64', '-I' + str(root / 'include'),
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(test), '-o', str(path / 'keystrokes')], check=True)
    subprocess.run([str(path / 'keystrokes')], check=True)
