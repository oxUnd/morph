#define _POSIX_C_SOURCE 200809L
#include "toml.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *(*ppmalloc)(size_t) = malloc;
static void (*ppfree)(void *) = free;

void toml_set_memutil(void *(*xxmalloc)(size_t), void (*xxfree)(void *)) {
  if (xxmalloc) ppmalloc = xxmalloc;
  if (xxfree) ppfree = xxfree;
}

#define ALIGN8(sz) (((sz) + 7) & ~7)
#define MALLOC(a) ppmalloc(a)
#define FREE(a) ppfree(a)

static void *CALLOC(size_t nmemb, size_t sz) {
  int nb = ALIGN8(sz) * nmemb;
  void *p = MALLOC(nb);
  if (p) memset(p, 0, nb);
  return p;
}

#undef strdup
#define strdup(x) error - forbidden - use STRDUP instead

static char *STRDUP(const char *s) {
  int len = strlen(s);
  char *p = MALLOC(len + 1);
  if (p) { memcpy(p, s, len); p[len] = 0; }
  return p;
}

#undef strndup
#define strndup(x) error - forbiden - use STRNDUP instead

static char *STRNDUP(const char *s, size_t n) {
  size_t len = strnlen(s, n);
  char *p = MALLOC(len + 1);
  if (p) { memcpy(p, s, len); p[len] = 0; }
  return p;
}

int toml_utf8_to_ucs(const char *orig, int len, int64_t *ret) {
  const unsigned char *buf = (const unsigned char *)orig;
  unsigned i = *buf++;
  int64_t v;
  if (0 == (i >> 7)) {
    if (len < 1) return -1;
    v = i;
    return *ret = v, 1;
  }
  if (0x6 == (i >> 5)) {
    if (len < 2) return -1;
    v = i & 0x1f;
    for (int j = 0; j < 1; j++) {
      i = *buf++;
      if (0x2 != (i >> 6)) return -1;
      v = (v << 6) | (i & 0x3f);
    }
    return *ret = v, (const char *)buf - orig;
  }
  if (0xE == (i >> 4)) {
    if (len < 3) return -1;
    v = i & 0x0F;
    for (int j = 0; j < 2; j++) {
      i = *buf++;
      if (0x2 != (i >> 6)) return -1;
      v = (v << 6) | (i & 0x3f);
    }
    return *ret = v, (const char *)buf - orig;
  }
  if (0x1E == (i >> 3)) {
    if (len < 4) return -1;
    v = i & 0x07;
    for (int j = 0; j < 3; j++) {
      i = *buf++;
      if (0x2 != (i >> 6)) return -1;
      v = (v << 6) | (i & 0x3f);
    }
    return *ret = v, (const char *)buf - orig;
  }
  if (0x3E == (i >> 2)) {
    if (len < 5) return -1;
    v = i & 0x03;
    for (int j = 0; j < 4; j++) {
      i = *buf++;
      if (0x2 != (i >> 6)) return -1;
      v = (v << 6) | (i & 0x3f);
    }
    return *ret = v, (const char *)buf - orig;
  }
  if (0x7e == (i >> 1)) {
    if (len < 6) return -1;
    v = i & 0x01;
    for (int j = 0; j < 5; j++) {
      i = *buf++;
      if (0x2 != (i >> 6)) return -1;
      v = (v << 6) | (i & 0x3f);
    }
    return *ret = v, (const char *)buf - orig;
  }
  return -1;
}

int toml_ucs_to_utf8(int64_t code, char buf[6]) {
  if (0xd800 <= code && code <= 0xdfff) return -1;
  if (0xfffe <= code && code <= 0xffff) return -1;
  if (code < 0) return -1;
  if (code <= 0x7F) { buf[0] = (unsigned char)code; return 1; }
  if (code <= 0x000007FF) {
    buf[0] = (unsigned char)(0xc0 | (code >> 6));
    buf[1] = (unsigned char)(0x80 | (code & 0x3f));
    return 2;
  }
  if (code <= 0x0000FFFF) {
    buf[0] = (unsigned char)(0xe0 | (code >> 12));
    buf[1] = (unsigned char)(0x80 | ((code >> 6) & 0x3f));
    buf[2] = (unsigned char)(0x80 | (code & 0x3f));
    return 3;
  }
  if (code <= 0x001FFFFF) {
    buf[0] = (unsigned char)(0xf0 | (code >> 18));
    buf[1] = (unsigned char)(0x80 | ((code >> 12) & 0x3f));
    buf[2] = (unsigned char)(0x80 | ((code >> 6) & 0x3f));
    buf[3] = (unsigned char)(0x80 | (code & 0x3f));
    return 4;
  }
  if (code <= 0x03FFFFFF) {
    buf[0] = (unsigned char)(0xf8 | (code >> 24));
    buf[1] = (unsigned char)(0x80 | ((code >> 18) & 0x3f));
    buf[2] = (unsigned char)(0x80 | ((code >> 12) & 0x3f));
    buf[3] = (unsigned char)(0x80 | ((code >> 6) & 0x3f));
    buf[4] = (unsigned char)(0x80 | (code & 0x3f));
    return 5;
  }
  if (code <= 0x7FFFFFFF) {
    buf[0] = (unsigned char)(0xfc | (code >> 30));
    buf[1] = (unsigned char)(0x80 | ((code >> 24) & 0x3f));
    buf[2] = (unsigned char)(0x80 | ((code >> 18) & 0x3f));
    buf[3] = (unsigned char)(0x80 | ((code >> 12) & 0x3f));
    buf[4] = (unsigned char)(0x80 | ((code >> 6) & 0x3f));
    buf[5] = (unsigned char)(0x80 | (code & 0x3f));
    return 6;
  }
  return -1;
}

typedef struct toml_keyval_t toml_keyval_t;
struct toml_keyval_t { const char *key; const char *val; };

typedef struct toml_arritem_t toml_arritem_t;
struct toml_arritem_t {
  int valtype;
  char *val;
  toml_array_t *arr;
  toml_table_t *tab;
};

struct toml_array_t {
  const char *key;
  int kind;
  int type;
  int nitem;
  toml_arritem_t *item;
};

struct toml_table_t {
  const char *key;
  bool implicit;
  bool readonly;
  int nkval;
  toml_keyval_t **kval;
  int narr;
  toml_array_t **arr;
  int ntab;
  toml_table_t **tab;
};

