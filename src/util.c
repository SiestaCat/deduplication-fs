#define _POSIX_C_SOURCE 200809L

#include "util.h"

#include <string.h>
#include <time.h>

void bytes_to_hex(const unsigned char *bytes, size_t len, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0xF];
    }
    out[len * 2] = '\0';
}

void split_path(const char *path, char *parent_out, char *name_out) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        strcpy(parent_out, "/");
        strcpy(name_out, path);
        return;
    }
    if (slash == path) {
        strcpy(parent_out, "/");
        strcpy(name_out, path + 1);
        return;
    }
    size_t plen = (size_t)(slash - path);
    memcpy(parent_out, path, plen);
    parent_out[plen] = '\0';
    strcpy(name_out, slash + 1);
}

int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}
