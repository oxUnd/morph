#include "nb/nbase.h"

#include <stdio.h>

static void show_valid(const char *tag, nb_str input, nb_b64_variant variant)
{
    printf("valid %s = %d\n", tag,
           nb_base64_valid(input, variant) ? 1 : 0);
}

int main(void)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *encoded = nb_buf_new();
    nb_buf *decoded = nb_buf_new();
    if (!encoded || !decoded) {
        nb_buf_free(encoded);
        nb_buf_free(decoded);
        return 1;
    }

    show_valid("std padded", NB_STR_LIT("aGVsbG8="), NB_B64_STD);
    show_valid("std unpadded", NB_STR_LIT("aGVsbG8"), NB_B64_STD);
    show_valid("trailing data", NB_STR_LIT("aGVsbG8=extra"), NB_B64_STD);
    show_valid("unused bits", NB_STR_LIT("AB=="), NB_B64_STD);
    show_valid("std alphabet", NB_STR_LIT("a-b_"), NB_B64_STD);
    show_valid("url alphabet", NB_STR_LIT("a-b_"), NB_B64_URL);
    show_valid("inline space", NB_STR_LIT("aGVs bG8="), NB_B64_STD);
    show_valid("padding inside", NB_STR_LIT("AA=A"), NB_B64_STD);

    nb_base64_encode("nbase", 5, NB_B64_STD, true, encoded, &err);
    printf("base64=%s\n", nb_buf_cstr(encoded));
    nb_base64_decode(nb_buf_str(encoded), NB_B64_STD, decoded, &err);
    printf("decoded=%s\n", nb_buf_cstr(decoded));

    nb_buf_clear(encoded);
    nb_hex_encode("\x01\xab", 2, true, encoded, &err);
    printf("hex=%s\n", nb_buf_cstr(encoded));

    nb_buf_clear(encoded);
    nb_url_encode(NB_STR_LIT("a b/c"), true, encoded, &err);
    printf("url=%s\n", nb_buf_cstr(encoded));

    nb_buf_free(encoded);
    nb_buf_free(decoded);
    return 0;
}