static inline void xfree(const void *x) { if (x) FREE((void *)(intptr_t)x); }

typedef enum tokentype_t { INVALID, DOT, COMMA, EQUAL, LBRACE, RBRACE, NEWLINE, LBRACKET, RBRACKET, STRING } tokentype_t;

typedef struct token_t { tokentype_t tok; int lineno; char *ptr; int len; int eof; } token_t;

typedef struct context_t {
  char *start, *stop, *errbuf; int errbufsz;
  token_t tok;
  toml_table_t *root, *curtab;
  struct { int top; char *key[10]; token_t tok[10]; } tpath;
} context_t;

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define FLINE __FILE__ ":" TOSTRING(__LINE__)

static int next_token(context_t *ctx, int dotisspecial);

static int e_outofmemory(context_t *ctx, const char *fline) {
  snprintf(ctx->errbuf, ctx->errbufsz, "ERROR: out of memory (%s)", fline); return -1;
}
static int e_internal(context_t *ctx, const char *fline) {
  snprintf(ctx->errbuf, ctx->errbufsz, "internal error (%s)", fline); return -1;
}
static int e_syntax(context_t *ctx, int lineno, const char *msg) {
  snprintf(ctx->errbuf, ctx->errbufsz, "line %d: %s", lineno, msg); return -1;
}
static int e_badkey(context_t *ctx, int lineno) {
  snprintf(ctx->errbuf, ctx->errbufsz, "line %d: bad key", lineno); return -1;
}
static int e_keyexists(context_t *ctx, int lineno) {
  snprintf(ctx->errbuf, ctx->errbufsz, "line %d: key exists", lineno); return -1;
}
static int e_forbid(context_t *ctx, int lineno, const char *msg) {
  snprintf(ctx->errbuf, ctx->errbufsz, "line %d: %s", lineno, msg); return -1;
}

static void *expand(void *p, int sz, int newsz) {
  void *s = MALLOC(newsz);
  if (!s) return 0;
  if (p) { memcpy(s, p, sz); FREE(p); }
  return s;
}

static void **expand_ptrarr(void **p, int n) {
  void **s = MALLOC((n + 1) * sizeof(void *));
  if (!s) return 0;
  s[n] = 0;
  if (p) { memcpy(s, p, n * sizeof(void *)); FREE(p); }
  return s;
}

static toml_arritem_t *expand_arritem(toml_arritem_t *p, int n) {
  toml_arritem_t *pp = expand(p, n * sizeof(*p), (n + 1) * sizeof(*p));
  if (!pp) return 0;
  memset(&pp[n], 0, sizeof(pp[n]));
  return pp;
}

static char *norm_lit_str(const char *src, int srclen, int multiline, char *errbuf, int errbufsz) {
  char *dst = 0; int max = 0; int off = 0;
  const char *sp = src, *sq = src + srclen; int ch;
  for (;;) {
    if (off >= max - 10) {
      int newmax = max + 50; char *x = expand(dst, max, newmax);
      if (!x) { xfree(dst); snprintf(errbuf, errbufsz, "out of memory"); return 0; }
      dst = x; max = newmax;
    }
    if (sp >= sq) break;
    ch = *sp++;
    if ((0 <= ch && ch <= 0x08) || (0x0a <= ch && ch <= 0x1f) || (ch == 0x7f)) {
      if (!(multiline && (ch == '\r' || ch == '\n'))) {
        xfree(dst); snprintf(errbuf, errbufsz, "invalid char U+%04x", ch); return 0;
      }
    }
    dst[off++] = ch;
  }
  dst[off++] = 0; return dst;
}

static char *norm_basic_str(const char *src, int srclen, int multiline, char *errbuf, int errbufsz) {
  char *dst = 0; int max = 0, off = 0;
  const char *sp = src, *sq = src + srclen; int ch;
  for (;;) {
    if (off >= max - 10) {
      int newmax = max + 50; char *x = expand(dst, max, newmax);
      if (!x) { xfree(dst); snprintf(errbuf, errbufsz, "out of memory"); return 0; }
      dst = x; max = newmax;
    }
    if (sp >= sq) break;
    ch = *sp++;
    if (ch != '\\') {
      if ((0 <= ch && ch <= 0x08) || (0x0a <= ch && ch <= 0x1f) || (ch == 0x7f)) {
        if (!(multiline && (ch == '\r' || ch == '\n'))) {
          xfree(dst); snprintf(errbuf, errbufsz, "invalid char U+%04x", ch); return 0;
        }
      }
      dst[off++] = ch; continue;
    }
    if (sp >= sq) { snprintf(errbuf, errbufsz, "last backslash is invalid"); xfree(dst); return 0; }
    if (multiline) {
      if (sp[strspn(sp, " \t\r")] == '\n') { sp += strspn(sp, " \t\r\n"); continue; }
    }
    ch = *sp++;
    switch (ch) {
    case 'u': case 'U': {
      int64_t ucs = 0; int nhex = (ch == 'u' ? 4 : 8);
      for (int i = 0; i < nhex; i++) {
        if (sp >= sq) { snprintf(errbuf, errbufsz, "\\%c expects %d hex chars", ch, nhex); xfree(dst); return 0; }
        ch = *sp++;
        int v = ('0' <= ch && ch <= '9') ? ch - '0' : (('A' <= ch && ch <= 'F') ? ch - 'A' + 10 : -1);
        if (-1 == v) { snprintf(errbuf, errbufsz, "invalid hex chars for \\u or \\U"); xfree(dst); return 0; }
        ucs = ucs * 16 + v;
      }
      int n = toml_ucs_to_utf8(ucs, &dst[off]);
      if (-1 == n) { snprintf(errbuf, errbufsz, "illegal ucs code in \\u or \\U"); xfree(dst); return 0; }
      off += n;
    } continue;
    case 'b': ch = '\b'; break; case 't': ch = '\t'; break; case 'n': ch = '\n'; break;
    case 'f': ch = '\f'; break; case 'r': ch = '\r'; break; case '"': ch = '"'; break;
    case '\\': ch = '\\'; break;
    default: snprintf(errbuf, errbufsz, "illegal escape char \\%c", ch); xfree(dst); return 0;
    }
    dst[off++] = ch;
  }
  dst[off++] = 0; return dst;
}

