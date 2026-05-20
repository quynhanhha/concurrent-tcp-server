#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <sys/types.h>

/* ── Wire constants ─────────────────────────────────────────────────────── */

/* Maximum coordinate string size including null terminator.
 * Longest valid coordinate is "J10" (3 chars) → 4 bytes with '\0'. */
#define COORD_SIZE 4

/* Bytes sent on the wire per ship: COORD_SIZE + 1 (length) + 1 (direction). */
#define SHIP_WIRE_SIZE (COORD_SIZE + 2)

/* ── Message types ──────────────────────────────────────────────────────── */

typedef enum {
    MSG_JOIN            =  1,  /* C→S: join game; payload = uint32_t game_id */
    MSG_JOIN_ACCEPTED   =  2,  /* S→C: join accepted; no payload             */
    MSG_JOIN_REJECTED   =  3,  /* S→C: game full; no payload                 */
    MSG_GAME_READY      =  4,  /* S→C: both players present; no payload      */
    MSG_SHIP_SUBMIT     =  5,  /* C→S: ship placements; payload = 4 ships    */
    MSG_SHIP_RESULT     =  6,  /* S→C: player number assigned; payload = 1 B */
    MSG_MOVE_SUBMIT     =  7,  /* C→S: normal move; payload = coordinate     */
    MSG_MOVE_RESULT     =  8,  /* S→C: turn result; payload = 1 B TurnResult */
    MSG_OPPONENT_MOVE   =  9,  /* S→C: opponent's move; payload = coord + result */
    MSG_EXT_MOVE_SUBMIT = 10,  /* C→S: extended move; payload = coordinate   */
    MSG_EXT_MOVE_RESULT = 11,  /* S→C: opaque binary payload from engine     */
    MSG_ERROR           = 12,  /* S→C or C→S: protocol/engine error          */
} MsgType;

/* ── Status codes ───────────────────────────────────────────────────────── */

typedef enum {
    STATUS_OK             = 0,
    STATUS_FAIL           = 1,
    STATUS_PROTOCOL_ERROR = 2,  
    STATUS_GAME_FULL      = 3,
    STATUS_NOT_YOUR_TURN  = 4,
    STATUS_ENGINE_FAIL    = 5,
    STATUS_DISCONNECTED   = 6,
} StatusCode;

/* ── Message header ─────────────────────────────────────────────────────── */

/*
 * Fixed 4-byte header that precedes every message.
 *
 *   type    – MsgType   (1 byte)
 *   status  – StatusCode (1 byte)
 *   length  – payload length in bytes, network byte order (2 bytes)
 *
 * Fields are written/read individually with write_exact/read_exact so
 * compiler padding in this struct never touches the wire.
 */
typedef struct {
    uint8_t  type;
    uint8_t  status;
    uint16_t length;   /* network byte order */
} MsgHeader;

#define MSG_HEADER_SIZE 4  /* sizeof(MsgHeader) as sent on the wire */

/* ── Exact socket I/O ───────────────────────────────────────────────────────── */

/*
 * Read exactly n bytes from fd into buf.
 * Returns n on success, 0 on clean EOF, -1 on error.
 */
ssize_t read_exact(int fd, void *buf, size_t n);

/*
 * Write exactly n bytes from buf to fd.
 * Returns n on success, -1 on error.
 */
ssize_t write_exact(int fd, const void *buf, size_t n);

/*
 * Send a framed message: write 4-byte header then payload bytes.
 * payload may be NULL when length is 0.
 * Returns 0 on success, -1 on error.
 */
int send_msg(int fd, uint8_t type, uint8_t status, const void *payload, uint16_t length);

/*
 * Receive a framed message: read 4-byte header then payload bytes.
 * On success, *out_hdr is filled and *out_payload points to a heap-allocated
 * buffer of hdr.length bytes (caller must free), or NULL if length is 0.
 * Returns 0 on success, 0-on-EOF returns -1, -1 on error.
 */
int receive_msg(int fd, MsgHeader *out_hdr, void **out_payload);

#endif /* COMMON_H */
