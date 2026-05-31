CC=gcc
CFLAGS=-Wall -Wextra -Werror -std=c11
TARGET=app
SRC=src/main.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

run: all
	./$(TARGET)

clean:
	rm -f $(TARGET)
