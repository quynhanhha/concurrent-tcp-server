#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "common.h"
#include "engine.h"

/*
 * Accept one client and complete the JOIN handshake.
 * Returns the client fd on success, -1 on failure.
 * Sets *out_game_id to the game_id sent by the client.
 */
static int accept_and_join(int listen_fd, uint32_t *out_game_id) {
    struct sockaddr_in ca = {0};
    socklen_t ca_len = sizeof(ca);

    int fd = accept(listen_fd, (struct sockaddr *)&ca, &ca_len);
    if (fd < 0) {
        perror("accept");
        return -1;
    }
    fprintf(stderr, "Client connected from %s\n", inet_ntoa(ca.sin_addr));

    MsgHeader hdr;
    void *payload = NULL;
    if (receive_msg(fd, &hdr, &payload) < 0 || hdr.type != MSG_JOIN) {
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

    if (send_msg(fd, MSG_JOIN_ACCEPTED, STATUS_OK, NULL, 0) < 0) {
        close(fd);
        return -1;
    }

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
 * Run one complete game on listen_fd.
 * Accepts two clients, plays through available phases, then returns.
 * The while(1) loop in main() immediately starts the next game.
 */
static void run_game(Engine *engine, int listen_fd) {
    int p1_fd = -1, p2_fd = -1;
    uint32_t game_id = 0;
    int game_inited = 0;

    /* Accept Player 1 */
    uint32_t p1_game_id = 0;
    p1_fd = accept_and_join(listen_fd, &p1_game_id);
    if (p1_fd < 0) return;
    game_id = p1_game_id;
    fprintf(stderr, "Player 1 joined (game_id=%u, fd=%d)\n", game_id, p1_fd);

    /* Accept Player 2 */
    uint32_t p2_game_id = 0;
    p2_fd = accept_and_join(listen_fd, &p2_game_id);
    if (p2_fd < 0) {
        close(p1_fd);
        return;
    }

    if (p2_game_id != game_id) {
        fprintf(stderr, "Game ID mismatch: P1=%u P2=%u\n", game_id, p2_game_id);
        send_msg(p2_fd, MSG_JOIN_REJECTED, STATUS_GAME_FULL, NULL, 0);
        close(p2_fd);
        close(p1_fd);
        return;
    }
    fprintf(stderr, "Player 2 joined (game_id=%u, fd=%d)\n", p2_game_id, p2_fd);

    /* Initialise engine game */
    if (!engine_init_game(engine, game_id)) {
        fprintf(stderr, "engine_init_game failed for game_id=%u\n", game_id);
        send_msg(p1_fd, MSG_ERROR, STATUS_ENGINE_FAIL, NULL, 0);
        send_msg(p2_fd, MSG_ERROR, STATUS_ENGINE_FAIL, NULL, 0);
        goto game_end;
    }
    game_inited = 1;

    /* Notify both players */
    if (send_msg(p1_fd, MSG_GAME_READY, STATUS_OK, NULL, 0) < 0 ||
        send_msg(p2_fd, MSG_GAME_READY, STATUS_OK, NULL, 0) < 0) {
        fprintf(stderr, "Failed to send GAME_READY\n");
        goto game_end;
    }
    fprintf(stderr, "Game %u ready: P1=fd%d P2=fd%d\n", game_id, p1_fd, p2_fd);

    /* Ship placement */
    {
        MsgHeader hdr;
        void *payload = NULL;

        Ship      p1_ships[4], p2_ships[4];
        char      p1_coords[4][COORD_SIZE], p2_coords[4][COORD_SIZE];

        /* Receive P1's ships first, accept-order determines player identity. */
        if (receive_msg(p1_fd, &hdr, &payload) < 0 ||
            hdr.type != MSG_SHIP_SUBMIT ||
            hdr.length != 4 * SHIP_WIRE_SIZE) {
            fprintf(stderr, "Bad SHIP_SUBMIT from P1 (type=%d len=%u)\n",
                    hdr.type, hdr.length);
            free(payload);
            send_msg(p1_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
            send_msg(p2_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
            goto game_end;
        }
        deserialize_ships(payload, p1_ships, p1_coords);
        free(payload);
        payload = NULL;

        /* Receive P2's ships. */
        if (receive_msg(p2_fd, &hdr, &payload) < 0 ||
            hdr.type != MSG_SHIP_SUBMIT ||
            hdr.length != 4 * SHIP_WIRE_SIZE) {
            fprintf(stderr, "Bad SHIP_SUBMIT from P2 (type=%d len=%u)\n",
                    hdr.type, hdr.length);
            free(payload);
            send_msg(p1_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
            send_msg(p2_fd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
            goto game_end;
        }
        deserialize_ships(payload, p2_ships, p2_coords);
        free(payload);
        payload = NULL;

        /* Place P1's ships first, engine assigns player numbers by call order. */
        int8_t r1 = engine_place_ships(engine, game_id,
                                        (const Ship (*)[4])p1_ships);
        int8_t r2 = engine_place_ships(engine, game_id,
                                        (const Ship (*)[4])p2_ships);

        if (r1 < 0 || r2 < 0) {
            fprintf(stderr, "engine_place_ships failed (r1=%d r2=%d)\n", r1, r2);
            send_msg(p1_fd, MSG_ERROR, STATUS_ENGINE_FAIL, NULL, 0);
            send_msg(p2_fd, MSG_ERROR, STATUS_ENGINE_FAIL, NULL, 0);
            goto game_end;
        }

        uint8_t pn1 = (uint8_t)r1, pn2 = (uint8_t)r2;
        if (send_msg(p1_fd, MSG_SHIP_RESULT, STATUS_OK, &pn1, 1) < 0 ||
            send_msg(p2_fd, MSG_SHIP_RESULT, STATUS_OK, &pn2, 1) < 0) {
            fprintf(stderr, "Failed to send SHIP_RESULT\n");
            goto game_end;
        }
        fprintf(stderr, "Ships placed: P1 → player %d, P2 → player %d\n",
                r1, r2);
    }

    /* TODO: gameplay */
    /* Block until a client sends a move or disconnects. */
    {
        void *payload = NULL;
        MsgHeader hdr;
        receive_msg(p1_fd, &hdr, &payload);
        free(payload);
    }

game_end:
    if (game_inited) engine_end_game(engine, game_id);
    if (p1_fd >= 0) close(p1_fd);
    if (p2_fd >= 0) close(p2_fd);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: ./server <port>\n");
        exit(1);
    }

    char *endptr;
    long port = strtol(argv[1], &endptr, 10);
    if (*endptr != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        exit(1);
    }

    Engine *engine = engine_init();
    if (!engine) {
        fprintf(stderr, "Failed to initialise engine\n");
        exit(2);
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        engine_free(engine);
        exit(3);
    }

    int enable = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        perror("setsockopt");
        engine_free(engine);
        exit(3);
    }

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        engine_free(engine);
        exit(3);
    }

    if (listen(sockfd, 8) < 0) {
        perror("listen");
        engine_free(engine);
        exit(3);
    }

    fprintf(stderr, "Listening on port %ld\n", port);

    while (1) {
        run_game(engine, sockfd);
    }
}
