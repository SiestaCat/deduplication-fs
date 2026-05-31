CC=gcc
PKGS=fuse3 sqlite3 libcrypto
CFLAGS=-Wall -Wextra -Werror -std=c11 $(shell pkg-config --cflags $(PKGS))
LDLIBS=$(shell pkg-config --libs $(PKGS))
TARGET=app
SRC=$(wildcard src/*.c)
OBJ=$(SRC:.c=.o)

GUI_TARGET=dedup-gui
GUI_SRC=gui/main.c
GUI_PKGS=gtk+-3.0
GUI_CFLAGS=-Wall -Wextra -Werror -std=c11 $(shell pkg-config --cflags $(GUI_PKGS))
GUI_LDLIBS=$(shell pkg-config --libs $(GUI_PKGS))

DIST_DIR=dist

.PHONY: all clean run gui all-host dist

# --- targets that run inside the Docker build stages ---
all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

gui: $(GUI_TARGET)

$(GUI_TARGET): $(GUI_SRC)
	$(CC) $(GUI_CFLAGS) -o $@ $< $(GUI_LDLIBS)

all-host: $(TARGET) $(GUI_TARGET)

# --- host-facing entrypoint: build both host binaries via Docker ---
# Produces $(DIST_DIR)/app and $(DIST_DIR)/dedup-gui on the host.
dist:
	DOCKER_BUILDKIT=1 docker build \
		--target export \
		--output type=local,dest=$(DIST_DIR) \
		.

run: all
	./$(TARGET) -f -s -o allow_other /mnt/dedup

clean:
	rm -f $(TARGET) $(GUI_TARGET) src/*.o
	rm -rf $(DIST_DIR)
