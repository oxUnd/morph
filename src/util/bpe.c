#include "bpe.h"
#include "buf.h"
#include "error.h"
#include "file.h"
#include "log.h"
#include "utf8.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Hash table: FNV-1a open addressing for byte-sequence → rank       */
/* ------------------------------------------------------------------ */

#define HT_LOAD_FACTOR 70
#define RANK_EMPTY    (-1)
#define RANK_DELETED  (-2)

struct ht_entry {
	uint32_t key_off;
	uint16_t key_len;
	int32_t rank;
};

struct bpe_encoder {
	struct ht_entry *ht;
	uint32_t ht_cap;
	uint32_t ht_mask;
	uint32_t ht_count;
	morph_buf_t key_pool;
	enum bpe_encoding encoding;
};

static uint32_t fnv1a(const unsigned char *data, size_t len)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < len; i++) {
		h ^= data[i];
		h *= 16777619u;
	}
	return h;
}

static uint32_t ht_next_cap(uint32_t min_slots)
{
	uint32_t cap = 64;
	while (cap < min_slots)
		cap *= 2;
	return cap;
}

static void ht_insert(struct bpe_encoder *enc, uint32_t key_off,
		      uint16_t key_len, int32_t rank)
{
	const unsigned char *key = (unsigned char *)enc->key_pool.data + key_off;
	uint32_t idx = fnv1a(key, key_len) & enc->ht_mask;
	for (;;) {
		struct ht_entry *e = &enc->ht[idx];
		if (e->rank == RANK_EMPTY || e->rank == RANK_DELETED) {
			e->key_off = key_off;
			e->key_len = key_len;
			e->rank = rank;
			enc->ht_count++;
			return;
		}
		if (e->key_len == key_len &&
		    memcmp((unsigned char *)enc->key_pool.data + e->key_off,
			   key, key_len) == 0) {
			e->rank = rank;
			return;
		}
		idx = (idx + 1) & enc->ht_mask;
	}
}

static int32_t ht_lookup(const struct bpe_encoder *enc,
			 const unsigned char *key, uint16_t key_len)
{
	uint32_t idx = fnv1a(key, key_len) & enc->ht_mask;
	for (;;) {
		const struct ht_entry *e = &enc->ht[idx];
		if (e->rank == RANK_EMPTY)
			return RANK_EMPTY;
		if (e->rank != RANK_DELETED &&
		    e->key_len == key_len &&
		    memcmp((unsigned char *)enc->key_pool.data + e->key_off,
			   key, key_len) == 0)
			return e->rank;
		idx = (idx + 1) & enc->ht_mask;
	}
}

/* ------------------------------------------------------------------ */
/*  Base64 decoder                                                     */
/* ------------------------------------------------------------------ */

