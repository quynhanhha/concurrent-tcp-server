#ifndef SERVER_GAME_H
#define SERVER_GAME_H

#include <stdint.h>
#include "engine.h"

/* All mutable state for one game instance. */
typedef struct {
    uint32_t game_id;
    int      p1_fd;
    int      p2_fd;
    int      game_inited;  /* 1 after engine_init_game succeeds */
    uint8_t  current_turn; /* 1 or 2; set to 1 after ship placement */
    int      game_over;    /* 1 once engine reports a win */
} GameState;

/*
 * Run one complete game on listen_fd.
 * Accepts two clients, plays through all phases, then returns.
 * The while(1) loop in main() immediately starts the next game.
 */
void run_game(Engine *engine, int listen_fd);

#endif /* SERVER_GAME_H */
