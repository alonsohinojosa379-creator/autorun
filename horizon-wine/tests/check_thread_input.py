#!/usr/bin/env python3
"""Exercise Horizon input ownership and cross-thread foreground transitions."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "dlls/ntdll/unix/horizon.c").read_text()


def declaration(name):
    start = source.index("struct " + name + "\n{")
    return source[start:source.index("\n};", start) + 3]


def function(name):
    start = re.search(r"^static [^\n]*\b" + name + r"\(", source, re.M).start()
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define horizon_trace(...) ((void)0)
'''
fixture += "\n".join(re.findall(
    r"^#define HORIZON_(?:STATUS_(?:SUCCESS|INVALID_HANDLE|NO_MEMORY|ACCESS_DENIED)|"
    r"REQ_SET_(?:FOCUS|ACTIVE)_WINDOW|CAPTURE_\w+|SET_CARET_\w+|CARET_STATE_\w+)\s+.*$",
    source, re.M))
fixture += "\n" + "\n".join(declaration(name) for name in (
    "horizon_server_request_header", "horizon_server_reply_header", "horizon_obj_locator",
    "horizon_rectangle", "horizon_shared_cursor", "horizon_desktop_shm",
    "horizon_input_shm", "horizon_thread_input",
    "horizon_get_thread_input_request", "horizon_get_thread_input_reply",
    "horizon_input_window_request", "horizon_input_window_reply",
    "horizon_set_foreground_window_request", "horizon_set_foreground_window_reply",
    "horizon_set_capture_window_request", "horizon_set_capture_window_reply",
    "horizon_set_caret_window_request", "horizon_set_caret_window_reply",
    "horizon_set_caret_info_request", "horizon_set_caret_info_reply"))
fixture += r'''
struct horizon_shared_object {
    unsigned long long seq, id;
    union { struct horizon_input_shm input; } shm;
};
static struct horizon_shared_object objects[16];
static unsigned char *horizon_session_data = (void *)objects;
static unsigned long long horizon_session_next_id;
static struct horizon_thread_input *horizon_thread_inputs, *horizon_foreground_input;
static unsigned int allocations, flushes;
static int fail_alloc;
static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
struct horizon_server_connection { unsigned int tid; int reply_fd; };
struct horizon_user_window { unsigned int handle, tid; };
static struct horizon_user_window windows[] = {{0x10030, 4}, {0x10044, 16}, {0x10048, 16}};
static unsigned char result[256];
#define HORIZON_IMDT_KEYBOARD 1
#define HORIZON_KBD_WM_KEYDOWN 0x100
#define HORIZON_KBD_WM_SYSKEYUP 0x105
struct horizon_input_message {
    unsigned int tid, device, raw_keyboard, msg;
    struct horizon_input_message *next;
};
static struct horizon_input_message *horizon_input_messages;
static unsigned int horizon_server_flush_session_range_locked(unsigned long long off,
                                                              unsigned long long size)
{
    assert(off % sizeof(*objects) == 0 && size == sizeof(*objects));
    assert(off / sizeof(*objects) < allocations);
    objects[off / sizeof(*objects)].seq += 2;
    flushes++;
    return 0;
}
static unsigned int horizon_server_alloc_shared_object_locked(unsigned long long size,
                                                              struct horizon_obj_locator *loc)
{
    if (fail_alloc) return HORIZON_STATUS_NO_MEMORY;
    assert(allocations < 16 && size == sizeof(struct horizon_input_shm));
    loc->offset = allocations++ * sizeof(*objects);
    loc->id = ++horizon_session_next_id;
    objects[allocations - 1].id = loc->id;
    return 0;
}
static struct horizon_shared_object *horizon_server_shared_object_locked(struct horizon_obj_locator loc)
{ return (void *)(horizon_session_data + loc.offset); }
static struct horizon_user_window *horizon_server_find_window_locked(unsigned int hwnd)
{
    for (unsigned int i = 0; i < 3; i++) if (windows[i].handle == hwnd) return &windows[i];
    return NULL;
}
static int horizon_server_write_reply(int fd, const void *data, size_t size, const void *extra, size_t extra_size)
{
    (void)fd; (void)extra;
    assert(!extra_size && size <= sizeof(result));
    assert(!pthread_mutex_trylock(&horizon_server_objects_mutex));
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    memset(result, 0, sizeof(result));
    memcpy(result, data, size);
    return 0;
}
'''

tests = r'''
static void set_window(unsigned int tid, unsigned int hwnd, int which)
{
    struct horizon_server_connection conn = {tid, 0};
    struct horizon_input_window_request req = {.handle = hwnd};
    horizon_server_handle_set_input_window(&conn, (void *)&req, which);
}
static struct horizon_set_foreground_window_reply foreground(unsigned int tid, unsigned int hwnd)
{
    struct horizon_server_connection conn = {tid, 0};
    struct horizon_set_foreground_window_request req = {.handle = hwnd};
    struct horizon_set_foreground_window_reply reply;
    horizon_server_handle_set_foreground_window(&conn, (void *)&req);
    memcpy(&reply, result, sizeof(reply));
    return reply;
}
int main(void)
{
    struct horizon_server_connection main_conn = {4, 0}, render_conn = {16, 0};
    struct horizon_get_thread_input_request get = {.tid = 4};
    struct horizon_get_thread_input_reply got;
    struct horizon_set_foreground_window_reply fg;
    struct horizon_set_capture_window_request capture = {.handle = 0x10030};
    struct horizon_set_caret_window_request caret = {.handle = 0x10030, .width = 1, .height = 10};
    struct horizon_set_caret_info_request info = {.flags = HORIZON_SET_CARET_HIDE, .hide = -1};
    struct horizon_thread_input *main_input, *render_input, *reused;

    fail_alloc = 1;
    assert(!horizon_server_thread_input_locked(4, 1) && !horizon_thread_inputs);
    fail_alloc = 0;
    horizon_server_handle_get_thread_input(&main_conn, (void *)&get);
    memcpy(&got, result, sizeof(got));
    assert(got.locator.id && !got.header.error);
    main_input = horizon_server_thread_input_locked(4, 0);
    assert(main_input && main_input->locator.id == got.locator.id);
    get.tid = 999;
    horizon_server_handle_get_thread_input(&main_conn, (void *)&get);
    memcpy(&got, result, sizeof(got));
    assert(!got.locator.id && allocations == 1);

    set_window(4, 0x10030, HORIZON_REQ_SET_ACTIVE_WINDOW);
    set_window(4, 0x10030, HORIZON_REQ_SET_FOCUS_WINDOW);
    fg = foreground(4, 0x10030);
    assert(!fg.header.error && !fg.previous && !fg.send_msg_old && !fg.send_msg_new);
    assert(horizon_server_input_shared_locked(0) == main_input->shared);
    horizon_server_handle_set_capture_window(&main_conn, (void *)&capture);
    horizon_server_handle_set_caret_window(&main_conn, (void *)&caret);
    horizon_server_handle_set_caret_info(&main_conn, (void *)&info);
    assert(!main_input->caret_hide && main_input->shared->capture == 0x10030);

    render_input = horizon_server_thread_input_locked(16, 1);
    assert(render_input != main_input && !render_input->shared->active && !render_input->shared->focus);
    fg = foreground(16, 0x10044);
    assert(!fg.header.error && fg.previous == 0x10030 && fg.send_msg_old && !fg.send_msg_new);
    assert(!main_input->shared->foreground && render_input->shared->foreground);
    assert(horizon_server_input_shared_locked(0) == render_input->shared);
    assert(!render_input->shared->active); /* No synchronous deactivation of the main thread. */
    set_window(16, 0x10044, HORIZON_REQ_SET_ACTIVE_WINDOW);
    set_window(16, 0x10048, HORIZON_REQ_SET_FOCUS_WINDOW);
    assert(main_input->shared->active == 0x10030 && main_input->shared->focus == 0x10030);
    assert(render_input->shared->active == 0x10044 && render_input->shared->focus == 0x10048);
    set_window(16, 0x10030, HORIZON_REQ_SET_ACTIVE_WINDOW);
    struct horizon_input_window_reply input_reply;
    memcpy(&input_reply, result, sizeof(input_reply));
    assert(input_reply.header.error == HORIZON_STATUS_ACCESS_DENIED);
    assert(render_input->shared->active == 0x10044);
    set_window(16, 0x9999, HORIZON_REQ_SET_FOCUS_WINDOW);
    memcpy(&input_reply, result, sizeof(input_reply));
    assert(input_reply.header.error == HORIZON_STATUS_INVALID_HANDLE);

    capture.handle = 0x10044;
    horizon_server_handle_set_capture_window(&render_conn, (void *)&capture);
    caret.handle = 0x10048;
    horizon_server_handle_set_caret_window(&render_conn, (void *)&caret);
    assert(main_input->shared->capture == 0x10030 && render_input->shared->capture == 0x10044);
    assert(main_input->shared->caret == 0x10030 && render_input->shared->caret == 0x10048);
    assert(main_input->caret_hide == 0 && render_input->caret_hide == 1);

    fg = foreground(16, 0x10030);
    assert(fg.previous == 0x10044 && !fg.send_msg_old && fg.send_msg_new);
    fg = foreground(4, 0x10044);
    assert(fg.previous == 0x10030 && !fg.send_msg_old && fg.send_msg_new);
    fg = foreground(4, 0x9999);
    assert(fg.header.error == HORIZON_STATUS_INVALID_HANDLE && horizon_foreground_input == render_input);
    horizon_server_release_input_locked(0);
    assert(horizon_foreground_input == render_input);

    struct horizon_desktop_shm desktop = {.keystate_serial = 7};
    struct horizon_input_message key = {.tid = 4, .device = HORIZON_IMDT_KEYBOARD,
                                        .msg = HORIZON_KBD_WM_KEYDOWN};
    desktop.keystate[0x10] = 0x80;
    horizon_input_messages = &key;
    horizon_server_sync_keystate_locked(main_input->shared, &desktop);
    horizon_server_sync_keystate_locked(render_input->shared, &desktop);
    assert(!main_input->shared->keystate[0x10] && render_input->shared->keystate[0x10] == 0x80);
    horizon_input_messages = NULL;
    horizon_server_sync_keystate_locked(main_input->shared, &desktop);
    assert(main_input->shared->keystate[0x10] == 0x80);
    assert(main_input->shared->keystate_serial == 7);
    main_input->shared->keystate[0x10] = 0;
    horizon_server_sync_keystate_locked(main_input->shared, &desktop);
    assert(!main_input->shared->keystate[0x10]);

    unsigned long long old_id = render_input->locator.id, old_offset = render_input->locator.offset;
    horizon_server_release_input_locked(16);
    assert(!horizon_foreground_input && !horizon_server_thread_input_locked(16, 0));
    assert(!horizon_server_shared_object_locked(render_input->locator)->id);
    reused = horizon_server_thread_input_locked(20, 1);
    assert(reused == render_input && allocations == 2);
    assert(reused->locator.id != old_id && reused->locator.offset == old_offset);
    assert(!reused->shared->focus && !reused->shared->capture && !reused->shared->caret && !reused->caret_hide);
    assert(!reused->shared->keystate[0x10] && !reused->desktop_keystate[0x10]);
    assert(main_input->shared->focus == 0x10030 && flushes > 0);
    while (horizon_thread_inputs)
    {
        struct horizon_thread_input *next = horizon_thread_inputs->next;
        free(horizon_thread_inputs);
        horizon_thread_inputs = next;
    }
    puts("PASS: per-thread focus, foreground notification routing, capture, caret and input-state reuse");
}
'''
body = "\n".join(function(name) for name in (
    "horizon_server_flush_input_locked", "horizon_server_thread_input_locked",
    "horizon_server_input_shared_locked", "horizon_server_input_state_locked",
    "horizon_server_release_input_locked", "horizon_server_set_caret_window_locked",
    "horizon_server_set_caret_info_locked", "horizon_server_handle_get_thread_input",
    "horizon_server_handle_set_foreground_window", "horizon_server_handle_set_input_window",
    "horizon_server_handle_set_capture_window", "horizon_server_handle_set_caret_window",
    "horizon_server_handle_set_caret_info", "horizon_server_key_messages_pending_locked",
    "horizon_server_sync_keystate_locked"))
with tempfile.TemporaryDirectory(prefix="horizon-thread-input-") as tmp:
    code, exe = Path(tmp) / "test.c", Path(tmp) / "test"
    code.write_text(fixture + body + tests)
    subprocess.run(["clang", "-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-fsanitize=address,undefined", str(code), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