static char *normalize_key(context_t *ctx, token_t strtok) {
  const char *sp = strtok.ptr, *sq = strtok.ptr + strtok.len;
  int lineno = strtok.lineno, ch = *sp;
  char ebuf[80], *ret;
  if (ch == '\'' || ch == '\"') {
    int multiline = 0;
    if (sp[1] == ch && sp[2] == ch) { sp += 3; sq -= 3; multiline = 1; } else { sp++; sq--; }
    if (ch == '\'') {
      if (!(ret = STRNDUP(sp, sq - sp))) { e_outofmemory(ctx, FLINE); return 0; }
    } else {
      ret = norm_basic_str(sp, sq - sp, multiline, ebuf, sizeof(ebuf));
      if (!ret) { e_syntax(ctx, lineno, ebuf); return 0; }
    }
    if (strchr(ret, '\n')) { xfree(ret); e_badkey(ctx, lineno); return 0; }
    return ret;
  }
  for (const char *xp = sp; xp != sq; xp++) {
    int k = *xp;
    if (isalnum(k) || k == '_' || k == '-') continue;
    e_badkey(ctx, lineno); return 0;
  }
  if (!(ret = STRNDUP(sp, sq - sp))) { e_outofmemory(ctx, FLINE); return 0; }
  return ret;
}

static int check_key(toml_table_t *tab, const char *key,
                     toml_keyval_t **ret_val, toml_array_t **ret_arr, toml_table_t **ret_tab) {
  int i; void *dummy;
  if (!ret_tab) ret_tab = (toml_table_t **)&dummy;
  if (!ret_arr) ret_arr = (toml_array_t **)&dummy;
  if (!ret_val) ret_val = (toml_keyval_t **)&dummy;
  *ret_tab = 0; *ret_arr = 0; *ret_val = 0;
  for (i = 0; i < tab->nkval; i++) { if (0 == strcmp(key, tab->kval[i]->key)) { *ret_val = tab->kval[i]; return 'v'; } }
  for (i = 0; i < tab->narr; i++) { if (0 == strcmp(key, tab->arr[i]->key)) { *ret_arr = tab->arr[i]; return 'a'; } }
  for (i = 0; i < tab->ntab; i++) { if (0 == strcmp(key, tab->tab[i]->key)) { *ret_tab = tab->tab[i]; return 't'; } }
  return 0;
}

static int key_kind(toml_table_t *tab, const char *key) { return check_key(tab, key, 0, 0, 0); }

static toml_keyval_t *create_keyval_in_table(context_t *ctx, toml_table_t *tab, token_t keytok) {
  char *newkey = normalize_key(ctx, keytok);
  if (!newkey) return 0;
  if (key_kind(tab, newkey)) { xfree(newkey); e_keyexists(ctx, keytok.lineno); return 0; }
  int n = tab->nkval;
  toml_keyval_t **base = (toml_keyval_t **)expand_ptrarr((void **)tab->kval, n);
  if (!base) { xfree(newkey); e_outofmemory(ctx, FLINE); return 0; }
  tab->kval = base;
  if (!(base[n] = (toml_keyval_t *)CALLOC(1, sizeof(*base[n])))) { xfree(newkey); e_outofmemory(ctx, FLINE); return 0; }
  toml_keyval_t *dest = tab->kval[tab->nkval++];
  dest->key = newkey; return dest;
}

static toml_table_t *create_keytable_in_table(context_t *ctx, toml_table_t *tab, token_t keytok) {
  char *newkey = normalize_key(ctx, keytok);
  if (!newkey) return 0;
  toml_table_t *dest = 0;
  if (check_key(tab, newkey, 0, 0, &dest)) {
    xfree(newkey);
    if (dest && dest->implicit) { dest->implicit = false; return dest; }
    e_keyexists(ctx, keytok.lineno); return 0;
  }
  int n = tab->ntab;
  toml_table_t **base = (toml_table_t **)expand_ptrarr((void **)tab->tab, n);
  if (!base) { xfree(newkey); e_outofmemory(ctx, FLINE); return 0; }
  tab->tab = base;
  if (!(base[n] = (toml_table_t *)CALLOC(1, sizeof(*base[n])))) { xfree(newkey); e_outofmemory(ctx, FLINE); return 0; }
  dest = tab->tab[tab->ntab++];
  dest->key = newkey; return dest;
}

static toml_array_t *create_keyarray_in_table(context_t *ctx, toml_table_t *tab, token_t keytok, char kind) {
  char *newkey = normalize_key(ctx, keytok);
  if (!newkey) return 0;
  if (key_kind(tab, newkey)) { xfree(newkey); e_keyexists(ctx, keytok.lineno); return 0; }
  int n = tab->narr;
  toml_array_t **base = (toml_array_t **)expand_ptrarr((void **)tab->arr, n);
  if (!base) { xfree(newkey); e_outofmemory(ctx, FLINE); return 0; }
  tab->arr = base;
  if (!(base[n] = (toml_array_t *)CALLOC(1, sizeof(*base[n])))) { xfree(newkey); e_outofmemory(ctx, FLINE); return 0; }
  toml_array_t *dest = tab->arr[tab->narr++];
  dest->key = newkey; dest->kind = kind; return dest;
}

static toml_arritem_t *create_value_in_array(context_t *ctx, toml_array_t *parent) {
  int n = parent->nitem;
  toml_arritem_t *base = expand_arritem(parent->item, n);
  if (!base) { e_outofmemory(ctx, FLINE); return 0; }
  parent->item = base; parent->nitem++;
  return &parent->item[n];
}

static toml_array_t *create_array_in_array(context_t *ctx, toml_array_t *parent) {
  int n = parent->nitem;
  toml_arritem_t *base = expand_arritem(parent->item, n);
  if (!base) { e_outofmemory(ctx, FLINE); return 0; }
  toml_array_t *ret = (toml_array_t *)CALLOC(1, sizeof(toml_array_t));
  if (!ret) { e_outofmemory(ctx, FLINE); return 0; }
  base[n].arr = ret; parent->item = base; parent->nitem++; return ret;
}

static toml_table_t *create_table_in_array(context_t *ctx, toml_array_t *parent) {
  int n = parent->nitem;
  toml_arritem_t *base = expand_arritem(parent->item, n);
  if (!base) { e_outofmemory(ctx, FLINE); return 0; }
  toml_table_t *ret = (toml_table_t *)CALLOC(1, sizeof(toml_table_t));
  if (!ret) { e_outofmemory(ctx, FLINE); return 0; }
  base[n].tab = ret; parent->item = base; parent->nitem++; return ret;
}

