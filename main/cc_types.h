// main/cc_types.h — shared data model of the Claude Code Companion.
//
// The bridge on the Mac aggregates Claude Code hook events into sessions and
// sends complete snapshots over BLE (see docs/claude-code-companion/protocol.md).
// These types hold one decoded snapshot. They are plain C with fixed-size
// buffers so the same code runs in host tests and on the ESP32-C3 without heap
// allocation. Every text field is NUL-terminated, valid UTF-8, and may contain
// '\n' only in multi-line fields (q_body).
//
// The byte limits below are a protocol contract: tools/cc_companion/ccp.py
// truncates to the same values, and tests/test_cc_companion.py fails when the
// two drift apart. Each limit includes the terminating NUL.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CC_PROTO_VERSION 1

#define CC_MAX_SESSIONS 6        // sessions kept from one snapshot
#define CC_MAX_AGENTS 5          // subagent rows kept per session

#define CC_SID_MAX 12            // short session / agent id
#define CC_PROJECT_MAX 48        // project label (cwd basename)
#define CC_ACTIVITY_MAX 128      // current activity line
#define CC_NOTE_MAX 128          // last prompt or last assistant message
#define CC_AGENT_TYPE_MAX 24     // subagent type, e.g. "Explore"
#define CC_AGENT_ACTIVITY_MAX 96 // subagent activity line
#define CC_Q_TITLE_MAX 64        // attention title (tool name / question header)
#define CC_Q_BODY_MAX 512        // attention body (command / question + options)
#define CC_EVT_TEXT_MAX 96       // one-shot alert text
#define CC_VERSION_MAX 24        // bridge version string

#define CC_LINE_MAX 640          // longest accepted protocol line, excluding '\n'

typedef enum {
    CC_SESSION_WORK = 0,   // Claude is working
    CC_SESSION_PERM,       // waiting for a permission decision in the terminal
    CC_SESSION_ASK,        // waiting for an answer (AskUserQuestion, elicitation)
    CC_SESSION_DONE,       // finished its turn, waiting for the next prompt
    CC_SESSION_IDLE,       // open but quiet
    CC_SESSION_ERR,        // turn ended on an API error
    CC_SESSION_BG,         // turn ended, but shells/subagents/monitors still run
    CC_SESSION_STATE_COUNT
} cc_session_state_t;

typedef enum {
    CC_AGENT_RUN = 0,
    CC_AGENT_DONE,
    CC_AGENT_ERR,
} cc_agent_state_t;

typedef enum {
    CC_Q_NONE = 0,
    CC_Q_PERM,
    CC_Q_ASK,
} cc_q_kind_t;

typedef enum {
    CC_EVT_PERM = 0,
    CC_EVT_ASK,
    CC_EVT_DONE,
    CC_EVT_ERR,
    CC_EVT_KIND_COUNT
} cc_evt_kind_t;

typedef struct {
    char aid[CC_SID_MAX];
    cc_agent_state_t state;
    uint32_t age_s;                       // seconds in this state when sent
    char type[CC_AGENT_TYPE_MAX];
    char activity[CC_AGENT_ACTIVITY_MAX];
} cc_agent_t;

typedef struct {
    char sid[CC_SID_MAX];
    cc_session_state_t state;
    uint32_t age_s;                       // seconds in this state when sent
    char project[CC_PROJECT_MAX];
    char activity[CC_ACTIVITY_MAX];
    char note[CC_NOTE_MAX];
    uint16_t agents_total;                // may exceed agent_count
    uint16_t agents_running;
    uint8_t agent_count;
    cc_agent_t agents[CC_MAX_AGENTS];
    cc_q_kind_t q_kind;
    char q_title[CC_Q_TITLE_MAX];
    char q_body[CC_Q_BODY_MAX];
} cc_session_t;

typedef struct {
    uint32_t seq;
    uint16_t total_sessions;              // sessions the bridge tracks (may exceed count)
    uint8_t count;                        // sessions stored below
    cc_session_t sessions[CC_MAX_SESSIONS];
} cc_snapshot_t;

typedef struct {
    cc_evt_kind_t kind;
    char sid[CC_SID_MAX];
    char project[CC_PROJECT_MAX];
    char text[CC_EVT_TEXT_MAX];
} cc_event_t;

// True when a session needs the user's attention (approval or answer).
static inline bool cc_session_needs_user(cc_session_state_t state)
{
    return state == CC_SESSION_PERM || state == CC_SESSION_ASK;
}
