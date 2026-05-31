#ifndef DEDUP_OPS_H
#define DEDUP_OPS_H

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

extern const struct fuse_operations dedup_ops;

#endif
