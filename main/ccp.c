// main/ccp.c — see ccp.h.
#include "ccp.h"

#include <stdio.h>
#include <string.h>

#include "cc_text.h"

#define CCP_MAX_FIELDS 10

// ---------------------------------------------------------------------------
// Line assembly
// ---------------------------------------------------------------------------
void ccp_line_asm_reset(ccp_line_asm_t *a)
{
    a->len = 0;
    a->discarding = false;
    a->overflows = 0;
}

void ccp_line_asm_feed(ccp_line_asm_t *a, const uint8_t *data, size_t n,
                       ccp_line_fn fn, void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        char c = (char)data[i];
        if (c == '\n') {
            if (!a->discarding) {
                size_t len = a->len;
                if (len > 0 && a->buf[len - 1] == '\r') len--;
                a->buf[len] = '\0';
                if (len > 0) fn(a->buf, len, ctx);
            }
            a->len = 0;
            a->discarding = false;
            continue;
        }
        if (a->discarding) continue;
        if (a->len >= CC_LINE_MAX) {
            // Never grow: drop the whole line and resynchronize at the next '\n'.
            a->discarding = true;
            a->overflows++;
            a->len = 0;
            continue;
        }
        a->buf[a->len++] = c;
    }
}

// ---------------------------------------------------------------------------
// Token helpers
// ---------------------------------------------------------------------------
static const char *const SESSION_TOKENS[CC_SESSION_STATE_COUNT] = {
    "work", "perm", "ask", "done", "idle", "err", "bg",
};
static const char *const EVT_TOKENS[CC_EVT_KIND_COUNT] = { "perm", "ask", "done", "err" };

const char *ccp_session_state_token(cc_session_state_t state)
{
    return (unsigned)state < CC_SESSION_STATE_COUNT ? SESSION_TOKENS[state] : "?";
}

const char *ccp_evt_kind_token(cc_evt_kind_t kind)
{
    return (unsigned)kind < CC_EVT_KIND_COUNT ? EVT_TOKENS[kind] : "?";
}

static size_t split(char *line, char **fields, size_t max)
{
    size_t count = 0;
    char *p = line;
    fields[count++] = p;
    while (*p) {
        if (*p == '\t') {
            *p = '\0';
            if (count == max) break;   // extra fields are ignored
            fields[count++] = p + 1;
        }
        p++;
    }
    return count;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    if (*s == '\0') return false;
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (uint64_t)(*s - '0');
        if (v > UINT32_MAX) return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool parse_i64(const char *s, int64_t *out)
{
    bool neg = false;
    if (*s == '-') {
        neg = true;
        s++;
    }
    if (*s == '\0') return false;
    int64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        if (v > (INT64_MAX - 9) / 10) return false;
        v = v * 10 + (*s - '0');
    }
    *out = neg ? -v : v;
    return true;
}

static bool parse_session_state(const char *s, cc_session_state_t *out)
{
    for (int i = 0; i < CC_SESSION_STATE_COUNT; i++) {
        if (strcmp(s, SESSION_TOKENS[i]) == 0) {
            *out = (cc_session_state_t)i;
            return true;
        }
    }
    return false;
}

static bool parse_agent_state(const char *s, cc_agent_state_t *out)
{
    if (strcmp(s, "run") == 0) *out = CC_AGENT_RUN;
    else if (strcmp(s, "done") == 0) *out = CC_AGENT_DONE;
    else if (strcmp(s, "err") == 0) *out = CC_AGENT_ERR;
    else return false;
    return true;
}

static bool parse_evt_kind(const char *s, cc_evt_kind_t *out)
{
    for (int i = 0; i < CC_EVT_KIND_COUNT; i++) {
        if (strcmp(s, EVT_TOKENS[i]) == 0) {
            *out = (cc_evt_kind_t)i;
            return true;
        }
    }
    return false;
}

#define COPY(dst, src, multiline) cc_text_copy((dst), sizeof(dst), (src), strlen(src), (multiline))

