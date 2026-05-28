CC = gcc
CFLAGS = -Wall -Wextra -O2 -fPIC

SRC = src/vfs.c
LIB = libtinyvfs.so
TEST_BIN = vfs_test

.PHONY: all clean test

# Default target builds the shared library for the Python CLI
all: $(LIB)

# Build the shared object
$(LIB): $(SRC)
	$(CC) $(CFLAGS) -shared $< -o $@

# Build the standalone C test binary (drops -fPIC and -shared)
$(TEST_BIN): $(SRC)
	$(CC) -Wall -Wextra -O2 $< -o $@

# Compile and run the C integration tests
test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -f $(LIB) $(TEST_BIN) *.bin