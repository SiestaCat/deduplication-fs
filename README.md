# Proyecto CLI en C Dockerizado

## Estructura

```text
.
├── Dockerfile
├── Makefile
├── README.md
└── src
    └── main.c
```

## Ejecutar con Docker

```bash
docker build -t deduplication-fs-c-cli .
docker run --rm -it deduplication-fs-c-cli
```

Para detenerlo:

```bash
Ctrl+C
```
Para detenerlo:

```bash
Ctrl+C
```

## Ejecutar localmente sin Docker

Necesitas `gcc` y `make` instalados.

```bash
make
./app
```

O directamente:

```bash
make run
```

Limpiar binarios:

```bash
make clean
```
