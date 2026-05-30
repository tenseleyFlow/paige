#include "term.h"
#include "search.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

static void check_key(const char *name, int got, int want)
{
    if (got != want) {
        printf("FAIL: %s got %d want %d\n", name, got, want);
        fails++;
    }
}

static void test_command_decode(void)
{
    struct paige_term t;
    memset(&t, 0, sizeof t);

    check_key("command digit", paige_term_decode_command_byte(&t, '7'),
              PK_DIGIT);
    if (t.digit != 7) {
        printf("FAIL: command digit value got %d want 7\n", t.digit);
        fails++;
    }
    check_key("command quit", paige_term_decode_command_byte(&t, 'q'),
              PK_QUIT);
    check_key("command down", paige_term_decode_command_byte(&t, 'j'),
              PK_DOWN);
    check_key("command up", paige_term_decode_command_byte(&t, 'k'), PK_UP);
    check_key("command search forward", paige_term_decode_command_byte(&t, '/'),
              PK_SEARCH_FWD);
    check_key("command search back", paige_term_decode_command_byte(&t, '?'),
              PK_SEARCH_BACK);
    check_key("command search next", paige_term_decode_command_byte(&t, 'n'),
              PK_SEARCH_NEXT);
    check_key("command search prev", paige_term_decode_command_byte(&t, 'N'),
              PK_SEARCH_PREV);
    check_key("command page down", paige_term_decode_command_byte(&t, ' '),
              PK_PGDN);
    check_key("command top", paige_term_decode_command_byte(&t, 'g'), PK_TOP);
    check_key("command bottom", paige_term_decode_command_byte(&t, 'G'),
              PK_BOTTOM);
}

static void test_input_decode(void)
{
    struct paige_term t;
    memset(&t, 0, sizeof t);

    check_key("input char", paige_term_decode_input_byte(&t, 'a'), PK_CHAR);
    if (t.ch != 'a') {
        printf("FAIL: input char value got %u want %u\n", (unsigned)t.ch,
               (unsigned)'a');
        fails++;
    }
    check_key("input digit as char", paige_term_decode_input_byte(&t, '7'),
              PK_CHAR);
    if (t.ch != '7') {
        printf("FAIL: input digit char value got %u want %u\n", (unsigned)t.ch,
               (unsigned)'7');
        fails++;
    }
    check_key("input slash", paige_term_decode_input_byte(&t, '/'), PK_CHAR);
    check_key("input enter", paige_term_decode_input_byte(&t, '\n'), PK_ENTER);
    check_key("input carriage return", paige_term_decode_input_byte(&t, '\r'),
              PK_ENTER);
    check_key("input backspace", paige_term_decode_input_byte(&t, 0x7f),
              PK_BACKSPACE);
    check_key("input ctrl-c", paige_term_decode_input_byte(&t, 3), PK_QUIT);
}

static void check_bool(const char *name, bool got, bool want)
{
    if (got != want) {
        printf("FAIL: %s got %d want %d\n", name, got ? 1 : 0,
               want ? 1 : 0);
        fails++;
    }
}

static void check_size(const char *name, size_t got, size_t want)
{
    if (got != want) {
        printf("FAIL: %s got %zu want %zu\n", name, got, want);
        fails++;
    }
}

static void test_search(void)
{
    size_t off = 0;
    const char *hay = "alpha Beta beta ALPHA";

    check_bool("smart lower", paige_search_smart_case("beta", 4), false);
    check_bool("smart upper", paige_search_smart_case("Beta", 4), true);
    check_bool("forward insensitive",
               paige_search_find_forward(hay, strlen(hay), "ALPHA", 5, 0,
                                         false, &off),
               true);
    check_size("forward insensitive offset", off, 0);
    check_bool("forward sensitive",
               paige_search_find_forward(hay, strlen(hay), "Beta", 4, 0,
                                         true, &off),
               true);
    check_size("forward sensitive offset", off, 6);
    check_bool("forward start",
               paige_search_find_forward(hay, strlen(hay), "beta", 4, 7,
                                         false, &off),
               true);
    check_size("forward start offset", off, 11);
    check_bool("backward insensitive",
               paige_search_find_backward(hay, strlen(hay), "alpha", 5,
                                          strlen(hay) + 1, false, &off),
               true);
    check_size("backward insensitive offset", off, 16);
    check_bool("not found",
               paige_search_find_forward(hay, strlen(hay), "gamma", 5, 0,
                                         false, &off),
               false);
}

int main(void)
{
    test_command_decode();
    test_input_decode();
    test_search();
    if (fails == 0) {
        printf("unit: key decoding OK\n");
        return 0;
    }
    return 1;
}