static void fail(ccp_result_t *out, const char *why)
{
    out->kind = CCP_ERROR;
    out->error = why;
}

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------
void ccp_builder_init(ccp_builder_t *b, cc_snapshot_t *pending)
{
    b->pending = pending;
    ccp_builder_reset(b);
}

void ccp_builder_reset(ccp_builder_t *b)
{
    b->active = false;
    b->seq = 0;
    b->current = -1;
    b->dropping = false;
}

static cc_session_t *current_session(ccp_builder_t *b)
{
    if (!b->active || b->dropping || b->current < 0) return NULL;
    return &b->pending->sessions[b->current];
}

static void handle_snap(ccp_builder_t *b, char **f, size_t n, ccp_result_t *out)
{
    uint32_t seq, total;
    if (n < 3 || !parse_u32(f[1], &seq) || !parse_u32(f[2], &total)) {
        fail(out, "bad SNAP");
        return;
    }
    memset(b->pending, 0, sizeof(*b->pending));
    b->pending->seq = seq;
    b->pending->total_sessions = total > UINT16_MAX ? UINT16_MAX : (uint16_t)total;
    b->active = true;
    b->seq = seq;
    b->current = -1;
    b->dropping = false;
}

static void handle_session(ccp_builder_t *b, char **f, size_t n, ccp_result_t *out)
{
    if (!b->active) {
        fail(out, "S outside snapshot");
        return;
    }
    cc_session_state_t state;
    uint32_t age, total, running;
    if (n < 8 || !parse_session_state(f[2], &state) || !parse_u32(f[3], &age) ||
        !parse_u32(f[6], &total) || !parse_u32(f[7], &running)) {
        // Do not attach following rows to the previous session.
        b->dropping = true;
        fail(out, "bad S");
        return;
    }
    if (b->pending->count >= CC_MAX_SESSIONS) {
        b->dropping = true;
        return;
    }
    cc_session_t *s = &b->pending->sessions[b->pending->count];
    memset(s, 0, sizeof(*s));
    COPY(s->sid, f[1], false);
    s->state = state;
    s->age_s = age;
    COPY(s->project, f[4], false);
    COPY(s->activity, f[5], false);
    s->agents_total = total > UINT16_MAX ? UINT16_MAX : (uint16_t)total;
    s->agents_running = running > UINT16_MAX ? UINT16_MAX : (uint16_t)running;
    b->current = b->pending->count++;
    b->dropping = false;
}

static void handle_agent(ccp_builder_t *b, char **f, size_t n, ccp_result_t *out)
{
    if (!b->active) {
        fail(out, "A outside snapshot");
        return;
    }
    cc_session_t *s = current_session(b);
    if (!s) return;   // parent dropped or absent: skip silently
    cc_agent_state_t state;
    uint32_t age;
    if (n < 6 || !parse_agent_state(f[2], &state) || !parse_u32(f[3], &age)) {
        fail(out, "bad A");
        return;
    }
    if (s->agent_count >= CC_MAX_AGENTS) return;
    cc_agent_t *a = &s->agents[s->agent_count++];
    COPY(a->aid, f[1], false);
    a->state = state;
    a->age_s = age;
    COPY(a->type, f[4], false);
    COPY(a->activity, f[5], false);
}

static void handle_question(ccp_builder_t *b, char **f, size_t n, ccp_result_t *out)
{
    if (!b->active) {
        fail(out, "Q outside snapshot");
        return;
    }
    cc_session_t *s = current_session(b);
    if (!s) return;
    if (n < 4) {
        fail(out, "bad Q");
        return;
    }
    if (strcmp(f[1], "perm") == 0) s->q_kind = CC_Q_PERM;
    else if (strcmp(f[1], "ask") == 0) s->q_kind = CC_Q_ASK;
    else {
        fail(out, "bad Q kind");
        return;
    }
    COPY(s->q_title, f[2], false);
    COPY(s->q_body, f[3], true);
}

