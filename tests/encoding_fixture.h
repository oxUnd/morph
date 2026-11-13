#ifndef MORPH_TEST_ENCODING_FIXTURE_H
#define MORPH_TEST_ENCODING_FIXTURE_H

#include "nb/encoding.h"
#include "nb/cleanup.h"
#include "media/image.h"
#include <cstdlib>
#include <cstring>

static inline char *test_encode_bytes(const unsigned char *data, size_t len)
{
	nb_buf out NB_CLEANUP(nb_buf_destroy) = {};
	if (nb_base64_encode(data, len, NB_B64_STD, true, &out, nullptr) != NB_OK)
		return nullptr;
	return strdup(nb_buf_cstr(&out));
}

static inline unsigned char *test_decode_bytes(const char *text, size_t *length)
{
	if (length)
		*length = 0;
	if (!text || strlen(text) == 0 || strlen(text) % 4 != 0)
		return nullptr;
	nb_buf out NB_CLEANUP(nb_buf_destroy) = {};
	if (nb_base64_decode(nb_str_from_cstr(text), NB_B64_STD, &out, nullptr) != NB_OK)
		return nullptr;
	size_t len = nb_buf_len(&out);
	auto *copy = static_cast<unsigned char *>(malloc(len ? len : 1));
	if (!copy)
		return nullptr;
	memcpy(copy, nb_buf_cstr(&out), len);
	if (length)
		*length = len;
	return copy;
}

static inline char *test_media_file_payload(const char *path)
{
	char *out = nullptr;
	if (image_file_payload_copy(path, "", MORPH_MEDIA_MAX_FILE_BYTES, &out) != 0)
		return nullptr;
	return out;
}
#endif
