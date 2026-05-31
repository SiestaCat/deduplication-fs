#ifndef DEDUP_STORE_H
#define DEDUP_STORE_H

#include <stddef.h>
#include <sys/types.h>

/* 1MB chunk size*/
#define CHUNK_SIZE 1000000
#define SHA512_HEX_LEN 128

int store_init(const char *root);

/* Returns 1 if a new chunk file was written, 0 if it was already present
 * (deduplicated), or a negative errno on failure. */
int store_put(const void *data, size_t len, char *hash_out);

ssize_t store_get(const char *hash, off_t offset, void *buf, size_t len);

#endif
