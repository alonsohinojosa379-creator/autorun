/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "mmreg.h"
#include "mferror.h"
#include "wine/unixlib.h"
#include "../../dlls/wmadmod/native_unixlib.h"

struct wma_decoder
{
    AVCodecContext *codec;
    AVFrame *frame;
    AVPacket *packet;
    SwrContext *resampler;
    uint8_t *pcm;
    unsigned int pcm_capacity, pcm_size, pcm_offset, input_offset;
    unsigned int frame_bytes, rate;
    enum AVSampleFormat format;
    INT64 origin, samples;
    BOOL has_time, draining, eof;
};

static void decoder_free(struct wma_decoder *decoder)
{
    swr_free(&decoder->resampler);
    av_free(decoder->pcm);
    av_frame_free(&decoder->frame);
    av_packet_free(&decoder->packet);
    avcodec_free_context(&decoder->codec);
    free(decoder);
}

static NTSTATUS decoder_create(void *args)
{
    struct wma_create_params *params = args;
    const WAVEFORMATEX *input = (void *)(uintptr_t)params->input;
    const WAVEFORMATEX *output = (void *)(uintptr_t)params->output;
    struct wma_decoder *decoder;
    const AVCodec *codec;
    enum AVCodecID id;
    int ret;

    params->handle = 0;
    if (!input || !output || params->input_size < sizeof(*input) || params->output_size < sizeof(*output)
            || input->cbSize > params->input_size - sizeof(*input) || !input->nBlockAlign
            || !input->nChannels || input->nChannels > 8 || !input->nSamplesPerSec
            || input->nSamplesPerSec > 192000 || output->nChannels != input->nChannels
            || output->nSamplesPerSec != input->nSamplesPerSec)
        return MF_E_INVALIDMEDIATYPE;
    switch (input->wFormatTag)
    {
    case WAVE_FORMAT_MSAUDIO1: id = AV_CODEC_ID_WMAV1; break;
    case WAVE_FORMAT_WMAUDIO2: id = AV_CODEC_ID_WMAV2; break;
    case WAVE_FORMAT_WMAUDIO3: id = AV_CODEC_ID_WMAPRO; break;
    case WAVE_FORMAT_WMAUDIO_LOSSLESS: id = AV_CODEC_ID_WMALOSSLESS; break;
    default: return MF_E_INVALIDMEDIATYPE;
    }
    if (!((output->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && output->wBitsPerSample == 32)
            || (output->wFormatTag == WAVE_FORMAT_PCM && output->wBitsPerSample == 16)))
        return MF_E_INVALIDMEDIATYPE;
    if (!(codec = avcodec_find_decoder(id))) return MF_E_TOPO_CODEC_NOT_FOUND;
    if (!(decoder = calloc(1, sizeof(*decoder)))) return E_OUTOFMEMORY;
    decoder->codec = avcodec_alloc_context3(codec);
    decoder->packet = av_packet_alloc();
    decoder->frame = av_frame_alloc();
    if (!decoder->codec || !decoder->packet || !decoder->frame) goto oom;
    decoder->codec->sample_rate = input->nSamplesPerSec;
    decoder->codec->pkt_timebase = (AVRational){1, input->nSamplesPerSec};
    decoder->codec->block_align = input->nBlockAlign;
    decoder->codec->bit_rate = (int64_t)input->nAvgBytesPerSec * 8;
    decoder->codec->bits_per_coded_sample = input->wBitsPerSample;
    decoder->codec->thread_count = 1;
    av_channel_layout_default(&decoder->codec->ch_layout, input->nChannels);
    if (!(decoder->codec->extradata = av_mallocz(input->cbSize + AV_INPUT_BUFFER_PADDING_SIZE))) goto oom;
    memcpy(decoder->codec->extradata, input + 1, input->cbSize);
    decoder->codec->extradata_size = input->cbSize;
    decoder->format = output->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_S16;
    decoder->frame_bytes = output->nChannels * (output->wBitsPerSample / 8);
    decoder->rate = input->nSamplesPerSec;
    if ((ret = avcodec_open2(decoder->codec, codec, NULL)) < 0)
    {
        decoder_free(decoder);
        return ret == AVERROR(ENOMEM) ? E_OUTOFMEMORY : MF_E_INVALIDMEDIATYPE;
    }
    params->handle = (uintptr_t)decoder;
    return S_OK;
oom:
    decoder_free(decoder);
    return E_OUTOFMEMORY;
}

static NTSTATUS decoder_destroy(void *args)
{
    decoder_free((void *)(uintptr_t)*(UINT64 *)args);
    return S_OK;
}

static NTSTATUS decoder_push(void *args)
{
    struct wma_buffer_params *params = args;
    struct wma_decoder *decoder = (void *)(uintptr_t)params->handle;
    int ret;
    if (decoder->draining || decoder->packet->size || decoder->pcm_size) return MF_E_NOTACCEPTING;
    if (!params->size || params->size > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE
            || params->size % decoder->codec->block_align || !params->data) return E_INVALIDARG;
    if ((ret = av_new_packet(decoder->packet, params->size)) < 0) return E_OUTOFMEMORY;
    memcpy(decoder->packet->data, (void *)(uintptr_t)params->data, params->size);
    decoder->input_offset = 0;
    if (params->has_time)
    {
        decoder->origin = params->time;
        decoder->samples = 0;
        decoder->has_time = TRUE;
    }
    return S_OK;
}

