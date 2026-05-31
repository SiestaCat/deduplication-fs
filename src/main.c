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

    /* Force single-threaded FUSE. Our per-handle buffer and SQLite usage
     * aren't safe under FUSE's default multi-threaded dispatch — concurrent
     * writes on the same fd corrupt the handle's buffer and concurrent
     * commits race on the DB. Inject "-s" unless the user already passed it. */
    int has_s = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) { has_s = 1; break; }
    }
    char **new_argv = NULL;
    int new_argc = argc;
    if (!has_s) {
        new_argv = malloc((size_t)(argc + 2) * sizeof(char *));
        if (!new_argv) { LOG("error: malloc"); return 1; }
        new_argv[0] = argv[0];
        new_argv[1] = (char *)"-s";
        for (int i = 1; i < argc; i++) new_argv[i + 1] = argv[i];
        new_argv[argc + 1] = NULL;
        new_argc = argc + 1;
        LOG("forcing single-threaded FUSE (-s)");
    }

    int rc = fuse_main(new_argc, new_argv ? new_argv : argv, &dedup_ops, NULL);
    free(new_argv);
    db_close();
    LOG("shutdown: rc=%d", rc);
    return rc;
}
