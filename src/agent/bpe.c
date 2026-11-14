#include "bpe.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/vec.h"
#include "nb/map.h"
#include "nb/encoding.h"
#include "runtime/resources.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/log.h"
#include "nb/utf8.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* Binary token keys are copied by nbase; rank zero uses a non-NULL value. */
#define RANK_EMPTY (-1)

struct bpe_encoder {
	nb_map *ranks;
	enum bpe_encoding encoding;
};

static void bpe_encoder_owner_cleanup(struct bpe_encoder **owner)
{
	bpe_encoder_destroy(*owner);
	*owner = NULL;
}

static int32_t ht_lookup(const struct bpe_encoder *enc,
			 const unsigned char *key, uint16_t key_len)
{
	void *value = nb_map_get(enc->ranks,
				nb_str_from_parts((const char *)key, key_len));
	return value ? (int32_t)((uintptr_t)value - 1u) : RANK_EMPTY;
}

/* ------------------------------------------------------------------ */
/*  Vocabulary loader                                                  */
/* ------------------------------------------------------------------ */

static void bpe_file_cleanup(FILE **owner)
{
	if (*owner)
		fclose(*owner);
	*owner = NULL;
}

static int load_vocab(struct nb_log *logger, struct bpe_encoder *enc, const char *path)
{
	FILE *fp NB_CLEANUP(bpe_file_cleanup) = fopen(path, "r");
	if (!fp) {
		int err = errno;
		nb_log_write(logger, NB_LOG_ERROR, "bpe: cannot open vocab: %s: %s", path,
			     strerror(err));
		return nb_status_from_errno(err, NULL);
	}

	char *line NB_CLEANUP(nb_charp_cleanup) = NULL;
	nb_buf decoded NB_CLEANUP(nb_buf_cleanup) = {0};
	size_t line_capacity = 0;
	size_t count = 0;
	for (;;) {
		errno = 0;
		ssize_t read_count = getline(&line, &line_capacity, fp);
		if (read_count < 0) {
			if (feof(fp))
				break;
			return errno ? nb_status_from_errno(errno, NULL) : NB_ERR_IO;
		}
		size_t line_len = (size_t)read_count;
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

		/* Hash-table key lengths are represented by uint16_t. */
		if (b64_len > 4u * (((size_t)UINT16_MAX + 2u) / 3u)) {
			nb_log_write(logger, NB_LOG_ERROR,
				     "bpe: vocabulary token exceeds %u bytes: %s",
				     (unsigned)UINT16_MAX, path);
			return NB_ERR_TOO_BIG;
		}
		/* Retain the vocabulary's padded, whitespace-free base64 format. */
		if (b64_len % 4u || strpbrk(b64, "\t\r\n"))
			continue;
		nb_buf_clear(&decoded);
		int rc = nb_base64_decode(nb_str_from_parts(b64, b64_len),
					  NB_B64_STD, &decoded, NULL);
		if (rc == NB_ERR_NOMEM || rc == NB_ERR_RANGE)
			return rc;
		if (rc != NB_OK)
			continue;
		if (nb_buf_len(&decoded) > UINT16_MAX)
			return NB_ERR_TOO_BIG;

		errno = 0;
		long rank_val = strtol(rank_str, NULL, 10);
		if (errno == ERANGE || rank_val < 0 || rank_val > INT32_MAX)
			continue;
		void *rank = (void *)(uintptr_t)((uint32_t)rank_val + 1u);
		if (!nb_map_put(enc->ranks, nb_buf_str(&decoded), rank, NULL))
			return NB_ERR_NOMEM;
		count++;
	}

	nb_log_write(logger, NB_LOG_INFO, "bpe: loaded %zu tokens from %s", count, path);
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

	switch (nb_utf8_classify_codepoint(cp)) {
	case NB_UTF8_CLASS_UPPER:
		return UC_UPPER;
	case NB_UTF8_CLASS_LOWER:
		return UC_LOWER;
	case NB_UTF8_CLASS_LETTER:
	case NB_UTF8_CLASS_MARK:
		return UC_OTHER_LETTER;
	case NB_UTF8_CLASS_NUMBER:
		return UC_NUMBER;
	case NB_UTF8_CLASS_SEPARATOR:
		return UC_SPACE;
	default:
		return UC_OTHER;
	}
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
	if (!nb_utf8_read((const char *)it->text + it->pos, it->len - it->pos, &cp, &cp_len))
		return it->text[it->pos];
	return cp;
}

