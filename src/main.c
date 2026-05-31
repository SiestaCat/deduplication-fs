#define _POSIX_C_SOURCE 200809L
#define FUSE_USE_VERSION 31

#include "db.h"
#include "ops.h"
#include "store.h"
#include "util.h"

#include <errno.h>
#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int ensure_dir(const char *path) {
    if (mkdir(path, 0700) < 0 && errno != EEXIST) {
        perror(path);
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    const char *data_root = getenv("DEDUP_DATA");
    if (!data_root || !*data_root) data_root = "/data";

    LOG("startup: data_root=%s", data_root);

    if (ensure_dir(data_root) < 0) return 1;

    if (store_init(data_root) < 0) {
        LOG("error: store_init failed");
        return 1;
    }

    char dbpath[1024];
    snprintf(dbpath, sizeof(dbpath), "%s/files.sqlite", data_root);
    if (db_open(dbpath) < 0) {
        LOG("error: db_open failed (%s)", dbpath);
        return 1;
    }
    LOG("db: %s", dbpath);

    int rc = fuse_main(argc, argv, &dedup_ops, NULL);
    db_close();
    LOG("shutdown: rc=%d", rc);
    return rc;
}
