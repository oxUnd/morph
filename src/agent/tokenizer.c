#include "tokenizer.h"
#include "agent/context.h"
#include "util/bpe.h"
#include "util/utf8.h"
#include <stdlib.h>
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

static enum char_class classify_utf8(const unsigned char **p)
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

	if (!utf8_decode_codepoint((const char *)*p, strlen((const char *)*p),
			 &cp, &cp_len)) {
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
	/* Emoji and symbols */
	if ((cp >= 0x1F600 && cp <= 0x1F64F) ||
	    (cp >= 0x1F300 && cp <= 0x1F5FF) ||
	    (cp >= 0x1F680 && cp <= 0x1F6FF) ||
	    (cp >= 0x1F900 && cp <= 0x1F9FF) ||
	    (cp >= 0x1FA00 && cp <= 0x1FA6F) ||
	    (cp >= 0x1FA70 && cp <= 0x1FAFF) ||
	    (cp >= 0x2600 && cp <= 0x26FF) ||
	    (cp >= 0x2700 && cp <= 0x27BF) ||
	    (cp >= 0xFE00 && cp <= 0xFE0F) ||
	    (cp >= 0x1F000 && cp <= 0x1F02F) ||
	    (cp >= 0x1F0A0 && cp <= 0x1F0FF) ||
	    cp == 0x200D || cp == 0xFE0F)
		return CHAR_EMOJI_OTHER;
	if (utf8_is_latin_extended_cp(cp))
		return CHAR_LATIN_EXTENDED;

	return CHAR_OTHER;
}

int tokenizer_estimate_tokens(const char *text)
{
	if (!text || *text == '\0')
		return 0;

	int tokens = 0;
	int word_chars = 0;		/* consecutive ASCII word chars */
	int consecutive_newlines = 0;

	const unsigned char *p = (const unsigned char *)text;
	while (*p) {
		enum char_class cc = classify_utf8(&p);

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
			if (word_chars > 0) {
				/* Spaces inside a word sequence: likely merged.
				 * Don't break the word yet */
				word_chars++;
			}
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

	return tokens;
}

struct tokenizer *tokenizer_create(const char *model_name, int context_limit)
{
	struct tokenizer *tok = calloc(1, sizeof(*tok));
	if (!tok)
		return NULL;
	strncpy(tok->model_name, model_name ? model_name : "gpt-4o",
		sizeof(tok->model_name) - 1);
	tok->context_limit = context_limit > 0 ? context_limit : 128000;
	tok->count = tokenizer_estimate_tokens;

	enum bpe_encoding enc = BPE_CL100K_BASE;
	if (model_name) {
		if (strstr(model_name, "o200k") ||
		    strstr(model_name, "gpt-4o-2024-08") ||
		    strstr(model_name, "gpt-4o-2025") ||
		    strstr(model_name, "o1-") ||
		    strstr(model_name, "o3-") ||
		    strstr(model_name, "o4-"))
			enc = BPE_O200K_BASE;
	}

	tok->encoder = bpe_encoder_create(enc, NULL);
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
