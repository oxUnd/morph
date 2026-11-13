#include "test.h"

#include "nb/nbase.h"

#include <string.h>

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void check_hex(const unsigned char *got, size_t got_len,
                      const char *hex)
{
    size_t hex_len = strlen(hex);
    NB_CHECK_INT(hex_len % 2u, 0);
    NB_CHECK_INT(got_len * 2u, hex_len);
    if (hex_len % 2u != 0 || got_len * 2u != hex_len ||
        hex_len / 2u > 64u) {
        return;
    }
    unsigned char expected[64];
    for (size_t i = 0; i < hex_len / 2u; i++) {
        int hi = hex_nibble(hex[i * 2u]);
        int lo = hex_nibble(hex[i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            NB_CHECK(0);
            return;
        }
        expected[i] = (unsigned char)((hi << 4) | lo);
    }
    NB_CHECK_MEM(got, expected, hex_len / 2u);
}

static void test_crc32(void)
{
    NB_CHECK_INT(nb_crc32("123456789", 9), 0xCBF43926u);
    NB_CHECK_INT(nb_crc32("", 0), 0);
    static const char fox[] =
        "The quick brown fox jumps over the lazy dog";
    NB_CHECK_INT(nb_crc32(fox, sizeof(fox) - 1u), 0x414FA339u);
}

/* The bitwise oracle shares neither tables nor block boundaries with CRC-32. */
static uint32_t reference_crc32(const unsigned char *data, size_t len)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8u; bit++) {
            crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xEDB88320) : 0u);
        }
    }
    return ~crc;
}

static void test_crc32_boundaries(void)
{
    static const size_t lengths[] = {
        127, 128, 129, 255, 256, 257, 511, 512, 513,
        1023, 1024, 1025, 4095, 4096, 4097
    };
    NB_CHECK_INT(nb_crc32(NULL, 0), 0);
    NB_CHECK_INT(nb_crc32(NULL, 1), 0);
    for (size_t alignment = 0; alignment < 16u; alignment++) {
        for (size_t test = 0; test < 96u + NB_ARRAY_SIZE(lengths); test++) {
            size_t len = test < 96u ? test : lengths[test - 96u];
            size_t size = alignment + len;
            unsigned char *storage = malloc(size ? size : 1u);
            NB_CHECK(storage != NULL);
            if (!storage) {
                return;
            }
            unsigned char *data = storage + alignment;
            uint32_t random = UINT32_C(0x51A7E);
            for (size_t i = 0; i < len; i++) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                data[i] = (unsigned char)random;
            }
            NB_CHECK_INT(nb_crc32(data, len), reference_crc32(data, len));
            free(storage);
        }
    }
}

static void test_sha256(void)
{
    unsigned char out[NB_SHA256_DIGEST_SIZE];

    nb_sha256(NULL, 0, out);
    check_hex(out, sizeof(out),
              "e3b0c44298fc1c149afbf4c8996fb924"
              "27ae41e4649b934ca495991b7852b855");

    nb_sha256("abc", 3, out);
    check_hex(out, sizeof(out),
              "ba7816bf8f01cfea414140de5dae2223"
              "b00361a396177a9cb410ff61f20015ad");

    static const char msg[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    nb_sha256(msg, sizeof(msg) - 1u, out);
    check_hex(out, sizeof(out),
              "248d6a61d20638b8e5c026930c3e6039"
              "a33ce45964ff2167f6ecedd419db06c1");

    nb_sha256_ctx ctx;
    nb_sha256_init(&ctx);
    nb_sha256_update(&ctx, "a", 1);
    nb_sha256_update(&ctx, "bc", 2);
    nb_sha256_final(&ctx, out);
    check_hex(out, sizeof(out),
              "ba7816bf8f01cfea414140de5dae2223"
              "b00361a396177a9cb410ff61f20015ad");

    static char chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    nb_sha256_init(&ctx);
    for (int i = 0; i < 1000; i++) {
        nb_sha256_update(&ctx, chunk, sizeof(chunk));
    }
    nb_sha256_final(&ctx, out);
    check_hex(out, sizeof(out),
              "cdc76e5c9914fb9281a1c7e284d73e67"
              "f1809a48a497200e046d39ccc7112cd0");
}

static void test_hmac_sha256(void)
{
    unsigned char out[NB_SHA256_DIGEST_SIZE];
    unsigned char key[131];
    unsigned char data[50];

    memset(key, 0x0b, 20);
    nb_hmac_sha256(key, 20, "Hi There", 8, out);
    check_hex(out, sizeof(out),
              "b0344c61d8db38535ca8afceaf0bf12b"
              "881dc200c9833da726e9376c2e32cff7");

    nb_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
    check_hex(out, sizeof(out),
              "5bdcc146bf60754e6a042426089575c7"
              "5a003f089d2739839dec58b964ec3843");

    memset(key, 0xaa, 20);
    memset(data, 0xdd, 50);
    nb_hmac_sha256(key, 20, data, 50, out);
    check_hex(out, sizeof(out),
              "773ea91e36800e46854db8ebd09181a7"
              "2959098b3ef8c122d9635514ced565fe");

    unsigned char key25[25];
    for (size_t i = 0; i < sizeof(key25); i++) {
        key25[i] = (unsigned char)(i + 1u);
    }
    memset(data, 0xcd, 50);
    nb_hmac_sha256(key25, sizeof(key25), data, 50, out);
    check_hex(out, sizeof(out),
              "82558a389a443c0ea4cc819899f2083a"
              "85f0faa3e578f8077a2e3ff46729665b");

    memset(key, 0xaa, 131);
    static const char msg6[] =
        "Test Using Larger Than Block-Size Key - Hash Key First";
    nb_hmac_sha256(key, 131, msg6, sizeof(msg6) - 1u, out);
    check_hex(out, sizeof(out),
              "60e431591ee0b67f0d8a26aacbf5b77f"
              "8e0bc6213728c5140546040f0ee37f54");

    static const char msg7[] =
        "This is a test using a larger than block-size key and a larger "
        "than block-size data. The key needs to be hashed before being used "
        "by the HMAC algorithm.";
    nb_hmac_sha256(key, 131, msg7, sizeof(msg7) - 1u, out);
    check_hex(out, sizeof(out),
              "9b09ffa71b942fcb27635fbcd5b0e944"
              "bfdc63644f0713938a7f51535c3a35e2");
}

static void test_hash_equal(void)
{
    static const unsigned char a[] = { 1, 2, 3, 4 };
    static const unsigned char b[] = { 1, 2, 3, 4 };
    static const unsigned char c[] = { 1, 2, 3, 5 };
    NB_CHECK(nb_hash_equal(a, b, sizeof(a)));
    NB_CHECK(!nb_hash_equal(a, c, sizeof(a)));
    NB_CHECK(nb_hash_equal(a, b, sizeof(a) - 1u));
    NB_CHECK(nb_hash_equal(NULL, NULL, 0));
    NB_CHECK(!nb_hash_equal(NULL, b, sizeof(b)));
    NB_CHECK(!nb_hash_equal(a, NULL, sizeof(a)));
}

void test_hash(void)
{
    test_crc32();
    test_crc32_boundaries();
    test_sha256();
    test_hmac_sha256();
    test_hash_equal();
}
