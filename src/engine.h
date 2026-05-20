#ifndef ENGINE_H
#define ENGINE_H

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

// Opaque engine handle.
//
// Note: The engine is not thread-safe.
// Concurrent access will result in abort of process.
typedef struct Engine Engine;

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

// Initialise game engine.
// Caller is responsible for freeing the returned pointer with `engine_free`.
// Note: Only one engine instance can be active at a time.
//
// Returns a pointer to the active engine instance, or NULL on failure.
struct Engine *engine_init(void);

// Initialise a game with the given ID.
//
// * `engine` - Pointer to the game engine.
// * `game_id` - ID of game.
//
// Returns true on success, false on failure (e.g. game is already initialised).
bool engine_init_game(struct Engine *engine, uint32_t game_id);

// End a game with the given ID.
// If the specified game has not been initialised, this is a no-op.
//
// * `engine` - Pointer to the game engine.
// * `game_id` - ID of game.
void engine_end_game(struct Engine *engine, uint32_t game_id);

// Place ships.
// The first call for a particular game will place ships for player 1,
// and the second call will place ships for player 2.
//
// * `engine` - Pointer to game engine.
// * `game_id` - ID of game.
// * `ships` - Array of 4 ships to place for the current player. The caller retains ownership of the array.
//
// Returns player number (1 or 2) on success, or -1 on failure.
int8_t engine_place_ships(struct Engine *engine,
                          uint32_t game_id,
                          const struct Ship (*ships)[4]);

// Take a turn.
//
// * `engine` - Pointer to game engine.
// * `game_id` - ID of game.
// * `player_number` - Player number of player making the move.
// * `coordinate` - Coordinate to attack. The caller retains ownership of the string.
//
// Returns the result of the turn, or `TurnResult` of `Invalid` on failure.
enum TurnResult engine_take_turn(struct Engine *engine,
                                 uint32_t game_id,
                                 uint8_t player_number,
                                 const char *coordinate);

// If the client requests for extended move encoding, this function should be
// called instead of `engine_take_turn`. They have the same parameters.
//
// Returns an `ExtendedTurnResult`, which the engine on the client side will interpret.
// The `ExtendedTurnResult` must be freed by caller with `engine_free_extended_result` after use.
struct ExtendedTurnResult engine_take_turn_extended(struct Engine *engine,
                                                    uint32_t game_id,
                                                    uint8_t player_number,
                                                    const char *coordinate);

// Extracts a `TurnResult` from an `ExtendedTurnResult`.
//
// * `extended_result` - The `ExtendedTurnResult` to extract from. The caller retains ownership.
enum TurnResult engine_extract_turn_result(struct ExtendedTurnResult extended_result);

// Frees the data associated with an `ExtendedTurnResult`.
//
// * `result` - The `ExtendedTurnResult` to free.
void engine_free_extended_result(struct ExtendedTurnResult result);

// Free the engine and associated state.
// You may call this to perform cleanup on program termination.
//
// * `engine` - Pointer to game engine.
//   If a `NULL` pointer is passed, this is a no-op.
void engine_free(struct Engine *engine);

#endif  /* ENGINE_H */
