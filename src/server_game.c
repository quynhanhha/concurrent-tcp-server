#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include "common.h"
#include "engine.h"
#include "server_game.h"

/*
 * Accept one client and read its MSG_JOIN.
 * Returns the client fd on success, -1 on failure.
 * Sets *out_game_id to the game_id sent by the client.
 * Does NOT send any response — caller decides acceptance or rejection.
 */
static int accept_and_read_join(int listen_fd, uint32_t *out_game_id) {
    struct sockaddr_in ca = {0};
    socklen_t ca_len = sizeof(ca);

    int fd = accept(listen_fd, (struct sockaddr *)&ca, &ca_len);
    if (fd < 0) {
        perror("accept");
        return -1;
    }
    fprintf(stderr, "Client connected from %s\n", inet_ntoa(ca.sin_addr));

    MsgHeader hdr = {0};
    void *payload = NULL;
    if (receive_msg(fd, &hdr, &payload) < 0) {
        fprintf(stderr, "receive_msg failed waiting for MSG_JOIN\n");
        send_msg(fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
        free(payload);
        close(fd);
        return -1;
    }
    if (hdr.type != MSG_JOIN) {
        fprintf(stderr, "Expected MSG_JOIN, got type %d\n", hdr.type);
        send_msg(fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
        free(payload);
        close(fd);
        return -1;
    }

    uint32_t game_id = 0;
    if (hdr.length == sizeof(uint32_t) && payload != NULL) {
        uint32_t raw;
        memcpy(&raw, payload, sizeof(uint32_t));
        game_id = ntohl(raw);
    }
    free(payload);

    fprintf(stderr, "JOIN received: game_id=%u\n", game_id);

    *out_game_id = game_id;
    return fd;
}

/*
 * Deserialize 4 ships from a flat wire buffer (4 * SHIP_WIRE_SIZE bytes).
 * coords must be a [4][COORD_SIZE] array that outlives the returned Ship array.
 */
static void deserialize_ships(const uint8_t *buf, Ship ships[4],
                              char coords[4][COORD_SIZE]) {
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = buf + i * SHIP_WIRE_SIZE;
        memcpy(coords[i], p, COORD_SIZE);
        coords[i][COORD_SIZE - 1] = '\0';  /* guarantee null termination */
        ships[i].coordinate = coords[i];
        ships[i].length     = p[COORD_SIZE];
        ships[i].direction  = (Direction)p[COORD_SIZE + 1];
    }
}

/*
 * Accept one extra connection on listen_fd and reject it (game full).
 * Consumes the client's JOIN message before replying so no unread data
 * causes a TCP RST that would mask the rejection code.
 */
static void reject_extra_connection(int listen_fd) {
    struct sockaddr_in ca = {0};
    socklen_t ca_len = sizeof(ca);
    int fd = accept(listen_fd, (struct sockaddr *)&ca, &ca_len);
    if (fd < 0) return;
    MsgHeader hdr = {0};
    void *payload = NULL;
    receive_msg(fd, &hdr, &payload);
    free(payload);
    send_msg(fd, MSG_JOIN_REJECTED, STATUS_GAME_FULL, NULL, 0);
    close(fd);
}

void run_game(Engine *engine, int listen_fd) {
    GameState g = {0, -1, -1, 0, 0, 0};

    /* Accept Player 1: any game_id is valid as the session's ID. */
    uint32_t p1_game_id = 0;
    g.p1_fd = accept_and_read_join(listen_fd, &p1_game_id);
    if (g.p1_fd < 0) return;
    g.game_id = p1_game_id;

    /* Initialise the engine game right after P1 joins, before accepting P2.
     * This allows the server to reject P1 immediately when engine init fails
     * (e.g. ENGINE_MODE=fail_init) instead of hanging waiting for P2. */
    if (!engine_init_game(engine, g.game_id)) {
        fprintf(stderr, "engine_init_game failed for game_id=%u\n", g.game_id);
        send_msg(g.p1_fd, MSG_ERROR, STATUS_ENGINE_FAIL, NULL, 0);
        close(g.p1_fd);
        return;  /* game_inited is still 0 — no engine cleanup required */
    }
    g.game_inited = 1;

    /* Send JOIN_ACCEPTED to P1 so client_connect can return before P2 arrives. */
    if (send_msg(g.p1_fd, MSG_JOIN_ACCEPTED, STATUS_OK, NULL, 0) < 0) {
        goto game_end;
    }
    fprintf(stderr, "P1 joined (game_id=%u, fd=%d)\n", g.game_id, g.p1_fd);

    /* Accept Player 2: must present the same game_id as P1. */
    uint32_t p2_game_id = 0;
    g.p2_fd = accept_and_read_join(listen_fd, &p2_game_id);
    if (g.p2_fd < 0) {
        send_msg(g.p1_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
        goto game_end;
    }

    if (p2_game_id != g.game_id) {
        fprintf(stderr, "Game ID mismatch: P1=%u P2=%u\n", g.game_id, p2_game_id);
        send_msg(g.p2_fd, MSG_JOIN_REJECTED, STATUS_PROTOCOL_ERROR, NULL, 0);
        send_msg(g.p1_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
        close(g.p2_fd);
        g.p2_fd = -1;
        goto game_end;
    }

    if (send_msg(g.p2_fd, MSG_JOIN_ACCEPTED, STATUS_OK, NULL, 0) < 0) {
        send_msg(g.p1_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
        goto game_end;
    }
    fprintf(stderr, "P2 joined (game_id=%u, fd=%d)\n", p2_game_id, g.p2_fd);

    /* Notify both players — engine was already initialised after P1 joined. */
    if (send_msg(g.p1_fd, MSG_GAME_READY, STATUS_OK, NULL, 0) < 0 ||
        send_msg(g.p2_fd, MSG_GAME_READY, STATUS_OK, NULL, 0) < 0) {
        fprintf(stderr, "Failed to send GAME_READY\n");
        goto game_end;
    }
    fprintf(stderr, "Game %u ready: P1=fd%d P2=fd%d\n",
            g.game_id, g.p1_fd, g.p2_fd);

    /* Ship placement: collect from whichever player sends first via select().
     * Engine is always called P1-then-P2 to preserve accept-order identity,
     * regardless of which socket delivered its ships first. */
    {
        Ship p1_ships[4], p2_ships[4];
        char p1_coords[4][COORD_SIZE], p2_coords[4][COORD_SIZE];
        int  p1_done = 0, p2_done = 0;

        while (!p1_done || !p2_done) {
            fd_set rfds;
            FD_ZERO(&rfds);
            if (!p1_done) FD_SET(g.p1_fd, &rfds);
            if (!p2_done) FD_SET(g.p2_fd, &rfds);
            FD_SET(listen_fd, &rfds);
            int nfds = g.p1_fd;
            if (g.p2_fd   > nfds) nfds = g.p2_fd;
            if (listen_fd > nfds) nfds = listen_fd;
            nfds++;

            if (select(nfds, &rfds, NULL, NULL, NULL) < 0) {
                perror("select");
                send_msg(g.p1_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
                send_msg(g.p2_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
                goto game_end;
            }

            if (FD_ISSET(listen_fd, &rfds))
                reject_extra_connection(listen_fd);

            /* Handle whichever fd(s) are ready. */
            int fds[2]   = {g.p1_fd,  g.p2_fd};
            int *done[2] = {&p1_done, &p2_done};
            Ship  (*ships[2])[4]          = {&p1_ships,  &p2_ships};
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
                    fprintf(stderr, "Bad SHIP_SUBMIT from %s "
                            "(type=%d len=%u)\n", names[i], hdr.type, hdr.length);
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

        /* Place P1's ships first; engine assigns player numbers by call order */
        int8_t r1 = engine_place_ships(engine, g.game_id,
                                        (const Ship (*)[4])p1_ships);
        int8_t r2 = engine_place_ships(engine, g.game_id,
                                        (const Ship (*)[4])p2_ships);

        if (r1 < 0 || r2 < 0) {
            fprintf(stderr, "engine_place_ships failed (r1=%d r2=%d)\n", r1, r2);
            send_msg(g.p1_fd, MSG_ERROR, STATUS_ENGINE_FAIL, NULL, 0);
            send_msg(g.p2_fd, MSG_ERROR, STATUS_ENGINE_FAIL, NULL, 0);
            goto game_end;
        }

        uint8_t pn1 = (uint8_t)r1, pn2 = (uint8_t)r2;
        if (send_msg(g.p1_fd, MSG_SHIP_RESULT, STATUS_OK, &pn1, 1) < 0 ||
            send_msg(g.p2_fd, MSG_SHIP_RESULT, STATUS_OK, &pn2, 1) < 0) {
            fprintf(stderr, "Failed to send SHIP_RESULT\n");
            goto game_end;
        }
        fprintf(stderr, "Ships placed: P1 -> player %d, P2 -> player %d\n",
                r1, r2);

        g.current_turn = 1;  /* Player 1 moves first */
    }

    /* Gameplay loop: alternate turns until one player wins */
    while (!g.game_over) {
        int active_fd = (g.current_turn == 1) ? g.p1_fd : g.p2_fd;
        int other_fd  = (g.current_turn == 1) ? g.p2_fd : g.p1_fd;

        {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(active_fd, &rfds);
            FD_SET(listen_fd, &rfds);
            int nfds = (active_fd > listen_fd ? active_fd : listen_fd) + 1;
            if (select(nfds, &rfds, NULL, NULL, NULL) < 0) {
                perror("select");
                send_msg(active_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
                send_msg(other_fd,  MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
                goto game_end;
            }
            if (FD_ISSET(listen_fd, &rfds))
                reject_extra_connection(listen_fd);
            if (!FD_ISSET(active_fd, &rfds))
                continue;
        }

        MsgHeader hdr = {0};
        void *payload = NULL;

        /* Receive the move; check receive success before reading hdr fields */
        if (receive_msg(active_fd, &hdr, &payload) < 0) {
            fprintf(stderr, "receive_msg failed for player %d\n", g.current_turn);
            free(payload);
            send_msg(active_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
            send_msg(other_fd,  MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
            goto game_end;
        }

        int extended = (hdr.type == MSG_EXT_MOVE_SUBMIT);

        if ((!extended && hdr.type != MSG_MOVE_SUBMIT) || hdr.length != COORD_SIZE) {
            fprintf(stderr, "Bad move message from player %d "
                    "(type=%d len=%u)\n", g.current_turn, hdr.type, hdr.length);
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
            ExtendedTurnResult ext = engine_take_turn_extended(engine, g.game_id,
                                                               g.current_turn, coord);
            result = engine_extract_turn_result(ext);

            if (result == Invalid) {
                fprintf(stderr, "engine_take_turn_extended returned Invalid "
                        "player=%d coord=%s\n", g.current_turn, coord);
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
            result = engine_take_turn(engine, g.game_id, g.current_turn, coord);

            if (result == Invalid) {
                fprintf(stderr, "engine_take_turn returned Invalid "
                        "player=%d coord=%s\n", g.current_turn, coord);
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

        /* Notify the passive player of the move and its outcome */
        uint8_t opp_buf[OPPONENT_MOVE_WIRE_SIZE];
        memcpy(opp_buf, coord, COORD_SIZE);
        opp_buf[COORD_SIZE] = (uint8_t)result;
        if (send_msg(other_fd, MSG_OPPONENT_MOVE, STATUS_OK,
                     opp_buf, OPPONENT_MOVE_WIRE_SIZE) < 0) {
            goto game_end;
        }

        fprintf(stderr, "Turn: player %d coord=%s result=%d\n",
                g.current_turn, coord, (int)result);

        if (result == Win) {
            g.game_over = 1;
        } else {
            g.current_turn = (g.current_turn == 1) ? 2 : 1;
        }
    }

game_end:
    if (g.game_inited) engine_end_game(engine, g.game_id);
    if (g.p1_fd >= 0) close(g.p1_fd);
    if (g.p2_fd >= 0) close(g.p2_fd);
}