static int skip_newlines(context_t *ctx, int isdotspecial) {
  while (ctx->tok.tok == NEWLINE) { if (next_token(ctx, isdotspecial)) return -1; if (ctx->tok.eof) break; }
  return 0;
}

static int parse_keyval(context_t *ctx, toml_table_t *tab);

static inline int eat_token(context_t *ctx, tokentype_t typ, int isdotspecial, const char *fline) {
  if (ctx->tok.tok != typ) return e_internal(ctx, fline);
  if (next_token(ctx, isdotspecial)) return -1;
  return 0;
}

static int parse_inline_table(context_t *ctx, toml_table_t *tab) {
  if (eat_token(ctx, LBRACE, 1, FLINE)) return -1;
  for (;;) {
    if (ctx->tok.tok == NEWLINE) return e_syntax(ctx, ctx->tok.lineno, "newline not allowed in inline table");
    if (ctx->tok.tok == RBRACE) break;
    if (ctx->tok.tok != STRING) return e_syntax(ctx, ctx->tok.lineno, "expect a string");
    if (parse_keyval(ctx, tab)) return -1;
    if (ctx->tok.tok == NEWLINE) return e_syntax(ctx, ctx->tok.lineno, "newline not allowed in inline table");
    if (ctx->tok.tok == COMMA) { if (eat_token(ctx, COMMA, 1, FLINE)) return -1; continue; }
    break;
  }
  if (eat_token(ctx, RBRACE, 1, FLINE)) return -1;
  tab->readonly = 1; return 0;
}

static int valtype(const char *val) {
  toml_timestamp_t ts;
  if (*val == '\'' || *val == '"') return 's';
  if (0 == toml_rtob(val, 0)) return 'b';
  if (0 == toml_rtoi(val, 0)) return 'i';
  if (0 == toml_rtod(val, 0)) return 'd';
  if (0 == toml_rtots(val, &ts)) { if (ts.year && ts.hour) return 'T'; if (ts.year) return 'D'; return 't'; }
  return 'u';
}

static int parse_array(context_t *ctx, toml_array_t *arr) {
  if (eat_token(ctx, LBRACKET, 0, FLINE)) return -1;
  for (;;) {
    if (skip_newlines(ctx, 0)) return -1;
    if (ctx->tok.tok == RBRACKET) break;
    switch (ctx->tok.tok) {
    case STRING: {
      if (arr->kind == 0) arr->kind = 'v'; else if (arr->kind != 'v') arr->kind = 'm';
      toml_arritem_t *newval = create_value_in_array(ctx, arr);
      if (!newval) return e_outofmemory(ctx, FLINE);
      if (!(newval->val = STRNDUP(ctx->tok.ptr, ctx->tok.len))) return e_outofmemory(ctx, FLINE);
      newval->valtype = valtype(newval->val);
      if (arr->nitem == 1) arr->type = newval->valtype; else if (arr->type != newval->valtype) arr->type = 'm';
      if (eat_token(ctx, STRING, 0, FLINE)) return -1; break;
    }
    case LBRACKET: {
      if (arr->kind == 0) arr->kind = 'a'; else if (arr->kind != 'a') arr->kind = 'm';
      toml_array_t *subarr = create_array_in_array(ctx, arr);
      if (!subarr) return -1;
      if (parse_array(ctx, subarr)) return -1; break;
    }
    case LBRACE: {
      if (arr->kind == 0) arr->kind = 't'; else if (arr->kind != 't') arr->kind = 'm';
      toml_table_t *subtab = create_table_in_array(ctx, arr);
      if (!subtab) return -1;
      if (parse_inline_table(ctx, subtab)) return -1; break;
    }
    default: return e_syntax(ctx, ctx->tok.lineno, "syntax error");
    }
    if (skip_newlines(ctx, 0)) return -1;
    if (ctx->tok.tok == COMMA) { if (eat_token(ctx, COMMA, 0, FLINE)) return -1; continue; }
    break;
  }
  if (eat_token(ctx, RBRACKET, 1, FLINE)) return -1;
  return 0;
}

static int parse_keyval(context_t *ctx, toml_table_t *tab) {
  if (tab->readonly) return e_forbid(ctx, ctx->tok.lineno, "cannot insert new entry into existing table");
  token_t key = ctx->tok;
  if (eat_token(ctx, STRING, 1, FLINE)) return -1;
  if (ctx->tok.tok == DOT) {
    toml_table_t *subtab = 0;
    { char *subtabstr = normalize_key(ctx, key); if (!subtabstr) return -1; subtab = toml_table_in(tab, subtabstr); xfree(subtabstr); }
    if (!subtab) { subtab = create_keytable_in_table(ctx, tab, key); if (!subtab) return -1; }
    if (next_token(ctx, 1)) return -1;
    if (parse_keyval(ctx, subtab)) return -1;
    return 0;
  }
  if (ctx->tok.tok != EQUAL) return e_syntax(ctx, ctx->tok.lineno, "missing =");
  if (next_token(ctx, 0)) return -1;
  switch (ctx->tok.tok) {
  case STRING: {
    toml_keyval_t *keyval = create_keyval_in_table(ctx, tab, key);
    if (!keyval) return -1;
    if (!(keyval->val = STRNDUP(ctx->tok.ptr, ctx->tok.len))) return e_outofmemory(ctx, FLINE);
    if (next_token(ctx, 1)) return -1;
    return 0;
  }
  case LBRACKET: {
    toml_array_t *arr = create_keyarray_in_table(ctx, tab, key, 0);
    if (!arr) return -1;
    if (parse_array(ctx, arr)) return -1;
    return 0;
  }
  case LBRACE: {
    toml_table_t *nxttab = create_keytable_in_table(ctx, tab, key);
    if (!nxttab) return -1;
    if (parse_inline_table(ctx, nxttab)) return -1;
    return 0;
  }
  default: return e_syntax(ctx, ctx->tok.lineno, "syntax error");
  }
}

