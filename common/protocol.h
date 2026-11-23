/*
 * protocol.h — the wire protocol shared by the server and the client (SPEC §4).
 *
 * Every message on the TCP connection is one "frame":
 *
 *     +-----------------+---------------+------------------------------+
 *     | wire_len : u32  | type : u8     | payload : (wire_len-1) bytes |
 *     +-----------------+---------------+------------------------------+
 *
 * Naming rule used everywhere in the code:
 *   - wire_len    = the u32 on the wire = 1 (type byte) + payload_len
 *   - payload_len = number of bytes after the type byte
 * No field is called just "len", so the two can never be confused.
 *
 * All integers are big-endian (network byte order). A string is a u16 length
 * followed by that many bytes, with no NUL terminator.
 *
 * Nothing in this module uses locks or threads.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Limits (SPEC §4.1) ---- */
#define HEADER_SIZE   4                  /* size of the wire_len field            */
#define MAX_WIRE_LEN  8192               /* largest allowed wire_len              */
#define MAX_PAYLOAD   (MAX_WIRE_LEN - 1) /* largest payload                       */
#define MAX_NAME      32                 /* username / group name, characters     */
#define MAX_TEXT      1000               /* message text, bytes                   */
#define NAME_BUF      (MAX_NAME + 2)     /* buffer for a name + NUL (+1 spare)    */
#define TEXT_BUF      (MAX_TEXT + 1)     /* buffer for a text + NUL               */
#define PREVIEW_MAX   30                 /* read-receipt preview, bytes           */
#define MAX_TIMED_SECONDS 3600
#define DEFAULT_PORT  5555

/* ---- Client -> server frame types (SPEC §4.2) ---- */
enum {
    T_LOGIN          = 0x01,
    T_SEND           = 0x02,
    T_SEND_TIMED     = 0x03,
    T_GET_HISTORY    = 0x04,
    T_DELETE_HISTORY = 0x05,
    T_GET_USERS      = 0x06,
    T_GROUP_CREATE   = 0x07,
    T_GROUP_ADD      = 0x08,
    T_GROUP_SEND     = 0x09,
    T_GROUP_LEAVE    = 0x0A,
    T_GROUP_LIST     = 0x0B,
    T_ACK            = 0x0C,
    T_MARK_SEEN      = 0x0D,
    T_LOGOUT         = 0x0E,
};

/* ---- Server -> client frame types (SPEC §4.3) ---- */
enum {
    T_LOGIN_OK          = 0x81,
    T_OK                = 0x82,
    T_ERROR             = 0x83,
    T_MSG               = 0x84,
    T_HISTORY_ITEM      = 0x85,
    T_HISTORY_END       = 0x86,
    T_USER_LIST         = 0x87,
    T_RECEIPT           = 0x88,
    T_EXPIRED           = 0x89,
    T_GROUP_EVENT       = 0x8A,
    T_SHUTDOWN          = 0x8B,
    T_GROUP_LIST_RESULT = 0x8C,
};

/* ---- Error codes carried in ERROR frames (SPEC §4.4) ---- */
enum {
    E_UNKNOWN_USER      = 1,
    E_ALREADY_LOGGED_IN = 2,
    E_SERVER_FULL       = 3,
    E_NOT_LOGGED_IN     = 4,
    E_BAD_REQUEST       = 5,
    E_NO_SUCH_GROUP     = 6,
    E_NOT_MEMBER        = 7,
    E_NOT_CREATOR       = 8,
    E_GROUP_EXISTS      = 9,
    E_SELF_MESSAGE      = 10,
    E_TOO_LONG          = 11,
    E_INTERNAL          = 12,
};

/* Message status in HISTORY_ITEM (SPEC §4.3). */
enum {
    STATUS_PENDING   = 0,   /* shown as "sent"      */
    STATUS_DELIVERED = 1,
    STATUS_SEEN      = 2,
    STATUS_NA        = 255, /* group messages        */
};

/* One received frame. Allocated with malloc; free it with free(). */
typedef struct {
    uint8_t  type;
    uint32_t payload_len;
    uint8_t  payload[];      /* flexible array member: payload_len bytes */
} frame_t;

/* Allocates a frame and copies the payload in. Returns NULL if out of memory. */
frame_t *frame_new(uint8_t type, const uint8_t *payload, uint32_t payload_len);

