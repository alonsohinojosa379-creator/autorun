/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef WMA_NATIVE_UNIXLIB_H
#define WMA_NATIVE_UNIXLIB_H

enum wma_call { wma_create, wma_destroy, wma_push, wma_read, wma_flush, wma_drain, wma_call_count };

struct wma_create_params
{
    UINT64 input, output, handle;
    DWORD input_size, output_size;
};

struct wma_buffer_params
{
    UINT64 handle, data;
    INT64 time, duration;
    DWORD size, has_time;
};

C_ASSERT(sizeof(struct wma_create_params) == 32);
C_ASSERT(sizeof(struct wma_buffer_params) == 40);

#endif