static uint32_t iter_next(struct piece_iter *it)
{
	if (it->pos >= it->len) return (uint32_t)-1;
	unsigned cp;
	size_t cp_len;
	if (!nb_utf8_read((const char *)it->text + it->pos, it->len - it->pos, &cp, &cp_len)) {
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
	return nb_utf8_next_codepoint_len((const char *)it->text + it->pos, it->len - it->pos);
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

#define BPE_PIECE_BATCH_SIZE 8192

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

		int letter_prefix = is_letter(cc);
		if (cc == UC_APOSTROPHE) {
			struct piece_iter next = it;
			next.pos += iter_cp_bytes(&it);
			uint32_t next_cp = iter_peek(&next);

			letter_prefix = next_cp != (uint32_t)-1 &&
				is_letter(classify_cp(next_cp));
		}
		if (letter_prefix) {
			if (cc == UC_APOSTROPHE)
				iter_next(&it);
			while (it.pos < it.len) {
				uint32_t next_cp = iter_peek(&it);
				if (next_cp == (uint32_t)-1 || !is_letter(classify_cp(next_cp)))
					break;
				iter_next(&it);
			}
			pieces[npieces].offset = start;
			pieces[npieces].len = it.pos - start;
			npieces++;
			continue;
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

					if (!nb_utf8_read((const char *)it.text + ws_end,
							  it.len - ws_end, &pcp, &cp_len))
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
					it.pos = (size_t)(nb_utf8_prev(
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

					if (!nb_utf8_read((const char *)it.text + ws_end,
							  it.len - ws_end, &pcp, &cp_len))
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

static int bpe_merge_count_small(const struct bpe_encoder *enc,
			   const unsigned char *piece, size_t piece_len)
{
	if (piece_len == 0) return 0;
	if (piece_len == 1) return 1;

	size_t parts[MAX_PIECE_BYTES + 1];
	size_t nparts = piece_len;

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
			sizeof(*parts) * (nparts - min_idx - 1));
		nparts--;
	}

	return (int)nparts;
}

struct bpe_merge_node {
	size_t previous;
	size_t next;
	size_t heap_position;
	int32_t rank;
};

struct bpe_merge_heap {
	nb_vec nodes;
	nb_vec order;
};

static void bpe_merge_heap_cleanup(struct bpe_merge_heap *heap)
{
	nb_vec_cleanup(&heap->nodes);
	nb_vec_cleanup(&heap->order);
}

static int bpe_heap_precedes(const struct bpe_merge_heap *heap, size_t left, size_t right)
{
	const struct bpe_merge_node *nodes = ((void *)heap->nodes.data);

	return nodes[left].rank < nodes[right].rank ||
		(nodes[left].rank == nodes[right].rank && left < right);
}

static void bpe_heap_swap(struct bpe_merge_heap *heap, size_t left, size_t right)
{
	struct bpe_merge_node *nodes = ((void *)heap->nodes.data);
	size_t *order = ((void *)heap->order.data);
	size_t saved = order[left];

	order[left] = order[right];
	order[right] = saved;
	nodes[order[left]].heap_position = left;
	nodes[order[right]].heap_position = right;
}

static size_t bpe_heap_up(struct bpe_merge_heap *heap, size_t position)
{
	const size_t *order = ((void *)heap->order.data);

	while (position > 0) {
		size_t parent = (position - 1) / 2;

		if (!bpe_heap_precedes(heap, order[position], order[parent]))
			break;
		bpe_heap_swap(heap, position, parent);
		position = parent;
	}
	return position;
}

static void bpe_heap_down(struct bpe_merge_heap *heap, size_t position)
{
	const size_t *order = ((void *)heap->order.data);

	while (position < heap->order.len / 2) {
		size_t child = position * 2 + 1;

		if (child + 1 < heap->order.len &&
		    bpe_heap_precedes(heap, order[child + 1], order[child]))
			child++;
		if (!bpe_heap_precedes(heap, order[child], order[position]))
			break;
		bpe_heap_swap(heap, position, child);
		position = child;
	}
}

static void bpe_heap_remove(struct bpe_merge_heap *heap, size_t index)
{
	struct bpe_merge_node *nodes = ((void *)heap->nodes.data);
	size_t position = nodes[index].heap_position;

	if (position == SIZE_MAX)
		return;
	size_t last = heap->order.len - 1;
	if (position != last)
		bpe_heap_swap(heap, position, last);
	nb_vec_pop(&heap->order);
	nodes[index].heap_position = SIZE_MAX;
	if (position < heap->order.len)
		bpe_heap_down(heap, bpe_heap_up(heap, position));
}

static int bpe_heap_refresh(struct bpe_merge_heap *heap, const struct bpe_encoder *enc,
			    const unsigned char *piece, size_t piece_len, size_t index)
{
	struct bpe_merge_node *nodes = ((void *)heap->nodes.data);
	size_t right = nodes[index].next;
	int32_t rank = RANK_EMPTY;

	if (right < piece_len) {
		size_t length = nodes[right].next - index;

		if (length <= UINT16_MAX)
			rank = ht_lookup(enc, piece + index, (uint16_t)length);
	}
	/* The scan implementation treats INT32_MAX as the no-merge sentinel. */
	if (rank < 0 || rank == INT32_MAX) {
		bpe_heap_remove(heap, index);
		nodes[index].rank = RANK_EMPTY;
		return 0;
	}
	nodes[index].rank = rank;
	if (nodes[index].heap_position == SIZE_MAX) {
		size_t *slot = nb_vec_push(&heap->order);

		if (!slot)
			return NB_ERR_NOMEM;
		*slot = index;
		nodes[index].heap_position = heap->order.len - 1;
	}
	size_t position = bpe_heap_up(heap, nodes[index].heap_position);
	bpe_heap_down(heap, position);
	return 0;
}

static int bpe_merge_count_large(const struct bpe_encoder *enc,
				 const unsigned char *piece, size_t piece_len)
{
	struct bpe_merge_heap heap NB_CLEANUP(bpe_merge_heap_cleanup) = {0};
	struct bpe_merge_node *nodes;
	size_t *order;
	size_t remaining = piece_len;
	int rc;

	if (piece_len >= INT_MAX || piece_len > SIZE_MAX / sizeof(*nodes) ||
	    piece_len > SIZE_MAX / sizeof(*order))
		return INT_MAX;
	rc = (nb_vec_init(&heap.nodes, sizeof(*nodes)),
	      nb_vec_reserve(&heap.nodes, piece_len) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_vec_init(&heap.order, sizeof(*order)),
		      nb_vec_reserve(&heap.order, piece_len) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return (int)piece_len;
	nodes = nb_vec_push_n(&heap.nodes, piece_len);
	if (!nodes)
		return (int)piece_len;
	for (size_t i = 0; i < piece_len; i++) {
		nodes[i].previous = i ? i - 1 : SIZE_MAX;
		nodes[i].next = i + 1;
		nodes[i].heap_position = SIZE_MAX;
		nodes[i].rank = RANK_EMPTY;
	}
	for (size_t i = 0; i + 1 < piece_len; i++) {
		rc = bpe_heap_refresh(&heap, enc, piece, piece_len, i);
		if (rc != 0)
			return (int)piece_len;
	}
	/* Byte offsets preserve the original leftmost tie break. Only the two
	 * neighboring candidates change after a merge; there is no full rescan. */
	while (heap.order.len) {
		order = ((void *)heap.order.data);
		size_t left = order[0];
		size_t right = nodes[left].next;
		size_t next = nodes[right].next;
		size_t previous = nodes[left].previous;

		bpe_heap_remove(&heap, right);
		nodes[left].next = next;
		if (next < piece_len)
			nodes[next].previous = left;
		nodes[right].next = SIZE_MAX;
		remaining--;
		rc = bpe_heap_refresh(&heap, enc, piece, piece_len, left);
		if (rc == 0 && previous != SIZE_MAX)
			rc = bpe_heap_refresh(&heap, enc, piece, piece_len, previous);
		if (rc != 0)
			return (int)piece_len;
	}
	return (int)remaining;
}

static int bpe_merge_count(const struct bpe_encoder *enc,
			   const unsigned char *piece, size_t piece_len)
{
	return piece_len <= MAX_PIECE_BYTES ? bpe_merge_count_small(enc, piece, piece_len)
					 : bpe_merge_count_large(enc, piece, piece_len);
}

/* Inputs are ordinary message/schema text. Strings that spell a reserved token
 * are counted as literal text, not counted a second time as control tokens. */

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

struct bpe_encoder *bpe_encoder_create(struct nb_log *logger, enum bpe_encoding encoding,
				       const char *vocab_dir)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_2 = NB_OK;

	const char *filename = (encoding == BPE_O200K_BASE)
		? "o200k_base.tiktoken" : "cl100k_base.tiktoken";

	char path[PATH_MAX] = {0};
	const char *found_path = NULL;

	if (vocab_dir &&
	    ((file_status_1 = nb_path_concat(&file_value_1, vocab_dir, filename)) != NB_OK
		     ? file_status_1
	     : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), path, sizeof(path))) ==
			     NB_ERR_RANGE
		     ? NB_ERR_NAME_TOO_LONG
		     : file_status_1) == 0) {
		FILE *fp = fopen(path, "r");
		if (fp) {
			fclose(fp);
			found_path = path;
		}
	}

	if (!found_path) {
		char relative[PATH_MAX];

		if (((file_status_2 = nb_path_concat(&file_value_2, "tiktoken", filename)) != NB_OK
			     ? file_status_2
		     : (file_status_2 = nb_str_copy_to(nb_buf_str(&file_value_2), relative,
						       sizeof(relative))) == NB_ERR_RANGE
			     ? NB_ERR_NAME_TOO_LONG
			     : file_status_2) == 0 &&
		    morph_data_find(path, sizeof(path), relative) == 0)
			found_path = path;
	}

	if (!found_path) {
		nb_log_write(logger, NB_LOG_WARN,
			     "bpe: vocab %s not found, "
			     "token counts will use estimation",
			     filename);
		return NULL;
	}

	struct bpe_encoder *enc NB_CLEANUP(bpe_encoder_owner_cleanup) = calloc(1, sizeof(*enc));
	if (!enc) return NULL;

	enc->encoding = encoding;
	enc->ranks = nb_map_new_with_cap(200000);
	if (!enc->ranks)
		return NULL;
	int rc = load_vocab(logger, enc, found_path);
	if (rc < 0)
		return NULL;
	struct bpe_encoder *created = enc;
	enc = NULL;
	return created;
}