static int fill_tabpath(context_t *ctx) {
  int lineno = ctx->tok.lineno;
  for (int i = 0; i < ctx->tpath.top; i++) { xfree(ctx->tpath.key[i]); ctx->tpath.key[i] = 0; }
  ctx->tpath.top = 0;
  for (;;) {
    if (ctx->tpath.top >= 10) return e_syntax(ctx, lineno, "table path is too deep; max allowed is 10.");
    if (ctx->tok.tok != STRING) return e_syntax(ctx, lineno, "invalid or missing key");
    char *key = normalize_key(ctx, ctx->tok);
    if (!key) return -1;
    ctx->tpath.tok[ctx->tpath.top] = ctx->tok;
    ctx->tpath.key[ctx->tpath.top] = key;
    ctx->tpath.top++;
    if (next_token(ctx, 1)) return -1;
    if (ctx->tok.tok == RBRACKET) break;
    if (ctx->tok.tok != DOT) return e_syntax(ctx, lineno, "invalid key");
    if (next_token(ctx, 1)) return -1;
  }
  if (ctx->tpath.top <= 0) return e_syntax(ctx, lineno, "empty table selector");
  return 0;
}

static int walk_tabpath(context_t *ctx) {
  toml_table_t *curtab = ctx->root;
  for (int i = 0; i < ctx->tpath.top; i++) {
    const char *key = ctx->tpath.key[i];
    toml_keyval_t *nextval = 0; toml_array_t *nextarr = 0; toml_table_t *nexttab = 0;
    switch (check_key(curtab, key, &nextval, &nextarr, &nexttab)) {
    case 't': break;
    case 'a':
      if (nextarr->kind != 't') return e_internal(ctx, FLINE);
      if (nextarr->nitem == 0) return e_internal(ctx, FLINE);
      nexttab = nextarr->item[nextarr->nitem - 1].tab; break;
    case 'v': return e_keyexists(ctx, ctx->tpath.tok[i].lineno);
    default: {
      int n = curtab->ntab;
      toml_table_t **base = (toml_table_t **)expand_ptrarr((void **)curtab->tab, n);
      if (!base) return e_outofmemory(ctx, FLINE);
      curtab->tab = base;
      if (!(base[n] = (toml_table_t *)CALLOC(1, sizeof(*base[n])))) return e_outofmemory(ctx, FLINE);
      if (!(base[n]->key = STRDUP(key))) return e_outofmemory(ctx, FLINE);
      nexttab = curtab->tab[curtab->ntab++];
      nexttab->implicit = true;
    } break;
    }
    curtab = nexttab;
  }
  ctx->curtab = curtab;
  return 0;
}

static int parse_select(context_t *ctx) {
  assert(ctx->tok.tok == LBRACKET);
  int llb = (ctx->tok.ptr + 1 < ctx->stop && ctx->tok.ptr[1] == '[');
  if (eat_token(ctx, LBRACKET, 1, FLINE)) return -1;
  if (llb) { assert(ctx->tok.tok == LBRACKET); if (eat_token(ctx, LBRACKET, 1, FLINE)) return -1; }
  if (fill_tabpath(ctx)) return -1;
  token_t z = ctx->tpath.tok[ctx->tpath.top - 1];
  xfree(ctx->tpath.key[ctx->tpath.top - 1]);
  ctx->tpath.top--;
  if (walk_tabpath(ctx)) return -1;
  if (!llb) {
    toml_table_t *curtab = create_keytable_in_table(ctx, ctx->curtab, z);
    if (!curtab) return -1;
    ctx->curtab = curtab;
  } else {
    char *zstr = normalize_key(ctx, z);
    if (!zstr) return -1;
    toml_array_t *arr = toml_array_in(ctx->curtab, zstr);
    xfree(zstr);
    if (!arr) { arr = create_keyarray_in_table(ctx, ctx->curtab, z, 't'); if (!arr) return -1; }
    if (arr->kind != 't') return e_syntax(ctx, z.lineno, "array mismatch");
    toml_table_t *t = create_table_in_array(ctx, arr);
    if (!t) return -1;
    if (!(t->key = STRDUP("__anon__"))) return e_outofmemory(ctx, FLINE);
    ctx->curtab = t;
  }
  if (ctx->tok.tok != RBRACKET) return e_syntax(ctx, ctx->tok.lineno, "expects ]");
  if (llb) {
    if (!(ctx->tok.ptr + 1 < ctx->stop && ctx->tok.ptr[1] == ']')) return e_syntax(ctx, ctx->tok.lineno, "expects ]]");
    if (eat_token(ctx, RBRACKET, 1, FLINE)) return -1;
  }
  if (eat_token(ctx, RBRACKET, 1, FLINE)) return -1;
  if (ctx->tok.tok != NEWLINE) return e_syntax(ctx, ctx->tok.lineno, "extra chars after ] or ]]");
  return 0;
}

toml_table_t *toml_parse(char *conf, char *errbuf, int errbufsz) {
  context_t ctx;
  if (errbufsz <= 0) errbufsz = 0;
  if (errbufsz > 0) errbuf[0] = 0;
  memset(&ctx, 0, sizeof(ctx));
  ctx.start = conf; ctx.stop = ctx.start + strlen(conf);
  ctx.errbuf = errbuf; ctx.errbufsz = errbufsz;
  ctx.tok.tok = NEWLINE; ctx.tok.lineno = 1; ctx.tok.ptr = conf; ctx.tok.len = 0;
  if (0 == (ctx.root = CALLOC(1, sizeof(*ctx.root)))) { e_outofmemory(&ctx, FLINE); return 0; }
  ctx.curtab = ctx.root;
  for (token_t tok = ctx.tok; !tok.eof; tok = ctx.tok) {
    switch (tok.tok) {
    case NEWLINE: if (next_token(&ctx, 1)) goto fail; break;
    case STRING:
      if (parse_keyval(&ctx, ctx.curtab)) goto fail;
      if (ctx.tok.tok != NEWLINE) { e_syntax(&ctx, ctx.tok.lineno, "extra chars after value"); goto fail; }
      if (eat_token(&ctx, NEWLINE, 1, FLINE)) goto fail; break;
    case LBRACKET: if (parse_select(&ctx)) goto fail; break;
    default: e_syntax(&ctx, tok.lineno, "syntax error"); goto fail;
    }
  }
  for (int i = 0; i < ctx.tpath.top; i++) xfree(ctx.tpath.key[i]);
  return ctx.root;
fail:
  for (int i = 0; i < ctx.tpath.top; i++) xfree(ctx.tpath.key[i]);
  toml_free(ctx.root); return 0;
}

