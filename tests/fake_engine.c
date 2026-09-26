#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"

#define MAX_GAMES 128

typedef struct {
    bool active;
    uint32_t game_id;
    int placements;
} FakeGame;

struct Engine {
    FakeGame games[MAX_GAMES];
};

static bool env_enabled(const char *name) {
    const char *value = getenv(name);
    return value != NULL && strcmp(value, "1") == 0;
}

static FakeGame *find_game(struct Engine *engine, uint32_t game_id) {
    for (int i = 0; i < MAX_GAMES; i++) {
        if (engine->games[i].active && engine->games[i].game_id == game_id)
            return &engine->games[i];
    }
    return NULL;
}

struct Engine *engine_init(void) {
    if (env_enabled("FAKE_ENGINE_INIT_FAIL")) return NULL;
    return calloc(1, sizeof(struct Engine));
}

bool engine_init_game(struct Engine *engine, uint32_t game_id) {
    if (env_enabled("FAKE_GAME_INIT_FAIL")) return false;
    for (int i = 0; i < MAX_GAMES; i++) {
        if (!engine->games[i].active) {
            engine->games[i].active = true;
            engine->games[i].game_id = game_id;
            engine->games[i].placements = 0;
            return true;
        }
    }
    return false;
}

void engine_end_game(struct Engine *engine, uint32_t game_id) {
    FakeGame *game = find_game(engine, game_id);
    if (game != NULL) game->active = false;
}

int8_t engine_place_ships(struct Engine *engine, uint32_t game_id,
                           const struct Ship (*ships)[4]) {
    (void)ships;
    if (env_enabled("FAKE_PLACE_FAIL")) return -1;
    FakeGame *game = find_game(engine, game_id);
    if (game == NULL || game->placements >= 2) return -1;
    game->placements++;
    return (int8_t)game->placements;
}

static enum TurnResult result_for_coordinate(const char *coordinate) {
    if (strcmp(coordinate, "A1") == 0) return Hit;
    if (strcmp(coordinate, "B1") == 0) return Win;
    if (strcmp(coordinate, "C1") == 0) return Sunk2;
    return Miss;
}

enum TurnResult engine_take_turn(struct Engine *engine, uint32_t game_id,
                                 uint8_t player_number,
                                 const char *coordinate) {
    (void)player_number;
    if (env_enabled("FAKE_TURN_FAIL") || find_game(engine, game_id) == NULL)
        return Invalid;
    return result_for_coordinate(coordinate);
}

struct ExtendedTurnResult engine_take_turn_extended(
    struct Engine *engine, uint32_t game_id, uint8_t player_number,
    const char *coordinate) {
    struct ExtendedTurnResult result = {0, NULL};
    if (env_enabled("FAKE_EXTENDED_FAIL") ||
        find_game(engine, game_id) == NULL) return result;

    enum TurnResult turn = result_for_coordinate(coordinate);
    size_t suffix_length = strlen(coordinate) + 8;
    uint8_t *data = malloc(suffix_length);
    if (data == NULL) return result;
    data[0] = (uint8_t)turn;
    memcpy(data + 1, "EXT:", 4);
    memcpy(data + 5, coordinate, strlen(coordinate) + 1);
    data[suffix_length - 1] = player_number;
    result.length = (uint16_t)suffix_length;
    result.data = data;
    return result;
}

enum TurnResult engine_extract_turn_result(
    struct ExtendedTurnResult extended_result) {
    if (extended_result.data == NULL || extended_result.length == 0)
        return Invalid;
    return (enum TurnResult)((const uint8_t *)extended_result.data)[0];
}

void engine_free_extended_result(struct ExtendedTurnResult result) {
    free((void *)result.data);
}

void engine_free(struct Engine *engine) {
    free(engine);
}
