#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include "common.h"
#include "engine.h"
#include "server_game.h"

/* ── Engine mutex ───────────────────────────────────────────────────────── */

/* One global mutex serialises all engine calls.
 * Never held while blocking on socket I/O. */
static pthread_mutex_t engine_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Registries ─────────────────────────────────────────────────────────── */

#define MAX_PENDING 64
#define MAX_ACTIVE  64

/* Waiting for a second player.  engine_init_game has already succeeded. */
typedef struct {
    int      active;
    uint32_t game_id;
    int      p1_fd;
} PendingGame;

/* Game thread is running. */
typedef struct {
    int      active;
    uint32_t game_id;
} ActiveGame;

static PendingGame     pending_games[MAX_PENDING];   /* zero-init → all empty */
static ActiveGame      active_games[MAX_ACTIVE];
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Registry helpers (all require registry_mutex held by caller) ─────────── */

static int find_pending(uint32_t game_id) {
    for (int i = 0; i < MAX_PENDING; i++)
        if (pending_games[i].active && pending_games[i].game_id == game_id)
            return i;
    return -1;
}

static int find_active(uint32_t game_id) {
    for (int i = 0; i < MAX_ACTIVE; i++)
        if (active_games[i].active && active_games[i].game_id == game_id)
            return i;
    return -1;
}

static int add_pending(uint32_t game_id, int p1_fd) {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!pending_games[i].active) {
            pending_games[i].active  = 1;
            pending_games[i].game_id = game_id;
            pending_games[i].p1_fd   = p1_fd;
            return i;
        }
    }
    return -1;
}

static void remove_pending(int idx) {
    if (idx >= 0 && idx < MAX_PENDING)
        pending_games[idx].active = 0;
}

static int add_active(uint32_t game_id) {
    for (int i = 0; i < MAX_ACTIVE; i++) {
        if (!active_games[i].active) {
            active_games[i].active  = 1;
            active_games[i].game_id = game_id;
            return i;
        }
    }
    return -1;
}

static void remove_active(uint32_t game_id) {
    for (int i = 0; i < MAX_ACTIVE; i++) {
        if (active_games[i].active && active_games[i].game_id == game_id) {
            active_games[i].active = 0;
            return;
        }
    }
}

/* ── Internal types ─────────────────────────────────────────────────────── */

typedef struct {
    Engine   *engine;
    int       p1_fd;
    int       p2_fd;
    uint32_t  game_id;
} GameThreadArgs;

/* GameState is internal to run_game_for_pair. */
typedef struct {
    uint32_t game_id;
    int      p1_fd;
    int      p2_fd;
    uint8_t  current_turn;
    int      game_over;
} GameState;

/* ── Helpers ────────────────────────────────────────────────────────────── */

static void deserialize_ships(const uint8_t *buf, Ship ships[4],
                              char coords[4][COORD_SIZE]) {
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = buf + i * SHIP_WIRE_SIZE;
        memcpy(coords[i], p, COORD_SIZE);
        coords[i][COORD_SIZE - 1] = '\0';
        ships[i].coordinate = coords[i];
        ships[i].length     = p[COORD_SIZE];
        ships[i].direction  = (Direction)p[COORD_SIZE + 1];
    }
}

/* ── Game pair runner ───────────────────────────────────────────────────── */

/*
 * Run a complete game for two already-matched players.
 * Precondition: engine_init_game(engine, game_id) has already succeeded,
 * and game_id is registered in active_games.
 * Takes ownership of p1_fd and p2_fd: both are closed before returning.
 * Removes game_id from active_games and calls engine_end_game before returning.
 */