toml_table_t *toml_parse_file(FILE *fp, char *errbuf, int errbufsz) {
  int bufsz = 0, off = 0; char *buf = 0;
  while (!feof(fp)) {
    if (off == bufsz) {
      int xsz = bufsz + 1000; char *x = expand(buf, bufsz, xsz);
      if (!x) { snprintf(errbuf, errbufsz, "out of memory"); xfree(buf); return 0; }
      buf = x; bufsz = xsz;
    }
    errno = 0; int n = fread(buf + off, 1, bufsz - off, fp);
    if (ferror(fp)) { snprintf(errbuf, errbufsz, "%s", errno ? strerror(errno) : "Error reading file"); xfree(buf); return 0; }
    off += n;
  }
  if (off == bufsz) {
    int xsz = bufsz + 1; char *x = expand(buf, bufsz, xsz);
    if (!x) { snprintf(errbuf, errbufsz, "out of memory"); xfree(buf); return 0; }
    buf = x; bufsz = xsz;
  }
  buf[off] = 0;
  toml_table_t *ret = toml_parse(buf, errbuf, errbufsz);
  xfree(buf); return ret;
}

static void xfree_kval(toml_keyval_t *p) { if (!p) return; xfree(p->key); xfree(p->val); xfree(p); }

static void xfree_tab(toml_table_t *p);

static void xfree_arr(toml_array_t *p) {
  if (!p) return;
  xfree(p->key);
  for (int i = 0; i < p->nitem; i++) {
    toml_arritem_t *a = &p->item[i];
    if (a->val) xfree(a->val); else if (a->arr) xfree_arr(a->arr); else if (a->tab) xfree_tab(a->tab);
  }
  xfree(p->item); xfree(p);
}

static void xfree_tab(toml_table_t *p) {
  if (!p) return;
  xfree(p->key);
  for (int i = 0; i < p->nkval; i++) xfree_kval(p->kval[i]);
  xfree(p->kval);
  for (int i = 0; i < p->narr; i++) xfree_arr(p->arr[i]);
  xfree(p->arr);
  for (int i = 0; i < p->ntab; i++) xfree_tab(p->tab[i]);
  xfree(p->tab);
  xfree(p);
}

void toml_free(toml_table_t *tab) { xfree_tab(tab); }

static void set_token(context_t *ctx, tokentype_t tok, int lineno, char *ptr, int len) {
  ctx->tok.tok = tok; ctx->tok.lineno = lineno; ctx->tok.ptr = ptr; ctx->tok.len = len; ctx->tok.eof = 0;
}

static void set_eof(context_t *ctx, int lineno) {
  set_token(ctx, NEWLINE, lineno, ctx->stop, 0); ctx->tok.eof = 1;
}

static int scan_digits(const char *p, int n) {
  int ret = 0;
  for (; n > 0 && isdigit(*p); n--, p++) ret = 10 * ret + (*p - '0');
  return n ? -1 : ret;
}

static int scan_date(const char *p, int *YY, int *MM, int *DD) {
  int year = scan_digits(p, 4);
  int month = (year >= 0 && p[4] == '-') ? scan_digits(p + 5, 2) : -1;
  int day = (month >= 0 && p[7] == '-') ? scan_digits(p + 8, 2) : -1;
  if (YY) *YY = year; if (MM) *MM = month; if (DD) *DD = day;
  return (year >= 0 && month >= 0 && day >= 0) ? 0 : -1;
}

static int scan_time(const char *p, int *hh, int *mm, int *ss) {
  int hour = scan_digits(p, 2);
  int minute = (hour >= 0 && p[2] == ':') ? scan_digits(p + 3, 2) : -1;
  int second = (minute >= 0 && p[5] == ':') ? scan_digits(p + 6, 2) : -1;
  if (hh) *hh = hour; if (mm) *mm = minute; if (ss) *ss = second;
  return (hour >= 0 && minute >= 0 && second >= 0) ? 0 : -1;
}

static int scan_string(context_t *ctx, char *p, int lineno, int dotisspecial) {
  char *orig = p;
  if (0 == strncmp(p, "'''", 3)) {
    char *q = p + 3;
    while (1) { q = strstr(q, "'''"); if (!q) return e_syntax(ctx, lineno, "unterminated triple-s-quote"); while (q[3] == '\'') q++; break; }
    set_token(ctx, STRING, lineno, orig, q + 3 - orig); return 0;
  }
  if (0 == strncmp(p, "\"\"\"", 3)) {
    char *q = p + 3;
    while (1) {
      q = strstr(q, "\"\"\"");
      if (!q) return e_syntax(ctx, lineno, "unterminated triple-d-quote");
      if (q[-1] == '\\') { q++; continue; }
      while (q[3] == '\"') q++;
      break;
    }
    int hexreq = 0, escape = 0;
    for (p += 3; p < q; p++) {
      if (escape) { escape = 0;
        if (strchr("btnfr\"\\", *p)) continue;
        if (*p == 'u') { hexreq = 4; continue; }
        if (*p == 'U') { hexreq = 8; continue; }
        if (p[strspn(p, " \t\r")] == '\n') continue;
        return e_syntax(ctx, lineno, "bad escape char"); }
      if (hexreq) { hexreq--; if (strchr("0123456789ABCDEF", *p)) continue; return e_syntax(ctx, lineno, "expect hex char"); }
      if (*p == '\\') { escape = 1; continue; }
    }
    if (escape) return e_syntax(ctx, lineno, "expect an escape char");
    if (hexreq) return e_syntax(ctx, lineno, "expected more hex char");
    set_token(ctx, STRING, lineno, orig, q + 3 - orig); return 0;
  }
  if ('\'' == *p) {
    for (p++; *p && *p != '\n' && *p != '\''; p++);
    if (*p != '\'') return e_syntax(ctx, lineno, "unterminated s-quote");
    set_token(ctx, STRING, lineno, orig, p + 1 - orig); return 0;
  }
  if ('\"' == *p) {
    int hexreq = 0, escape = 0;
    for (p++; *p; p++) {
      if (escape) { escape = 0;
        if (strchr("btnfr\"\\", *p)) continue;
        if (*p == 'u') { hexreq = 4; continue; }
        if (*p == 'U') { hexreq = 8; continue; }
        return e_syntax(ctx, lineno, "bad escape char"); }
      if (hexreq) { hexreq--; if (strchr("0123456789ABCDEF", *p)) continue; return e_syntax(ctx, lineno, "expect hex char"); }
      if (*p == '\\') { escape = 1; continue; }
      if (*p == '\'') { if (p[1] == '\'' && p[2] == '\'') return e_syntax(ctx, lineno, "triple-s-quote inside string lit"); continue; }
      if (*p == '\n') break;
      if (*p == '"') break;
    }
    if (*p != '"') return e_syntax(ctx, lineno, "unterminated quote");
    set_token(ctx, STRING, lineno, orig, p + 1 - orig); return 0;
  }
  if (0 == scan_date(p, 0, 0, 0) || 0 == scan_time(p, 0, 0, 0)) {
    p += strspn(p, "0123456789.:+-Tt Zz");
    for (; p[-1] == ' '; p--);
    set_token(ctx, STRING, lineno, orig, p - orig); return 0;
  }
  for (; *p && *p != '\n'; p++) {
    int ch = *p;
    if (ch == '.' && dotisspecial) break;
    if (isalnum(ch) || strchr("0123456789+-_.", ch)) continue;
    break;
  }
  set_token(ctx, STRING, lineno, orig, p - orig); return 0;
}

