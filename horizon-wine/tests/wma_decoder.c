#include <assert.h>
#include <stdio.h>
#include "../source/wma_unix.c"

static uint32_t read32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static size_t run(UINT64 handle, const uint8_t *data, unsigned int length, unsigned int block,
        unsigned int packets, unsigned int output_size, FILE *output)
{
    uint8_t buffer[65536];
    struct wma_buffer_params params = { .handle = handle };
    size_t offset = 0, total = 0;
    BOOL drained = FALSE;
    HRESULT hr;
    assert(output_size <= sizeof(buffer));
    while (1)
    {
        params.data = (uintptr_t)buffer;
        params.size = output_size;
        hr = decoder_read(&params);
        if (hr == S_OK)
        {
            assert(params.size && params.size <= output_size);
            assert(!output || fwrite(buffer, 1, params.size, output) == params.size);
            total += params.size;
            continue;
        }
        assert(hr == MF_E_TRANSFORM_NEED_MORE_INPUT);
        if (offset == length)
        {
            if (drained) break;
            assert(decoder_drain(&handle) == S_OK);
            drained = TRUE;
            continue;
        }
        params.data = (uintptr_t)data + offset;
        params.size = min(length - offset, (size_t)block * packets);
        params.has_time = FALSE;
        assert(decoder_push(&params) == S_OK);
        assert(decoder_push(&params) == MF_E_NOTACCEPTING);
        offset += params.size;
    }
    return total;
}

int main(int argc, char **argv)
{
    uint8_t *bytes, *data = NULL, format[128] = {0};
    WAVEFORMATEX *input = (void *)format;
    WAVEFORMATEX output = {0};
    struct wma_create_params create = {0};
    struct wma_buffer_params invalid = {0};
    FILE *file;
    size_t length, pos, size, data_size = 0, total;
    assert(argc == 6);
    file = fopen(argv[1], "rb");
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    length = ftell(file);
    rewind(file);
    bytes = malloc(length);
    assert(bytes && fread(bytes, 1, length, file) == length);
    fclose(file);
    assert(!memcmp(bytes, "RIFF", 4));
    for (pos = 12; pos + 8 <= length; pos += 8 + size + (size & 1))
    {
        size = read32(bytes + pos + 4);
        assert(size <= length - pos - 8);
        if (!memcmp(bytes + pos, "fmt ", 4))
        {
            assert(size >= sizeof(*input) && size <= sizeof(format));
            memcpy(format, bytes + pos + 8, size);
        }
        if (!memcmp(bytes + pos, "data", 4)) { data = bytes + pos + 8; data_size = size; }
    }
    assert(data && input->nBlockAlign && data_size % input->nBlockAlign == 0);
    if (!input->cbSize && input->wFormatTag == WAVE_FORMAT_WMAUDIO2)
    {
        input->cbSize = 16;
        format[sizeof(*input) + 4] = 31;
    }
    output.wBitsPerSample = atoi(argv[5]);
    output.wFormatTag = output.wBitsPerSample == 16 ? WAVE_FORMAT_PCM : WAVE_FORMAT_IEEE_FLOAT;
    output.nChannels = input->nChannels;
    output.nSamplesPerSec = input->nSamplesPerSec;
    create.input = (uintptr_t)input;
    create.input_size = sizeof(*input) + input->cbSize;
    create.output = (uintptr_t)&output;
    create.output_size = sizeof(output);
    assert(decoder_create(&create) == S_OK);
    invalid.handle = create.handle;
    invalid.data = (uintptr_t)data;
    invalid.size = 1;
    assert(decoder_push(&invalid) == E_INVALIDARG);
    file = fopen(argv[2], "wb");
    assert(file);
    total = run(create.handle, data, data_size, input->nBlockAlign, atoi(argv[3]), atoi(argv[4]), file);
    fclose(file);
    assert(total && decoder_flush(&create.handle) == S_OK);
    assert(run(create.handle, data, data_size, input->nBlockAlign, 1, 65536, NULL) == total);
    assert(decoder_destroy(&create.handle) == S_OK);
    output.nChannels = 0;
    assert(decoder_create(&create) == MF_E_INVALIDMEDIATYPE && !create.handle);
    free(bytes);
    printf("WMA: decoded %zu bytes; fragmented output, backpressure, drain and replay passed\n", total);
    return 0;
}
