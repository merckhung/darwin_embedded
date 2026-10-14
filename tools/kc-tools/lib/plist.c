#include "plist.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

PlistNode *plist_new_dict(void) {
  PlistNode *n = calloc(1, sizeof(*n));
  n->type = PLIST_DICT;
  return n;
}

PlistNode *plist_new_array(void) {
  PlistNode *n = calloc(1, sizeof(*n));
  n->type = PLIST_ARRAY;
  return n;
}

PlistNode *plist_new_string(const char *s) {
  PlistNode *n = calloc(1, sizeof(*n));
  n->type = PLIST_STRING;
  n->string = strdup(s);
  return n;
}

PlistNode *plist_new_integer(uint64_t v) {
  PlistNode *n = calloc(1, sizeof(*n));
  n->type = PLIST_INTEGER;
  n->integer = v;
  return n;
}

PlistNode *plist_new_data(const uint8_t *bytes, size_t len) {
  PlistNode *n = calloc(1, sizeof(*n));
  n->type = PLIST_DATA;
  n->data = malloc(len ? len : 1);
  if (len)
    memcpy(n->data, bytes, len);
  n->data_len = len;
  return n;
}

// --- base64 (plist <data>) ---
static const char b64_alpha[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint8_t *b64_decode(const char *s, size_t *out_len) {
  int8_t rev[256];
  memset(rev, -1, sizeof(rev));
  for (int i = 0; i < 64; i++)
    rev[(unsigned char)b64_alpha[i]] = (int8_t)i;

  size_t cap = strlen(s) / 4 * 3 + 3;
  uint8_t *out = malloc(cap);
  size_t o = 0;
  int quad[4], q = 0;
  for (const char *p = s; *p; p++) {
    if (*p == '=' || isspace((unsigned char)*p))
      continue;
    int v = rev[(unsigned char)*p];
    if (v < 0)
      continue;
    quad[q++] = v;
    if (q == 4) {
      out[o++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
      out[o++] = (uint8_t)((quad[1] << 4) | (quad[2] >> 2));
      out[o++] = (uint8_t)((quad[2] << 6) | quad[3]);
      q = 0;
    }
  }
  if (q == 3) {
    out[o++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
    out[o++] = (uint8_t)((quad[1] << 4) | (quad[2] >> 2));
  } else if (q == 2) {
    out[o++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
  }
  *out_len = o;
  return out;
}

void plist_dict_set(PlistNode *dict, const char *key, PlistNode *value) {
  PlistNode *e = dict->child;
  while (e) {
    if (e->key && strcmp(e->key, key) == 0) {
      plist_free(e->child);
      e->child = value;
      return;
    }
    e = e->next;
  }

  PlistNode *entry = calloc(1, sizeof(*entry));
  entry->type = PLIST_DICT;
  entry->key  = strdup(key);
  entry->child = value;

  PlistNode **tail = &dict->child;
  while (*tail)
    tail = &(*tail)->next;
  *tail = entry;
}

void plist_array_append(PlistNode *array, PlistNode *value) {
  PlistNode **tail = &array->child;
  while (*tail)
    tail = &(*tail)->next;
  *tail = value;
}

PlistNode *plist_dict_get(PlistNode *dict, const char *key) {
  PlistNode *e = dict->child;
  while (e) {
    if (e->key && strcmp(e->key, key) == 0)
      return e->child;
    e = e->next;
  }
  return NULL;
}

const char *plist_dict_get_str(PlistNode *dict, const char *key) {
  PlistNode *v = plist_dict_get(dict, key);
  return (v && v->type == PLIST_STRING) ? v->string : NULL;
}

void plist_free(PlistNode *n) {
  if (!n)
    return;

  if (n->key)
    free(n->key);

  plist_free(n->child);
  plist_free(n->next);

  if (n->type == PLIST_STRING && n->string)
    free(n->string);
  if (n->type == PLIST_DATA && n->data)
    free(n->data);
  free(n);
}

typedef struct { const char *p, *end; } P;

static void skip_ws(P *p) {
  while (p->p < p->end && isspace((unsigned char)*p->p))
    p->p++;
}

static int match(P *p, const char *s) {
  size_t l = strlen(s);
  if ((size_t)(p->end - p->p) >= l && !memcmp(p->p, s, l)) {
    p->p += l;
    return 1;
  }
  return 0;
}

static void skip_until(P *p, const char *end) {
  size_t l = strlen(end);
  while (p->p + l <= p->end) {
    if (!memcmp(p->p, end, l)) {
      p->p += l;
      return;
    }
    p->p++;
  }
}

static int skip_tag_rest(P *p) {
  int self_closing = 0;
  while (p->p < p->end && *p->p != '>') {
    self_closing = (*p->p == '/');
    p->p++;
  }
  if (p->p < p->end)
    p->p++;
  return self_closing;
}

static char *read_until(P *p, const char *end) {
  const char *s = p->p;
  size_t l = strlen(end);
  while (p->p + l <= p->end && memcmp(p->p, end, l))
    p->p++;

  size_t tl = (size_t)(p->p - s);
  char *buf = malloc(tl + 1);
  memcpy(buf, s, tl);
  buf[tl] = 0;

  char *o = buf, *i = buf;
  while (*i) {
    if (*i == '&') {
      if (!strncmp(i, "&amp;", 5)) {
        *o++ = '&';
        i += 5;
      } else if (!strncmp(i, "&lt;", 4)) {
        *o++ = '<';
        i += 4;
      } else if (!strncmp(i, "&gt;", 4)) {
        *o++ = '>';
        i += 4;
      } else if (!strncmp(i, "&apos;", 6)) {
        *o++ = '\'';
        i += 6;
      } else if (!strncmp(i, "&quot;", 6)) {
        *o++ = '"';
        i += 6;
      } else {
        *o++ = *i++;
      }
    } else {
      *o++ = *i++;
    }
  }
  *o = 0;

  return buf;
}

static PlistNode *parse_node(P *p);

static PlistNode *parse_dict(P *p) {
  PlistNode *d = plist_new_dict();
  while (1) {
    skip_ws(p);
    if (p->p >= p->end)
      break;

    if (!memcmp(p->p, "</dict>", 7)) {
      p->p += 7;
      break;
    }

    if (!match(p, "<key>"))
      break;

    char *key = read_until(p, "</key>");
    p->p += 6;
    skip_ws(p);

    PlistNode *val = parse_node(p);
    if (val)
      plist_dict_set(d, key, val);

    free(key);
  }
  return d;
}

static PlistNode *parse_array(P *p) {
  PlistNode *a = plist_new_array();
  while (1) {
    skip_ws(p);
    if (p->p >= p->end)
      break;

    if (!memcmp(p->p, "</array>", 8)) {
      p->p += 8;
      break;
    }

    PlistNode *val = parse_node(p);
    if (val)
      plist_array_append(a, val);
  }
  return a;
}

static PlistNode *parse_node(P *p) {
  skip_ws(p);
  if (p->p >= p->end || *p->p != '<')
    return NULL;

  p->p++;
  if (match(p, "!--")) {
    skip_until(p, "-->");
    return parse_node(p);
  }

  const char *ts = p->p;
  while (p->p < p->end && *p->p != '>' && *p->p != ' ' && *p->p != '/')
    p->p++;

  size_t tl = (size_t)(p->p - ts);
  char tag[32] = {0};
  if (tl < 32)
    memcpy(tag, ts, tl);

  int self_closing = skip_tag_rest(p);

  if (!strcmp(tag, "dict"))
    return self_closing ? plist_new_dict() : parse_dict(p);

  if (!strcmp(tag, "array"))
    return self_closing ? plist_new_array() : parse_array(p);

  if (!strcmp(tag, "string")) {
    char *t = read_until(p, "</string>");
    p->p += 9;
    PlistNode *n = plist_new_string(t);
    free(t);
    return n;
  }

  if (!strcmp(tag, "integer")) {
    char *t = read_until(p, "</integer>");
    p->p += 10;
    PlistNode *n = plist_new_integer(strtoull(t, NULL, 0));
    free(t);
    return n;
  }

  if (!strcmp(tag, "real")) {
    char *t = read_until(p, "</real>");
    p->p += 7;
    PlistNode *n = plist_new_string(t);
    free(t);
    return n;
  }

  if (!strcmp(tag, "true") || !strcmp(tag, "false")) {
    PlistNode *n = calloc(1, sizeof(PlistNode));
    n->type = PLIST_BOOL;
    n->integer = (tag[0] == 't') ? 1 : 0;
    return n;
  }

  if (!strcmp(tag, "data")) {
    char *t = read_until(p, "</data>");
    p->p += 7;
    size_t len = 0;
    uint8_t *bytes = b64_decode(t, &len);
    PlistNode *n = plist_new_data(bytes, len);
    free(bytes);
    free(t);
    return n;
  }

  return NULL;
}

PlistNode *plist_parse(const char *buf, size_t len) {
  P p = {buf, buf + len};
  while (p.p < p.end) {
    skip_ws(&p);
    if (p.p >= p.end)
      break;

    if (*p.p != '<') {
      p.p++;
      continue;
    }

    if (match(&p, "<?")) {
      skip_until(&p, "?>");
      continue;
    }

    if (match(&p, "<!DOCTYPE")) {
      skip_until(&p, ">");
      continue;
    }

    if (match(&p, "<!--")) {
      skip_until(&p, "-->");
      continue;
    }

    if (match(&p, "<plist")) {
      skip_tag_rest(&p);
      continue;
    }

    if (match(&p, "</plist>"))
      break;

    return parse_node(&p);
  }
  return NULL;
}

typedef struct { char *b; size_t l, c; } Buf;

static void bstr(Buf *b, const char *s) {
  size_t n = strlen(s);
  if (b->l + n + 1 > b->c) {
    b->c = (b->c + n + 1) * 2;
    b->b = realloc(b->b, b->c);
  }
  memcpy(b->b + b->l, s, n);
  b->l += n;
  b->b[b->l] = 0;
}

static void b64_encode(Buf *b, const uint8_t *data, size_t len) {
  char out[5];
  size_t i = 0;
  for (; i + 3 <= len; i += 3) {
    uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out[0] = b64_alpha[(v >> 18) & 0x3f];
    out[1] = b64_alpha[(v >> 12) & 0x3f];
    out[2] = b64_alpha[(v >> 6) & 0x3f];
    out[3] = b64_alpha[v & 0x3f];
    out[4] = 0;
    bstr(b, out);
  }
  size_t rem = len - i;
  if (rem == 1) {
    uint32_t v = data[i] << 16;
    out[0] = b64_alpha[(v >> 18) & 0x3f];
    out[1] = b64_alpha[(v >> 12) & 0x3f];
    out[2] = '=';
    out[3] = '=';
    out[4] = 0;
    bstr(b, out);
  } else if (rem == 2) {
    uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
    out[0] = b64_alpha[(v >> 18) & 0x3f];
    out[1] = b64_alpha[(v >> 12) & 0x3f];
    out[2] = b64_alpha[(v >> 6) & 0x3f];
    out[3] = '=';
    out[4] = 0;
    bstr(b, out);
  }
}

static void besc(Buf *b, const char *s) {
  for (; *s; s++) {
    if (*s == '&') {
      bstr(b, "&amp;");
    } else if (*s == '<') {
      bstr(b, "&lt;");
    } else if (*s == '>') {
      bstr(b, "&gt;");
    } else {
      char tmp[2] = {*s, 0};
      bstr(b, tmp);
    }
  }
}

static void write_node(Buf *b, PlistNode *n, int ind) {
  char pad[32] = {0};
  for (int i = 0; i < ind && i < 30; i++)
    pad[i] = '\t';

  switch (n->type) {
  case PLIST_DICT:
    bstr(b, pad); bstr(b, "<dict>\n");
    for (PlistNode *e = n->child; e; e = e->next) {
      bstr(b, pad); bstr(b, "\t<key>"); besc(b, e->key); bstr(b, "</key>\n");
      if (e->child) write_node(b, e->child, ind + 1);
    }
    bstr(b, pad); bstr(b, "</dict>\n");
    break;
  case PLIST_ARRAY:
    bstr(b, pad); bstr(b, "<array>\n");
    for (PlistNode *c = n->child; c; c = c->next)
      write_node(b, c, ind + 1);
    bstr(b, pad); bstr(b, "</array>\n");
    break;
  case PLIST_STRING:
    bstr(b, pad); bstr(b, "<string>"); besc(b, n->string); bstr(b, "</string>\n");
    break;
  case PLIST_INTEGER: {
    char tmp[32];
    // Apple's kernelcache plist uses hex with size="64" for address fields
    snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)n->integer);
    bstr(b, pad); bstr(b, "<integer size=\"64\">"); bstr(b, tmp); bstr(b, "</integer>\n");
    break;
  }
  case PLIST_BOOL:
    bstr(b, pad); bstr(b, n->integer ? "<true/>\n" : "<false/>\n");
    break;
  case PLIST_DATA:
    bstr(b, pad); bstr(b, "<data>");
    b64_encode(b, n->data, n->data_len);
    bstr(b, "</data>\n");
    break;
  }
}

char *plist_write_xml(PlistNode *root, size_t *out_len) {
  Buf b = {0};
  bstr(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  bstr(&b, "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n");
  bstr(&b, "<plist version=\"1.0\">\n");
  write_node(&b, root, 0);
  bstr(&b, "</plist>\n");
  if (out_len)
    *out_len = b.l;
  return b.b;
}
