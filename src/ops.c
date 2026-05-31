#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define FUSE_USE_VERSION 31

#include "ops.h"
#include "db.h"
#include "store.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

typedef struct {
    int64_t file_id;
    int writable;
    int dirty;
    char *buf;
    size_t size;
    size_t cap;
} handle_t;

static handle_t *h_from(struct fuse_file_info *fi) {
    return (handle_t *)(uintptr_t)fi->fh;
}

static int ensure_cap(handle_t *h, size_t need) {
    if (need <= h->cap) return 0;
    size_t newcap = h->cap ? h->cap : CHUNK_SIZE;
    while (newcap < need) newcap = newcap * 2 + CHUNK_SIZE;
    char *nb = realloc(h->buf, newcap);
    if (!nb) return -ENOMEM;
    h->buf = nb;
    h->cap = newcap;
    return 0;
}

typedef struct {
    char *out;
    size_t off;
    size_t cap;
} load_ctx_t;

static int load_cb(int64_t idx, const char *hash, int size, void *ctx_) {
    (void)idx;
    load_ctx_t *ctx = ctx_;
    if (ctx->off + (size_t)size > ctx->cap) return 1;
    ssize_t n = store_get(hash, 0, ctx->out + ctx->off, size);
    if (n < 0) return 1;
    ctx->off += (size_t)n;
    return 0;
}

static int load_all_chunks(int64_t file_id, char *out, size_t expected) {
    load_ctx_t ctx = { .out = out, .off = 0, .cap = expected };
    int rc = db_chunks_iter(file_id, load_cb, &ctx);
    if (rc < 0) return rc;
    return (int)ctx.off;
}

typedef struct {
    off_t want_start;
    off_t want_end;
    off_t cursor;
    char *out;
    size_t out_off;
} read_ctx_t;

static int read_cb(int64_t idx, const char *hash, int size, void *ctx_) {
    (void)idx;
    read_ctx_t *ctx = ctx_;
    off_t chunk_start = ctx->cursor;
    off_t chunk_end = chunk_start + size;
    ctx->cursor = chunk_end;
    if (chunk_end <= ctx->want_start) return 0;
    if (chunk_start >= ctx->want_end) return 1;
    off_t local_start = ctx->want_start > chunk_start ? ctx->want_start - chunk_start : 0;
    off_t local_end   = ctx->want_end   < chunk_end   ? ctx->want_end   - chunk_start : size;
    size_t n = (size_t)(local_end - local_start);
    ssize_t got = store_get(hash, local_start, ctx->out + ctx->out_off, n);
    if (got < 0) return 1;
    ctx->out_off += (size_t)got;
    return 0;
}

static int read_from_chunks(int64_t file_id, off_t offset, char *out, size_t len) {
    read_ctx_t ctx = {
        .want_start = offset,
        .want_end   = offset + (off_t)len,
        .cursor     = 0,
        .out        = out,
        .out_off    = 0,
    };
    db_chunks_iter(file_id, read_cb, &ctx);
    return (int)ctx.out_off;
}

static int commit_chunks(int64_t file_id, const char *buf, size_t size) {
    int rc = db_exec("BEGIN IMMEDIATE");
    if (rc < 0) return rc;

    rc = db_chunks_clear(file_id);
    if (rc < 0) goto fail;

    size_t off = 0;
    int64_t idx = 0;
    int new_chunks = 0;
    int dedup_hits = 0;
    while (off < size) {
        size_t n = size - off;
        if (n > CHUNK_SIZE) n = CHUNK_SIZE;
        char hash[SHA512_HEX_LEN + 1];
        rc = store_put(buf + off, n, hash);
        if (rc < 0) goto fail;
        if (rc == 1) new_chunks++; else dedup_hits++;
        rc = db_chunk_add(file_id, idx, hash, (int)n);
        if (rc < 0) goto fail;
        off += n;
        idx++;
    }

    rc = db_update_file(file_id, (int64_t)size, now_ns());
    if (rc < 0) goto fail;

    rc = db_exec("COMMIT");
    if (rc < 0) goto fail;

    LOG("commit: file_id=%lld size=%zu chunks=%lld new=%d dedup=%d",
        (long long)file_id, size, (long long)idx, new_chunks, dedup_hits);
    return 0;

fail:
    db_exec("ROLLBACK");
    LOG("commit FAILED: file_id=%lld size=%zu rc=%d",
        (long long)file_id, size, rc);
    return rc;
}

/* ---- FUSE callbacks ---- */

