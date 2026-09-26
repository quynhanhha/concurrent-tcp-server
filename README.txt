# Concurrent Battleship server

A small C client/server implementation for a networked two-player Battleship
game. The server accepts multiple games concurrently, pairs clients by game ID,
and serialises access to the shared game engine.

## Architecture

```text
TCP listener
    |
    +-- detached join thread -- pending/active game registries
                                  |
                                  +-- detached game thread
                                        |
                              player 1 <-> player 2
                                        |
                                  shared engine
                              (one global mutex)
```

The listener creates one detached join thread per connection. The first client
for a game ID is held in the pending registry; the second client completes the
pair and starts a detached game thread. Each game thread owns both sockets and
does not hold a mutex while waiting for network input. Calls into the shared
engine are protected by a global mutex.

## Protocol

Every message is a length-prefixed frame:

```text
byte 0       type
byte 1       status
bytes 2..3  payload length, unsigned 16-bit big-endian
bytes 4..    payload
```

The implementation reads and writes complete frames, so TCP packet boundaries
do not affect message parsing. Main message flow:

```text
JOIN(game_id) -> JOIN_ACCEPTED
               -> GAME_READY
SHIP_SUBMIT(4 x ship records) -> SHIP_RESULT(player number)
MOVE_SUBMIT(coordinate)       -> MOVE_RESULT(result)
                                  OPPONENT_MOVE(coordinate, result)
```

The extended move path uses `EXT_MOVE_SUBMIT(coordinate)` and returns the
engine-owned opaque payload in `EXT_MOVE_RESULT`. Errors use `MSG_ERROR` with a
status such as protocol error, engine failure, game full, or disconnection.

Coordinates occupy four bytes including a NUL terminator. Each ship record is
six bytes: four-byte coordinate, one-byte length, and one-byte direction.

## Failure handling

- Short reads/writes, EOF, invalid message types, and invalid payload lengths
  terminate the affected game and notify both players where possible.
- A third client joining an active game receives `GAME_FULL`.
- Engine initialisation, placement, and turn failures are reported as engine
  errors and end the game cleanly.
- Sockets, engine game state, and dynamically allocated payloads are released
  on normal completion and disconnect paths.

## Build

Requirements: a POSIX environment, C compiler, GNU Make, and pthreads. Python
3 is also required by `make test`. The public repository does not include the
closed-source engine or client-runner libraries.

Clone and build the distributable implementation libraries:

```sh
git clone <repository-url>
cd <repository-directory>
make
```

To build the runnable programs, obtain compatible external libraries and point
`ENGINE_DIR` at their directory:

```sh
make client server ENGINE_DIR=/path/to/engine-libraries
```

This produces `client.a` and `server.a`. The external directory must contain
`libengine.a` and `librunner.a` for the target architecture.

Remove generated outputs with `make clean`.

Run the self-contained test suite with:

```sh
make test
```

The tests compile a temporary fake engine and exercise protocol framing, the
client API, game matching, concurrent games, normal and extended turns, error
handling, and disconnect cleanup. No test binaries are kept in the repository.

## Run

After building the runnable programs, start one server, then connect two
clients using the same game ID:

```sh
./server 30023
./client
./client
```

The runner prompts each client for the server name, port, and game ID, then
reads ship placements and moves from standard input. Use the same game ID for
both clients. The server listens on all interfaces; use a firewall or bind it
only inside a trusted network when experimenting beyond localhost.