static void run_game_for_pair(Engine *engine,
                              int p1_fd, int p2_fd, uint32_t game_id) {
    GameState g = {game_id, p1_fd, p2_fd, 0, 0};

    fprintf(stderr, "Game %u starting: P1=fd%d P2=fd%d\n",
            game_id, p1_fd, p2_fd);

    /* Send JOIN_ACCEPTED to both. P1 has been blocking in client_connect
     * since its join handler ran; P2 is waiting too. */
    if (send_msg(g.p1_fd, MSG_JOIN_ACCEPTED, STATUS_OK, NULL, 0) < 0) {
        send_msg(g.p2_fd, MSG_ERROR, STATUS_DISCONNECTED, NULL, 0);
        goto game_end;
    }
    if (send_msg(g.p2_fd, MSG_JOIN_ACCEPTED, STATUS_OK, NULL, 0) < 0) {
        goto game_end;
    }

    /* Notify both players that the game is ready to start. */
    if (send_msg(g.p1_fd, MSG_GAME_READY, STATUS_OK, NULL, 0) < 0 ||
        send_msg(g.p2_fd, MSG_GAME_READY, STATUS_OK, NULL, 0) < 0) {
        fprintf(stderr, "Failed to send GAME_READY for game %u\n", game_id);
        goto game_end;
    }
    fprintf(stderr, "Game %u ready\n", game_id);

    /* Ship placement: collect from whichever player sends first.
     * Engine is always called P1-then-P2 to preserve accept-order identity. */
    {
        Ship p1_ships[4], p2_ships[4];
        char p1_coords[4][COORD_SIZE], p2_coords[4][COORD_SIZE];
        int  p1_done = 0, p2_done = 0;

        while (!p1_done || !p2_done) {
            fd_set rfds;
            FD_ZERO(&rfds);
            if (!p1_done) FD_SET(g.p1_fd, &rfds);
            if (!p2_done) FD_SET(g.p2_fd, &rfds);
            int nfds = (g.p1_fd > g.p2_fd ? g.p1_fd : g.p2_fd) + 1;

            if (select(nfds, &rfds, NULL, NULL, NULL) < 0) {
                perror("select");
                send_msg(g.p1_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
                send_msg(g.p2_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
                goto game_end;
            }

            int fds[2]   = {g.p1_fd,  g.p2_fd};
            int *done[2] = {&p1_done, &p2_done};
            Ship  (*ships[2])[4]              = {&p1_ships,  &p2_ships};
            char  (*coords[2])[4][COORD_SIZE] = {&p1_coords, &p2_coords};
            const char *names[2] = {"P1", "P2"};

            for (int i = 0; i < 2; i++) {
                if (!FD_ISSET(fds[i], &rfds)) continue;

                MsgHeader hdr = {0};
                void *payload = NULL;

                if (receive_msg(fds[i], &hdr, &payload) < 0) {
                    fprintf(stderr, "receive_msg failed for %s SHIP_SUBMIT\n",
                            names[i]);
                    free(payload);
                    send_msg(g.p1_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
                    send_msg(g.p2_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
                    goto game_end;
                }
                if (hdr.type != MSG_SHIP_SUBMIT ||
                    hdr.length != 4 * SHIP_WIRE_SIZE) {
                    fprintf(stderr, "Bad SHIP_SUBMIT from %s\n", names[i]);
                    free(payload);
                    send_msg(g.p1_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
                    send_msg(g.p2_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
                    goto game_end;
                }
                deserialize_ships(payload, *ships[i], *coords[i]);
                free(payload);
                *done[i] = 1;
            }
        }

        pthread_mutex_lock(&engine_mutex);
        int8_t r1 = engine_place_ships(engine, g.game_id,
                                        (const Ship (*)[4])p1_ships);
        int8_t r2 = engine_place_ships(engine, g.game_id,
                                        (const Ship (*)[4])p2_ships);
        pthread_mutex_unlock(&engine_mutex);

        if (r1 < 0 || r2 < 0) {
            fprintf(stderr, "engine_place_ships failed (r1=%d r2=%d)\n", r1, r2);
            send_msg(g.p1_fd, MSG_ERROR, STATUS_ENGINE_FAIL, NULL, 0);
            send_msg(g.p2_fd, MSG_ERROR, STATUS_ENGINE_FAIL, NULL, 0);
            goto game_end;
        }

        uint8_t pn1 = (uint8_t)r1, pn2 = (uint8_t)r2;
        if (send_msg(g.p1_fd, MSG_SHIP_RESULT, STATUS_OK, &pn1, 1) < 0 ||
            send_msg(g.p2_fd, MSG_SHIP_RESULT, STATUS_OK, &pn2, 1) < 0) {
            goto game_end;
        }
        fprintf(stderr, "Game %u ships placed: P1→player%d P2→player%d\n",
                game_id, r1, r2);

        g.current_turn = 1;
    }

    /* Gameplay loop */
    while (!g.game_over) {
        int active_fd = (g.current_turn == 1) ? g.p1_fd : g.p2_fd;
        int other_fd  = (g.current_turn == 1) ? g.p2_fd : g.p1_fd;

        MsgHeader hdr = {0};
        void *payload = NULL;

        if (receive_msg(active_fd, &hdr, &payload) < 0) {
            fprintf(stderr, "receive_msg failed for player %d game %u\n",
                    g.current_turn, game_id);
            free(payload);
            send_msg(active_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
            send_msg(other_fd,  MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
            goto game_end;
        }

        int extended = (hdr.type == MSG_EXT_MOVE_SUBMIT);

        if ((!extended && hdr.type != MSG_MOVE_SUBMIT) || hdr.length != COORD_SIZE) {
            fprintf(stderr, "Bad move from player %d game %u\n",
                    g.current_turn, game_id);
            free(payload);
            send_msg(active_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
            send_msg(other_fd,  MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
            goto game_end;
        }

        char coord[COORD_SIZE];
        memcpy(coord, payload, COORD_SIZE);
        coord[COORD_SIZE - 1] = '\0';
        free(payload);

        TurnResult result;

        if (extended) {
            pthread_mutex_lock(&engine_mutex);
            ExtendedTurnResult ext = engine_take_turn_extended(engine, g.game_id,
                                                               g.current_turn, coord);
            result = engine_extract_turn_result(ext);
            pthread_mutex_unlock(&engine_mutex);

            if (result == Invalid) {
                engine_free_extended_result(ext);
                send_msg(active_fd, MSG_ERROR, STATUS_ENGINE_FAIL, NULL, 0);
                send_msg(other_fd,  MSG_ERROR, STATUS_ENGINE_FAIL, NULL, 0);
                goto game_end;
            }

            if (send_msg(active_fd, MSG_EXT_MOVE_RESULT, STATUS_OK,
                         ext.data, ext.length) < 0) {
                engine_free_extended_result(ext);
                send_msg(other_fd, MSG_ERROR, STATUS_DISCONNECTED, NULL, 0);
                goto game_end;
            }
            engine_free_extended_result(ext);
        } else {
            pthread_mutex_lock(&engine_mutex);
            result = engine_take_turn(engine, g.game_id, g.current_turn, coord);
            pthread_mutex_unlock(&engine_mutex);

            if (result == Invalid) {
                send_msg(active_fd, MSG_ERROR, STATUS_ENGINE_FAIL, NULL, 0);
                send_msg(other_fd,  MSG_ERROR, STATUS_ENGINE_FAIL, NULL, 0);
                goto game_end;
            }

            uint8_t result_byte = (uint8_t)result;
            if (send_msg(active_fd, MSG_MOVE_RESULT, STATUS_OK,
                         &result_byte, 1) < 0) {
                send_msg(other_fd, MSG_ERROR, STATUS_DISCONNECTED, NULL, 0);
                goto game_end;
            }
        }

        uint8_t opp_buf[OPPONENT_MOVE_WIRE_SIZE];
        memcpy(opp_buf, coord, COORD_SIZE);
        opp_buf[COORD_SIZE] = (uint8_t)result;
        if (send_msg(other_fd, MSG_OPPONENT_MOVE, STATUS_OK,
                     opp_buf, OPPONENT_MOVE_WIRE_SIZE) < 0) {
            goto game_end;
        }

        fprintf(stderr, "Game %u: player %d coord=%s result=%d\n",
                game_id, g.current_turn, coord, (int)result);

        if (result == Win) {
            g.game_over = 1;
        } else {
            g.current_turn = (g.current_turn == 1) ? 2 : 1;
        }
    }

game_end:
    pthread_mutex_lock(&registry_mutex);
    remove_active(g.game_id);
    pthread_mutex_unlock(&registry_mutex);
    pthread_mutex_lock(&engine_mutex);
    engine_end_game(engine, g.game_id);
    pthread_mutex_unlock(&engine_mutex);
    if (g.p1_fd >= 0) close(g.p1_fd);
    if (g.p2_fd >= 0) close(g.p2_fd);
}

/* ── Game thread ────────────────────────────────────────────────────────── */

static void *game_thread_fn(void *arg) {
    GameThreadArgs *a = (GameThreadArgs *)arg;
    run_game_for_pair(a->engine, a->p1_fd, a->p2_fd, a->game_id);
    free(a);
    return NULL;
}

/* ── Join handler ───────────────────────────────────────────────────────── */

/*
 * Spawns a detached game thread for a matched pair.
 * On failure, cleans up both fds, the active registry slot, and the engine
 * game that was initialised by P1's join handler.
 */
static void spawn_game_thread(Engine *engine,
                              int p1_fd, int p2_fd, uint32_t game_id) {
    GameThreadArgs *args = malloc(sizeof(GameThreadArgs));
    if (!args) goto fail;

    args->engine  = engine;
    args->p1_fd   = p1_fd;
    args->p2_fd   = p2_fd;
    args->game_id = game_id;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, game_thread_fn, args);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        perror("pthread_create game");
        free(args);
        goto fail;
    }
    return;

fail:
    pthread_mutex_lock(&registry_mutex);
    remove_active(game_id);
    pthread_mutex_unlock(&registry_mutex);
    pthread_mutex_lock(&engine_mutex);
    engine_end_game(engine, game_id);
    pthread_mutex_unlock(&engine_mutex);
    send_msg(p1_fd, MSG_ERROR, STATUS_FAIL, NULL, 0);
    send_msg(p2_fd, MSG_ERROR, STATUS_FAIL, NULL, 0);
    close(p1_fd);
    close(p2_fd);
}

void *join_handler_fn(void *arg) {
    JoinHandlerArgs *a = (JoinHandlerArgs *)arg;
    Engine *engine = a->engine;
    int fd = a->fd;
    free(a);

    /* 1. Read MSG_JOIN and extract game_id. */
    MsgHeader hdr = {0};
    void *payload = NULL;
    if (receive_msg(fd, &hdr, &payload) < 0) {
        close(fd);
        return NULL;
    }
    if (hdr.type != MSG_JOIN) {
        send_msg(fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
        free(payload);
        close(fd);
        return NULL;
    }

    uint32_t game_id = 0;
    if (hdr.length == sizeof(uint32_t) && payload != NULL) {
        uint32_t raw;
        memcpy(&raw, payload, sizeof(uint32_t));
        game_id = ntohl(raw);
    }
    free(payload);
    fprintf(stderr, "JOIN: game_id=%u fd=%d\n", game_id, fd);

    /* 2. Registry decision (held for the entire check + init/add sequence
     *    to prevent TOCTOU between find and add). */
    pthread_mutex_lock(&registry_mutex);

    if (find_active(game_id) >= 0) {
        /* Game already running — reject. */
        pthread_mutex_unlock(&registry_mutex);
        send_msg(fd, MSG_JOIN_REJECTED, STATUS_GAME_FULL, NULL, 0);
        close(fd);
        return NULL;
    }

    int pidx = find_pending(game_id);
    if (pidx >= 0) {
        /* Match: this client is P2. */
        int p1_fd = pending_games[pidx].p1_fd;
        remove_pending(pidx);
        int aidx = add_active(game_id);
        pthread_mutex_unlock(&registry_mutex);

        if (aidx < 0) {
            /* Active registry full. */
            pthread_mutex_lock(&engine_mutex);
            engine_end_game(engine, game_id);
            pthread_mutex_unlock(&engine_mutex);
            send_msg(p1_fd, MSG_ERROR, STATUS_FAIL, NULL, 0);
            send_msg(fd,    MSG_ERROR, STATUS_FAIL, NULL, 0);
            close(p1_fd);
            close(fd);
            return NULL;
        }

        fprintf(stderr, "Matched game_id=%u P1=fd%d P2=fd%d\n",
                game_id, p1_fd, fd);
        /* Game thread sends JOIN_ACCEPTED + GAME_READY to both players. */
        spawn_game_thread(engine, p1_fd, fd, game_id);
        return NULL;
    }

    /* 3. New P1: engine_init_game (under engine_mutex, inside registry lock
     *    to prevent a concurrent join from matching before init completes). */
    pthread_mutex_lock(&engine_mutex);
    int init_ok = engine_init_game(engine, game_id);
    pthread_mutex_unlock(&engine_mutex);

    if (!init_ok) {
        pthread_mutex_unlock(&registry_mutex);
        fprintf(stderr, "engine_init_game failed for game_id=%u\n", game_id);
        send_msg(fd, MSG_ERROR, STATUS_ENGINE_FAIL, NULL, 0);
        close(fd);
        return NULL;
    }

    int slot = add_pending(game_id, fd);
    pthread_mutex_unlock(&registry_mutex);

    if (slot < 0) {
        /* Pending registry full. */
        pthread_mutex_lock(&engine_mutex);
        engine_end_game(engine, game_id);
        pthread_mutex_unlock(&engine_mutex);
        send_msg(fd, MSG_ERROR, STATUS_FAIL, NULL, 0);
        close(fd);
        return NULL;
    }

    fprintf(stderr, "P1 pending: game_id=%u fd=%d slot=%d\n", game_id, fd, slot);
    /* P1 keeps its fd open, blocking in client_connect until the game thread
     * sends MSG_JOIN_ACCEPTED (once P2 matches and the game thread starts). */
    return NULL;
}
