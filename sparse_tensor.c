#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Standalone implementation copied from offrac.c (CSR-style row splits, 64B padding)
static int sparse_transfer(void* out_buf , int req_size, void* in_buf)
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

static void print_u32_array(const char *label, const uint32_t *arr, int count)
{
    printf("%s (len=%d): ", label, count);
    for (int i = 0; i < count; i++) {
        printf("%u%s", arr[i], (i + 1 == count) ? "" : ",");
    }
    printf("\n");
}

int main(void)
{
    // 4096B input buffer of int32 => 1024 elements
    enum { ELEMS = 2048 / 4, COLS = 26, ROWS = ELEMS / COLS };

    uint32_t in_buf[ELEMS];
    uint32_t out_buf[ELEMS];
    for (int i = 0; i < ELEMS; i++)
        in_buf[i] = 1;
    memset(out_buf, 0, sizeof(out_buf));

    // Sample data: row 0 -> non-zeros at cols 0, 2, 5; row 1 -> non-zeros at cols 1, 20, 25
    // in_buf[0 * COLS + 0]  = 1;
    // in_buf[0 * COLS + 2]  = 3;
    // in_buf[0 * COLS + 5]  = 9;

    // in_buf[1 * COLS + 1]  = 5;
    // in_buf[1 * COLS + 20] = 7;
    // in_buf[1 * COLS + 25] = 11;

    int padded_bytes = sparse_transfer(out_buf, (int)sizeof(in_buf), in_buf);

    printf("%d\n", padded_bytes);

    return 0;
}
