/* test_args.c - Test parse_addr_type_cli */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Copy of the function from sw2d_final.c */
#define ADDR_AUTO    0xff
#define ADDR_PUBLIC  0x00
#define ADDR_RANDOM  0x01

static int parse_addr_type_cli(const char *s, uint8_t *out)
{
    if (!s)
        return -1;
    if (strcmp(s, "public") == 0) {
        *out = ADDR_PUBLIC;
        return 0;
    }
    if (strcmp(s, "random") == 0) {
        *out = ADDR_RANDOM;
        return 0;
    }
    if (strcmp(s, "auto") == 0) {
        *out = ADDR_AUTO;
        return 0;
    }
    return -1;
}

static const char *addr_type_str(uint8_t type)
{
    switch (type) {
    case ADDR_PUBLIC: return "public";
    case ADDR_RANDOM: return "random";
    case ADDR_AUTO:   return "auto";
    default:          return "unknown";
    }
}

static int failures = 0;

static void check(const char *name, int condition)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    } else {
        fprintf(stderr, "PASS: %s\n", name);
    }
}

int main(void)
{
    uint8_t out;

    check("public", parse_addr_type_cli("public", &out) == 0 && out == ADDR_PUBLIC);
    check("random", parse_addr_type_cli("random", &out) == 0 && out == ADDR_RANDOM);
    check("auto",   parse_addr_type_cli("auto", &out) == 0 && out == ADDR_AUTO);
    check("invalid", parse_addr_type_cli("invalid", &out) != 0);
    check("NULL",    parse_addr_type_cli(NULL, &out) != 0);
    check("empty",   parse_addr_type_cli("", &out) != 0);

    check("str_public", strcmp(addr_type_str(ADDR_PUBLIC), "public") == 0);
    check("str_random", strcmp(addr_type_str(ADDR_RANDOM), "random") == 0);
    check("str_auto",   strcmp(addr_type_str(ADDR_AUTO),   "auto") == 0);
    check("str_unknown", strcmp(addr_type_str(0x42), "unknown") == 0);

    if (failures) {
        fprintf(stderr, "\n%d tests FAILED\n", failures);
        return 1;
    }
    fprintf(stderr, "\nAll tests passed!\n");
    return 0;
}
