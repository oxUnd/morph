#include "tokenizer.h"
#include "agent/context.h"
#include "agent/bpe.h"
#include "nb/utf8.h"
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

/*
 * GPT BPE-style token estimation.
 *
 * GPT tokenizers (cl100k_base, o200k_base, p50k_base) use byte-level BPE.
 * The key characteristics:
 *
 * - Common short English words are 1 token (the, is, and, to, ...)
 * - Longer/rarer English words are split into ~1 token per 3-4 characters
 * - CJK ideographs (Chinese/Japanese Kanji) are typically 1-2 tokens each
 *   - In cl100k_base: most common ~5000 CJK chars = 1 token, rest = 2 tokens
 *   - In o200k_base: more CJK single-token coverage, average ~1.5 token/char
 *   - We approximate as 2 tokens per CJK char for safety (overcount is better)
 * - Hiragana/Katakana: ~1.5 tokens each → round to 2
 * - Korean Hangul: ~1.5 tokens each → round to 2
 * - Emoji: 2-4 tokens each
 * - Punctuation: 1 token each (',', '.', '!', etc.)
 * - Newlines: ~1 token for single, +1 for each additional
 * - Numbers: small numbers = 1 token, large = ~1 per 3 digits
 * - Whitespace: merged with adjacent tokens mostly
 * - JSON structural chars: 1 token each
 *
 * The estimation algorithm works in two passes:
 * 1. Classify each Unicode character and compute a "token weight"
 * 2. Add overhead for BPE merging inefficiency on short words
 */

static int utf8_is_cjk_cp(unsigned cp)
{
	if (cp >= 0x4E00 && cp <= 0x9FFF)
		return 1;
	if (cp >= 0x3400 && cp <= 0x4DBF)
		return 1;
	if (cp >= 0xF900 && cp <= 0xFAFF)
		return 1;
	if (cp >= 0x20000 && cp <= 0x2EBEF)
		return 1;
	if (cp >= 0x30000 && cp <= 0x3134F)
		return 1;
	if (cp >= 0x2E80 && cp <= 0x2EFF)
		return 1;
	if (cp >= 0x2F00 && cp <= 0x2FDF)
		return 1;
	if (cp >= 0x3100 && cp <= 0x312F)
		return 1;
	if (cp >= 0x3040 && cp <= 0x309F)
		return 1;
	if (cp >= 0x30A0 && cp <= 0x30FF)
		return 1;
	if (cp >= 0xAC00 && cp <= 0xD7AF)
		return 1;
	if (cp >= 0x1100 && cp <= 0x11FF)
		return 1;
	if (cp >= 0x3130 && cp <= 0x318F)
		return 1;
	if (cp >= 0xFF01 && cp <= 0xFFEF)
		return 1;
	return 0;
}

static int utf8_is_hiragana_cp(unsigned cp)
{
	return cp >= 0x3040 && cp <= 0x309F;
}

static int utf8_is_katakana_cp(unsigned cp)
{
	return cp >= 0x30A0 && cp <= 0x30FF;
}

static int utf8_is_hangul_cp(unsigned cp)
{
	return (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF) ||
	       (cp >= 0x3130 && cp <= 0x318F);
}

static int utf8_is_latin_extended_cp(unsigned cp)
{
	return (cp >= 0x00C0 && cp <= 0x024F) || (cp >= 0x1E00 && cp <= 0x1EFF);
}

