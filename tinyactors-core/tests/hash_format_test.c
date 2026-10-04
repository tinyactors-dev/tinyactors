/*
 * Blocks D5 (hash) and D6 (format): the hash functions and the number
 * formatters pin the values of the code they replaced.
 *
 * The vectors below were printed by the functions this block replaced
 * (ta_hash_bytes, the compiler's FNV-1a 64 loop, ta_trace_mix and
 * ta_session_hash) before they were deleted, so "bit-identical" is checked
 * against numbers from the old code, not computed from the new.
 *
 * The test includes the amalgamation so it can call the private functions;
 * it allocates nothing, so there is nothing to leak.
 */
#include "../include/tinyactors.c"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

static void test_fnv1a(void) {
    CHECK(ta_fnv1a32(TA_FNV1A32_BASIS, "", 0) == 0x811c9dc5u);
    CHECK(ta_fnv1a32(TA_FNV1A32_BASIS, "a", 1) == 0xe40c292cu);
    CHECK(ta_fnv1a32(TA_FNV1A32_BASIS, "foobar", 6) == 0xbf9cf968u);
    CHECK(ta_fnv1a64(TA_FNV1A64_BASIS, "", 0) == 0xcbf29ce484222325ull);
    CHECK(ta_fnv1a64(TA_FNV1A64_BASIS, "a", 1) == 0xaf63dc4c8601ec8cull);
    CHECK(ta_fnv1a64(TA_FNV1A64_BASIS, "foobar", 6) == 0x85944171f73967e8ull);

    /* Empty input returns the state unchanged, with or without bytes. */
    CHECK(ta_fnv1a32(0x12345678u, NULL, 0) == 0x12345678u);
    CHECK(ta_fnv1a64(0x123456789abcdef0ull, NULL, 0) == 0x123456789abcdef0ull);

    /* Chaining: the property address.c and the compiler rely on. */
    uint32_t state32 = ta_fnv1a32(TA_FNV1A32_BASIS, "foo", 3);
    CHECK(ta_fnv1a32(state32, "bar", 3) == 0xbf9cf968u);
    uint64_t state64 = ta_fnv1a64(TA_FNV1A64_BASIS, "foo", 3);
    CHECK(ta_fnv1a64(state64, "bar", 3) == 0x85944171f73967e8ull);

    /* The event-name trie's key: the old ta_hash_bytes(0x9e3779b9, key, 8). */
    uint32_t key[2] = { 1u, 2u };
    CHECK(ta_fnv1a32(TA_FNV1A32_BASIS ^ 0x9e3779b9u, key, sizeof(key)) == 0xeb28553fu);
    uint32_t other[2] = { 0x12345678u, 0x9abcdef0u };
    CHECK(ta_fnv1a32(TA_FNV1A32_BASIS ^ 0x9e3779b9u, other, sizeof(other)) == 0xc421568cu);
}

static void test_mix64(void) {
    CHECK(ta_mix64(0) == 0);
    CHECK(ta_mix64(1) == 0xb456bcfc34c2cb2cull);
    CHECK(ta_mix64(2) == 0x3abf2a20650683e7ull);
    CHECK(ta_mix64(0xffffffffffffffffull) == 0x64b5720b4b825f21ull);
    CHECK(ta_mix64(0x0123456789abcdefull) == 0x87cbfbfe89022ceaull);
    /* The session table's bucket hash is the low half. */
    CHECK((uint32_t)ta_mix64(0x0123456789abcdefull) == 0x89022ceau);
    /* A span ID derived from a step key, as span_test checks end to end. */
    ta_step_key key = { .session = 0x0102030405060708ull, .macrostep = 9u, .microstep = 3u };
    CHECK(ta_step_key_span_id(key) == 0x94f726c3a925fb58ull);
}