static int op_getattr(const char *path, struct stat *st,
                      struct fuse_file_info *fi) {
    (void)fi;
    memset(st, 0, sizeof(*st));

    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        st->st_uid = getuid();
        st->st_gid = getgid();
        return 0;
    }

    db_entry_t e;
    int rc = db_get(path, &e);
    if (rc < 0) return rc;

    if (e.is_dir) {
        st->st_mode = S_IFDIR | (e.mode & 0777);
        st->st_nlink = 2;
    } else {
        st->st_mode = S_IFREG | (e.mode & 0777);
        st->st_nlink = 1;
        st->st_size = e.size;
    }
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_mtim.tv_sec  = e.modified_at / 1000000000LL;
    st->st_mtim.tv_nsec = e.modified_at % 1000000000LL;
    st->st_atim = st->st_mtim;
    st->st_ctim.tv_sec  = e.created_at / 1000000000LL;
    st->st_ctim.tv_nsec = e.created_at % 1000000000LL;
    return 0;
}

typedef struct {
    void *buf;
    fuse_fill_dir_t filler;
} rd_ctx_t;

static int rd_cb(const char *name, int is_dir, void *ctx_) {
    (void)is_dir;
    rd_ctx_t *ctx = ctx_;
    ctx->filler(ctx->buf, name, NULL, 0, 0);
    return 0;
}

static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;

    if (strcmp(path, "/") != 0) {
        db_entry_t e;
        int rc = db_get(path, &e);
        if (rc < 0) return rc;
        if (!e.is_dir) return -ENOTDIR;
    }

    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    rd_ctx_t ctx = { .buf = buf, .filler = filler };
    return db_list(path, rd_cb, &ctx);
}

static int op_mkdir(const char *path, mode_t mode) {
    LOG("mkdir: %s mode=%04o", path, mode & 0777);
    return db_create_dir(path, (int)(mode & 0777), now_ns());
}

static int op_rmdir(const char *path) {
    int count = 0;
    int rc = db_count_children(path, &count);
    if (rc < 0) return rc;
    if (count > 0) return -ENOTEMPTY;
    LOG("rmdir: %s", path);
    return db_delete(path);
}

static int op_unlink(const char *path) {
    db_entry_t e;
    int rc = db_get(path, &e);
    if (rc < 0) return rc;
    if (e.is_dir) return -EISDIR;
    LOG("unlink: %s (size=%lld)", path, (long long)e.size);
    return db_delete(path);
}

static int op_rename(const char *from, const char *to, unsigned int flags) {
    (void)flags;
    LOG("rename: %s -> %s", from, to);
    db_entry_t e;
    if (db_get(to, &e) == 0) {
        int rc = db_delete(to);
        if (rc < 0) return rc;
    }
    return db_rename(from, to);
}

static int op_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    db_entry_t e;
    int rc = db_get(path, &e);
    if (rc < 0) return rc;
    return db_set_mode(e.id, (int)(mode & 0777));
}

static int op_chown(const char *path, uid_t uid, gid_t gid,
                    struct fuse_file_info *fi) {
    (void)path; (void)uid; (void)gid; (void)fi;
    return 0;
}

static int op_utimens(const char *path, const struct timespec tv[2],
                      struct fuse_file_info *fi) {
    (void)fi;
    db_entry_t e;
    int rc = db_get(path, &e);
    if (rc < 0) return rc;
    int64_t mt = tv ? ((int64_t)tv[1].tv_sec * 1000000000LL + tv[1].tv_nsec)
                    : now_ns();
    return db_touch(e.id, mt);
}

static int op_create(const char *path, mode_t mode,
                     struct fuse_file_info *fi) {
    int64_t id;
    int rc = db_create_file(path, (int)(mode & 0777), now_ns(), &id);
    if (rc < 0) return rc;
    LOG("create: %s mode=%04o file_id=%lld", path, mode & 0777, (long long)id);

    handle_t *h = calloc(1, sizeof(*h));
    if (!h) return -ENOMEM;
    h->file_id = id;
    h->writable = 1;
    h->dirty = 1;        /* even empty new files should commit (size=0) */
    fi->fh = (uint64_t)(uintptr_t)h;
    return 0;
}

static int op_open(const char *path, struct fuse_file_info *fi) {
    db_entry_t e;
    int rc = db_get(path, &e);
    if (rc < 0) return rc;
    if (e.is_dir) return -EISDIR;

    handle_t *h = calloc(1, sizeof(*h));
    if (!h) return -ENOMEM;
    h->file_id = e.id;
    h->size = (size_t)e.size;
    h->writable = (fi->flags & O_ACCMODE) != O_RDONLY;

    if (h->writable) {
        rc = ensure_cap(h, h->size > 0 ? h->size : 64);
        if (rc < 0) { free(h); return rc; }
        if (fi->flags & O_TRUNC) {
            h->size = 0;
            h->dirty = 1;
        } else if (e.size > 0) {
            int n = load_all_chunks(e.id, h->buf, h->size);
            if (n < 0) { free(h->buf); free(h); return n; }
        }
    }
    fi->fh = (uint64_t)(uintptr_t)h;
    return 0;
}

