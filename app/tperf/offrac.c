/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Author: Yixi Chen <yixi.chen@kaust.edu.sa>
 */

#include <stdio.h>
#include <string.h>
#include "tensorflow/c/c_api.h"
#include "offrac.h"
#include "tperf.h"
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>

// Global variables for mapid functionality
uint16_t *g_idmap = NULL;
int g_idmap_loaded = 0;
int mapid_printed_once = 1;
int logit_printed_once = 1;
int topk_printed_once = 1;

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

    // Default to identity mapping to ensure safe access even if file parsing fails
    for (size_t i = 0; i < IDMAP_SIZE; i++) {
        g_idmap[i] = (uint16_t)i;
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

void NoOpDeallocator(void* data, size_t a, void* b) {}
// Comparison function for qsort (descending order)
int compare_desc(const void *a, const void *b) {
    return (*(uint32_t *)b - *(uint32_t *)a);
}

int sparse_transfer(void* out_buf , int req_size, void* in_buf)
{
    uint32_t *input = (uint32_t *)in_buf;
    uint32_t *output = (uint32_t *)out_buf;

    int total_elements = req_size / (int)sizeof(uint32_t);
    const int cols_per_row = 26;
    int num_rows = (cols_per_row == 0) ? 0 : (total_elements / cols_per_row);

    if (num_rows <= 0) {
        for (int i = 0; i < total_elements; i++)
            output[i] = 0;
        return 0;
    }

    uint32_t *row_ptr = (uint32_t *)malloc((size_t)(num_rows + 1) * sizeof(uint32_t));
    if (!row_ptr)
        return -1;

    // First pass: count non-zeros per row and build exclusive prefix in row_ptr
    row_ptr[0] = 0;
    for (int r = 0; r < num_rows; r++) {
        int accepted = 0;
        int base = r * cols_per_row;
        for (int c = 0; c < cols_per_row; c++) {
            uint32_t v = input[base + c];
            if (v != 0)
                accepted++;
        }
        row_ptr[r + 1] = row_ptr[r] + (uint32_t)accepted; // exclusive end
    }

    int nnz = (int)row_ptr[num_rows];
    int required_written = nnz + (num_rows + 1);

    // Compute 64-byte padded size (in bytes) based on the required element count
    int bytes_required = required_written * (int)sizeof(uint32_t);
    int padded_bytes = ((bytes_required + 63) / 64) * 64;

    // Second pass: only write if the provided output buffer has enough capacity
    if (required_written <= total_elements) {
        int k = 0; // number of values written
        for (int r = 0; r < num_rows; r++) {
            int base = r * cols_per_row;
            for (int c = 0; c < cols_per_row; c++) {
                uint32_t v = input[base + c];
                if (v != 0) {
                    output[k++] = v;
                }
            }
        }

        // Write per-row exclusive end indices with a leading 0
        int offsets_base = k;
        output[offsets_base + 0] = 0;
        for (int r = 1; r <= num_rows; r++) {
            output[offsets_base + r] = row_ptr[r];
        }

        // Zero-fill remaining capacity
        for (int i = required_written; i < total_elements; i++)
            output[i] = 0;
    } else {
        // Not enough capacity in out_buf to hold all data; avoid truncation
        for (int i = 0; i < total_elements; i++)
            output[i] = 0;
    }

    free(row_ptr);

    return padded_bytes;
}

int norm(void* out_buf , int req_size, void* in_buf) {
    float *float_buf = (float *)out_buf;

    // Find the min and max values
    float min_val = FLT_MAX;
    float max_val = -FLT_MAX;

    int size = req_size/sizeof(uint32_t);

    for (int i = 0; i < size; i++) {
        if (float_buf[i] < min_val) {
            min_val = float_buf[i];
        }
        if (float_buf[i] > max_val) {
            max_val = float_buf[i];
        }
    }

    // Check if min and max values are the same
    if (min_val == max_val) {
        //fprintf(stderr, "All elements are the same\n");
        max_val += min_val;
        //return -1;
    }

    // Normalize the elements using min-max normalization
    for (int i = 0; i < size; i++) {
        float_buf[i] = (float_buf[i] - min_val) / (max_val - min_val);
    }

    return size;
}

int logit(void* out_buf , int req_size, void* in_buf) {
    float *float_buf = (float *)out_buf;

    int size = req_size/sizeof(uint32_t);

    // Apply the logistic function to each element
    for (int i = 0; i < size; i++) {
        float_buf[i] = 1.0f / (1.0f + expf(-float_buf[i]));
    }

    if (!logit_printed_once) {
        logit_printed_once = 1;
        int trailing = req_size - size * (int)sizeof(uint32_t);
        printf("[logit] Input size: %d bytes; Output size: %d bytes\n", req_size, req_size);
        printf("[logit] Words processed: %d; Trailing bytes: %d\n", size, trailing);
        printf("[logit] Input bytes (from in_buf):\n");
        hexdump((const uint8_t *)in_buf, req_size);
        printf("[logit] Output bytes (from out_buf):\n");
        hexdump((const uint8_t *)out_buf, req_size);
    }

    return size;
}

int cnn(void* out_buf, int req_size, void* in_buf, cnn_tf *tf_obj){


    int64_t input_dims[] = {1, 64, 64, 3};  // (1 image, 64x64, RGB)

    float* input_data = (float*) malloc(IMAGE_SIZE*sizeof(float));
    if (input_data == NULL) {
        fprintf(stderr, "Failed to allocate input data\n");
        return -1;
    }

    // Copy one image from the buffer
    uint8_t* input_bytes = (uint8_t*)in_buf;
    for (int i = 0; i < IMAGE_SIZE; i++) {
      input_data[i] = (float)input_bytes[i];
    }

    // Create an input tensor for this single image
    TF_Tensor* input_tensor = TF_NewTensor(TF_FLOAT, input_dims, 4, input_data, IMAGE_SIZE * sizeof(float), &NoOpDeallocator, NULL);

    // Run inference
    TF_Tensor* output_tensor = NULL;
    TF_SessionRun(tf_obj->session, NULL, &tf_obj->input_op, &input_tensor, 1, &tf_obj->output_op, &output_tensor, 1, NULL, 0, NULL, tf_obj->status);

    // Retrieve and print the result for this image
    void* buff = TF_TensorData(output_tensor);
    float* offsets = (float*)buff;

    // TODO: update to output
    memcpy(out_buf, offsets, 10 * sizeof(float));

    // Clean up for this image
    TF_DeleteTensor(input_tensor);
    TF_DeleteTensor(output_tensor);

    memcpy(out_buf, buff, 10 * sizeof(float));
    // Clean up global resources

    return 10 * sizeof(float);
}

int topk(void* out_buf , int req_size, void* in_buf) {
    // fprintf(stderr, "[tperf] top k called (req_size=%d)\n", req_size);
    int k = 16;
    int size = req_size/sizeof(uint32_t);
    // Check if k is valid
    uint32_t *buf_u32 = (uint32_t *)out_buf;
    if (k <= 0 || k > size) {
        fprintf(stderr, "Invalid value of k: %d\n", k);
        return -1;
    }

    memcpy(out_buf, in_buf, req_size);

    qsort(buf_u32, size, sizeof(uint32_t), compare_desc);

    for (int i = k; i < size; i++) {
      buf_u32[i] = 0;
    }

    if (!topk_printed_once) {
        topk_printed_once = 1;
        int trailing = req_size - size * (int)sizeof(uint32_t);
        printf("[topk] Input size: %d bytes; Output size: %d bytes\n", req_size, req_size);
        printf("[topk] Words processed: %d; Trailing bytes: %d; k=%d\n", size, trailing, k);
        printf("[topk] Input bytes:\n");
        hexdump((const uint8_t *)in_buf, req_size);
        printf("[topk] Output bytes:\n");
        hexdump((const uint8_t *)out_buf, req_size);
    }

    return k;
}

int mapid(void* out_buf , int req_size, void* in_buf) {
    // Ensure idmap is initialized
    if (!g_idmap) {
        fprintf(stderr, "Error: g_idmap not initialized\n");
        return -1;
    }
    
    int process_bytes = MIN(req_size, MIN(MAX_BUF_SIZE, MBUF_SIZE));
    int size = process_bytes / (int)sizeof(uint32_t);
    uint32_t *out_u32 = (uint32_t *)out_buf;

    // Process like topk: operate on a copy sourced from in_buf, capped to buffer size
    memcpy(out_buf, in_buf, process_bytes);

    for (int i = 0; i < size; i++) {
        uint16_t addr = (uint16_t)(out_u32[i] & 0xFFFFu);
        uint16_t mapped = g_idmap[addr];
        out_u32[i] = (uint32_t)mapped; // zero-extend mapped 16-bit ID
    }


    if (!mapid_printed_once) {
       mapid_printed_once = 1;
        int print_in = process_bytes > 256 ? 256 : process_bytes;
        int print_out = (size * (int)sizeof(uint32_t)) > 256 ? 256 : (size * (int)sizeof(uint32_t));
        printf("[mapid] Input size: %d; Processed: %d; Output bytes: %d\n", req_size, process_bytes, size * (int)sizeof(uint32_t));
        printf("[mapid] Words processed: %d\n", size);
        if (print_in > 0) {
            printf("[mapid] Input (first %d bytes):\n", print_in);
            hexdump((const uint8_t *)in_buf, print_in);
        }
        if (print_out > 0) {
            printf("[mapid] Output (first %d bytes):\n", print_out);
            hexdump((const uint8_t *)out_buf, print_out);
        }
    }

    return size;
}