/* The reference: digit by digit with no shared code. */
static uint32_t reference_dec(char out[24], uint64_t value) {
    char reversed[24];
    uint32_t count = 0;
    do {
        reversed[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    for (uint32_t index = 0; index < count; index += 1) {
        out[index] = reversed[count - 1u - index];
    }
    return count;
}

static void check_dec(uint64_t value) {
    char expected[24];
    uint32_t expected_len = reference_dec(expected, value);
    /* A canary after the digits: the writer must not touch it. */
    char buffer[TA_DEC_U64_DIGITS + 1u];
    memset(buffer, '#', sizeof(buffer));
    CHECK(ta_dec_len(value) == expected_len);
    CHECK(ta_dec_write(buffer, value) == expected_len);
    CHECK(memcmp(buffer, expected, expected_len) == 0);
    for (uint32_t index = expected_len; index < sizeof(buffer); index += 1) {
        CHECK(buffer[index] == '#');
    }
}

static void test_decimal(void) {
    CHECK(TA_DEC_U64_DIGITS == 20u);
    CHECK(ta_dec_len(0) == 1u);
    CHECK(ta_dec_len(9) == 1u);
    CHECK(ta_dec_len(10) == 2u);
    CHECK(ta_dec_len(4294967295ull) == 10u);
    CHECK(ta_dec_len(4294967296ull) == 10u);
    CHECK(ta_dec_len(UINT64_MAX) == 20u);
    char text[TA_DEC_U64_DIGITS];
    uint32_t count = ta_dec_write(text, UINT64_MAX);
    CHECK(count == 20u && memcmp(text, "18446744073709551615", 20) == 0);
    for (uint64_t value = 0; value <= 100000u; value += 1) {
        check_dec(value);
    }
    uint64_t power = 1;
    for (uint32_t digits = 0; digits < 20u; digits += 1) {
        check_dec(power - 1u);
        check_dec(power);
        check_dec(power + 1u);
        power *= 10u;
    }
    check_dec(UINT64_MAX);
    check_dec(UINT64_MAX - 1u);
    check_dec(4294967295ull);
    check_dec(4294967296ull);
}

static void test_hex(void) {
    char buffer[17];

    memset(buffer, '#', sizeof(buffer));
    ta_hex_write(buffer, 0x0123456789abcdefull, 16u);
    CHECK(memcmp(buffer, "0123456789abcdef", 16) == 0 && buffer[16] == '#');

    memset(buffer, '#', sizeof(buffer));
    ta_hex_write(buffer, UINT64_MAX, 16u);
    CHECK(memcmp(buffer, "ffffffffffffffff", 16) == 0 && buffer[16] == '#');

    memset(buffer, '#', sizeof(buffer));
    ta_hex_write(buffer, 0, 16u);
    CHECK(memcmp(buffer, "0000000000000000", 16) == 0 && buffer[16] == '#');

    /* Leading zeros are kept, and only the low digits are written. */
    memset(buffer, '#', sizeof(buffer));
    ta_hex_write(buffer, 0xffu, 8u);
    CHECK(memcmp(buffer, "000000ff", 8) == 0 && buffer[8] == '#');
    memset(buffer, '#', sizeof(buffer));
    ta_hex_write(buffer, 0x1abu, 2u);
    CHECK(memcmp(buffer, "ab", 2) == 0 && buffer[2] == '#');
    memset(buffer, '#', sizeof(buffer));
    ta_hex_write(buffer, 0x1fu, 1u);
    CHECK(buffer[0] == 'f' && buffer[1] == '#');

    /* The revision fingerprint is count 16 of the content hash, as the old loop wrote it. */
    uint64_t hash = ta_fnv1a64(TA_FNV1A64_BASIS, "foobar", 6);
    static const char old_table[] = "0123456789abcdef";
    char old_loop[16];
    for (uint32_t index = 0; index < 16; index += 1) {
        old_loop[index] = old_table[(hash >> (60u - index * 4u)) & 0xfu];
    }
    ta_hex_write(buffer, hash, 16u);
    CHECK(memcmp(buffer, old_loop, 16) == 0);
    CHECK(memcmp(buffer, "85944171f73967e8", 16) == 0);

    for (uint32_t nibble = 0; nibble < 16u; nibble += 1) {
        CHECK(ta_hex_digit(nibble, false) == "0123456789abcdef"[nibble]);
        CHECK(ta_hex_digit(nibble, true) == "0123456789ABCDEF"[nibble]);
        /* Only the low four bits count. */
        CHECK(ta_hex_digit(nibble + 16u, false) == ta_hex_digit(nibble, false));
        CHECK(ta_hex_digit(0xf0u | nibble, true) == ta_hex_digit(nibble, true));
        CHECK(ta_hex_digit_value(ta_hex_digit(nibble, false)) == (int)nibble);
        CHECK(ta_hex_digit_value(ta_hex_digit(nibble, true)) == (int)nibble);
    }

    for (int c = 0; c < 256; c += 1) {
        int expected = -1;
        if (c >= '0' && c <= '9') {
            expected = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            expected = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            expected = c - 'A' + 10;
        }
        CHECK(ta_hex_digit_value((char)c) == expected);
    }
}

int main(void) {
    test_fnv1a();
    test_mix64();
    test_decimal();
    test_hex();
    if (failures != 0) {
        fprintf(stderr, "hash_format_test: %d checks failed\n", failures);
        return 1;
    }
    puts("hash_format_test: ok");
    return 0;
}
