# deduplication-fs

A small content-addressed FUSE3 filesystem written in C. When a file is
written through the mount, it is split into **64-byte chunks** (`CHUNK_SIZE`
in `src/store.h`), each chunk is hashed with **SHA-512**, and stored on the
backing volume keyed by its hash. The mount itself exposes the reassembled
files; identical chunks are stored only once.

Two front-ends are provided:

- **FUSE binary (`app`)** — headless, ships as a Docker image.
- **GUI (`dedup-gui`)** — GTK3, native Linux binary. Lets you pick a
  storage folder and a mount point, mount/unmount with one button, and adds
  the mount to your file manager's bookmarks (Nautilus/Nemo/Files), much
  like Cryptomator.

## On-disk layout

The backing store (the "storage folder") contains:

```text
<storage>/
├── files.sqlite                          # entries + chunks metadata
└── chunks/
    └── 13/02/1302f18e8f3eb...d370a       # one file per unique chunk
                                          # path = <hash[0:2]>/<hash[2:4]>/<hash>
```

SQLite schema (excerpt):

```sql
entries(id, path, parent, name, is_dir, size, mode, created_at, modified_at)
chunks (file_id, idx, hash, size)        -- ordered chunks per file
```

## Logs

The FUSE binary logs every meaningful event to stderr, prefixed `[dedup]`.
Example:

```
[dedup] startup: data_root=/data
[dedup] store: chunks_dir=/data/chunks chunk_size=64
[dedup] db: /data/files.sqlite
[dedup] create: /a.txt mode=0644 file_id=12
[dedup] commit: file_id=12 size=6 chunks=1 new=1 dedup=0
[dedup] create: /b.txt mode=0644 file_id=13
[dedup] commit: file_id=13 size=6 chunks=1 new=0 dedup=1
[dedup] mkdir: /sub mode=0755
[dedup] unlink: /a.txt (size=6)
```

`new=N dedup=M` on each `commit` line shows how many chunks were newly
written versus reused.

---

## Running with Docker (headless)

### Build the image

```bash
docker build -t deduplication-fs .
```

### Run

FUSE needs `/dev/fuse` and the `SYS_ADMIN` capability. The backing store is
mounted as a host volume so it persists across runs:

```bash
mkdir -p data
docker run --rm -d \
    --name dedup \
    --cap-add SYS_ADMIN \
    --device /dev/fuse \
    --security-opt apparmor:unconfined \
    -v "$PWD/data:/data" \
    deduplication-fs
```

The FUSE mount lives at `/mnt/dedup` inside the container. Use it via
`docker exec`:

```bash
docker exec dedup sh -c 'echo "hello world" > /mnt/dedup/test.txt'
docker exec dedup ls -la /mnt/dedup
docker exec dedup cat /mnt/dedup/test.txt
docker logs dedup        # see the [dedup] logs
```

Inspect the backing layout:

```bash
docker exec dedup find /data/chunks -type f
docker exec dedup sqlite3 /data/files.sqlite "SELECT path, size FROM entries;"
```

---

## GUI for Linux (build via Docker, run on host)

The GUI is meant to run on the host (it needs a display), but **the build
itself goes through Docker** — no build toolchain or `-dev` packages have to
be installed on your machine.

### Build the host binaries

```bash
make dist
```

This runs `docker build --target export --output type=local,dest=./dist .`
under the hood. Result:

```text
dist/
├── app           # FUSE binary
└── dedup-gui     # GTK3 GUI
```

Both binaries are dynamically linked, so the host needs the **runtime**
libraries (no `-dev` packages, no compiler):

```bash
# Debian / Ubuntu
sudo apt install fuse3 libfuse3-3 libsqlite3-0 libssl3 libgtk-3-0
```

Make sure your user is in the `fuse` group so it can mount without root:

```bash
sudo usermod -aG fuse "$USER"
# log out and back in
```

### Run the GUI

```bash
./dist/dedup-gui
```

In the window:

1. **Backing storage** — pick (or create) an absolute folder where the
   chunks + `files.sqlite` will live. Example: `/home/you/Dedup/storage`.
2. **Mount point** — pick (or create) an absolute folder that will *be* the
   filesystem. Example: `/home/you/Dedup/mount`.
3. Click **Mount**. The folder is registered in your file manager's
   bookmarks (under "deduplication-fs") and opens automatically.
4. Click **Unmount** to stop. The bookmark is removed.

Last-used paths are remembered in `~/.config/dedup-fs/config`.

The GUI looks for the `app` binary next to itself (the `./dist/app`
produced by `make dist`), and falls back to `dedup-fs` on `$PATH`.

---

## Source layout

```text
src/                  # FUSE binary
├── main.c            # entrypoint: init store + db, hand off to fuse_main
├── ops.c/h           # FUSE callbacks (getattr/readdir/open/create/read/...)
├── db.c/h            # SQLite wrapper (entries + chunks)
├── store.c/h         # chunk store: SHA-512 + sharded XX/YY/<hash> layout
└── util.c/h          # LOG macro, hex encoding, path split, clock helper

gui/
└── main.c            # GTK3 GUI: file pickers, Mount/Unmount, bookmarks
```