static int op_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi) {
    (void)path;
    handle_t *h = h_from(fi);

    if (h->writable) {
        if ((size_t)offset >= h->size) return 0;
        size_t n = size;
        if ((size_t)offset + n > h->size) n = h->size - (size_t)offset;
        memcpy(buf, h->buf + offset, n);
        return (int)n;
    }

    if ((size_t)offset >= h->size) return 0;
    size_t want = size;
    if ((size_t)offset + want > h->size) want = h->size - (size_t)offset;
    return read_from_chunks(h->file_id, offset, buf, want);
}

static int op_write(const char *path, const char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi) {
    (void)path;
    handle_t *h = h_from(fi);
    if (!h->writable) return -EBADF;

    size_t need = (size_t)offset + size;
    int rc = ensure_cap(h, need);
    if (rc < 0) return rc;
    if ((size_t)offset > h->size) {
        memset(h->buf + h->size, 0, (size_t)offset - h->size);
    }
    memcpy(h->buf + offset, buf, size);
    if (need > h->size) h->size = need;
    h->dirty = 1;
    return (int)size;
}

static int op_truncate(const char *path, off_t size,
                       struct fuse_file_info *fi) {
    if (fi) {
        handle_t *h = h_from(fi);
        if (!h->writable) return -EBADF;
        if ((size_t)size < h->size) {
            h->size = (size_t)size;
        } else if ((size_t)size > h->size) {
            int rc = ensure_cap(h, (size_t)size);
            if (rc < 0) return rc;
            memset(h->buf + h->size, 0, (size_t)size - h->size);
            h->size = (size_t)size;
        }
        h->dirty = 1;
        return 0;
    }

    db_entry_t e;
    int rc = db_get(path, &e);
    if (rc < 0) return rc;
    if (e.is_dir) return -EISDIR;

    char *buf = NULL;
    if (size > 0) {
        buf = calloc(1, (size_t)size);
        if (!buf) return -ENOMEM;
        if (e.size > 0) {
            size_t to_load = (size_t)size < (size_t)e.size ? (size_t)size : (size_t)e.size;
            load_all_chunks(e.id, buf, to_load);
        }
    }
    rc = commit_chunks(e.id, buf ? buf : "", (size_t)size);
    free(buf);
    return rc;
}

static int op_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    handle_t *h = h_from(fi);
    int rc = 0;
    if (h->writable && h->dirty) {
        rc = commit_chunks(h->file_id, h->buf ? h->buf : "", h->size);
    }
    free(h->buf);
    free(h);
    return rc;
}

static int op_flush(const char *path, struct fuse_file_info *fi) {
    (void)path; (void)fi;
    return 0;
}

static int op_fsync(const char *path, int datasync,
                    struct fuse_file_info *fi) {
    (void)path; (void)datasync; (void)fi;
    return 0;
}

static int op_statfs(const char *path, struct statvfs *st) {
    (void)path;
    /* Report whatever the backing filesystem reports for the chunks dir.
     * Without this, statvfs() reports 0 free → tools like unzip / cp
     * refuse to write ("no space left on device"). */
    if (statvfs(store_chunks_root(), st) < 0) return -errno;
    return 0;
}

static void *op_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    /* Disable kernel caching of attributes/lookups so a writer's commit
     * is visible to a subsequent reader (e.g. composer → unzip on the
     * same path). Default attr_timeout=1.0s would serve stale size=0
     * for up to a second after release. */
    cfg->kernel_cache    = 0;
    cfg->entry_timeout   = 0.0;
    cfg->attr_timeout    = 0.0;
    cfg->negative_timeout = 0.0;

    /* Bigger writes per FUSE round-trip for large files.
     * (conn->max_read is read-only here — FUSE3 negotiates it with the kernel.) */
    conn->max_write = 1024 * 1024;

    LOG("init: max_write=%u attr_timeout=0", conn->max_write);
    return NULL;
}

const struct fuse_operations dedup_ops = {
    .init     = op_init,
    .statfs   = op_statfs,
    .getattr  = op_getattr,
    .readdir  = op_readdir,
    .mkdir    = op_mkdir,
    .rmdir    = op_rmdir,
    .unlink   = op_unlink,
    .rename   = op_rename,
    .chmod    = op_chmod,
    .chown    = op_chown,
    .utimens  = op_utimens,
    .create   = op_create,
    .open     = op_open,
    .read     = op_read,
    .write    = op_write,
    .truncate = op_truncate,
    .release  = op_release,
    .flush    = op_flush,
    .fsync    = op_fsync,
};