static int next_token(context_t *ctx, int dotisspecial) {
  int lineno = ctx->tok.lineno; char *p = ctx->tok.ptr;
  for (int i = 0; i < ctx->tok.len; i++) { if (*p++ == '\n') lineno++; }
  while (p < ctx->stop) {
    if (*p == '#') { for (p++; p < ctx->stop && *p != '\n'; p++); continue; }
    if (dotisspecial && *p == '.') { set_token(ctx, DOT, lineno, p, 1); return 0; }
    switch (*p) {
    case ',': set_token(ctx, COMMA, lineno, p, 1); return 0;
    case '=': set_token(ctx, EQUAL, lineno, p, 1); return 0;
    case '{': set_token(ctx, LBRACE, lineno, p, 1); return 0;
    case '}': set_token(ctx, RBRACE, lineno, p, 1); return 0;
    case '[': set_token(ctx, LBRACKET, lineno, p, 1); return 0;
    case ']': set_token(ctx, RBRACKET, lineno, p, 1); return 0;
    case '\n': set_token(ctx, NEWLINE, lineno, p, 1); return 0;
    case '\r': case ' ': case '\t': p++; continue;
    }
    return scan_string(ctx, p, lineno, dotisspecial);
  }
  set_eof(ctx, lineno); return 0;
}

const char *toml_key_in(const toml_table_t *tab, int keyidx) {
  if (keyidx < tab->nkval) return tab->kval[keyidx]->key;
  keyidx -= tab->nkval;
  if (keyidx < tab->narr) return tab->arr[keyidx]->key;
  keyidx -= tab->narr;
  if (keyidx < tab->ntab) return tab->tab[keyidx]->key;
  return 0;
}

int toml_key_exists(const toml_table_t *tab, const char *key) {
  for (int i = 0; i < tab->nkval; i++) { if (0 == strcmp(key, tab->kval[i]->key)) return 1; }
  for (int i = 0; i < tab->narr; i++) { if (0 == strcmp(key, tab->arr[i]->key)) return 1; }
  for (int i = 0; i < tab->ntab; i++) { if (0 == strcmp(key, tab->tab[i]->key)) return 1; }
  return 0;
}

toml_raw_t toml_raw_in(const toml_table_t *tab, const char *key) {
  for (int i = 0; i < tab->nkval; i++) { if (0 == strcmp(key, tab->kval[i]->key)) return tab->kval[i]->val; }
  return 0;
}

toml_array_t *toml_array_in(const toml_table_t *tab, const char *key) {
  for (int i = 0; i < tab->narr; i++) { if (0 == strcmp(key, tab->arr[i]->key)) return tab->arr[i]; }
  return 0;
}

toml_table_t *toml_table_in(const toml_table_t *tab, const char *key) {
  for (int i = 0; i < tab->ntab; i++) { if (0 == strcmp(key, tab->tab[i]->key)) return tab->tab[i]; }
  return 0;
}

toml_raw_t toml_raw_at(const toml_array_t *arr, int idx) {
  return (0 <= idx && idx < arr->nitem) ? arr->item[idx].val : 0;
}

char toml_array_kind(const toml_array_t *arr) { return arr->kind; }
char toml_array_type(const toml_array_t *arr) { return (arr->kind != 'v' || arr->nitem == 0) ? 0 : arr->type; }
int toml_array_nelem(const toml_array_t *arr) { return arr->nitem; }
const char *toml_array_key(const toml_array_t *arr) { return arr ? arr->key : NULL; }
int toml_table_nkval(const toml_table_t *tab) { return tab->nkval; }
int toml_table_narr(const toml_table_t *tab) { return tab->narr; }
int toml_table_ntab(const toml_table_t *tab) { return tab->ntab; }
const char *toml_table_key(const toml_table_t *tab) { return tab ? tab->key : NULL; }

toml_array_t *toml_array_at(const toml_array_t *arr, int idx) {
  return (0 <= idx && idx < arr->nitem) ? arr->item[idx].arr : 0;
}

toml_table_t *toml_table_at(const toml_array_t *arr, int idx) {
  return (0 <= idx && idx < arr->nitem) ? arr->item[idx].tab : 0;
}

int toml_rtots(toml_raw_t src_, toml_timestamp_t *ret) {
  if (!src_) return -1;
  const char *p = src_;
  memset(ret, 0, sizeof(*ret));
  int *year = &ret->__buffer.year, *month = &ret->__buffer.month, *day = &ret->__buffer.day;
  int *hour = &ret->__buffer.hour, *minute = &ret->__buffer.minute, *second = &ret->__buffer.second, *millisec = &ret->__buffer.millisec;
  if (0 == scan_date(p, year, month, day)) {
    ret->year = year; ret->month = month; ret->day = day;
    p += 10;
    if (*p) { if (*p != 'T' && *p != 't' && *p != ' ') return -1; p++; }
  }
  if (0 == scan_time(p, hour, minute, second)) {
    ret->hour = hour; ret->minute = minute; ret->second = second;
    p += 8;
    if (*p == '.') {
      const char *end = 0; int ms = parse_millisec(p, &end);
      if (ms >= 0) { *millisec = ms; ret->millisec = millisec; p = end; }
    }
    if (*p) { if (*p != 'Z' && *p != 'z') return -1; ret->z = ret->__buffer.z; strncpy(ret->z, p, 9); p++; }
  }
  if (*p != 0) return -1;
  return 0;
}

