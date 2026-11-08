// Host tests for main/ccp.c, including the cross-language golden transcript
// written by tools/cc_companion (tests/fixtures/ccp_scenario.txt). Run from the
// repository root.
#include "ccp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)
#define CHECK_STR(actual, expected) do { \
    if (strcmp((actual), (expected)) != 0) { \
        fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, (actual), (expected)); \
        failures++; } \
} while (0)

// Collects parser results while feeding bytes through the line assembler.
typedef struct {
    ccp_builder_t builder;
    cc_snapshot_t pending;
    cc_snapshot_t committed;
    int commits;
    int errors;
    int events;
    cc_event_t last_event;
    ccp_result_t last;
    int hellos;
    int clocks;
    int pings;
    int unknown;
} harness_t;

static void on_line(char *line, size_t len, void *ctx)
{
    harness_t *h = ctx;
    ccp_result_t r;
    ccp_handle_line(&h->builder, line, len, &r);
    h->last = r;
    switch (r.kind) {
    case CCP_COMMIT:
        h->committed = h->pending;
        h->commits++;
        break;
    case CCP_ERROR:
        h->errors++;
        break;
    case CCP_EVENT:
        h->events++;
        h->last_event = r.event;
        break;
    case CCP_HELLO:
        h->hellos++;
        break;
    case CCP_CLOCK:
        h->clocks++;
        break;
    case CCP_PING:
        h->pings++;
        break;
    case CCP_UNKNOWN:
        h->unknown++;
        break;
    default:
        break;
    }
}

static harness_t *harness_new(void)
{
    harness_t *h = calloc(1, sizeof(*h));
    ccp_builder_init(&h->builder, &h->pending);
    return h;
}

static void feed(harness_t *h, ccp_line_asm_t *a, const char *text, size_t chunk)
{
    size_t n = strlen(text);
    for (size_t i = 0; i < n; i += chunk) {
        size_t part = n - i < chunk ? n - i : chunk;
        ccp_line_asm_feed(a, (const uint8_t *)text + i, part, on_line, h);
    }
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static void test_golden(void)
{
    char *text = read_file("tests/fixtures/ccp_scenario.txt");
    CHECK(text != NULL);
    if (!text) return;
    // Feed in awkward chunk sizes, as BLE writes split lines anywhere.
    const size_t chunks[] = { 1, 7, 20, 244, 4096 };
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        harness_t *h = harness_new();
        ccp_line_asm_t a;
        ccp_line_asm_reset(&a);
        feed(h, &a, text, chunks[c]);
        CHECK(h->errors == 0 && h->unknown == 0);
        CHECK(h->hellos == 1 && h->clocks == 1 && h->pings == 1);
        CHECK(h->events == 2 && h->commits == 1);
        CHECK(h->last_event.kind == CC_EVT_ASK);
        CHECK_STR(h->last_event.text, "Which channel?");

        const cc_snapshot_t *s = &h->committed;
        CHECK(s->seq == 7 && s->total_sessions == 4 && s->count == 4);
        const cc_session_t *a0 = &s->sessions[0];
        CHECK_STR(a0->sid, "aaaaaaaa");
        CHECK(a0->state == CC_SESSION_PERM && a0->age_s == 888);
        CHECK_STR(a0->project, "ai-passport #1");
        CHECK_STR(a0->activity, "$ idf.py build --ccache");
        CHECK_STR(a0->note, "> Build the firmware");
        CHECK(a0->q_kind == CC_Q_PERM);
        CHECK_STR(a0->q_title, "Bash");
        CHECK_STR(a0->q_body, "rm -rf build/\nidf.py fullclean\n# Clean");

        const cc_session_t *d = &s->sessions[1];   // background work after the turn
        CHECK_STR(d->project, "react-native-backend");
        CHECK(d->state == CC_SESSION_BG && d->agents_running == 1 && d->agent_count == 1);
        CHECK_STR(d->activity, "1 shell running");
        CHECK_STR(d->agents[0].type, "shell");
        CHECK_STR(d->agents[0].activity, "npm run dev");

        const cc_session_t *b = &s->sessions[2];
        CHECK_STR(b->project, "\xE7\x94\x9F\xE6\x88\x90\xE5\xAE\x9E\xE9\xAA\x8C\xE5\xAE\xA4");   // 生成实验室
        CHECK(b->state == CC_SESSION_WORK && b->agents_total == 2 && b->agents_running == 1);
        CHECK(b->agent_count == 2);
        CHECK(b->agents[0].state == CC_AGENT_RUN);
        CHECK_STR(b->agents[0].type, "Explore");
        CHECK_STR(b->agents[0].activity, "Grep \"assay\" in data");
        CHECK(b->agents[1].state == CC_AGENT_DONE);
        CHECK(b->q_kind == CC_Q_NONE);

        CHECK(s->sessions[3].state == CC_SESSION_DONE);
        free(h);
    }
    free(text);
}

static void test_assembler_overflow(void)
{
    harness_t *h = harness_new();
    ccp_line_asm_t a;
    ccp_line_asm_reset(&a);
    char *big = malloc(CC_LINE_MAX + 100);
    memset(big, 'x', CC_LINE_MAX + 50);
    memcpy(big, "EVT\tdone\ts\tp\t", 13);
    big[CC_LINE_MAX + 50] = '\0';
    feed(h, &a, big, 33);
    feed(h, &a, "\nPING\r\n\n\n", 3);
    CHECK(a.overflows == 1);
    CHECK(h->events == 0 && h->pings == 1);
    free(big);
    free(h);
}

