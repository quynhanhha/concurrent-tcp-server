#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include "common.h"
#include "engine.h"

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

    /* Initialise engine (exit 2 on failure). */
    Engine *engine = engine_init();
    if (!engine) {
        fprintf(stderr, "Failed to initialise engine\n");
        exit(2);
    }

    /* Create TCP socket. */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        engine_free(engine);
        exit(3);
    }

    /* Allow immediate reuse of port after restart. */
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

    /* Accept one client and perform JOIN handshake (TODO: add matching). */
    struct sockaddr_in client_addr = {0};
    socklen_t client_len = sizeof(client_addr);
    int clientfd = accept(sockfd, (struct sockaddr *)&client_addr, &client_len);
    if (clientfd < 0) {
        perror("accept");
        engine_free(engine);
        exit(4);
    }

    fprintf(stderr, "Client connected from %s\n",
            inet_ntoa(client_addr.sin_addr));

    /* Receive JOIN. */
    MsgHeader hdr;
    void *payload = NULL;
    if (receive_msg(clientfd, &hdr, &payload) < 0 || hdr.type != MSG_JOIN) {
        fprintf(stderr, "Expected MSG_JOIN, got type %d\n", hdr.type);
        send_msg(clientfd, MSG_ERROR, STATUS_PROTOCOL_ERROR, NULL, 0);
        free(payload);
        close(clientfd);
        engine_free(engine);
        exit(5);
    }

    uint32_t game_id = 0;
    if (hdr.length == sizeof(uint32_t) && payload != NULL) {
        uint32_t raw;
        __builtin_memcpy(&raw, payload, sizeof(uint32_t));
        game_id = ntohl(raw);
    }
    free(payload);
    payload = NULL;

    fprintf(stderr, "JOIN received: game_id=%u\n", game_id);

    /* Acknowledge. */
    if (send_msg(clientfd, MSG_JOIN_ACCEPTED, STATUS_OK, NULL, 0) < 0) {
        fprintf(stderr, "Failed to send JOIN_ACCEPTED\n");
        close(clientfd);
        engine_free(engine);
        exit(5);
    }

    /* TODO: game matching and full game loop go here. */

    close(clientfd);
    close(sockfd);
    engine_free(engine);
    return 0;
}