static int parse_millisec(const char *p, const char **endp) {
  int ms = 0;
  if (*p != '.') { *endp = p; return -1; }
  p++;
  if (!isdigit(*p)) { *endp = p; return -1; }
  int i;
  for (i = 0; i < 3; i++) { if (isdigit(*p)) { ms = ms * 10 + (*p - '0'); p++; } else break; }
  for (; i < 3; i++) ms *= 10;
  if (endp) *endp = p;
  return ms;
}

int toml_rtos(toml_raw_t s, char **ret) {
  if (!s) return -1;
  int lineno = 0; char *p;
  char ebuf[80];
  if (*s == '\'' || *s == '"') {
    if (s[1] == s[0] && s[2] == s[0]) {
      p = STRDUP(s);
      if (!p) return -1;
    } else {
      char *q = STRDUP(s);
      if (!q) return -1;
      p = norm_basic_str(q + 1, strlen(q) - 2, 0, ebuf, sizeof(ebuf));
      xfree(q);
    }
  } else {
    p = STRDUP(s);
  }
  if (!p) return -1;
  *ret = p;
  return 0;
}

int toml_rtob(toml_raw_t s, int *ret) {
  if (!s) return -1;
  if (0 == strcasecmp(s, "true")) { if (ret) *ret = 1; return 0; }
  if (0 == strcasecmp(s, "false")) { if (ret) *ret = 0; return 0; }
  return -1;
}

int toml_rtoi(toml_raw_t s, int64_t *ret) {
  if (!s) return -1;
  char *e; int64_t v = strtoll(s, &e, 0);
  if (*e != 0) return -1;
  if (ret) *ret = v;
  return 0;
}

int toml_rtod_ex(toml_raw_t s, double *ret, char *buf, int buflen) {
  if (!s) return -1;
  char *e; double v = strtod(s, &e);
  if (*e != 0) {
    int len = strlen(s);
    if (len >= buflen) return -1;
    memcpy(buf, s, len + 1);
    int i = len;
    while (i > 0 && buf[--i] == ' ') buf[i] = 0;
    v = strtod(buf, &e);
    if (*e != 0) return -1;
  }
  if (ret) *ret = v;
  return 0;
}

int toml_rtod(toml_raw_t s, double *ret) {
  char buf[128];
  return toml_rtod_ex(s, ret, buf, sizeof(buf));
}

toml_datum_t toml_string_at(const toml_array_t *arr, int idx) {
  toml_datum_t d; memset(&d, 0, sizeof(d));
  if (0 <= idx && idx < arr->nitem && arr->item[idx].val) {
    d.ok = (0 == toml_rtos(arr->item[idx].val, &d.u.s)) ? 1 : 0;
  }
  return d;
}

toml_datum_t toml_bool_at(const toml_array_t *arr, int idx) {
  toml_datum_t d; memset(&d, 0, sizeof(d));
  if (0 <= idx && idx < arr->nitem && arr->item[idx].val) {
    int x = 0; d.ok = (0 == toml_rtob(arr->item[idx].val, &x)) ? 1 : 0; if (d.ok) d.u.b = x;
  }
  return d;
}

toml_datum_t toml_int_at(const toml_array_t *arr, int idx) {
  toml_datum_t d; memset(&d, 0, sizeof(d));
  if (0 <= idx && idx < arr->nitem && arr->item[idx].val) {
    int64_t x = 0; d.ok = (0 == toml_rtoi(arr->item[idx].val, &x)) ? 1 : 0; if (d.ok) d.u.i = x;
  }
  return d;
}

toml_datum_t toml_double_at(const toml_array_t *arr, int idx) {
  toml_datum_t d; memset(&d, 0, sizeof(d));
  if (0 <= idx && idx < arr->nitem && arr->item[idx].val) {
    double x = 0; d.ok = (0 == toml_rtod(arr->item[idx].val, &x)) ? 1 : 0; if (d.ok) d.u.d = x;
  }
  return d;
}

toml_datum_t toml_timestamp_at(const toml_array_t *arr, int idx) {
  toml_datum_t d; memset(&d, 0, sizeof(d));
  if (0 <= idx && idx < arr->nitem && arr->item[idx].val) {
    toml_timestamp_t *ts = (toml_timestamp_t *)CALLOC(1, sizeof(*ts));
    if (ts) { d.ok = (0 == toml_rtots(arr->item[idx].val, ts)) ? 1 : 0; if (d.ok) d.u.ts = ts; else xfree(ts); }
  }
  return d;
}

toml_datum_t toml_string_in(const toml_table_t *tab, const char *key) {
  toml_datum_t d; memset(&d, 0, sizeof(d));
  toml_raw_t r = toml_raw_in(tab, key);
  if (r) { d.ok = (0 == toml_rtos(r, &d.u.s)) ? 1 : 0; }
  return d;
}

toml_datum_t toml_bool_in(const toml_table_t *tab, const char *key) {
  toml_datum_t d; memset(&d, 0, sizeof(d));
  toml_raw_t r = toml_raw_in(tab, key);
  if (r) { int x = 0; d.ok = (0 == toml_rtob(r, &x)) ? 1 : 0; if (d.ok) d.u.b = x; }
  return d;
}

toml_datum_t toml_int_in(const toml_table_t *tab, const char *key) {
  toml_datum_t d; memset(&d, 0, sizeof(d));
  toml_raw_t r = toml_raw_in(tab, key);
  if (r) { int64_t x = 0; d.ok = (0 == toml_rtoi(r, &x)) ? 1 : 0; if (d.ok) d.u.i = x; }
  return d;
}

toml_datum_t toml_double_in(const toml_table_t *tab, const char *key) {
  toml_datum_t d; memset(&d, 0, sizeof(d));
  toml_raw_t r = toml_raw_in(tab, key);
  if (r) { double x = 0; d.ok = (0 == toml_rtod(r, &x)) ? 1 : 0; if (d.ok) d.u.d = x; }
  return d;
}

toml_datum_t toml_timestamp_in(const toml_table_t *tab, const char *key) {
  toml_datum_t d; memset(&d, 0, sizeof(d));
  toml_raw_t r = toml_raw_in(tab, key);
  if (r) {
    toml_timestamp_t *ts = (toml_timestamp_t *)CALLOC(1, sizeof(*ts));
    if (ts) { d.ok = (0 == toml_rtots(r, ts)) ? 1 : 0; if (d.ok) d.u.ts = ts; else xfree(ts); }
  }
  return d;
}