static void test_malformed(void)
{
    harness_t *h = harness_new();
    ccp_line_asm_t a;
    ccp_line_asm_reset(&a);
    feed(h, &a, "S\tx\twork\t1\tp\ta\t0\t0\n", 64);            // outside snapshot
    CHECK(h->errors == 1);
    feed(h, &a, "SNAP\t1\t2\nS\tx\tbogus\t1\tp\ta\t0\t0\nA\ty\trun\t1\tt\tact\n", 64);
    CHECK(h->errors == 2);
    feed(h, &a, "S\tz\twork\t-1\tp\ta\t0\t0\nEND\t1\n", 64);   // negative age
    CHECK(h->errors == 3 && h->commits == 1 && h->committed.count == 0);
    feed(h, &a, "SNAP\t2\t1\nS\tz\twork\t1\tp\ta\t0\t0\nEND\t3\n", 64); // seq mismatch
    CHECK(h->errors == 4 && h->commits == 1);
    feed(h, &a, "END\t2\n", 64);                              // nothing active after mismatch
    CHECK(h->errors == 5);
    feed(h, &a, "EVT\tboom\ts\tp\tt\nCLOCK\tx\t0\nCLOCK\t1\t99999\nHELLO\n", 64);
    CHECK(h->errors == 9);
    feed(h, &a, "FUTURE\trecord\n", 64);
    CHECK(h->unknown == 1);
    free(h);
}

static void test_capacity_and_restart(void)
{
    harness_t *h = harness_new();
    ccp_line_asm_t a;
    ccp_line_asm_reset(&a);
    char buf[4096];
    size_t n = 0;
    n += (size_t)snprintf(buf + n, sizeof(buf) - n, "SNAP\t9\t8\n");
    for (int i = 0; i < 8; i++) {
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "S\ts%d\twork\t1\tp%d\ta\t9\t9\n", i, i);
        for (int j = 0; j < 7; j++) {
            n += (size_t)snprintf(buf + n, sizeof(buf) - n, "A\ta%d\trun\t1\tt\tx\n", j);
        }
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "Q\task\tt\tb\nM\tnote %d\n", i);
    }
    snprintf(buf + n, sizeof(buf) - n, "END\t9\n");
    feed(h, &a, buf, 50);
    CHECK(h->errors == 0 && h->commits == 1);
    CHECK(h->committed.count == CC_MAX_SESSIONS && h->committed.total_sessions == 8);
    CHECK(h->committed.sessions[CC_MAX_SESSIONS - 1].agent_count == CC_MAX_AGENTS);
    CHECK_STR(h->committed.sessions[CC_MAX_SESSIONS - 1].note, "note 5");

    // A new SNAP discards a partial one.
    feed(h, &a, "SNAP\t10\t1\nS\told\twork\t1\tp\ta\t0\t0\nSNAP\t11\t1\nS\tnew\tidle\t1\tp\ta\t0\t0\nEND\t11\n", 50);
    CHECK(h->commits == 2 && h->committed.count == 1);
    CHECK_STR(h->committed.sessions[0].sid, "new");

    // Reset (disconnect) drops the partial snapshot.
    feed(h, &a, "SNAP\t12\t1\nS\tx\twork\t1\tp\ta\t0\t0\n", 50);
    ccp_builder_reset(&h->builder);
    feed(h, &a, "END\t12\n", 50);
    CHECK(h->commits == 2 && h->errors == 1);
    free(h);
}

static void test_long_fields_are_truncated(void)
{
    harness_t *h = harness_new();
    ccp_line_asm_t a;
    ccp_line_asm_reset(&a);
    char line[CC_LINE_MAX];
    char project[200];
    memset(project, 'p', sizeof(project) - 1);
    project[sizeof(project) - 1] = '\0';
    snprintf(line, sizeof(line), "SNAP\t1\t1\nS\tsid-too-long-to-fit\twork\t1\t%s\ta\t0\t0\nEND\t1\n", project);
    feed(h, &a, line, 99);
    CHECK(h->commits == 1);
    CHECK(strlen(h->committed.sessions[0].project) == CC_PROJECT_MAX - 1);
    CHECK(strcmp(h->committed.sessions[0].project + CC_PROJECT_MAX - 4, "...") == 0);
    CHECK(strlen(h->committed.sessions[0].sid) < CC_SID_MAX);
    free(h);
}

static void test_format(void)
{
    char out[64];
    CHECK(ccp_format_hi(out, sizeof(out), "0.1.0", 87) == (int)strlen("HI\t1\t0.1.0\t87\n"));
    CHECK_STR(out, "HI\t1\t0.1.0\t87\n");
    CHECK(ccp_format_ack(out, sizeof(out), 42) > 0);
    CHECK_STR(out, "ACK\t42\n");
    CHECK(ccp_format_err(out, sizeof(out), "bad S") > 0);
    CHECK_STR(out, "ERR\tbad S\n");
    char tiny[4];
    CHECK(ccp_format_ack(tiny, sizeof(tiny), 123456) == -1);
    CHECK_STR(ccp_session_state_token(CC_SESSION_ASK), "ask");
    CHECK_STR(ccp_session_state_token(CC_SESSION_BG), "bg");
    CHECK_STR(ccp_evt_kind_token(CC_EVT_ERR), "err");
}

int main(void)
{
    test_golden();
    test_assembler_overflow();
    test_malformed();
    test_capacity_and_restart();
    test_long_fields_are_truncated();
    test_format();
    if (failures) {
        fprintf(stderr, "test_ccp: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("test_ccp: PASS\n");
    return EXIT_SUCCESS;
}
