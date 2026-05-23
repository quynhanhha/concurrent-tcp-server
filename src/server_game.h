#ifndef SERVER_GAME_H
#define SERVER_GAME_H

#include "engine.h"

/*
 * Argument block for each detached join-handler thread spawned by main().
 * The thread frees this after copying out its fields.
 */
typedef struct {
    Engine *engine;
    int     fd;
} JoinHandlerArgs;

/*
 * Entry point for a detached per-client join-handler thread.
 * Reads the MSG_JOIN, matches or registers the client in the pending/active
 * registries, and when a pair is formed, spawns a detached game thread.
 */
void *join_handler_fn(void *arg);

#endif /* SERVER_GAME_H */
