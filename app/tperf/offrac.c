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
int topk(uint32_t* out_buf , int req_size, void* in_buf) {

    int k = 16;

    // Check if k is valid
    if (k <= 0 || k > req_size) {
        fprintf(stderr, "Invalid value of k: %d\n", k);
        return -1;
    }

    memcpy(out_buf, in_buf, req_size * sizeof(uint32_t));
    // Sort the array in descending order
    qsort(out_buf, req_size, sizeof(uint32_t), compare_desc);

    // Set elements after the k-th element to 0
    for (int i = k; i < req_size; i++) {
        out_buf[i] = 0;
    }

    return k;
}

// return the buf size after minmax norm, ideally should remains the same
// return -1 if error
/* int minmax(uint32_t *buf, int size, int offrac_size, int offrac_args) { */
/*     float *float_buf = (float *)buf; */

/*     // Find the min and max values */
/*     float min_val = FLT_MAX; */
/*     float max_val = -FLT_MAX; */
/*     for (int i = 0; i < size; i++) { */
/*         if (float_buf[i] < min_val) { */
/*             min_val = float_buf[i]; */
/*         } */
/*         if (float_buf[i] > max_val) { */
/*             max_val = float_buf[i]; */
/*         } */
/*     } */

/*     // Check if min and max values are the same */
/*     if (min_val == max_val) { */
/*         fprintf(stderr, "All elements are the same\n"); */
/*         return -1; */
/*     } */

/*     // Normalize the elements using min-max normalization */
/*     for (int i = 0; i < size; i++) { */
/*         float_buf[i] = (float_buf[i] - min_val) / (max_val - min_val); */
/*     } */

/*     return size; */
/* } */

/* // return the buf size after logit transformation, ideally should remains the same */
/* // return -1 if error */
/* int logit(uint32_t *buf, int size, int offrac_size, int offrac_args) { */
/*     float *float_buf = (float *)buf; */

/*     // Apply the logistic function to each element */
/*     for (int i = 0; i < size; i++) { */
/*         float_buf[i] = 1.0f / (1.0f + expf(-float_buf[i])); */
/*     } */

/*     return size; */
/* } */