/* Unicode code point classification */
enum char_class {
	CHAR_ASCII_LOWER,	/* a-z */
	CHAR_ASCII_UPPER,	/* A-Z */
	CHAR_ASCII_DIGIT,	/* 0-9 */
	CHAR_ASCII_PUNCT,	/* ASCII punctuation */
	CHAR_ASCII_SPACE,	/* space, tab */
	CHAR_ASCII_NEWLINE,	/* \n, \r */
	CHAR_CJK_IDEOGRAPH,	/* U+4E00-U+9FFF, U+3400-U+4DBF (CJK Unified Ideographs) */
	CHAR_CJK_IDEOGRAPH_EXT,/* CJK Extension blocks */
	CHAR_HIRAGANA,		/* U+3040-U+309F */
	CHAR_KATAKANA,		/* U+30A0-U+30FF */
	CHAR_HANGUL,		/* Korean Hangul syllables */
	CHAR_EMOJI_OTHER,	/* Emoji, symbols, other multi-byte */
	CHAR_LATIN_EXTENDED,	/* Latin extended, diacritics */
	CHAR_OTHER		/* Everything else */
};

static enum char_class classify_utf8(const unsigned char **p,
				     const unsigned char *end)
{
	unsigned char b0 = **p;
	if (b0 < 0x80) {
		(*p)++;
		if (b0 >= 'a' && b0 <= 'z') return CHAR_ASCII_LOWER;
		if (b0 >= 'A' && b0 <= 'Z') return CHAR_ASCII_UPPER;
		if (b0 >= '0' && b0 <= '9') return CHAR_ASCII_DIGIT;
		if (b0 == ' ' || b0 == '\t') return CHAR_ASCII_SPACE;
		if (b0 == '\n' || b0 == '\r') return CHAR_ASCII_NEWLINE;
		if (b0 == '_' || b0 == '\'') return CHAR_ASCII_LOWER;
		return CHAR_ASCII_PUNCT;
	}

	unsigned cp;
	size_t cp_len;

	if (!nb_utf8_read((const char *)*p, (size_t)(end - *p), &cp, &cp_len)) {
		(*p)++;
		return CHAR_OTHER;
	}
	*p += cp_len;

	if (cp == 0)
		return CHAR_OTHER;

	if (utf8_is_cjk_cp(cp))
		return CHAR_CJK_IDEOGRAPH_EXT;
	if (utf8_is_hiragana_cp(cp))
		return CHAR_HIRAGANA;
	if (utf8_is_katakana_cp(cp))
		return CHAR_KATAKANA;
	if (utf8_is_hangul_cp(cp))
		return CHAR_HANGUL;
	if (nb_utf8_is_emoji_component_cp(cp))
		return CHAR_EMOJI_OTHER;
	if (utf8_is_latin_extended_cp(cp))
		return CHAR_LATIN_EXTENDED;

	return CHAR_OTHER;
}

