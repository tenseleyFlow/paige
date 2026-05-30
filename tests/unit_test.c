#include "term.h"

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

int main(void)
{
    test_command_decode();
    test_input_decode();
    if (fails == 0) {
        printf("unit: key decoding OK\n");
        return 0;
    }
    return 1;
}
