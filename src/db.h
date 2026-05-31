#ifndef DEDUP_DB_H
#define DEDUP_DB_H

#include <stdint.h>
#include <sys/types.h>

typedef struct {
    int64_t id;
    int is_dir;
    int mode;
    int64_t size;
    int64_t created_at;
    int64_t modified_at;
} db_entry_t;

int db_open(const char *path);
void db_close(void);

int db_get(const char *path, db_entry_t *out);

int db_create_file(const char *path, int mode, int64_t now_ns, int64_t *id_out);
int db_create_dir(const char *path, int mode, int64_t now_ns);

int db_update_file(int64_t id, int64_t size, int64_t now_ns);
int db_set_mode(int64_t id, int mode);
int db_touch(int64_t id, int64_t mtime_ns);

int db_delete(const char *path);
int db_rename(const char *from, const char *to);
int db_count_children(const char *dir_path, int *out_count);

typedef int (*list_cb)(const char *name, int is_dir, void *ctx);
int db_list(const char *dir_path, list_cb cb, void *ctx);

int db_chunks_clear(int64_t file_id);
int db_chunk_add(int64_t file_id, int64_t idx, const char *hash, int size);

typedef int (*chunk_cb)(int64_t idx, const char *hash, int size, void *ctx);
int db_chunks_iter(int64_t file_id, chunk_cb cb, void *ctx);

#endif