static const int8_t b64_table[256] = {
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
	52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
	-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
	15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
	-1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
	41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

static int b64_decode(const char *src, size_t src_len,
		      unsigned char *dst, size_t *out_len)
{
	size_t si = 0, di = 0;
	while (si < src_len) {
		int8_t a = (si < src_len) ? b64_table[(unsigned char)src[si++]] : -1;
		int8_t b = (si < src_len) ? b64_table[(unsigned char)src[si++]] : -1;
		if (a < 0 || b < 0) return -1;
		dst[di++] = (unsigned char)((a << 2) | (b >> 4));
		if (si < src_len && src[si] == '=')
			break;
		int8_t c = (si < src_len) ? b64_table[(unsigned char)src[si++]] : -1;
		if (c < 0) return -1;
		dst[di++] = (unsigned char)(((b & 0x0F) << 4) | (c >> 2));
		if (si < src_len && src[si] == '=')
			break;
		int8_t d = (si < src_len) ? b64_table[(unsigned char)src[si++]] : -1;
		if (d < 0) return -1;
		dst[di++] = (unsigned char)(((c & 0x03) << 6) | d);
	}
	*out_len = di;
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Vocabulary loader                                                  */
/* ------------------------------------------------------------------ */

static int load_vocab(struct bpe_encoder *enc, const char *path)
{
	FILE *fp = fopen(path, "r");
	if (!fp) {
		int err = errno;
		log_err("bpe: cannot open vocab: %s: %s", path, strerror(err));
		MORPH_RETURN(-err);
	}

	char line[1024];
	int count = 0;
	while (fgets(line, sizeof(line), fp)) {
		size_t line_len = strlen(line);
		while (line_len > 0 &&
		       (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
			line[--line_len] = '\0';
		if (line_len == 0)
			continue;

		char *space = strchr(line, ' ');
		if (!space)
			continue;
		*space = '\0';
		const char *b64 = line;
		const char *rank_str = space + 1;

		size_t b64_len = strlen(b64);
		if (b64_len == 0)
			continue;

		unsigned char decoded[256];
		size_t decoded_len = 0;
		if (b64_decode(b64, b64_len, decoded, &decoded_len) < 0)
			continue;

		long rank_val = strtol(rank_str, NULL, 10);
		if (rank_val < 0)
			continue;

		if (enc->key_pool.len > UINT32_MAX - decoded_len) {
			fclose(fp);
			return -EOVERFLOW;
		}
		if (morph_buf_append(&enc->key_pool, (const char *)decoded,
				     decoded_len) < 0) {
			fclose(fp);
			return -ENOMEM;
		}

		ht_insert(enc, (uint32_t)(enc->key_pool.len - decoded_len),
			  (uint16_t)decoded_len, (int32_t)rank_val);
		count++;
	}

	fclose(fp);
	log_info("bpe: loaded %d tokens from %s", count, path);
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Unicode character classification for pre-splitting                 */
/* ------------------------------------------------------------------ */

enum uc_class {
	UC_UPPER,
	UC_LOWER,
	UC_OTHER_LETTER,
	UC_NUMBER,
	UC_SPACE,
	UC_NEWLINE,
	UC_APOSTROPHE,
	UC_OTHER
};

static enum uc_class classify_cp(uint32_t cp)
{
	if (cp < 0x80) {
		if (cp >= 'A' && cp <= 'Z') return UC_UPPER;
		if (cp >= 'a' && cp <= 'z') return UC_LOWER;
		if (cp >= '0' && cp <= '9') return UC_NUMBER;
		if (cp == ' ' || cp == '\t' || cp == '\v' || cp == '\f')
			return UC_SPACE;
		if (cp == '\n' || cp == '\r') return UC_NEWLINE;
		if (cp == '\'') return UC_APOSTROPHE;
		return UC_OTHER;
	}

	if (utf8_is_cjk_cp(cp))
		return UC_OTHER_LETTER;
	if (utf8_is_hiragana_cp(cp))
		return UC_OTHER_LETTER;
	if (utf8_is_katakana_cp(cp))
		return UC_OTHER_LETTER;
	if (utf8_is_hangul_cp(cp))
		return UC_OTHER_LETTER;
	if ((cp >= 0x00C0 && cp <= 0x00D6) ||
	    (cp >= 0x00D8 && cp <= 0x00DE) ||
	    (cp >= 0x0100 && cp <= 0x024F && (cp & 1) == 0) ||
	    (cp >= 0x1E00 && cp <= 0x1EFF && (cp & 1) == 0) ||
	    (cp >= 0x0400 && cp <= 0x04FF && (cp & 1) == 0) ||
	    (cp >= 0x0500 && cp <= 0x052F && (cp & 1) == 0))
		return UC_UPPER;
	if ((cp >= 0x00DF && cp <= 0x00F6) ||
	    (cp >= 0x00F8 && cp <= 0x00FF) ||
	    (cp >= 0x0100 && cp <= 0x024F && (cp & 1) == 1) ||
	    (cp >= 0x1E00 && cp <= 0x1EFF && (cp & 1) == 1) ||
	    (cp >= 0x0400 && cp <= 0x04FF && (cp & 1) == 1) ||
	    (cp >= 0x0500 && cp <= 0x052F && (cp & 1) == 1))
		return UC_LOWER;
	if (cp >= 0x0300 && cp <= 0x036F)
		return UC_OTHER_LETTER;
	if ((cp >= 0x0660 && cp <= 0x0669) ||
	    (cp >= 0x06F0 && cp <= 0x06F9) ||
	    (cp >= 0x0966 && cp <= 0x096F) ||
	    (cp >= 0xFF10 && cp <= 0xFF19))
		return UC_NUMBER;
	if (utf8_is_unicode_space_cp(cp))
		return UC_SPACE;
	return UC_OTHER;
}

static int is_letter(enum uc_class c)
{
	return c == UC_UPPER || c == UC_LOWER || c == UC_OTHER_LETTER;
}

/* ------------------------------------------------------------------ */
/*  Pre-splitting: regex-like piece iterator                           */
/*                                                                     */
/*  cl100k_base pattern:                                               */
/*    '(?i:[sdmt]|ll|ve|re)                                           */
/*    |[^\r\n\p{L}\p{N}]?+\p{L}++                                     */
/*    |\p{N}{1,3}+                                                     */
/*    | ?[^\s\p{L}\p{N}]++[\r\n]*+                                    */
/*    |\s++$                                                           */
/*    |\s*[\r\n]                                                       */
/*    |\s+(?!\S)                                                       */
/*    |\s                                                              */
/*                                                                     */
/*  o200k_base pattern:                                                */
/*    [^\r\n\p{L}\p{N}]?[\p{Lu}\p{Lt}\p{Lm}\p{Lo}\p{M}]*             */
/*      [\p{Ll}\p{Lm}\p{Lo}\p{M}]+(?i:'s|'t|'re|'ve|'m|'ll|'d)?     */
/*    |[^\r\n\p{L}\p{N}]?[\p{Lu}\p{Lt}\p{Lm}\p{Lo}\p{M}]+            */
/*      [\p{Ll}\p{Lm}\p{Lo}\p{M}]*(?i:'s|'t|'re|'ve|'m|'ll|'d)?     */
/*    |\p{N}{1,3}                                                      */
/*    | ?[^\s\p{L}\p{N}]+[\r\n/]*                                     */
/*    |\s*[\r\n]+                                                       */
/*    |\s+(?!\S)                                                       */
/*    |\s+                                                             */
/* ------------------------------------------------------------------ */

struct piece_iter {
	const unsigned char *text;
	size_t len;
	size_t pos;
};

static uint32_t iter_peek(const struct piece_iter *it)
{
	if (it->pos >= it->len) return (uint32_t)-1;
	unsigned cp;
	size_t cp_len;
	if (!utf8_decode_codepoint((const char *)it->text + it->pos,
			 it->len - it->pos, &cp, &cp_len))
		return it->text[it->pos];
	return cp;
}

static uint32_t iter_next(struct piece_iter *it)
{
	if (it->pos >= it->len) return (uint32_t)-1;
	unsigned cp;
	size_t cp_len;
	if (!utf8_decode_codepoint((const char *)it->text + it->pos,
			 it->len - it->pos, &cp, &cp_len)) {
		cp = it->text[it->pos];
		it->pos++;
		return cp;
	}
	it->pos += cp_len;
	return cp;
}

static size_t iter_cp_bytes(const struct piece_iter *it)
{
	if (it->pos >= it->len) return 0;
	return utf8_next_codepoint_len((const char *)it->text + it->pos,
			     it->len - it->pos);
}

static int is_contraction_suffix(const unsigned char *text, size_t len,
				 size_t pos, size_t *suffix_len)
{
	if (pos >= len) return 0;
	unsigned char c = text[pos] | 0x20;
	if (c == 's' || c == 't' || c == 'd' || c == 'm') {
		*suffix_len = 1;
		return 1;
	}
	if (pos + 1 < len) {
		unsigned char c2 = text[pos + 1] | 0x20;
		if ((c == 'v' && c2 == 'e') ||
		    (c == 'r' && c2 == 'e') ||
		    (c == 'l' && c2 == 'l')) {
			*suffix_len = 2;
			return 1;
		}
	}
	return 0;
}

#define MAX_PIECES 8192

struct bpe_piece {
	size_t offset;
	size_t len;
};

static int split_cl100k(const char *text, size_t text_len,
			struct bpe_piece *pieces, int max_pieces)
{
	struct piece_iter it;
	it.text = (const unsigned char *)text;
	it.len = text_len;
	it.pos = 0;
	int npieces = 0;

	while (it.pos < it.len && npieces < max_pieces) {
		size_t start = it.pos;
		uint32_t cp = iter_peek(&it);

		if (cp == (uint32_t)-1) break;

		enum uc_class cc = classify_cp(cp);

		if (cc == UC_APOSTROPHE) {
			size_t suffix_len = 0;
			size_t apos_bytes = iter_cp_bytes(&it);
			if (is_contraction_suffix(it.text, it.len,
						  it.pos + apos_bytes,
						  &suffix_len)) {
				it.pos += apos_bytes + suffix_len;
				pieces[npieces].offset = start;
				pieces[npieces].len = it.pos - start;
				npieces++;
				continue;
			}
		}

		if (cc == UC_APOSTROPHE || is_letter(cc)) {
			if (cc != UC_APOSTROPHE && !is_letter(cc)) {
				/* skip */
			} else {
				enum uc_class first_cc = cc;
				if (first_cc != UC_APOSTROPHE) {
					/* not a standalone letter start */
				}
				/*
				 * Alt 2: [^\r\n\p{L}\p{N}]?+\p{L}++
				 * We're at a letter or apostrophe.
				 * But apostrophe only counts as prefix
				 * for alt2 if followed by a letter.
				 */
				if (cc == UC_APOSTROPHE) {
					uint32_t next_cp;
					size_t apos_bytes2 = iter_cp_bytes(&it);
					struct piece_iter it2 = it;
					it2.pos += apos_bytes2;
					next_cp = iter_peek(&it2);
					if (next_cp != (uint32_t)-1 &&
					    is_letter(classify_cp(next_cp))) {
						iter_next(&it);
					} else {
						goto try_digits;
					}
				}
				while (it.pos < it.len) {
					uint32_t p = iter_peek(&it);
					if (p == (uint32_t)-1) break;
					if (!is_letter(classify_cp(p))) break;
					iter_next(&it);
				}
				pieces[npieces].offset = start;
				pieces[npieces].len = it.pos - start;
				npieces++;
				continue;
			}
		}

		/*
		 * Alt 2 with non-letter/digit/newline prefix:
		 * [^\r\n\p{L}\p{N}]?+\p{L}++
		 */
		if (cc != UC_NEWLINE && cc != UC_NUMBER &&
		    !is_letter(cc) && cc != UC_SPACE) {
			size_t prefix_bytes = iter_cp_bytes(&it);
			struct piece_iter it2 = it;
			it2.pos += prefix_bytes;
			uint32_t next_cp = iter_peek(&it2);
			if (next_cp != (uint32_t)-1 &&
			    is_letter(classify_cp(next_cp))) {
				iter_next(&it);
				while (it.pos < it.len) {
					uint32_t p = iter_peek(&it);
					if (p == (uint32_t)-1) break;
					if (!is_letter(classify_cp(p))) break;
					iter_next(&it);
				}
				pieces[npieces].offset = start;
				pieces[npieces].len = it.pos - start;
				npieces++;
				continue;
			}
		}

try_digits:
		if (cc == UC_NUMBER) {
			int count = 0;
			while (it.pos < it.len && count < 3) {
				uint32_t p = iter_peek(&it);
				if (p == (uint32_t)-1) break;
				if (classify_cp(p) != UC_NUMBER) break;
				iter_next(&it);
				count++;
			}
			pieces[npieces].offset = start;
			pieces[npieces].len = it.pos - start;
			npieces++;
			continue;
		}

		if (cc == UC_SPACE) {
			/*
			 * Space can start several alternatives.
			 * Try in order:
			 *   Alt 2: space + letters
			 *   Alt 4: space + punctuation
			 *   Alt 6: optional spaces + newline(s)
			 *   Alt 5/7: \s+(?!\S) or \s++$ — whitespace
			 *            not followed by non-whitespace, or
			 *            trailing whitespace at end of string.
			 *   Alt 8: single space
			 */
			struct piece_iter save = it;
			iter_next(&it);
			uint32_t next_cp = iter_peek(&it);

			if (next_cp != (uint32_t)-1 &&
			    is_letter(classify_cp(next_cp))) {
				/* Alt 2: space + letters */
				while (it.pos < it.len) {
					uint32_t p = iter_peek(&it);
					if (p == (uint32_t)-1) break;
					if (!is_letter(classify_cp(p))) break;
					iter_next(&it);
				}
				pieces[npieces].offset = start;
				pieces[npieces].len = it.pos - start;
				npieces++;
				continue;
			}

			if (next_cp != (uint32_t)-1 &&
			    !is_letter(classify_cp(next_cp)) &&
			    classify_cp(next_cp) != UC_NUMBER &&
			    classify_cp(next_cp) != UC_SPACE &&
			    classify_cp(next_cp) != UC_NEWLINE) {
				/* Alt 4: space + punctuation + optional newlines */
				iter_next(&it);
				while (it.pos < it.len) {
					uint32_t p = iter_peek(&it);
					if (p == (uint32_t)-1) break;
					enum uc_class pc = classify_cp(p);
					if (pc == UC_SPACE || pc == UC_NEWLINE ||
					    is_letter(pc) || pc == UC_NUMBER)
						break;
					iter_next(&it);
				}
				while (it.pos < it.len) {
					uint32_t p = iter_peek(&it);
					if (p == (uint32_t)-1) break;
					if (classify_cp(p) != UC_NEWLINE) break;
					iter_next(&it);
				}
				pieces[npieces].offset = start;
				pieces[npieces].len = it.pos - start;
				npieces++;
				continue;
			}

			it = save;

			if (next_cp != (uint32_t)-1 &&
			    classify_cp(next_cp) == UC_NEWLINE) {
				/* Alt 6: \s*[\r\n] */
				while (it.pos < it.len) {
					uint32_t p = iter_peek(&it);
					if (p == (uint32_t)-1) break;
					enum uc_class pc = classify_cp(p);
					if (pc != UC_SPACE &&
					    pc != UC_NEWLINE)
						break;
					iter_next(&it);
				}
				pieces[npieces].offset = start;
				pieces[npieces].len = it.pos - start;
				npieces++;
				continue;
			}

			/*
			 * Alt 5/7/8: handle remaining whitespace
			 * \s+(?!\S) matches whitespace where the char
			 * after is also whitespace or end-of-string.
			 * \s matches a single whitespace char.
			 */
			{
				it.pos = start;
				size_t ws_end = start;
				size_t ws_cp_count = 0;
				while (ws_end < it.len) {
					unsigned pcp;
					size_t cp_len;
					enum uc_class pc;

					if (!utf8_decode_codepoint(
						(const char *)it.text + ws_end,
						it.len - ws_end, &pcp,
						&cp_len))
						break;
					pc = classify_cp(pcp);
					if (pc != UC_SPACE &&
					    pc != UC_NEWLINE)
						break;
					ws_end += cp_len;
					ws_cp_count++;
				}

				if (ws_end >= it.len) {
					/* Alt 5: trailing whitespace at end */
					it.pos = ws_end;
				} else if (ws_cp_count > 1) {
					/*
					 * Alt 7: \s+(?!\S)
					 * Multiple whitespace chars
					 * followed by non-whitespace.
					 * Back up one codepoint so the
					 * last ws char becomes a prefix
					 * for the next word.
					 */
					it.pos = (size_t)(utf8_prev_codepoint(
						(const char *)it.text + start,
						(const char *)it.text + ws_end) -
						(const char *)it.text);
				} else {
					/* Alt 8: single whitespace char */
					iter_next(&it);
				}

				if (it.pos > start) {
					pieces[npieces].offset = start;
					pieces[npieces].len =
						it.pos - start;
					npieces++;
				}
				continue;
			}
		}

		if (cc == UC_NEWLINE) {
			/*
			 * Alt 6: \s*[\r\n]
			 * \s* greedily consumes whitespace (including
			 * newlines), then [\r\n] needs one more newline.
			 * With backtracking this consumes all consecutive
			 * whitespace ending with a newline as one piece.
			 */
			while (it.pos < it.len) {
				uint32_t p = iter_peek(&it);
				if (p == (uint32_t)-1) break;
				enum uc_class pc = classify_cp(p);
				if (pc != UC_SPACE && pc != UC_NEWLINE) break;
				iter_next(&it);
			}
			pieces[npieces].offset = start;
			pieces[npieces].len = it.pos - start;
			npieces++;
			continue;
		}

		/* UC_OTHER: punctuation/symbol */
		{
			/*
			 * Alt 4: ?[^\s\p{L}\p{N}]++[\r\n]*+
			 * No leading space here (no space before punct).
			 */
			iter_next(&it);
			while (it.pos < it.len) {
				uint32_t p = iter_peek(&it);
				if (p == (uint32_t)-1) break;
				enum uc_class pc = classify_cp(p);
				if (pc == UC_SPACE || pc == UC_NEWLINE ||
				    is_letter(pc) || pc == UC_NUMBER)
					break;
				iter_next(&it);
			}
			while (it.pos < it.len) {
				uint32_t p = iter_peek(&it);
				if (p == (uint32_t)-1) break;
				if (classify_cp(p) != UC_NEWLINE) break;
				iter_next(&it);
			}
			pieces[npieces].offset = start;
			pieces[npieces].len = it.pos - start;
			npieces++;
			continue;
		}
	}

	return npieces;
}

static int split_o200k(const char *text, size_t text_len,
		       struct bpe_piece *pieces, int max_pieces)
{
	struct piece_iter it;
	it.text = (const unsigned char *)text;
	it.len = text_len;
	it.pos = 0;
	int npieces = 0;

	while (it.pos < it.len && npieces < max_pieces) {
		size_t start = it.pos;
		uint32_t cp = iter_peek(&it);
		if (cp == (uint32_t)-1) break;
		enum uc_class cc = classify_cp(cp);

		int has_prefix = 0;
		if (cc != UC_NEWLINE && cc != UC_NUMBER &&
		    !is_letter(cc) && cc != UC_SPACE && cc != UC_APOSTROPHE) {
			has_prefix = 1;
			iter_next(&it);
			cp = iter_peek(&it);
			if (cp == (uint32_t)-1) {
				pieces[npieces].offset = start;
				pieces[npieces].len = it.pos - start;
				npieces++;
				continue;
			}
			cc = classify_cp(cp);
		}

		if (is_letter(cc)) {
			enum uc_class first_type = cc;
			if (first_type == UC_OTHER_LETTER)
				first_type = UC_UPPER;

			while (it.pos < it.len) {
				uint32_t p = iter_peek(&it);
				if (p == (uint32_t)-1) break;
				enum uc_class pc = classify_cp(p);
				if (!is_letter(pc)) break;
				if (first_type == UC_UPPER &&
				    pc == UC_LOWER) {
					first_type = UC_LOWER;
				} else if (first_type == UC_LOWER &&
					   (pc == UC_UPPER)) {
					break;
				}
				iter_next(&it);
			}

			if (it.pos < it.len) {
				size_t slen = 0;
				size_t cp_bytes = iter_cp_bytes(&it);
				if (iter_peek(&it) == '\'' &&
				    is_contraction_suffix(
					    it.text, it.len,
					    it.pos + cp_bytes, &slen)) {
					it.pos += cp_bytes + slen;
				}
			}

			pieces[npieces].offset = start;
			pieces[npieces].len = it.pos - start;
			npieces++;
			continue;
		}

		if (has_prefix) {
			pieces[npieces].offset = start;
			pieces[npieces].len = it.pos - start;
			npieces++;
			continue;
		}

		if (cc == UC_NUMBER) {
			int count = 0;
			while (it.pos < it.len && count < 3) {
				uint32_t p = iter_peek(&it);
				if (p == (uint32_t)-1) break;
				if (classify_cp(p) != UC_NUMBER) break;
				iter_next(&it);
				count++;
			}
			pieces[npieces].offset = start;
			pieces[npieces].len = it.pos - start;
			npieces++;
			continue;
		}

		if (cc == UC_APOSTROPHE) {
			iter_next(&it);
			pieces[npieces].offset = start;
			pieces[npieces].len = it.pos - start;
			npieces++;
			continue;
		}

		if (cc == UC_SPACE) {
			struct piece_iter save = it;
			iter_next(&it);
			uint32_t next_cp = iter_peek(&it);

			if (next_cp != (uint32_t)-1 &&
			    !is_letter(classify_cp(next_cp)) &&
			    classify_cp(next_cp) != UC_NUMBER &&
			    classify_cp(next_cp) != UC_SPACE &&
			    classify_cp(next_cp) != UC_NEWLINE) {
				iter_next(&it);
				while (it.pos < it.len) {
					uint32_t p = iter_peek(&it);
					if (p == (uint32_t)-1) break;
					enum uc_class pc = classify_cp(p);
					if (pc == UC_SPACE || pc == UC_NEWLINE ||
					    is_letter(pc) || pc == UC_NUMBER)
						break;
					iter_next(&it);
				}
				while (it.pos < it.len) {
					uint32_t p = iter_peek(&it);
					if (p == (uint32_t)-1) break;
					enum uc_class pc = classify_cp(p);
					if (pc != UC_NEWLINE && pc != UC_OTHER)
						break;
					if (pc == UC_OTHER) {
						if (p == '/')
							iter_next(&it);
						break;
					}
					iter_next(&it);
				}
				pieces[npieces].offset = start;
				pieces[npieces].len = it.pos - start;
				npieces++;
				continue;
			}

			it = save;
			iter_next(&it);

			if (next_cp != (uint32_t)-1 &&
			    classify_cp(next_cp) == UC_NEWLINE) {
				while (it.pos < it.len) {
					uint32_t p = iter_peek(&it);
					if (p == (uint32_t)-1) break;
					if (classify_cp(p) != UC_SPACE) break;
					iter_next(&it);
				}
				while (it.pos < it.len) {
					uint32_t p = iter_peek(&it);
					if (p == (uint32_t)-1) break;
					if (classify_cp(p) != UC_NEWLINE) break;
					iter_next(&it);
				}
				pieces[npieces].offset = start;
				pieces[npieces].len = it.pos - start;
				npieces++;
				continue;
			}

			it = save;
			iter_next(&it);
			{
				size_t ws_end = it.pos;
				while (ws_end < it.len) {
					unsigned pcp;
					size_t cp_len;
					enum uc_class pc;

					if (!utf8_decode_codepoint(
						(const char *)it.text + ws_end,
						it.len - ws_end, &pcp,
						&cp_len))
						break;
					pc = classify_cp(pcp);
					if (pc != UC_SPACE && pc != UC_NEWLINE)
						break;
					ws_end += cp_len;
				}
				if (ws_end >= it.len) {
					it.pos = ws_end;
					pieces[npieces].offset = start;
					pieces[npieces].len = it.pos - start;
					npieces++;
					continue;
				}
			}

			pieces[npieces].offset = start;
			pieces[npieces].len = it.pos - start;
			npieces++;
			continue;
		}

		if (cc == UC_NEWLINE) {
			while (it.pos < it.len) {
				uint32_t p = iter_peek(&it);
				if (p == (uint32_t)-1) break;
				if (classify_cp(p) != UC_SPACE) break;
				iter_next(&it);
			}
			while (it.pos < it.len) {
				uint32_t p = iter_peek(&it);
				if (p == (uint32_t)-1) break;
				if (classify_cp(p) != UC_NEWLINE) break;
				iter_next(&it);
			}
			pieces[npieces].offset = start;
			pieces[npieces].len = it.pos - start;
			npieces++;
			continue;
		}

		/* UC_OTHER */
		iter_next(&it);
		while (it.pos < it.len) {
			uint32_t p = iter_peek(&it);
			if (p == (uint32_t)-1) break;
			enum uc_class pc = classify_cp(p);
			if (pc == UC_SPACE || pc == UC_NEWLINE ||
			    is_letter(pc) || pc == UC_NUMBER)
				break;
			iter_next(&it);
		}
		while (it.pos < it.len) {
			uint32_t p = iter_peek(&it);
			if (p == (uint32_t)-1) break;
			enum uc_class pc = classify_cp(p);
			if (pc != UC_NEWLINE && pc != UC_OTHER)
				break;
			if (pc == UC_OTHER) {
				if (p == '/')
					iter_next(&it);
				break;
			}
			iter_next(&it);
		}
		pieces[npieces].offset = start;
		pieces[npieces].len = it.pos - start;
		npieces++;
	}

	return npieces;
}

/* ------------------------------------------------------------------ */
/*  BPE merge algorithm                                                */
/* ------------------------------------------------------------------ */

#define MAX_PIECE_BYTES 512

static int bpe_merge_count(const struct bpe_encoder *enc,
			   const unsigned char *piece, size_t piece_len)
{
	if (piece_len == 0) return 0;
	if (piece_len == 1) return 1;

	size_t stack_buf[MAX_PIECE_BYTES + 1];
	size_t *parts = stack_buf;
	size_t nparts = piece_len;

	if (piece_len > MAX_PIECE_BYTES) {
		parts = malloc(sizeof(size_t) * (piece_len + 1));
		if (!parts) return (int)piece_len;
	}

	for (size_t i = 0; i <= piece_len; i++)
		parts[i] = i;

	while (nparts > 1) {
		int32_t min_rank = INT32_MAX;
		size_t min_idx = 0;

		for (size_t i = 0; i + 1 < nparts; i++) {
			size_t klen = parts[i + 2] - parts[i];
			if (klen > UINT16_MAX) continue;
			int32_t r = ht_lookup(enc, piece + parts[i],
					      (uint16_t)klen);
			if (r >= 0 && r < min_rank) {
				min_rank = r;
				min_idx = i;
			}
		}

		if (min_rank == INT32_MAX) break;

		memmove(&parts[min_idx + 1], &parts[min_idx + 2],
			sizeof(size_t) * (nparts - min_idx - 1));
		nparts--;
	}

	if (parts != stack_buf)
		free(parts);
	return (int)nparts;
}

/* ------------------------------------------------------------------ */
/*  Special token handling                                             */
/* ------------------------------------------------------------------ */

static const char *cl100k_specials[] = {
	"<|endoftext|>",
	"<|fim_prefix|>",
	"<|fim_middle|>",
	"<|fim_suffix|>",
	"<|endofprompt|>",
	NULL
};

static const char *o200k_specials[] = {
	"<|endoftext|>",
	"<|endofprompt|>",
	NULL
};

static int count_special_tokens(enum bpe_encoding encoding,
				const char *text, size_t text_len)
{
	const char **specs = (encoding == BPE_O200K_BASE)
		? o200k_specials : cl100k_specials;
	int count = 0;
	for (const char **sp = specs; *sp; sp++) {
		size_t slen = strlen(*sp);
		const char *p = text;
		const char *end = text + text_len;
		while (p < end) {
			const char *found = memmem(p, (size_t)(end - p),
						   *sp, slen);
			if (!found) break;
			count++;
			p = found + slen;
		}
	}
	return count;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

struct bpe_encoder *bpe_encoder_create(enum bpe_encoding encoding,
					const char *vocab_dir)
{
	const char *filename = (encoding == BPE_O200K_BASE)
		? "o200k_base.tiktoken" : "cl100k_base.tiktoken";

	char path[PATH_MAX] = {0};

	const char *search_dirs[] = {
		vocab_dir,
		getenv("HOME") ? (char[]){0} : NULL,
		NULL, NULL, NULL
	};
	char home_path[PATH_MAX];

	const char *home = getenv("HOME");
	if (home) {
		snprintf(home_path, sizeof(home_path),
			 "%s/.morph/tiktoken", home);
		search_dirs[1] = home_path;
	}

	search_dirs[2] = VENDOR_TIKTOKEN_DIR;
	search_dirs[3] = "vendor/tiktoken";

	const char *found_path = NULL;
	for (int i = 0; i < 4; i++) {
		if (!search_dirs[i] || !search_dirs[i][0]) continue;
		if (file_path_join(path, sizeof(path),
				   search_dirs[i], filename) != 0)
			continue;
		FILE *fp = fopen(path, "r");
		if (fp) {
			fclose(fp);
			found_path = path;
			break;
		}
	}

	if (!found_path) {
		log_warn("bpe: vocab %s not found, "
			 "token counts will use estimation", filename);
		return NULL;
	}

	struct bpe_encoder *enc = calloc(1, sizeof(*enc));
	if (!enc) return NULL;

	enc->encoding = encoding;
	enc->ht_cap = ht_next_cap(200000 / HT_LOAD_FACTOR * 100);
	enc->ht_mask = enc->ht_cap - 1;
	enc->ht = malloc(sizeof(struct ht_entry) * enc->ht_cap);
	if (!enc->ht) {
		free(enc);
		return NULL;
	}
	for (uint32_t i = 0; i < enc->ht_cap; i++)
		enc->ht[i].rank = RANK_EMPTY;

	if (morph_buf_init(&enc->key_pool, 1024 * 1024) < 0) {
		free(enc->ht);
		free(enc);
		return NULL;
	}
	int rc = load_vocab(enc, found_path);
	if (rc < 0) {
		free(enc->ht);
		morph_buf_cleanup(&enc->key_pool);
		free(enc);
		return NULL;
	}

	return enc;
}

void bpe_encoder_destroy(struct bpe_encoder *enc)
{
	if (!enc) return;
	free(enc->ht);
	morph_buf_cleanup(&enc->key_pool);
	free(enc);
}

int bpe_count_tokens_n(struct bpe_encoder *enc, const char *text, size_t len)
{
	if (!enc || !text || len == 0) return 0;

	int specials = count_special_tokens(enc->encoding, text, len);

	struct bpe_piece *pieces = malloc(sizeof(struct bpe_piece) * MAX_PIECES);
	if (!pieces) return (int)(len / 4 + 1);

	int npieces;
	if (enc->encoding == BPE_O200K_BASE)
		npieces = split_o200k(text, len, pieces, MAX_PIECES);
	else
		npieces = split_cl100k(text, len, pieces, MAX_PIECES);

	int total = 0;
	for (int i = 0; i < npieces; i++)
		total += bpe_merge_count(enc,
			(const unsigned char *)text + pieces[i].offset,
			pieces[i].len);

	free(pieces);
	return total + specials;
}

int bpe_count_tokens(struct bpe_encoder *enc, const char *text)
{
	if (!enc || !text) return 0;
	return bpe_count_tokens_n(enc, text, strlen(text));
}
