#include <stdio.h>
#include <stdlib.h>
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

    /* TODO: engine init, socket setup, accept loop. */
    (void)port;
    return 0;
}
