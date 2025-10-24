#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifndef IDMAP_SIZE
#define IDMAP_SIZE 65536
#endif

uint16_t *g_idmap = NULL;
int g_idmap_loaded = 0;
void idmap_init_from_coe_once(void) {
    if (g_idmap_loaded)
        return;

    // Allocate memory for the idmap array
    g_idmap = (uint16_t *)calloc(IDMAP_SIZE, sizeof(uint16_t));
    if (!g_idmap) {
        fprintf(stderr, "Failed to allocate memory for g_idmap\n");
        g_idmap_loaded = 1;
        return;
    }

    const char *coe_path = "/home/yangz0e/libtpa/idmap_weights.coe";
    FILE *f = fopen(coe_path, "r");
    if (!f) {
        g_idmap_loaded = 1;
        return;
    }

    // Read whole file
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        g_idmap_loaded = 1;
        return;
    }
    long flen = ftell(f);
    if (flen < 0) {
        fclose(f);
        g_idmap_loaded = 1;
        return;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        g_idmap_loaded = 1;
        return;
    }

    char *buf = (char *)malloc((size_t)flen + 1);
    if (!buf) {
        fclose(f);
        g_idmap_loaded = 1;
        return;
    }
    size_t nread = fread(buf, 1, (size_t)flen, f);
    fclose(f);
    buf[nread] = '\0';

    // Parse hex tokens anywhere in the file (COE: radix=16, comma/semicolon separated)
    size_t idx = 0;
    uint32_t accum = 0;
    int have_digits = 0;
    for (size_t i = 0; buf[i] != '\0' && idx < IDMAP_SIZE; i++) {
        char c = buf[i];
        int hex = -1;
        if (c >= '0' && c <= '9') hex = c - '0';
        else if (c >= 'a' && c <= 'f') hex = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') hex = 10 + (c - 'A');

        if (hex >= 0) {
            if (!have_digits) {
                accum = 0;
                have_digits = 1;
            }
            accum = (accum << 4) | (uint32_t)hex;
        } else {
            if (have_digits) {
                g_idmap[idx++] = (uint16_t)(accum & 0xFFFFu);
                have_digits = 0;
            }
        }
    }
    if (have_digits && idx < IDMAP_SIZE) {
        g_idmap[idx++] = (uint16_t)(accum & 0xFFFFu);
    }

    free(buf);
    g_idmap_loaded = 1;
}


int mapid(void* out_buf , int req_size, void* in_buf) {
    // Ensure idmap is initialized
    if (!g_idmap) {
        fprintf(stderr, "Error: g_idmap not initialized\n");
        return -1;
    }
    
    int count = req_size / (int)sizeof(uint32_t);
    const uint32_t *in_u32 = (const uint32_t *)in_buf;
    uint32_t *out_u32 = (uint32_t *)out_buf;

    for (int i = 0; i < count; i++) {
        uint16_t addr = (uint16_t)(in_u32[i] & 0xFFFFu);
        uint16_t mapped = g_idmap[addr];
        out_u32[i] = (uint32_t)mapped; // zero-extend mapped 16-bit ID
    }

    // Zero any trailing bytes (if input size not multiple of 4)
    int remaining = req_size - count * (int)sizeof(uint32_t);
    if (remaining > 0) {
        memset(((uint8_t *)out_buf) + count * sizeof(uint32_t), 0, (size_t)remaining);
    }

    return req_size;
}

static void hexdump(const uint8_t *buf, int len)
{
    for (int i = 0; i < len; i++) {
        if (i && (i % 16) == 0)
            printf("\n");
        printf("%02x ", buf[i]);
    }
    if (len)
        printf("\n");
}

int main(int argc, char **argv)
{
    int sizes[] = {1, 3, 4, 5, 8, 16, 32, 64};
    int num_sizes = (int)(sizeof(sizes) / sizeof(sizes[0]));

    if (argc == 2) {
        int s = atoi(argv[1]);
        if (s > 0)
            sizes[0] = s, num_sizes = 1;
    }

    idmap_init_from_coe_once();
    if (!g_idmap) {
        fprintf(stderr, "g_idmap is NULL; proceeding with zero mapping.\n");
    }

    for (int t = 0; t < num_sizes; t++) {
        int req_size = sizes[t];
        uint8_t *in_bytes = (uint8_t *)malloc((size_t)req_size);
        uint8_t *out_bytes = (uint8_t *)malloc((size_t)req_size);

        if (!in_bytes || !out_bytes) {
            fprintf(stderr, "alloc failed for size %d\n", req_size);
            free(in_bytes);
            free(out_bytes);
            continue;
        }

        memset(out_bytes, 0xEE, (size_t)req_size);

        int count = req_size / (int)sizeof(uint32_t);
        uint32_t *in_u32 = (uint32_t *)in_bytes;
        for (int i = 0; i < count; i++) {
            in_u32[i] = (uint32_t)i; // low 16 bits will index g_idmap
        }
        // Fill trailing bytes (if any) with non-zero pattern to verify zeroing
        for (int i = count * (int)sizeof(uint32_t); i < req_size; i++)
            in_bytes[i] = 0xAB;

        int ret = mapid(out_bytes, req_size, in_bytes);

        int trailing = req_size - count * (int)sizeof(uint32_t);
        int trailing_zero = 1;
        for (int i = 0; i < trailing; i++) {
            if (out_bytes[count * 4 + i] != 0) {
                trailing_zero = 0;
                break;
            }
        }

        printf("req_size=%d bytes, words=%d, ret=%d bytes, trailing_zero=%s\n",
               req_size, count, ret, trailing_zero ? "yes" : "no");

        // Uncomment to inspect buffers
        // printf("IN:\n"); hexdump(in_bytes, req_size);
        // printf("OUT:\n"); hexdump(out_bytes, req_size);

        free(in_bytes);
        free(out_bytes);
    }

    free(g_idmap);
    g_idmap = NULL;
    return 0;
}