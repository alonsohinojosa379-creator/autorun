#!/usr/bin/env python3
"""Exercise the Horizon window-destruction handler with interleaved owners."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "dlls/ntdll/unix/horizon.c").read_text()
start = source.index("static int horizon_server_handle_destroy_window(")
end = source.index("\nstatic void ", start)
handler = source[start:end]

fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#define HORIZON_STATUS_SUCCESS 0
#define HORIZON_STATUS_INVALID_HANDLE 6
struct horizon_server_connection { unsigned int pid, tid; int reply_fd; };
struct horizon_destroy_window_request { unsigned int handle; };
struct horizon_window_property { struct horizon_window_property *next; };
struct horizon_user_window {
    unsigned int handle, pid, tid;
    struct horizon_window_property *properties;
    void *text, *win_region;
    struct horizon_user_window *next;
};
struct horizon_input_shm { unsigned int active, focus, capture, menu_owner, move_size, caret; };
static struct horizon_user_window *horizon_windows;
static struct horizon_input_shm input;
static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
static int horizon_posted_messages, horizon_timers, horizon_clipboard;
static unsigned int dropped[32], ndropped, refreshed, flushed;

static void horizon_message_queue_drop(int *queue, unsigned int tid, unsigned int handle)
{
    (void)queue; (void)tid;
    assert(ndropped < 32);
    dropped[ndropped++] = handle;
}
static void horizon_win_timers_drop(int *timers, unsigned int tid, unsigned int handle)
{ (void)timers; (void)tid; assert(dropped[ndropped - 1] == handle); }
static int horizon_clip_window_destroyed(int *clipboard, unsigned int handle)
{ (void)clipboard; (void)handle; return 0; }
static void horizon_server_clipboard_notify_locked(void) {}
static struct horizon_input_shm *horizon_server_input_shared_locked(unsigned int tid)
{ (void)tid; return &input; }
static void horizon_server_set_caret_window_locked(struct horizon_input_shm *shm,
                                                  unsigned int handle, int w, int h)
{ (void)w; (void)h; shm->caret = handle; }
static void horizon_server_flush_input_locked(struct horizon_input_shm *shm)
{ (void)shm; flushed++; }
static void horizon_server_refresh_queues_locked(void) { refreshed++; }
static int horizon_server_write_status(int fd, unsigned int status) { (void)fd; return status; }
'''

tests = r'''
static void add(unsigned int handle, unsigned int pid, unsigned int tid)
{
    struct horizon_user_window *window = calloc(1, sizeof(*window));
    assert(window);
    window->handle = handle;
    window->pid = pid;
    window->tid = tid;
    window->text = malloc(16);
    window->win_region = malloc(16);
    window->properties = calloc(1, sizeof(*window->properties));
    assert(window->text && window->win_region && window->properties);
    window->next = horizon_windows;
    horizon_windows = window;
}

static int destroy(unsigned int pid, unsigned int tid, unsigned int handle)
{
    struct horizon_server_connection connection = {pid, tid, 0};
    struct horizon_destroy_window_request request = {handle};
    unsigned int before = refreshed;
    int result = horizon_server_handle_destroy_window(&connection, (const unsigned char *)&request);
    assert(refreshed == before + 1);
    assert(!pthread_mutex_trylock(&horizon_server_objects_mutex));
    pthread_mutex_unlock(&horizon_server_objects_mutex);
    return result;
}

int main(void)
{
    assert(!destroy(1, 4, 0));
    assert(destroy(1, 4, 99) == HORIZON_STATUS_INVALID_HANDLE);
    assert(!ndropped);

    for (unsigned int count = 1; count <= 8; count++)
    {
        ndropped = 0;
        for (unsigned int i = 1; i <= count; i++) add(i, 1, 44);
        assert(!destroy(1, 44, 0));
        assert(!horizon_windows && ndropped == count);
    }

    ndropped = 0;
    add(100, 0, 0);  /* desktop */
    add(101, 1, 4);  /* main game window */
    add(102, 1, 44);
    add(103, 2, 44);
    add(104, 1, 44);
    add(105, 1, 4);
    add(106, 1, 44);
    input.caret = 105;
    assert(!destroy(1, 44, 0));
    assert(ndropped == 3 && dropped[0] == 106 && dropped[1] == 104 && dropped[2] == 102);
    assert(input.caret == 105 && !flushed);
    const unsigned int expected[] = {105, 103, 101, 100};
    struct horizon_user_window *window = horizon_windows;
    for (unsigned int i = 0; i < 4; i++, window = window->next)
        assert(window && window->handle == expected[i]);
    assert(!window);
    assert(!destroy(1, 44, 0) && ndropped == 3);

    assert(!destroy(1, 4, 101));
    assert(ndropped == 4 && dropped[3] == 101);
    assert(horizon_windows->handle == 105 && horizon_windows->next->handle == 103);
    assert(!destroy(1, 4, 0));
    assert(!input.caret && flushed == 1);
    assert(horizon_windows->handle == 103 && horizon_windows->next->handle == 100);
    assert(!destroy(2, 44, 0));
    assert(horizon_windows->handle == 100 && !horizon_windows->next);
    assert(destroy(1, 4, 105) == HORIZON_STATUS_INVALID_HANDLE);
    assert(!destroy(0, 0, 100));
    assert(!horizon_windows);
    puts("PASS: thread cleanup preserves other owners and visits every removed window");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="wine-nx-window-destroy-") as temp:
    path = Path(temp)
    src, exe = path / "test.c", path / ("test.exe" if os.name == "nt" else "test")
    src.write_text(fixture + handler + tests)
    compiler = shlex.split(os.environ.get("CC", "cc"))
    flags = shlex.split(os.environ.get("CFLAGS", "-fsanitize=address,undefined"))
    subprocess.run(compiler + ["-std=gnu99", "-g", "-Wall", "-Wextra", "-Werror", "-pthread"] +
                   flags + [str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
