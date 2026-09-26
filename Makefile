ENGINE_DIR ?= project2-bin
ARCH ?= $(shell uname -m)

ifeq ($(ARCH),arm64)
LIB_DIR = $(ENGINE_DIR)/arm64
else ifeq ($(ARCH),aarch64)
LIB_DIR = $(ENGINE_DIR)/arm64
else
LIB_DIR = $(ENGINE_DIR)
endif

LIBENGINE = $(LIB_DIR)/libengine.a
LIBRUNNER = $(LIB_DIR)/librunner.a

CC=cc
CFLAGS=-Wall -Isrc
LDFLAGS=-pthread

all: client.a server.a

# Executables 
client: $(LIBRUNNER) client.a
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

server: server.a $(LIBENGINE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Static libraries
client.a: src/client.o src/common.o
	rm -f $@
	ar rcs $@ $^

server.a: src/server.o src/server_game.o src/common.o
	rm -f $@
	ar rcs $@ $^

# Compilation
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o client.a server.a client server

test:
	python3 tests/run_tests.py

.PHONY: all clean test