/* ------------------------------------------------------------------------
 * Building frames to send.
 *
 * A frame_builder_t is a fixed buffer large enough for the biggest frame.
 * Usage:  fb_begin(&fb, T_SEND); fb_str(&fb, "bob"); fb_str(&fb, "hi");
 *         fb_finish(&fb);  -> send fb.buf, fb.used bytes.
 * If anything does not fit, fb.err becomes true and the frame must not be sent.
 * ------------------------------------------------------------------------ */
typedef struct {
    uint8_t buf[HEADER_SIZE + MAX_WIRE_LEN];
    size_t  used;   /* bytes written so far, header included */
    bool    err;    /* set if a field did not fit             */
} frame_builder_t;

void fb_begin(frame_builder_t *fb, uint8_t type);
void fb_u8(frame_builder_t *fb, uint8_t v);
void fb_u16(frame_builder_t *fb, uint16_t v);
void fb_u32(frame_builder_t *fb, uint32_t v);
void fb_u64(frame_builder_t *fb, uint64_t v);
/* Writes a string: u16 length + bytes. s must be NUL-terminated. */
void fb_str(frame_builder_t *fb, const char *s);
/* Writes the first n bytes of s as a string. */
void fb_strn(frame_builder_t *fb, const char *s, size_t n);
/* Fills in the wire_len header. Returns false if fb.err is set. */
bool fb_finish(frame_builder_t *fb);

/* ---- One helper per server->client frame (all call fb_begin..fb_finish) ---- */
void enc_login_ok(frame_builder_t *fb, uint32_t n_unread);
void enc_ok(frame_builder_t *fb, uint64_t ref, const char *text);
void enc_error(frame_builder_t *fb, uint8_t code, const char *text);
void enc_msg(frame_builder_t *fb, uint64_t id, const char *from, const char *group,
             const char *text, uint64_t expires_at);
void enc_history_item(frame_builder_t *fb, uint64_t id, const char *from, const char *to,
                      const char *text, uint8_t status, uint64_t created_at,
                      uint64_t expires_at);
void enc_history_end(frame_builder_t *fb, uint32_t count);
void enc_name_list(frame_builder_t *fb, uint8_t type, char (*names)[NAME_BUF], int n);
void enc_receipt(frame_builder_t *fb, uint64_t id, const char *reader, const char *preview);
void enc_expired(frame_builder_t *fb, uint64_t id, const char *from);
void enc_text_frame(frame_builder_t *fb, uint8_t type, const char *text); /* GROUP_EVENT, SHUTDOWN */

/* ------------------------------------------------------------------------
 * Reading fields out of a received frame.
 *
 * reader_t walks through the payload. Every rd_* checks bounds first; on any
 * problem it sets r->err and returns 0 / an empty string, so callers can read
 * all fields and check r->err once at the end (see rd_ok).
 * ------------------------------------------------------------------------ */
typedef struct {
    const uint8_t *p;
    size_t         left;
    bool           err;       /* malformed: truncated field, bad string ...      */
    bool           too_long;  /* a text field was longer than MAX_TEXT (TOO_LONG) */
} reader_t;

void     reader_init(reader_t *r, const frame_t *f);
uint8_t  rd_u8(reader_t *r);
uint16_t rd_u16(reader_t *r);
uint32_t rd_u32(reader_t *r);
uint64_t rd_u64(reader_t *r);
/* Reads a string into out (size out_size, NUL-terminated). Sets err if the
 * string is longer than out_size-1 or contains a NUL byte. */
void     rd_str(reader_t *r, char *out, size_t out_size);
/* Reads a message text (out must hold TEXT_BUF bytes). A text longer than
 * MAX_TEXT sets too_long (not err), so the server can answer TOO_LONG. */
void     rd_text(reader_t *r, char *out);
/* True if every field was read correctly and nothing is left over. */
bool     rd_ok(const reader_t *r);

/* ---- Name validation (SPEC §4.1) ---- */
bool valid_username(const char *s);   /* 1-32 chars of [A-Za-z0-9_]      */
bool valid_groupname(const char *s);  /* '#' + 1-31 chars of [A-Za-z0-9_] */
bool is_group_name(const char *s);    /* starts with '#'                 */

/* Copies at most PREVIEW_MAX bytes of text into out (size PREVIEW_MAX+1),
 * ending in "..." if it had to shorten it. Never cuts a UTF-8 character. */
void make_preview(const char *text, char *out);

#endif
