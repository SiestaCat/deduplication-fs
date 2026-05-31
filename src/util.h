#ifndef DEDUP_UTIL_H
#define DEDUP_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define LOG(...) do { \
    fprintf(stderr, "[dedup] " __VA_ARGS__); \
    fputc('\n', stderr); \
    fflush(stderr); \
} while (0)

void bytes_to_hex(const unsigned char *bytes, size_t len, char *out);
void split_path(const char *path, char *parent_out, char *name_out);
int64_t now_ns(void);

#endif
