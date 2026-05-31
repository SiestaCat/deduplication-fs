#define _POSIX_C_SOURCE 200809L

#include "store.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SHA512_DIGEST_BYTES 64
#define ROOT_MAX 512
#define DIR1_BUF (ROOT_MAX + 8)
#define DIR2_BUF (DIR1_BUF + 8)
#define PATH_BUF (DIR2_BUF + SHA512_HEX_LEN + 8)

static char chunks_root[ROOT_MAX];

int store_init(const char *root) {
    int n = snprintf(chunks_root, sizeof(chunks_root), "%s/chunks", root);
    if (n < 0 || (size_t)n >= sizeof(chunks_root)) return -ENAMETOOLONG;
    if (mkdir(chunks_root, 0700) < 0 && errno != EEXIST) return -errno;
    LOG("store: chunks_dir=%s chunk_size=%d", chunks_root, CHUNK_SIZE);
    return 0;
}

static int ensure_dir(const char *path) {
    if (mkdir(path, 0700) < 0 && errno != EEXIST) return -errno;
    return 0;
}

static int sha512_hex(const void *data, size_t len, char *hex_out) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return -ENOMEM;
    int rc = -EIO;
    unsigned char digest[SHA512_DIGEST_BYTES];
    unsigned int olen = 0;
    if (EVP_DigestInit_ex(ctx, EVP_sha512(), NULL) != 1) goto out;
    if (EVP_DigestUpdate(ctx, data, len) != 1) goto out;
    if (EVP_DigestFinal_ex(ctx, digest, &olen) != 1) goto out;
    bytes_to_hex(digest, olen, hex_out);
    rc = 0;
out:
    EVP_MD_CTX_free(ctx);
    return rc;
}

int store_put(const void *data, size_t len, char *hash_out) {
    int rc = sha512_hex(data, len, hash_out);
    if (rc < 0) return rc;

    char dir1[DIR1_BUF], dir2[DIR2_BUF], path[PATH_BUF];
    snprintf(dir1, sizeof(dir1), "%s/%c%c", chunks_root, hash_out[0], hash_out[1]);
    snprintf(dir2, sizeof(dir2), "%s/%c%c", dir1, hash_out[2], hash_out[3]);
    if ((rc = ensure_dir(dir1)) < 0) return rc;
    if ((rc = ensure_dir(dir2)) < 0) return rc;
    snprintf(path, sizeof(path), "%s/%s", dir2, hash_out);

    if (access(path, F_OK) == 0) return 0;  /* dedup hit */

    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        if (errno == EEXIST) return 0;       /* dedup hit (race) */
        return -errno;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, (const char *)data + off, len - off);
        if (n < 0) {
            int e = errno;
            close(fd);
            unlink(path);
            return -e;
        }
        off += (size_t)n;
    }
    close(fd);
    return 1;  /* new chunk written */
}

ssize_t store_get(const char *hash, off_t offset, void *buf, size_t len) {
    char path[PATH_BUF];
    snprintf(path, sizeof(path), "%s/%c%c/%c%c/%s",
             chunks_root, hash[0], hash[1], hash[2], hash[3], hash);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    ssize_t n = pread(fd, buf, len, offset);
    int e = errno;
    close(fd);
    if (n < 0) return -e;
    return n;
}