void bpe_encoder_destroy(struct bpe_encoder *enc)
{
	if (!enc) return;
	nb_map_free(enc->ranks);
	free(enc);
}

int bpe_count_tokens_n(struct bpe_encoder *enc, const char *text, size_t len)
{
	struct bpe_piece *pieces;
	size_t offset = 0;
	int64_t total = 0;

	if (!enc || !text || len == 0)
		return 0;
	pieces = malloc(sizeof(*pieces) * BPE_PIECE_BATCH_SIZE);
	/* One token per byte is a conservative bound when allocation fails. */
	if (!pieces)
		return len > INT_MAX ? INT_MAX : (int)len;
	while (offset < len && total < INT_MAX) {
		const char *remaining = text + offset;
		int count = enc->encoding == BPE_O200K_BASE ?
			split_o200k(remaining, len - offset, pieces, BPE_PIECE_BATCH_SIZE) :
			split_cl100k(remaining, len - offset, pieces, BPE_PIECE_BATCH_SIZE);

		if (count == 0) {
			/* Malformed UTF-8 must not silently discard the unparsed suffix. */
			total += len - offset > INT_MAX ? INT_MAX : (int)(len - offset);
			break;
		}
		for (int i = 0; i < count; i++)
			total += bpe_merge_count(enc,
				(const unsigned char *)remaining + pieces[i].offset,
				pieces[i].len);
		/* Continue only at a completed regex piece, preserving lookahead. */
		offset += pieces[count - 1].offset + pieces[count - 1].len;
	}
	free(pieces);
	return total > INT_MAX ? INT_MAX : (int)total;
}

int bpe_count_tokens(struct bpe_encoder *enc, const char *text)
{
	if (!enc || !text) return 0;
	return bpe_count_tokens_n(enc, text, strlen(text));
}
