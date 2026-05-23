#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include "engine.h"
#include "server_game.h"

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

    signal(SIGPIPE, SIG_IGN);

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

    if (listen(sockfd, 64) < 0) {
        perror("listen");
        engine_free(engine);
        exit(3);
    }

    fprintf(stderr, "Listening on port %ld\n", port);
    fprintf(stderr, "MULTIPLE_GAMES\n");

    while (1) {
        int client_fd = accept(sockfd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        JoinHandlerArgs *args = malloc(sizeof(JoinHandlerArgs));
        if (!args) {
            close(client_fd);
            continue;
        }
        args->engine = engine;
        args->fd     = client_fd;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, join_handler_fn, args) != 0) {
            perror("pthread_create");
            free(args);
            close(client_fd);
        }
        pthread_attr_destroy(&attr);
    }
}
