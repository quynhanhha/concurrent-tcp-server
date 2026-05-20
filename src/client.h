#ifndef CLIENT_H
#define CLIENT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

// The direction a ship extends towards.
typedef enum Direction {
  Horizontal = 0,
  Vertical = 1,
} Direction;

// Outcome of a move.
typedef enum TurnResult {
  Miss = 0,
  Hit = 1,
  Win = 2,
  Sunk2 = 4,
  Sunk3 = 5,
  Sunk4 = 6,
  Sunk5 = 7,
  Invalid = -1,
} TurnResult;

// Pointer to the client struct holding the state for your client implementation.
typedef void ClientImplementation;

// Ship representation as a C interface.
typedef struct Ship {
  // Coordinate of ship's starting position (e.g. "A1").
  const char *coordinate;
  // Length of ship in units (2, 3, 4, 5).
  uint8_t length;
  // Direction of ship (Horizontal or Vertical).
  enum Direction direction;
} Ship;

// Extended result payload, from `engine_take_turn_extended`.
typedef struct ExtendedTurnResult {
  // Length of data in bytes.
  uint16_t length;
  // Pointer to data. This can be a NULL pointer.
  const void *data;
} ExtendedTurnResult;

// Represents the result from a move made by the opponent.
typedef struct MoveResult {
  // Coordinate of the opponent's move.
  const char *coordinate;
  // Outcome of the opponent's move.
  enum TurnResult result;
} MoveResult;

// Initialise the client-side implementation.
//
// Returns a pointer to a struct (that you define) containing client state
// information on success and NULL on failure.
extern ClientImplementation *client_init(void);

// Connect to the game server.
//
// * `client` - Pointer to client implementation.
// * `addr` - Address or hostname of server. The caller retains ownership of the string.
// * `port` - Port number of server.
// * `game_id` - ID of the game to join.
//
// Returns true on successful connection, false otherwise.
extern bool client_connect(ClientImplementation *client,
                           const char *addr,
                           uint16_t port,
                           uint32_t game_id);

// Wait until 2 players have joined the game.
//
// * `client` - Pointer to client implementation.
//
// Returns true when both players have joined, false on failure.
extern bool client_wait_for_opponent(ClientImplementation *client);

// Send the player's ship placements to the server.
// This function should not return until both player's ship placements have been acknowledged by the server.
//
// * `client` - Pointer to client implementation.
// * `ships` - Array of 4 ships to place for the current player. The caller retains ownership of the array.
//
// Returns player number of this instance (1 or 2) on success, or -1 on failure.
extern int8_t client_send_ships(ClientImplementation *client,
                                const struct Ship (*ships)[4]);

// Send the player's move to the server.
//
// * `client` - Pointer to client implementation.
// * `coordinate` - Coordinate of the move to perform. The caller retains ownership of the string.
//
// Returns result of the move.
extern enum TurnResult client_send_move(ClientImplementation *client,
                                        const char *coordinate);

// Send the player's move to the server, with request for extended move encoding.
//
// * `client` - Pointer to client implementation.
// * `coordinate` - Coordinate of the move to perform. The caller retains ownership of the string.
//
// Returns `ExtendedTurnResult`, which is parsed and freed by the caller.
extern struct ExtendedTurnResult client_send_move_extended(ClientImplementation *client,
                                                           const char *coordinate);

// Receive the opponent's move from the server.
//
// * `client` - Pointer to client implementation.
//
// Returns coordinate and result of the opponent's move, which is freed by the caller.
extern struct MoveResult client_receive_move(ClientImplementation *client);

// Frees the data associated with an `ExtendedTurnResult` returned by `client_send_move_extended`.
//
// * `result` - The `ExtendedTurnResult` to free.
extern void client_free_extended_result(struct ExtendedTurnResult result);

// Frees the data associated with a `MoveResult` returned by `client_receive_move`.
//
// * `result` - The `MoveResult` to free.
extern void client_free_move_result(struct MoveResult result);

// Disconnect and clean up client state.
//
// * `client` - Pointer to client implementation.
//   If a `NULL` pointer is passed, it should return without error.
extern void client_close(ClientImplementation *client);

#endif  /* CLIENT_H */
