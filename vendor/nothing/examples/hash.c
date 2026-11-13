#include "nb/nbase.h"

#include <stdio.h>

static void print_hex(const unsigned char *data, size_t len)
{
    nb_buf *hex = nb_buf_new();
    nb_err err;
    nb_err_clear(&err);
    if (nb_hex_encode(data, len, false, hex, &err) == NB_OK) {
        printf("%s", nb_buf_cstr(hex));
    }
    nb_buf_free(hex);
}

int main(void)
{
    unsigned char digest[NB_SHA256_DIGEST_SIZE];

    nb_sha256("hello", 5, digest);
    printf("sha256(hello)=");
    print_hex(digest, sizeof(digest));
    printf("\n");

    nb_hmac_sha256("key", 3, "hello", 5, digest);
    printf("hmac-sha256(key,hello)=");
    print_hex(digest, sizeof(digest));
    printf("\n");

    printf("crc32(123456789)=%08x\n",
           (unsigned)nb_crc32("123456789", 9));
    return 0;
}
