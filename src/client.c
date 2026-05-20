#include <stdlib.h>
#include "client.h"
#include "common.h"

/* Internal client state. */
typedef struct Client {
    int sockfd;
    uint32_t game_id;
    int player_num;
} Client;

ClientImplementation *client_init(void) {
    Client *c = malloc(sizeof(Client));
    if (!c) return NULL;
    c->sockfd = -1;
    c->game_id = 0;
    c->player_num = 0;
    return c;
}

bool client_connect(ClientImplementation *client,
                    const char *addr,
                    uint16_t port,
                    uint32_t game_id) {
    if (!client) return false;
    (void)addr;
    (void)port;
    (void)game_id;
    return false;
}

bool client_wait_for_opponent(ClientImplementation *client) {
    if (!client) return false;
    return false;
}

int8_t client_send_ships(ClientImplementation *client,
                         const struct Ship (*ships)[4]) {
    if (!client) return -1;
    (void)ships;
    return -1;
}

enum TurnResult client_send_move(ClientImplementation *client,
                                 const char *coordinate) {
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

void client_close(ClientImplementation *client) {
    if (!client) return;
    free(client);
}
