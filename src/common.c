#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include "common.h"

/* ── Exact socket I/O ───────────────────────────────────────────────────────── */

ssize_t read_exact(int fd, void *buf, size_t n) {
    size_t total = 0;
    uint8_t *p = buf;

    while (total < n) {
        ssize_t r = read(fd, p + total, n - total);
        if (r == 0) return 0;     
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)r;
    }
    return (ssize_t)total;
}

ssize_t write_exact(int fd, const void *buf, size_t n) {
    size_t total = 0;
    const uint8_t *p = buf;

    while (total < n) {
        ssize_t w = write(fd, p + total, n - total);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)w;
    }
    return (ssize_t)total;
}

/* ── Framed message I/O ─────────────────────────────────────────────────── */

int send_msg(int fd, uint8_t type, uint8_t status,
             const void *payload, uint16_t length) {
    uint8_t hdr[MSG_HEADER_SIZE];
    uint16_t length_be = htons(length);

    hdr[0] = type;
    hdr[1] = status;
    hdr[2] = (uint8_t)(length_be >> 8);
    hdr[3] = (uint8_t)(length_be & 0xFF);

    if (write_exact(fd, hdr, MSG_HEADER_SIZE) < 0) return -1;
    if (length > 0 && payload != NULL) {
        if (write_exact(fd, payload, length) < 0) return -1;
    }
    return 0;
}

int receive_msg(int fd, MsgHeader *out_hdr, void **out_payload) {
    uint8_t hdr[MSG_HEADER_SIZE];

    ssize_t r = read_exact(fd, hdr, MSG_HEADER_SIZE);
    if (r <= 0) return -1;  /* EOF or error */

    out_hdr->type   = hdr[0];
    out_hdr->status = hdr[1];
    out_hdr->length = (uint16_t)((hdr[2] << 8) | hdr[3]);  /* network → host */

    if (out_hdr->length == 0) {
        *out_payload = NULL;
        return 0;
    }

    *out_payload = malloc(out_hdr->length);
    if (!*out_payload) return -1;

    r = read_exact(fd, *out_payload, out_hdr->length);
    if (r <= 0) {
        free(*out_payload);
        *out_payload = NULL;
        return -1;
    }
    return 0;
}
