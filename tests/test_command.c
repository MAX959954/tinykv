#include "check.h"
#include "command.h"

static void test_set(void) {
    Command c;
    CHECK(parse_command("SET user:1 alice", &c) == 0);
    CHECK(c.type == CMD_SET);
    CHECK_STR(c.key, "user:1");
    CHECK_STR(c.value, "alice");
}

static void test_set_value_with_spaces(void) {
    Command c;
    CHECK(parse_command("SET k hello big   world", &c) == 0);
    CHECK_STR(c.value, "hello big   world");
}

static void test_extra_spaces_between_tokens(void) {
    Command c;
    CHECK(parse_command("  SET    k    v", &c) == 0);
    CHECK_STR(c.key, "k");
    CHECK_STR(c.value, "v");
}

static void test_get_del(void) {
    Command c;
    CHECK(parse_command("GET abc", &c) == 0);
    CHECK(c.type == CMD_GET);
    CHECK_STR(c.key, "abc");
    CHECK(parse_command("DEL abc", &c) == 0);
    CHECK(c.type == CMD_DEL);
    CHECK_STR(c.key, "abc");
}

static void test_malformed(void) {
    Command c;
    CHECK(parse_command("", &c) == -1);
    CHECK(parse_command("   ", &c) == -1);
    CHECK(parse_command("SET", &c) == -1);
    CHECK(parse_command("SET k", &c) == -1);
    CHECK(parse_command("SET k   ", &c) == -1);
    CHECK(parse_command("GET", &c) == -1);
    CHECK(parse_command("FOO bar", &c) == -1);
    CHECK(parse_command("set k v", &c) == -1);       // verbs are case-sensitive
    CHECK(parse_command("SETX k v", &c) == -1);
    CHECK(parse_command("VERYLONGVERB k v", &c) == -1);
}

static void test_key_length_limit(void) {
    Command c;
    char line[MAX_LINE];
    char key[MAX_KEY + 2];

    memset(key, 'k', MAX_KEY);
    key[MAX_KEY] = '\0';
    snprintf(line, sizeof(line), "SET %s v", key);
    CHECK(parse_command(line, &c) == 0);             // exactly MAX_KEY: fine
    CHECK(strlen(c.key) == MAX_KEY);

    memset(key, 'k', MAX_KEY + 1);
    key[MAX_KEY + 1] = '\0';
    snprintf(line, sizeof(line), "SET %s v", key);
    CHECK(parse_command(line, &c) == -1);            // one more: rejected, not truncated
    snprintf(line, sizeof(line), "GET %s", key);
    CHECK(parse_command(line, &c) == -1);
}

int main(void) {
    RUN(test_set);
    RUN(test_set_value_with_spaces);
    RUN(test_extra_spaces_between_tokens);
    RUN(test_get_del);
    RUN(test_malformed);
    RUN(test_key_length_limit);
    return CHECK_DONE();
}
