#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "client.h"
#include "common.h"

/* Internal client state. */
typedef struct Client {
    int      sockfd;
    uint32_t game_id;
    int      player_num;  /* assigned by server; 0 = unknown */
} Client;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

ClientImplementation *client_init(void) {
    Client *c = malloc(sizeof(Client));
    if (!c) return NULL;
    c->sockfd     = -1;
    c->game_id    = 0;
    c->player_num = 0;
    return c;
}

bool client_connect(ClientImplementation *client, const char *addr, uint16_t port, uint32_t game_id) {
    if (!client) return false;
    Client *c = (Client *)client;

    /* Resolve host. */
    struct addrinfo hints = {0};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo *res = NULL;
    if (getaddrinfo(addr, port_str, &hints, &res) != 0 || res == NULL) {
        return false;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return false;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        close(fd);
        return false;
    }
    freeaddrinfo(res);

    /* Send JOIN with game_id in network byte order. */
    uint32_t game_id_be = htonl(game_id);
    if (send_msg(fd, MSG_JOIN, STATUS_OK,
                 &game_id_be, sizeof(uint32_t)) < 0) {
        close(fd);
        return false;
    }

    /* Wait for JOIN_ACCEPTED. */
    MsgHeader hdr;
    void *payload = NULL;
    if (receive_msg(fd, &hdr, &payload) < 0) {
        close(fd);
        return false;
    }
    free(payload);
    payload = NULL;

    if (hdr.type != MSG_JOIN_ACCEPTED) {
        close(fd);
        return false;
    }

    c->sockfd  = fd;
    c->game_id = game_id;
    return true;
}

void client_close(ClientImplementation *client) {
    if (!client) return;
    Client *c = (Client *)client;
    if (c->sockfd >= 0) {
        close(c->sockfd);
        c->sockfd = -1;
    }
    free(c);
}

/* ── Stubs (TODO: add implementation) ────────────────────────────────────── */

bool client_wait_for_opponent(ClientImplementation *client) {
    if (!client) return false;
    return false;
}

int8_t client_send_ships(ClientImplementation *client, const struct Ship (*ships)[4]) {
    if (!client) return -1;
    (void)ships;
    return -1;
}

enum TurnResult client_send_move(ClientImplementation *client, const char *coordinate) {
    if (!client) return Invalid;
    (void)coordinate;
    return Invalid;
}

struct ExtendedTurnResult client_send_move_extended(ClientImplementation *client,
                                                    const char *coordinate) {
    struct ExtendedTurnResult r = {0, NULL};
    if (!client) return r;
    (void)coordinate;
    return r;
}

struct MoveResult client_receive_move(ClientImplementation *client) {
    struct MoveResult r = {NULL, Invalid};
    if (!client) return r;
    return r;
}

void client_free_extended_result(struct ExtendedTurnResult result) {
    free((void *)result.data);
}

void client_free_move_result(struct MoveResult result) {
    free((void *)result.coordinate);
}