static void handle_note(ccp_builder_t *b, char **f, size_t n, ccp_result_t *out)
{
    if (!b->active) {
        fail(out, "M outside snapshot");
        return;
    }
    cc_session_t *s = current_session(b);
    if (!s) return;
    if (n < 2) {
        fail(out, "bad M");
        return;
    }
    COPY(s->note, f[1], false);
}

static void handle_end(ccp_builder_t *b, char **f, size_t n, ccp_result_t *out)
{
    uint32_t seq;
    if (!b->active) {
        fail(out, "END outside snapshot");
        return;
    }
    if (n < 2 || !parse_u32(f[1], &seq) || seq != b->seq) {
        ccp_builder_reset(b);
        fail(out, "END mismatch");
        return;
    }
    out->kind = CCP_COMMIT;
    out->seq = seq;
    b->active = false;
    b->current = -1;
    b->dropping = false;
}

void ccp_handle_line(ccp_builder_t *b, char *line, size_t len, ccp_result_t *out)
{
    (void)len;
    memset(out, 0, sizeof(*out));
    out->kind = CCP_NONE;

    char *f[CCP_MAX_FIELDS];
    size_t n = split(line, f, CCP_MAX_FIELDS);
    const char *type = f[0];

    if (strcmp(type, "S") == 0) {
        handle_session(b, f, n, out);
    } else if (strcmp(type, "A") == 0) {
        handle_agent(b, f, n, out);
    } else if (strcmp(type, "Q") == 0) {
        handle_question(b, f, n, out);
    } else if (strcmp(type, "M") == 0) {
        handle_note(b, f, n, out);
    } else if (strcmp(type, "SNAP") == 0) {
        handle_snap(b, f, n, out);
    } else if (strcmp(type, "END") == 0) {
        handle_end(b, f, n, out);
    } else if (strcmp(type, "EVT") == 0) {
        cc_evt_kind_t kind;
        if (n < 5 || !parse_evt_kind(f[1], &kind)) {
            fail(out, "bad EVT");
            return;
        }
        out->kind = CCP_EVENT;
        out->event.kind = kind;
        COPY(out->event.sid, f[2], false);
        COPY(out->event.project, f[3], false);
        COPY(out->event.text, f[4], false);
    } else if (strcmp(type, "PING") == 0) {
        out->kind = CCP_PING;
    } else if (strcmp(type, "CLOCK") == 0) {
        int64_t unix_s, offset;
        if (n < 3 || !parse_i64(f[1], &unix_s) || !parse_i64(f[2], &offset) ||
            offset < -24 * 60 || offset > 24 * 60) {
            fail(out, "bad CLOCK");
            return;
        }
        out->kind = CCP_CLOCK;
        out->unix_s = unix_s;
        out->utc_offset_min = (int32_t)offset;
    } else if (strcmp(type, "HELLO") == 0) {
        if (n < 3 || !parse_u32(f[1], &out->proto)) {
            fail(out, "bad HELLO");
            return;
        }
        out->kind = CCP_HELLO;
        COPY(out->version, f[2], false);
    } else {
        out->kind = CCP_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// Device -> bridge
// ---------------------------------------------------------------------------
static int finish(int written, size_t cap)
{
    return (written < 0 || (size_t)written >= cap) ? -1 : written;
}

int ccp_format_hi(char *out, size_t cap, const char *fw_version, int battery_pct)
{
    return finish(snprintf(out, cap, "HI\t%d\t%s\t%d\n", CC_PROTO_VERSION, fw_version,
                           battery_pct), cap);
}

int ccp_format_ack(char *out, size_t cap, uint32_t seq)
{
    return finish(snprintf(out, cap, "ACK\t%u\n", (unsigned)seq), cap);
}

int ccp_format_err(char *out, size_t cap, const char *reason)
{
    return finish(snprintf(out, cap, "ERR\t%s\n", reason), cap);
}