int tokenizer_estimate_tokens(const char *text)
{
	if (!text || *text == '\0')
		return 0;

	int64_t tokens = 0;
	int64_t word_chars = 0;		/* consecutive ASCII word chars */
	int consecutive_newlines = 0;

	const unsigned char *p = (const unsigned char *)text;
	const unsigned char *end = p + strlen(text);
	while (p < end) {
		enum char_class cc = classify_utf8(&p, end);

		switch (cc) {
		case CHAR_ASCII_LOWER:
		case CHAR_ASCII_UPPER:
		case CHAR_ASCII_DIGIT:
			word_chars++;
			consecutive_newlines = 0;
			break;

		case CHAR_CJK_IDEOGRAPH:
		case CHAR_CJK_IDEOGRAPH_EXT:
			/* Flush any pending word */
			if (word_chars > 0) {
				tokens += (word_chars + 3) / 4;
				word_chars = 0;
			}
			/* Most CJK chars: common ones = 1 token, rare = 2.
			 * Over-estimate by 2 for safety (better to compress
			 * early than overflow the context) */
			tokens += 2;
			consecutive_newlines = 0;
			break;

		case CHAR_HIRAGANA:
		case CHAR_KATAKANA:
			if (word_chars > 0) {
				tokens += (word_chars + 3) / 4;
				word_chars = 0;
			}
			tokens += 2;
			consecutive_newlines = 0;
			break;

		case CHAR_HANGUL:
			if (word_chars > 0) {
				tokens += (word_chars + 3) / 4;
				word_chars = 0;
			}
			tokens += 2;
			consecutive_newlines = 0;
			break;

		case CHAR_LATIN_EXTENDED:
			if (word_chars > 0) {
				tokens += (word_chars + 3) / 4;
				word_chars = 0;
			}
			/* Extended Latin: typically 1-2 tokens each */
			tokens += 2;
			consecutive_newlines = 0;
			break;

		case CHAR_EMOJI_OTHER:
			if (word_chars > 0) {
				tokens += (word_chars + 3) / 4;
				word_chars = 0;
			}
			/* Emoji: 2-4 tokens. Use 3 as average */
			tokens += 3;
			consecutive_newlines = 0;
			break;

		case CHAR_ASCII_PUNCT:
			if (word_chars > 0) {
				tokens += (word_chars + 3) / 4;
				word_chars = 0;
			}
			/* Punctuation is typically 1 token,
			 * but some are merged with adjacent word tokens */
			tokens += 1;
			consecutive_newlines = 0;
			break;

		case CHAR_ASCII_SPACE:
			/* Spaces are often merged into adjacent tokens,
			 * but some become separate tokens.
			 * Count 1 per ~4 spaces for overhead */
			/* Include leading/standalone whitespace as well as word separators. */
			word_chars++;
			consecutive_newlines = 0;
			break;

		case CHAR_ASCII_NEWLINE:
			if (word_chars > 0) {
				tokens += (word_chars + 3) / 4;
				word_chars = 0;
			}
			/* Single newline often merges with adjacent token.
			 * Consecutive newlines each add ~1 token. */
			consecutive_newlines++;
			if (consecutive_newlines <= 2)
				tokens += 1;
			else
				tokens += 1;
			break;

		case CHAR_OTHER:
			if (word_chars > 0) {
				tokens += (word_chars + 3) / 4;
				word_chars = 0;
			}
			/* Unknown Unicode: 2 tokens per char for safety */
			tokens += 2;
			consecutive_newlines = 0;
			break;
		}
	}

	/* Flush remaining word characters */
	if (word_chars > 0)
		tokens += (word_chars + 3) / 4;

	/* Minimum 1 token for any non-empty text */
	if (tokens == 0 && *text != '\0')
		tokens = 1;

	return tokens > INT_MAX ? INT_MAX : (int)tokens;
}

struct tokenizer *tokenizer_create(struct nb_log *logger, const char *model_name, int context_limit)
{
	struct tokenizer *tok = calloc(1, sizeof(*tok));
	if (!tok)
		return NULL;
	strncpy(tok->model_name, model_name ? model_name : "gpt-4o",
		sizeof(tok->model_name) - 1);
	tok->context_limit = context_limit > 0 ? context_limit : 128000;
	tok->count = tokenizer_estimate_tokens;

	enum bpe_encoding enc = BPE_CL100K_BASE;
	const char *name = tok->model_name;

	if (strstr(name, "o200k") || strstr(name, "gpt-4o") ||
	    strcmp(name, "o1") == 0 || strstr(name, "o1-") ||
	    strcmp(name, "o3") == 0 || strstr(name, "o3-") ||
	    strcmp(name, "o4") == 0 || strstr(name, "o4-"))
		enc = BPE_O200K_BASE;

	tok->encoder = bpe_encoder_create(logger, enc, NULL);
	return tok;
}

void tokenizer_destroy(struct tokenizer *tok)
{
	if (!tok) return;
	bpe_encoder_destroy(tok->encoder);
	free(tok);
}

int tokenizer_count(struct tokenizer *tok, const char *text)
{
	if (!tok)
		return tokenizer_estimate_tokens(text);
	if (tok->encoder)
		return bpe_count_tokens(tok->encoder, text);
	if (tok->count)
		return tok->count(text);
	return tokenizer_estimate_tokens(text);
}
