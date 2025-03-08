/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Author: Yixi Chen <yixi.chen@kaust.edu.sa>
 */

#include <stdio.h>
#include <string.h>

#include "offrac.h"

// Comparison function for qsort (descending order)
int compare_desc(const void *a, const void *b) {
    return (*(uint32_t *)b - *(uint32_t *)a);
}

// return the buf size after topk, ideally should be k
// return -1 if error
int topk(void* out_buf , int req_size, void* in_buf) {

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

    return k;
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
        fprintf(stderr, "All elements are the same\n");
        return -1;
    }

    // Normalize the elements using min-max normalization
    for (int i = 0; i < size; i++) {
        float_buf[i] = (float_buf[i] - min_val) / (max_val - min_val);
    }

    return size;
}

int logit(void* out_buf , int req_size, void* in_buf) {
    float *float_buf = (float *)buf;

    int size = req_size/sizeof(uint32_t);

    // Apply the logistic function to each element
    for (int i = 0; i < size; i++) {
        float_buf[i] = 1.0f / (1.0f + expf(-float_buf[i]));
    }

    return size;
}
