#define _POSIX_C_SOURCE 200809L

#include "db.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static sqlite3 *db = NULL;

static const char *kSchema =
    "PRAGMA foreign_keys = ON;"
    "PRAGMA journal_mode = WAL;"
    "CREATE TABLE IF NOT EXISTS entries ("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " path TEXT NOT NULL UNIQUE,"
    " parent TEXT NOT NULL,"
    " name TEXT NOT NULL,"
    " is_dir INTEGER NOT NULL,"
    " size INTEGER NOT NULL DEFAULT 0,"
    " mode INTEGER NOT NULL,"
    " created_at INTEGER NOT NULL,"
    " modified_at INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_entries_parent ON entries(parent);"
    "CREATE TABLE IF NOT EXISTS chunks ("
    " file_id INTEGER NOT NULL,"
    " idx INTEGER NOT NULL,"
    " hash TEXT NOT NULL,"
    " size INTEGER NOT NULL,"
    " PRIMARY KEY (file_id, idx),"
    " FOREIGN KEY (file_id) REFERENCES entries(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_chunks_hash ON chunks(hash);";

int db_open(const char *path) {
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite open: %s\n", sqlite3_errmsg(db));
        return -EIO;
    }
    char *err = NULL;
    rc = sqlite3_exec(db, kSchema, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "schema: %s\n", err ? err : "?");
        sqlite3_free(err);
        return -EIO;
    }
    return 0;
}

void db_close(void) {
    if (db) {
        sqlite3_close(db);
        db = NULL;
    }
}

int db_exec(const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[dedup] sqlite exec '%s': %s\n", sql,
                err ? err : "?");
        sqlite3_free(err);
        return -EIO;
    }
    return 0;
}

int db_get(const char *path, db_entry_t *out) {
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT id, is_dir, mode, size, created_at, modified_at"
        " FROM entries WHERE path = ?",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return -EIO;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    int ret = -ENOENT;
    if (rc == SQLITE_ROW) {
        out->id          = sqlite3_column_int64(st, 0);
        out->is_dir      = sqlite3_column_int(st, 1);
        out->mode        = sqlite3_column_int(st, 2);
        out->size        = sqlite3_column_int64(st, 3);
        out->created_at  = sqlite3_column_int64(st, 4);
        out->modified_at = sqlite3_column_int64(st, 5);
        ret = 0;
    }
    sqlite3_finalize(st);
    return ret;
}

static int db_create_entry(const char *path, int is_dir, int mode,
                           int64_t now, int64_t *id_out) {
    char parent[PATH_MAX], name[PATH_MAX];
    split_path(path, parent, name);

    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO entries (path, parent, name, is_dir, size, mode, created_at, modified_at)"
        " VALUES (?, ?, ?, ?, 0, ?, ?, ?)",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return -EIO;
    sqlite3_bind_text(st, 1, path,   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, parent, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, name,   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 4, is_dir);
    sqlite3_bind_int(st, 5, mode);
    sqlite3_bind_int64(st, 6, now);
    sqlite3_bind_int64(st, 7, now);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_CONSTRAINT) return -EEXIST;
    if (rc != SQLITE_DONE) return -EIO;
    if (id_out) *id_out = sqlite3_last_insert_rowid(db);
    return 0;
}

int db_create_file(const char *path, int mode, int64_t now, int64_t *id_out) {
    return db_create_entry(path, 0, mode, now, id_out);
}

int db_create_dir(const char *path, int mode, int64_t now) {
    return db_create_entry(path, 1, mode, now, NULL);
}

int db_update_file(int64_t id, int64_t size, int64_t now) {
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "UPDATE entries SET size = ?, modified_at = ? WHERE id = ?",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return -EIO;
    sqlite3_bind_int64(st, 1, size);
    sqlite3_bind_int64(st, 2, now);
    sqlite3_bind_int64(st, 3, id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -EIO;
}

int db_set_mode(int64_t id, int mode) {
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "UPDATE entries SET mode = ? WHERE id = ?", -1, &st, NULL);
    if (rc != SQLITE_OK) return -EIO;
    sqlite3_bind_int(st, 1, mode);
    sqlite3_bind_int64(st, 2, id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -EIO;
}

int db_touch(int64_t id, int64_t mtime_ns) {
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "UPDATE entries SET modified_at = ? WHERE id = ?", -1, &st, NULL);
    if (rc != SQLITE_OK) return -EIO;
    sqlite3_bind_int64(st, 1, mtime_ns);
    sqlite3_bind_int64(st, 2, id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -EIO;
}

int db_delete(const char *path) {
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "DELETE FROM entries WHERE path = ?", -1, &st, NULL);
    if (rc != SQLITE_OK) return -EIO;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    int changes = sqlite3_changes(db);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -EIO;
    if (changes == 0) return -ENOENT;
    return 0;
}

int db_rename(const char *from, const char *to) {
    char parent[PATH_MAX], name[PATH_MAX];
    split_path(to, parent, name);
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "UPDATE entries SET path = ?, parent = ?, name = ? WHERE path = ?",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return -EIO;
    sqlite3_bind_text(st, 1, to,     -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, parent, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, name,   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, from,   -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    int changes = sqlite3_changes(db);
    sqlite3_finalize(st);
    if (rc == SQLITE_CONSTRAINT) return -EEXIST;
    if (rc != SQLITE_DONE) return -EIO;
    if (changes == 0) return -ENOENT;
    return 0;
}

int db_count_children(const char *dir_path, int *out_count) {
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM entries WHERE parent = ?", -1, &st, NULL);
    if (rc != SQLITE_OK) return -EIO;
    sqlite3_bind_text(st, 1, dir_path, -1, SQLITE_STATIC);
    *out_count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) *out_count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return 0;
}

int db_list(const char *dir_path, list_cb cb, void *ctx) {
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT name, is_dir FROM entries WHERE parent = ? ORDER BY name",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return -EIO;
    sqlite3_bind_text(st, 1, dir_path, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        int is_dir = sqlite3_column_int(st, 1);
        if (cb(name, is_dir, ctx) != 0) break;
    }
    sqlite3_finalize(st);
    return 0;
}

int db_chunks_clear(int64_t file_id) {
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "DELETE FROM chunks WHERE file_id = ?", -1, &st, NULL);
    if (rc != SQLITE_OK) return -EIO;
    sqlite3_bind_int64(st, 1, file_id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -EIO;
}

int db_chunk_add(int64_t file_id, int64_t idx, const char *hash, int size) {
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO chunks (file_id, idx, hash, size) VALUES (?, ?, ?, ?)",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return -EIO;
    sqlite3_bind_int64(st, 1, file_id);
    sqlite3_bind_int64(st, 2, idx);
    sqlite3_bind_text(st, 3, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 4, size);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -EIO;
}

int db_chunks_iter(int64_t file_id, chunk_cb cb, void *ctx) {
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT idx, hash, size FROM chunks WHERE file_id = ? ORDER BY idx",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return -EIO;
    sqlite3_bind_int64(st, 1, file_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        int64_t idx = sqlite3_column_int64(st, 0);
        const char *hash = (const char *)sqlite3_column_text(st, 1);
        int size = sqlite3_column_int(st, 2);
        if (cb(idx, hash, size, ctx) != 0) break;
    }
    sqlite3_finalize(st);
    return 0;
}
