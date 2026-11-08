// main/ccp.h — Claude Code Companion protocol (CCP/1) parser and encoder.
//
// Wire format (docs/claude-code-companion/protocol.md): UTF-8 lines ending in
// '\n', fields separated by '\t'. The bridge strips tabs, newlines, and control
// characters from text fields and encodes in-field line breaks as 0x1F.
//
// This module is pure C (no ESP-IDF, no heap) so tests/test_ccp.c can exercise
// it on the host. The link task in cc_link.c owns one assembler and one builder
// and calls them from a single task; nothing here is thread-safe by itself.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cc_types.h"

// ---------------------------------------------------------------------------
// Line assembly: BLE writes arrive in MTU-sized chunks that split lines anywhere.
// ---------------------------------------------------------------------------
typedef struct {
    char buf[CC_LINE_MAX + 1];
    size_t len;
    bool discarding;      // inside an overlong line; drop bytes until '\n'
    uint32_t overflows;   // overlong lines dropped since reset
} ccp_line_asm_t;

// Called with a NUL-terminated line (no '\n', trailing '\r' removed). The
// callee may modify the buffer; it is reused after the callback returns.
typedef void (*ccp_line_fn)(char *line, size_t len, void *ctx);

void ccp_line_asm_reset(ccp_line_asm_t *a);
void ccp_line_asm_feed(ccp_line_asm_t *a, const uint8_t *data, size_t n,
                       ccp_line_fn fn, void *ctx);

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------
typedef enum {
    CCP_NONE = 0,      // line consumed, nothing for the caller to do (snapshot row)
    CCP_HELLO,         // bridge greeting; reply with ccp_format_hi()
    CCP_CLOCK,         // wall clock update
    CCP_COMMIT,        // snapshot complete in builder->pending; reply with ACK
    CCP_EVENT,         // one-shot alert
    CCP_PING,          // keepalive
    CCP_UNKNOWN,       // unknown record type (ignored for forward compatibility)
    CCP_ERROR,         // malformed line; see error
} ccp_kind_t;

typedef struct {
    ccp_kind_t kind;
    const char *error;                 // static string when kind == CCP_ERROR
    uint32_t proto;                    // CCP_HELLO
    char version[CC_VERSION_MAX];      // CCP_HELLO
    int64_t unix_s;                    // CCP_CLOCK
    int32_t utc_offset_min;            // CCP_CLOCK
    uint32_t seq;                      // CCP_COMMIT
    cc_event_t event;                  // CCP_EVENT
} ccp_result_t;

typedef struct {
    cc_snapshot_t *pending;   // caller-owned storage filled between SNAP and END
    bool active;
    uint32_t seq;
    int current;              // index of the session that A/Q/M rows attach to, -1 none
    bool dropping;            // current S row exceeded CC_MAX_SESSIONS; skip its children
} ccp_builder_t;

void ccp_builder_init(ccp_builder_t *b, cc_snapshot_t *pending);
// Discard any partial snapshot (e.g. after a disconnect).
void ccp_builder_reset(ccp_builder_t *b);

// Parse one line (modified in place) and update the builder.
void ccp_handle_line(ccp_builder_t *b, char *line, size_t len, ccp_result_t *out);

// ---------------------------------------------------------------------------
// Device -> bridge lines. Each returns the length written (including the
// trailing '\n'), or -1 when cap is too small.
// ---------------------------------------------------------------------------
int ccp_format_hi(char *out, size_t cap, const char *fw_version, int battery_pct);
int ccp_format_ack(char *out, size_t cap, uint32_t seq);
int ccp_format_err(char *out, size_t cap, const char *reason);

// Protocol tokens, exposed for tests and the UI.
const char *ccp_session_state_token(cc_session_state_t state);
const char *ccp_evt_kind_token(cc_evt_kind_t kind);