static HRESULT decode_frame(struct wma_decoder *decoder)
{
    AVPacket packet;
    int ret, samples, capacity;
    while ((ret = avcodec_receive_frame(decoder->codec, decoder->frame)) == AVERROR(EAGAIN))
    {
        if (decoder->packet->size)
        {
            packet = *decoder->packet;
            packet.data += decoder->input_offset;
            packet.size = decoder->codec->block_align;
            ret = avcodec_send_packet(decoder->codec, &packet);
            if (ret < 0) break;
            decoder->input_offset += packet.size;
            if (decoder->input_offset == (unsigned int)decoder->packet->size) av_packet_unref(decoder->packet);
        }
        else if (decoder->draining && !decoder->eof)
        {
            ret = avcodec_send_packet(decoder->codec, NULL);
            if (ret < 0) break;
            decoder->eof = TRUE;
        }
        else return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }
    if (ret == AVERROR_EOF) return MF_E_TRANSFORM_NEED_MORE_INPUT;
    if (ret < 0) return ret == AVERROR(ENOMEM) ? E_OUTOFMEMORY : MF_E_INVALID_STREAM_DATA;
    if ((unsigned int)decoder->frame->sample_rate != decoder->rate
            || decoder->frame->ch_layout.nb_channels != decoder->codec->ch_layout.nb_channels)
        return MF_E_INVALID_STREAM_DATA;
    if (!decoder->resampler)
    {
        ret = swr_alloc_set_opts2(&decoder->resampler, &decoder->codec->ch_layout, decoder->format,
                decoder->rate, &decoder->frame->ch_layout, decoder->frame->format, decoder->rate, 0, NULL);
        if (ret >= 0) ret = swr_init(decoder->resampler);
        if (ret < 0)
        {
            swr_free(&decoder->resampler);
            return ret == AVERROR(ENOMEM) ? E_OUTOFMEMORY : MF_E_INVALIDMEDIATYPE;
        }
    }
    capacity = swr_get_out_samples(decoder->resampler, decoder->frame->nb_samples);
    if (capacity < 0 || (unsigned int)capacity > INT_MAX / decoder->frame_bytes) return E_OUTOFMEMORY;
    av_fast_malloc(&decoder->pcm, &decoder->pcm_capacity, capacity * decoder->frame_bytes);
    if (!decoder->pcm) return E_OUTOFMEMORY;
    samples = swr_convert(decoder->resampler, &decoder->pcm, capacity,
            (const uint8_t **)decoder->frame->extended_data, decoder->frame->nb_samples);
    av_frame_unref(decoder->frame);
    if (samples < 0) return MF_E_INVALID_STREAM_DATA;
    decoder->pcm_size = samples * decoder->frame_bytes;
    decoder->pcm_offset = 0;
    return S_OK;
}

static NTSTATUS decoder_read(void *args)
{
    struct wma_buffer_params *params = args;
    struct wma_decoder *decoder = (void *)(uintptr_t)params->handle;
    unsigned int capacity = params->size - params->size % decoder->frame_bytes;
    INT64 start, end;
    HRESULT hr;
    params->size = 0;
    if (!capacity || !params->data) return MF_E_BUFFERTOOSMALL;
    while (!decoder->pcm_size)
        if (FAILED(hr = decode_frame(decoder))) return hr;
    params->size = min(capacity, decoder->pcm_size);
    memcpy((void *)(uintptr_t)params->data, decoder->pcm + decoder->pcm_offset, params->size);
    decoder->pcm_offset += params->size;
    decoder->pcm_size -= params->size;
    start = decoder->samples * 10000000 / decoder->rate;
    decoder->samples += params->size / decoder->frame_bytes;
    end = decoder->samples * 10000000 / decoder->rate;
    params->has_time = decoder->has_time;
    params->time = decoder->origin + start;
    params->duration = end - start;
    return S_OK;
}

static NTSTATUS decoder_flush(void *args)
{
    struct wma_decoder *decoder = (void *)(uintptr_t)*(UINT64 *)args;
    avcodec_flush_buffers(decoder->codec);
    av_packet_unref(decoder->packet);
    av_frame_unref(decoder->frame);
    swr_free(&decoder->resampler);
    decoder->pcm_size = decoder->pcm_offset = decoder->input_offset = 0;
    decoder->draining = decoder->eof = decoder->has_time = FALSE;
    decoder->samples = decoder->origin = 0;
    return S_OK;
}

static NTSTATUS decoder_drain(void *args)
{
    struct wma_decoder *decoder = (void *)(uintptr_t)*(UINT64 *)args;
    decoder->draining = TRUE;
    return S_OK;
}

const unixlib_entry_t wine_nx_wma_unix_funcs[] =
{
    decoder_create, decoder_destroy, decoder_push, decoder_read, decoder_flush, decoder_drain
};
C_ASSERT(ARRAY_SIZE(wine_nx_wma_unix_funcs) == wma_call_count);
const unsigned int wine_nx_wma_unix_count = ARRAY_SIZE(wine_nx_wma_unix_funcs);
