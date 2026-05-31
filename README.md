# Dockerized C CLI Project

## Structure

```text
.
├── Dockerfile
├── Makefile
├── README.md
└── src
    └── main.c
```

## Run with Docker

```bash
docker build -t deduplication-fs-c-cli .
docker run --rm -it deduplication-fs-c-cli
```

To stop it:

```bash
Ctrl+C
```

To stop it:

```bash
Ctrl+C
```

## Run locally without Docker

You need to have `gcc` and `make` installed.

```bash
make
./app
```

Or directly:

```bash
make run
```

Clean binaries:

```bash
make clean
```
