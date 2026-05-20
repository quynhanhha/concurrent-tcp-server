LIBENGINE=project2-bin/libengine.a
LIBRUNNER=project2-bin/librunner.a

CC=cc
CFLAGS=-Wall -Isrc
LDFLAGS=

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

.PHONY: all clean
