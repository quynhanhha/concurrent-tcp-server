#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"

static void require_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void test_frame_round_trip(void) {
    int fds[2];
    uint8_t payload[] = {0x00, 0x01, 0xfe, 0xff};
    MsgHeader header = {0};
    void *received = NULL;

    require_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
                 "socketpair creates connected descriptors");
    require_true(send_msg(fds[0], MSG_MOVE_RESULT, STATUS_OK,
                          payload, sizeof(payload)) == 0,
                 "send_msg succeeds for a payload");
    require_true(receive_msg(fds[1], &header, &received) == 0,
                 "receive_msg succeeds for a payload");
    require_true(header.type == MSG_MOVE_RESULT &&
                 header.status == STATUS_OK &&
                 header.length == sizeof(payload),
                 "header fields round-trip exactly");
    require_true(received != NULL &&
                 memcmp(received, payload, sizeof(payload)) == 0,
                 "payload bytes round-trip exactly");

    free(received);
    close(fds[0]);
    close(fds[1]);
}

static void test_empty_frame(void) {
    int fds[2];
    MsgHeader header = {0};
    void *received = (void *)1;

    require_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
                 "socketpair creates connected descriptors");
    require_true(send_msg(fds[0], MSG_GAME_READY, STATUS_OK, NULL, 0) == 0,
                 "send_msg succeeds for an empty payload");
    require_true(receive_msg(fds[1], &header, &received) == 0,
                 "receive_msg succeeds for an empty payload");
    require_true(header.type == MSG_GAME_READY && header.length == 0 &&
                 received == NULL,
                 "empty frames have no allocated payload");

    close(fds[0]);
    close(fds[1]);
}

static void test_truncated_frames(void) {
    int fds[2];
    MsgHeader header = {0};
    void *received = NULL;
    uint8_t short_header[] = {MSG_JOIN, STATUS_OK};
    uint8_t short_payload[] = {MSG_JOIN, STATUS_OK, 0, 4, 1, 2};

    require_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
                 "socketpair creates connected descriptors");
    require_true(write(fds[0], short_header, sizeof(short_header)) ==
                     (ssize_t)sizeof(short_header),
                 "short header can be written");
    shutdown(fds[0], SHUT_WR);
    require_true(receive_msg(fds[1], &header, &received) == -1,
                 "truncated header is rejected");
    close(fds[0]);
    close(fds[1]);

    require_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
                 "socketpair creates connected descriptors again");
    require_true(write(fds[0], short_payload, sizeof(short_payload)) ==
                     (ssize_t)sizeof(short_payload),
                 "short payload can be written");
    shutdown(fds[0], SHUT_WR);
    require_true(receive_msg(fds[1], &header, &received) == -1,
                 "truncated payload is rejected");
    require_true(received == NULL, "truncated payload is freed");
    close(fds[0]);
    close(fds[1]);
}

int main(void) {
    test_frame_round_trip();
    test_empty_frame();
    test_truncated_frames();
    require_true(write_exact(-1, "x", 1) == -1,
                 "write_exact reports an invalid descriptor");
    puts("common protocol tests passed");
    return EXIT_SUCCESS;
}
