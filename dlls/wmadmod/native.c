/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../winegstreamer/gst_private.h"
#include "mferror.h"
#include "mediaerr.h"
#include "wine/unixlib.h"
#include "native_unixlib.h"

HRESULT wg_transform_create_quartz(const AM_MEDIA_TYPE *input, const AM_MEDIA_TYPE *output,
        const struct wg_transform_attrs *attrs, wg_transform_t *handle)
{
    struct wma_create_params params = { .input = (UINT_PTR)input->pbFormat,
        .output = (UINT_PTR)output->pbFormat, .input_size = input->cbFormat, .output_size = output->cbFormat };
    HRESULT hr = WINE_UNIX_CALL(wma_create, &params);
    *handle = SUCCEEDED(hr) ? params.handle : 0;
    return hr;
}

void wg_transform_destroy(wg_transform_t handle) { WINE_UNIX_CALL(wma_destroy, &handle); }
HRESULT wg_transform_flush(wg_transform_t handle) { return WINE_UNIX_CALL(wma_flush, &handle); }
HRESULT wg_transform_drain(wg_transform_t handle) { return WINE_UNIX_CALL(wma_drain, &handle); }

HRESULT wg_transform_push_mf(wg_transform_t handle, IMFSample *sample, struct wg_sample_queue *queue)
{
    struct wma_buffer_params params = { .handle = handle };
    IMFMediaBuffer *buffer;
    BYTE *data;
    HRESULT hr;
    if (FAILED(hr = IMFSample_ConvertToContiguousBuffer(sample, &buffer))) return hr;
    if (SUCCEEDED(hr = IMFMediaBuffer_Lock(buffer, &data, NULL, &params.size)))
    {
        params.data = (UINT_PTR)data;
        params.has_time = SUCCEEDED(IMFSample_GetSampleTime(sample, &params.time));
        hr = WINE_UNIX_CALL(wma_push, &params);
        IMFMediaBuffer_Unlock(buffer);
    }
    IMFMediaBuffer_Release(buffer);
    return hr;
}

HRESULT wg_transform_read_mf(wg_transform_t handle, IMFSample *sample, DWORD sample_size, DWORD *status, bool *preserve)
{
    struct wma_buffer_params params = { .handle = handle };
    IMFMediaBuffer *buffer;
    BYTE *data;
    HRESULT hr;
    if (FAILED(hr = IMFSample_ConvertToContiguousBuffer(sample, &buffer))) return hr;
    if (SUCCEEDED(hr = IMFMediaBuffer_Lock(buffer, &data, &params.size, NULL)))
    {
        params.data = (UINT_PTR)data;
        hr = WINE_UNIX_CALL(wma_read, &params);
        IMFMediaBuffer_Unlock(buffer);
        IMFMediaBuffer_SetCurrentLength(buffer, SUCCEEDED(hr) ? params.size : 0);
        if (SUCCEEDED(hr))
        {
            if (params.has_time) IMFSample_SetSampleTime(sample, params.time);
            IMFSample_SetSampleDuration(sample, params.duration);
            *status = 0;
        }
    }
    IMFMediaBuffer_Release(buffer);
    return hr;
}

HRESULT wg_transform_push_dmo(wg_transform_t handle, IMediaBuffer *buffer, DWORD flags,
        REFERENCE_TIME time, REFERENCE_TIME duration, struct wg_sample_queue *queue)
{
    struct wma_buffer_params params = { .handle = handle, .time = time,
        .has_time = !!(flags & DMO_INPUT_DATA_BUFFERF_TIME) };
    BYTE *data;
    HRESULT hr = IMediaBuffer_GetBufferAndLength(buffer, &data, &params.size);
    if (FAILED(hr)) return hr;
    params.data = (UINT_PTR)data;
    hr = WINE_UNIX_CALL(wma_push, &params);
    return hr == MF_E_NOTACCEPTING ? DMO_E_NOTACCEPTING : hr;
}

HRESULT wg_transform_read_dmo(wg_transform_t handle, DMO_OUTPUT_DATA_BUFFER *buffer)
{
    struct wma_buffer_params params = { .handle = handle };
    BYTE *data;
    HRESULT hr;
    if (!buffer->pBuffer) return E_POINTER;
    if (FAILED(hr = IMediaBuffer_GetMaxLength(buffer->pBuffer, &params.size))) return hr;
    if (FAILED(hr = IMediaBuffer_GetBufferAndLength(buffer->pBuffer, &data, NULL))) return hr;
    params.data = (UINT_PTR)data;
    hr = WINE_UNIX_CALL(wma_read, &params);
    IMediaBuffer_SetLength(buffer->pBuffer, SUCCEEDED(hr) ? params.size : 0);
    if (SUCCEEDED(hr))
    {
        buffer->dwStatus = DMO_OUTPUT_DATA_BUFFERF_TIMELENGTH;
        if (params.has_time) buffer->dwStatus |= DMO_OUTPUT_DATA_BUFFERF_TIME;
        buffer->rtTimestamp = params.time;
        buffer->rtTimelength = params.duration;
    }
    return hr;
}
