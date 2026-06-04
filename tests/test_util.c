#include "../src/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int run_util_tests(void);

static int fails = 0;

#define EXPECT(cond, msg) do { \
    if (cond) { printf("ok  - %s\n", msg); } \
    else      { printf("not ok - %s (%s:%d)\n", msg, __FILE__, __LINE__); fails++; } \
} while (0)

static void test_buf(void) {
    pin_buf b;
    pin_buf_init(&b);
    pin_buf_puts(&b, "hello");
    pin_buf_putc(&b, ' ');
    pin_buf_puts(&b, "world");
    EXPECT(b.len == 11, "buf len after puts/putc");
    EXPECT(memcmp(b.ptr, "hello world", 11) == 0, "buf contents");
    pin_buf_clear(&b);
    EXPECT(b.len == 0, "buf clear");
    pin_buf_printf(&b, "%d/%s", 42, "x");
    EXPECT(b.len == 4 && memcmp(b.ptr, "42/x", 4) == 0, "buf printf");
    pin_buf_free(&b);
}

static void test_json_emit(void) {
    pin_buf b;
    pin_buf_init(&b);
    pin_json_str(&b, "ab\"c\nd");
    /* Expected: "ab\"c\nd" */
    EXPECT(b.len == 10 && memcmp(b.ptr, "\"ab\\\"c\\nd\"", 10) == 0,
           "json str escapes quote and newline");
    pin_buf_free(&b);

    pin_buf c;
    pin_buf_init(&c);
    /* Control char 0x01 must become \u0001. */
    char in[] = {'a', 0x01, 'b', 0};
    pin_json_str(&c, in);
    EXPECT(c.len == strlen("\"a\\u0001b\"") &&
           memcmp(c.ptr, "\"a\\u0001b\"", c.len) == 0,
           "json str escapes control as \\u00XX");
    pin_buf_free(&c);
}

static void test_json_parse(void) {
    const char *s = "  \"hi\\nthere\"  ";
    char *out = NULL;
    bool ok = pin_json_parse_string(&s, &out);
    EXPECT(ok && out && strcmp(out, "hi\nthere") == 0, "json parse string + escape");
    free(out);

    const char *p = "{\"k\":42,\"x\":true,\"y\":\"v\"}";
    const char *vp = NULL;
    long long ll = 0;
    EXPECT(pin_json_find_key(p, "k", &vp) && pin_json_parse_int(&vp, &ll) && ll == 42,
           "find_key + parse_int");
    bool bl = false;
    EXPECT(pin_json_find_key(p, "x", &vp) && pin_json_parse_bool(&vp, &bl) && bl,
           "find_key + parse_bool");
    char *str = NULL;
    EXPECT(pin_json_find_key(p, "y", &vp) && pin_json_parse_string(&vp, &str) && strcmp(str, "v") == 0,
           "find_key + parse_string");
    free(str);
    EXPECT(!pin_json_find_key(p, "missing", &vp), "find_key miss");
}

static void test_text_sanitize(void) {
    pin_buf out;
    pin_buf_init(&out);
    /* Strip ESC (0x1B) and bell (0x07), keep \n and \t. */
    const char in[] = {'a', 0x1b, '[', '3', '1', 'm', 'b', '\n', 'c', '\t', 'd', 0x07, 'e', 0};
    bool fit = pin_text_sanitize(&out, in, sizeof(in) - 1, 64);
    EXPECT(fit, "sanitize fit");
    /* Expected: "a[31mb\nc\tde" — ESC dropped, bell dropped. */
    EXPECT(out.len == strlen("a[31mb\nc\tde") &&
           memcmp(out.ptr, "a[31mb\nc\tde", out.len) == 0,
           "sanitize strips C0 except \\t/\\n");
    pin_buf_free(&out);

    /* Invalid UTF-8 single 0xFF should be replaced with U+FFFD (3 bytes EFBFBD). */
    pin_buf out2;
    pin_buf_init(&out2);
    const char bad[] = {'a', (char)0xff, 'b', 0};
    pin_text_sanitize(&out2, bad, 3, 64);
    EXPECT(out2.len == 5 &&
           (unsigned char)out2.ptr[0] == 'a' &&
           (unsigned char)out2.ptr[1] == 0xef &&
           (unsigned char)out2.ptr[2] == 0xbf &&
           (unsigned char)out2.ptr[3] == 0xbd &&
           (unsigned char)out2.ptr[4] == 'b',
           "sanitize replaces invalid UTF-8 with U+FFFD");
    pin_buf_free(&out2);
}

static void test_sha1_base64(void) {
    /* RFC 6455 example: "dGhlIHNhbXBsZSBub25jZQ==" + GUID -> base64 of sha1
     * = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" */
    const char *combo = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[20];
    pin_sha1_bytes(combo, strlen(combo), digest);
    char b64[29];
    pin_base64_20(digest, b64);
    EXPECT(strcmp(b64, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0,
           "rfc6455 sha1+base64 example matches");
}

int run_util_tests(void) {
    test_buf();
    test_json_emit();
    test_json_parse();
    test_text_sanitize();
    test_sha1_base64();
    return fails;
}
