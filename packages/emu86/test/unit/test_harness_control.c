/*
 * test_harness_control.c (EMU-34)
 *
 * Exercises the transport-agnostic control-plane dispatcher directly,
 * with no socket in the picture. Links harness/control.c — the dispatcher
 * has zero dependencies on harness state, so the test supplies its own
 * variable table.
 */
#include "harness.h"
#include "../../harness/control.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* Backing storage for the test's variable table. */
static int      v_bool_rw;
static int      v_bool_ro;
static uint64_t v_u64_rw;
static uint64_t v_u64_ro;

static const ControlVar test_vars[] = {
    { "bool_rw", VT_BOOL, 0, &v_bool_rw },
    { "bool_ro", VT_BOOL, 1, &v_bool_ro },
    { "u64_rw",  VT_U64,  0, &v_u64_rw  },
    { "u64_ro",  VT_U64,  1, &v_u64_ro  },
    { NULL, 0, 0, NULL }
};

static void reset(void)
{
    v_bool_rw = 0;
    v_bool_ro = 1;
    v_u64_rw  = 0;
    v_u64_ro  = 42;
    harness_control_set_vars(test_vars);
}

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int contains(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

TEST(ping_returns_pong) {
    reset();
    char resp[256];
    harness_control_handle("ping", resp, sizeof(resp));
    ASSERT(strcmp(resp, "OK pong") == 0);
}

TEST(unknown_command_returns_err) {
    reset();
    char resp[256];
    harness_control_handle("wibble", resp, sizeof(resp));
    ASSERT(starts_with(resp, "ERR unknown command"));
    ASSERT(contains(resp, "wibble"));
}

TEST(get_bool_returns_value) {
    reset();
    char resp[256];
    v_bool_rw = 0;
    harness_control_handle("get bool_rw", resp, sizeof(resp));
    ASSERT(strcmp(resp, "OK 0") == 0);

    v_bool_rw = 1;
    harness_control_handle("get bool_rw", resp, sizeof(resp));
    ASSERT(strcmp(resp, "OK 1") == 0);
}

TEST(get_u64_returns_value) {
    reset();
    char resp[256];
    v_u64_rw = 123456789ULL;
    harness_control_handle("get u64_rw", resp, sizeof(resp));
    ASSERT(strcmp(resp, "OK 123456789") == 0);
}

TEST(set_then_get_roundtrips) {
    reset();
    char resp[256];
    harness_control_handle("set bool_rw 1", resp, sizeof(resp));
    ASSERT(strcmp(resp, "OK bool_rw=1") == 0);
    ASSERT_EQ(v_bool_rw, 1);

    harness_control_handle("get bool_rw", resp, sizeof(resp));
    ASSERT(strcmp(resp, "OK 1") == 0);

    harness_control_handle("set u64_rw 9999", resp, sizeof(resp));
    ASSERT(strcmp(resp, "OK u64_rw=9999") == 0);
    ASSERT_EQ(v_u64_rw, 9999);
}

TEST(set_bool_non_01_rejected) {
    reset();
    char resp[256];
    harness_control_handle("set bool_rw frog", resp, sizeof(resp));
    ASSERT(starts_with(resp, "ERR expected 0 or 1"));
    ASSERT_EQ(v_bool_rw, 0);

    harness_control_handle("set bool_rw 2", resp, sizeof(resp));
    ASSERT(starts_with(resp, "ERR expected 0 or 1"));
    ASSERT_EQ(v_bool_rw, 0);
}

TEST(set_u64_non_int_rejected) {
    reset();
    char resp[256];
    harness_control_handle("set u64_rw abc", resp, sizeof(resp));
    ASSERT(starts_with(resp, "ERR expected unsigned integer"));
    ASSERT_EQ(v_u64_rw, 0);

    harness_control_handle("set u64_rw 12x", resp, sizeof(resp));
    ASSERT(starts_with(resp, "ERR expected unsigned integer"));
    ASSERT_EQ(v_u64_rw, 0);
}

TEST(set_readonly_rejected) {
    reset();
    char resp[256];
    harness_control_handle("set bool_ro 0", resp, sizeof(resp));
    ASSERT(starts_with(resp, "ERR"));
    ASSERT(contains(resp, "read-only"));
    ASSERT_EQ(v_bool_ro, 1);

    harness_control_handle("set u64_ro 0", resp, sizeof(resp));
    ASSERT(starts_with(resp, "ERR"));
    ASSERT(contains(resp, "read-only"));
    ASSERT_EQ(v_u64_ro, 42);
}

TEST(get_unknown_variable_returns_err) {
    reset();
    char resp[256];
    harness_control_handle("get no_such_var", resp, sizeof(resp));
    ASSERT(starts_with(resp, "ERR unknown variable"));
    ASSERT(contains(resp, "no_such_var"));
}

TEST(empty_command_returns_err) {
    reset();
    char resp[256];
    harness_control_handle("", resp, sizeof(resp));
    ASSERT(starts_with(resp, "ERR empty"));

    harness_control_handle("   ", resp, sizeof(resp));
    ASSERT(starts_with(resp, "ERR empty"));

    harness_control_handle("\n", resp, sizeof(resp));
    ASSERT(starts_with(resp, "ERR empty"));
}

TEST(help_lists_every_command) {
    reset();
    char resp[4096];
    harness_control_handle("help", resp, sizeof(resp));
    ASSERT(starts_with(resp, "OK"));
    ASSERT(contains(resp, "ping"));
    ASSERT(contains(resp, "help"));
    ASSERT(contains(resp, "get"));
    ASSERT(contains(resp, "set"));
    /* Variables section should list every registered variable. */
    ASSERT(contains(resp, "bool_rw"));
    ASSERT(contains(resp, "bool_ro"));
    ASSERT(contains(resp, "u64_rw"));
    ASSERT(contains(resp, "u64_ro"));
}

TEST(get_all_dumps_every_variable) {
    reset();
    v_bool_rw = 1;
    v_u64_rw  = 777;
    char resp[4096];
    harness_control_handle("get all", resp, sizeof(resp));
    ASSERT(starts_with(resp, "OK"));
    ASSERT(contains(resp, "bool_rw=1"));
    ASSERT(contains(resp, "bool_ro=1"));
    ASSERT(contains(resp, "u64_rw=777"));
    ASSERT(contains(resp, "u64_ro=42"));
}

TEST(trailing_newline_is_tolerated) {
    reset();
    char resp[256];
    harness_control_handle("ping\n", resp, sizeof(resp));
    ASSERT(strcmp(resp, "OK pong") == 0);
    harness_control_handle("ping\r\n", resp, sizeof(resp));
    ASSERT(strcmp(resp, "OK pong") == 0);
}

TEST(leading_whitespace_is_tolerated) {
    reset();
    char resp[256];
    harness_control_handle("  ping", resp, sizeof(resp));
    ASSERT(strcmp(resp, "OK pong") == 0);
}

int main(void)
{
    printf("test_harness_control:\n");
    RUN_TEST(ping_returns_pong);
    RUN_TEST(unknown_command_returns_err);
    RUN_TEST(get_bool_returns_value);
    RUN_TEST(get_u64_returns_value);
    RUN_TEST(set_then_get_roundtrips);
    RUN_TEST(set_bool_non_01_rejected);
    RUN_TEST(set_u64_non_int_rejected);
    RUN_TEST(set_readonly_rejected);
    RUN_TEST(get_unknown_variable_returns_err);
    RUN_TEST(empty_command_returns_err);
    RUN_TEST(help_lists_every_command);
    RUN_TEST(get_all_dumps_every_variable);
    RUN_TEST(trailing_newline_is_tolerated);
    RUN_TEST(leading_whitespace_is_tolerated);

    printf("\n%d passed, %d failed\n", test_passes, test_failures);
    return test_failures ? 1 : 0;
}
