#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "common.h"
#include "engine.h"

/* Per-game state for single-game mode */
typedef struct {
    uint32_t game_id;
    int      p1_fd;   /* first accepted connection = Player 1 */
    int      p2_fd;   /* second accepted connection = Player 2 */
} GameState;

/*
 * Accept one incoming client and complete the JOIN handshake.
 * Sends MSG_JOIN_ACCEPTED on success, MSG_ERROR on failure.
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
    payload = NULL;

    fprintf(stderr, "JOIN received: game_id=%u\n", game_id);

    if (send_msg(fd, MSG_JOIN_ACCEPTED, STATUS_OK, NULL, 0) < 0) {
        close(fd);
        return -1;
    }

    *out_game_id = game_id;
    return fd;
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

    /* Initialise engine, exit 2 on failure */
    Engine *engine = engine_init();
    if (!engine) {
        fprintf(stderr, "Failed to initialise engine\n");
        exit(2);
    }

    /* Create TCP socket */
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

    /* ── Accept Player 1 ── */
    uint32_t game_id = 0;
    int p1_fd = accept_and_join(sockfd, &game_id);
    if (p1_fd < 0) {
        engine_free(engine);
        exit(5);
    }
    fprintf(stderr, "Player 1 joined (game_id=%u, fd=%d)\n", game_id, p1_fd);

    /* ── Accept Player 2 ── */
    uint32_t p2_game_id = 0;
    int p2_fd = accept_and_join(sockfd, &p2_game_id);
    if (p2_fd < 0) {
        close(p1_fd);
        engine_free(engine);
        exit(5);
    }

    /* Reject if game IDs don't match. */
    if (p2_game_id != game_id) {
        fprintf(stderr, "Game ID mismatch: P1=%u P2=%u\n", game_id, p2_game_id);
        send_msg(p2_fd, MSG_JOIN_REJECTED, STATUS_GAME_FULL, NULL, 0);
        close(p2_fd);
        close(p1_fd);
        engine_free(engine);
        exit(5);
    }
    fprintf(stderr, "Player 2 joined (game_id=%u, fd=%d)\n", p2_game_id, p2_fd);

    /* ── Initialise game in engine ── */
    if (!engine_init_game(engine, game_id)) {
        fprintf(stderr, "engine_init_game failed for game_id=%u\n", game_id);
        send_msg(p1_fd, MSG_ERROR, STATUS_ENGINE_FAIL, NULL, 0);
        send_msg(p2_fd, MSG_ERROR, STATUS_ENGINE_FAIL, NULL, 0);
        close(p1_fd);
        close(p2_fd);
        engine_free(engine);
        exit(5);
    }

    /* ── Notify both players the game is ready ── */
    if (send_msg(p1_fd, MSG_GAME_READY, STATUS_OK, NULL, 0) < 0 ||
        send_msg(p2_fd, MSG_GAME_READY, STATUS_OK, NULL, 0) < 0) {
        engine_end_game(engine, game_id);
        close(p1_fd);
        close(p2_fd);
        engine_free(engine);
        exit(5);
    }
    fprintf(stderr, "Game %u ready: P1=fd%d P2=fd%d\n", game_id, p1_fd, p2_fd);

    GameState game = {game_id, p1_fd, p2_fd};

    /* TODO: ship placement. */
    (void)game;

    engine_end_game(engine, game_id);
    close(p1_fd);
    close(p2_fd);
    close(sockfd);
    engine_free(engine);
    return 0;
}
