#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"

static int fail(const char *message) {
    fprintf(stderr, "client probe: %s\n", message);
    return EXIT_FAILURE;
}

int main(int argc, char **argv) {
    if (argc != 4) return fail("usage: client_probe HOST PORT GAME_ID");

    ClientImplementation *client = client_init();
    if (client == NULL) return fail("client_init failed");

    uint16_t port = (uint16_t)strtoul(argv[2], NULL, 10);
    uint32_t game_id = (uint32_t)strtoul(argv[3], NULL, 10);
    if (!client_connect(client, argv[1], port, game_id)) {
        client_close(client);
        return fail("client_connect failed");
    }
    if (!client_wait_for_opponent(client)) {
        client_close(client);
        return fail("client_wait_for_opponent failed");
    }

    struct Ship ships[4] = {
        {"A1", 2, Horizontal}, {"B2", 3, Vertical},
        {"C3", 4, Horizontal}, {"D4", 5, Vertical}
    };
    if (client_send_ships(client, &ships) != 2) {
        client_close(client);
        return fail("client_send_ships failed");
    }
    if (client_send_move(client, "A1") != Hit) {
        client_close(client);
        return fail("client_send_move failed");
    }

    struct MoveResult opponent = client_receive_move(client);
    if (opponent.result != Sunk2 || opponent.coordinate == NULL ||
        strcmp(opponent.coordinate, "C1") != 0) {
        client_free_move_result(opponent);
        client_close(client);
        return fail("client_receive_move parsed the wrong result");
    }
    client_free_move_result(opponent);

    struct ExtendedTurnResult extended =
        client_send_move_extended(client, "B1");
    if (extended.length != 4 || extended.data == NULL ||
        memcmp(extended.data, "\x04xyz", 4) != 0) {
        client_free_extended_result(extended);
        client_close(client);
        return fail("client_send_move_extended parsed the wrong result");
    }
    client_free_extended_result(extended);
    client_close(client);
    puts("client API tests passed");
    return EXIT_SUCCESS;
}
