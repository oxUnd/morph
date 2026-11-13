#include "nb/nbase.h"

#include <stdio.h>

int main(void)
{
    static const unsigned char text[] = {
        ' ', 0xE3, 0x80, 0x80,
        0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87,
        ' ', 0xF0, 0x9F, 0x98, 0x80,
        0xE3, 0x80, 0x80
    };
    nb_str s = nb_str_from_parts((const char *)text, sizeof(text));
    nb_str trimmed = nb_utf8_trim(s);

    printf("valid=%d bytes=%zu codepoints=%zu\n",
           nb_utf8_valid(trimmed) ? 1 : 0, trimmed.len,
           nb_utf8_count(trimmed));

    nb_str first = nb_utf8_slice(trimmed, 0, 2);
    printf("first2=%.*s\n", (int)first.len, first.ptr);

    size_t index = 0;
    uint32_t cp;
    printf("codepoints:");
    while (nb_utf8_decode(trimmed, &index, &cp, NULL)) {
        printf(" U+%04X", (unsigned)cp);
    }
    printf("\n");

    static const unsigned char bad[] = { 0xE4, 0xB8 };
    size_t offset = 0;
    nb_utf8_status status = nb_utf8_validate(
        nb_str_from_parts((const char *)bad, sizeof(bad)), &offset);
    printf("invalid status=%d offset=%zu\n", (int)status, offset);
    return 0;
}
