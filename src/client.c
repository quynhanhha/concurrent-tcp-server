#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
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
    signal(SIGPIPE, SIG_IGN);
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

    /* Send JOIN with game_id in network byte order */
    uint32_t game_id_be = htonl(game_id);
    if (send_msg(fd, MSG_JOIN, STATUS_OK,
                 &game_id_be, sizeof(uint32_t)) < 0) {
        close(fd);
        return false;
    }

    /* Wait for JOIN_ACCEPTED */
    MsgHeader hdr = {0};
    void *payload = NULL;
    if (receive_msg(fd, &hdr, &payload) < 0) {
        close(fd);
        return false;
    }
    free(payload);
    payload = NULL;

    if (hdr.type != MSG_JOIN_ACCEPTED ||
        hdr.status != STATUS_OK ||
        hdr.length != 0) {
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

bool client_wait_for_opponent(ClientImplementation *client) {
    if (!client) return false;
    Client *c = (Client *)client;
    if (c->sockfd < 0) return false;

    MsgHeader hdr = {0};
    void *payload = NULL;
    if (receive_msg(c->sockfd, &hdr, &payload) < 0) return false;
    free(payload);

    return hdr.type   == MSG_GAME_READY &&
           hdr.status == STATUS_OK      &&
           hdr.length == 0;
}

int8_t client_send_ships(ClientImplementation *client,
                         const struct Ship (*ships)[4]) {
    if (!client || !ships) return -1;
    Client *c = (Client *)client;
    if (c->sockfd < 0) return -1;

    /* Serialize 4 ships into a flat buffer: COORD(4B) + length(1B) + dir(1B). */
    uint8_t buf[4 * SHIP_WIRE_SIZE];
    for (int i = 0; i < 4; i++) {
        uint8_t *p = buf + i * SHIP_WIRE_SIZE;
        memset(p, 0, COORD_SIZE);
        strncpy((char *)p, (*ships)[i].coordinate, COORD_SIZE - 1);
        p[COORD_SIZE]     = (*ships)[i].length;
        p[COORD_SIZE + 1] = (uint8_t)(*ships)[i].direction;
    }

    if (send_msg(c->sockfd, MSG_SHIP_SUBMIT, STATUS_OK,
                 buf, (uint16_t)sizeof(buf)) < 0) return -1;

    MsgHeader hdr = {0};
    void *payload = NULL;
    if (receive_msg(c->sockfd, &hdr, &payload) < 0) return -1;

    if (hdr.type != MSG_SHIP_RESULT || hdr.status != STATUS_OK ||
        hdr.length != 1 || payload == NULL) {
        free(payload);
        return -1;
    }

    int8_t player_num = (int8_t)((uint8_t *)payload)[0];
    free(payload);

    if (player_num != 1 && player_num != 2) return -1;

    c->player_num = player_num;
    return player_num;
}

enum TurnResult client_send_move(ClientImplementation *client, const char *coordinate) {
    if (!client || !coordinate) return Invalid;
    Client *c = (Client *)client;
    if (c->sockfd < 0) return Invalid;

    uint8_t coord_buf[COORD_SIZE] = {0};
    strncpy((char *)coord_buf, coordinate, COORD_SIZE - 1);

    if (send_msg(c->sockfd, MSG_MOVE_SUBMIT, STATUS_OK,
                 coord_buf, COORD_SIZE) < 0) return Invalid;

    MsgHeader hdr = {0};
    void *payload = NULL;
    if (receive_msg(c->sockfd, &hdr, &payload) < 0) return Invalid;

    if (hdr.type != MSG_MOVE_RESULT || hdr.status != STATUS_OK ||
        hdr.length != 1 || payload == NULL) {
        free(payload);
        return Invalid;
    }

    TurnResult result = (TurnResult)(int8_t)((uint8_t *)payload)[0];
    free(payload);
    return result;
}

struct ExtendedTurnResult client_send_move_extended(ClientImplementation *client,
                                                    const char *coordinate) {
    struct ExtendedTurnResult r = {0, NULL};
    if (!client || !coordinate) return r;
    Client *c = (Client *)client;
    if (c->sockfd < 0) return r;

    uint8_t coord_buf[COORD_SIZE] = {0};
    strncpy((char *)coord_buf, coordinate, COORD_SIZE - 1);

    if (send_msg(c->sockfd, MSG_EXT_MOVE_SUBMIT, STATUS_OK,
                 coord_buf, COORD_SIZE) < 0) return r;

    MsgHeader hdr = {0};
    void *payload = NULL;
    if (receive_msg(c->sockfd, &hdr, &payload) < 0) return r;

    if (hdr.type != MSG_EXT_MOVE_RESULT || hdr.status != STATUS_OK) {
        free(payload);
        return r;
    }

    r.length = hdr.length;
    r.data   = payload;
    return r;
}

struct MoveResult client_receive_move(ClientImplementation *client) {
    struct MoveResult r = {NULL, Invalid};
    if (!client) return r;
    Client *c = (Client *)client;
    if (c->sockfd < 0) return r;

    MsgHeader hdr = {0};
    void *payload = NULL;
    if (receive_msg(c->sockfd, &hdr, &payload) < 0) return r;

    if (hdr.type != MSG_OPPONENT_MOVE || hdr.status != STATUS_OK ||
        hdr.length != OPPONENT_MOVE_WIRE_SIZE || payload == NULL) {
        free(payload);
        return r;
    }

    char *coord = malloc(COORD_SIZE);
    if (!coord) { free(payload); return r; }
    memcpy(coord, payload, COORD_SIZE);
    coord[COORD_SIZE - 1] = '\0';

    r.coordinate = coord;
    r.result     = (TurnResult)(int8_t)((uint8_t *)payload)[COORD_SIZE];
    free(payload);
    return r;
}

void client_free_extended_result(struct ExtendedTurnResult result) {
    free((void *)result.data);
}

void client_free_move_result(struct MoveResult result) {
    free((void *)result.coordinate);
}
