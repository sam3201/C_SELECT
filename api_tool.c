// api_tool.c - generate api.def + index.json + auto_import.h, with
// public/private filtering Build:
//   cc -O2 -Wall -Wextra -std=c11 api_tool.c -o api_tool
//
// Commands:
//   ./api_tool gen --root . --out framework/api.def --index
//   framework/api_index.json
//   ./api_tool search --root . --kind fn_proto --name fw_add
//   ./api_tool needs --root . --entry game.c --out framework/auto_import.h
//   --vis public
//   ./api_tool needs --root . --entry game.c --out framework/auto_import.h
//   --vis private --preprocess "cc -E -P -I. game.c"
//
// Visibility rules (generator):
//   - If file path contains "/include/" or "/public/" -> PUBLIC
//   - Else PRIVATE
//   - Override by annotation in the 6 lines before symbol:
//       // @api public
//       // @api private
//
// Notes:
// - Heuristic parser, but fully working for conventional C style.
// - Multi-line prototypes are handled in "needs" (identifier scan), but
// extraction focuses on common forms.

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef enum {
  SYM_FN_PROTO,
  SYM_FN_DEF,
  SYM_STRUCT,
  SYM_TYPEDEF_STRUCT
} SymKind;

typedef enum { VIS_PRIVATE = 0, VIS_PUBLIC = 1 } Visibility;

typedef struct {
  SymKind kind;
  Visibility vis;
  char *name;
  char *file;
  int line_start;
  int line_end;
  char *backend; // "core" | "sdl" | "raylib" | ...
  char *snippet; // raw snippet lines
  char *sigline; // for functions: normalized first-line signature (best-effort)
} Symbol;

typedef struct {
  Symbol *data;
  size_t len;
  size_t cap;
} SymVec;

typedef struct {
  char **keys;
  size_t cap;
  size_t len;
} StrSet;

static void die(const char *msg) {
  fprintf(stderr, "error: %s\n", msg);
  exit(1);
}

static void *xmalloc(size_t n) {
  void *p = malloc(n);
  if (!p)
    die("out of memory");
  return p;
}

static char *xstrdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = (char *)xmalloc(n);
  memcpy(p, s, n);
  return p;
}

static void vec_push(SymVec *v, Symbol s) {
  if (v->len == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 128;
    v->data = (Symbol *)realloc(v->data, v->cap * sizeof(Symbol));
    if (!v->data)
      die("out of memory");
  }
  v->data[v->len++] = s;
}

static const char *kind_str(SymKind k) {
  switch (k) {
  case SYM_FN_PROTO:
    return "fn_proto";
  case SYM_FN_DEF:
    return "fn_def";
  case SYM_STRUCT:
    return "struct";
  case SYM_TYPEDEF_STRUCT:
    return "typedef_struct";
  }
  return "unknown";
}

static const char *vis_str(Visibility v) {
  return v == VIS_PUBLIC ? "PUBLIC" : "PRIVATE";
}

static bool has_c_ext(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot)
    return false;
  return strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0 ||
         strcmp(dot, ".cc") == 0 || strcmp(dot, ".cpp") == 0 ||
         strcmp(dot, ".hpp") == 0;
}

static bool is_dir(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  return S_ISDIR(st.st_mode);
}

static bool is_file(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  return S_ISREG(st.st_mode);
}

static char *path_join(const char *a, const char *b) {
  size_t na = strlen(a), nb = strlen(b);
  bool need = (na > 0 && a[na - 1] != '/');
  size_t n = na + (need ? 1 : 0) + nb + 1;
  char *p = (char *)xmalloc(n);
  snprintf(p, n, "%s%s%s", a, need ? "/" : "", b);
  return p;
}

static int count_lines_upto(const char *s, size_t off) {
  int line = 1;
  for (size_t i = 0; i < off; i++)
    if (s[i] == '\n')
      line++;
  return line;
}

static void json_escape_write(FILE *f, const char *s) {
  fputc('"', f);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    unsigned char c = *p;
    switch (c) {
    case '\\':
      fputs("\\\\", f);
      break;
    case '"':
      fputs("\\\"", f);
      break;
    case '\n':
      fputs("\\n", f);
      break;
    case '\r':
      fputs("\\r", f);
      break;
    case '\t':
      fputs("\\t", f);
      break;
    default:
      if (c < 0x20)
        fprintf(f, "\\u%04x", (unsigned)c);
      else
        fputc(c, f);
    }
  }
  fputc('"', f);
}

static char *read_entire_file(const char *path, size_t *out_len) {
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return NULL;
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return NULL;
  }
  long n = ftell(fp);
  if (n < 0) {
    fclose(fp);
    return NULL;
  }
  rewind(fp);
  char *buf = (char *)xmalloc((size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, fp);
  fclose(fp);
  buf[got] = '\0';
  if (out_len)
    *out_len = got;
  return buf;
}

static char *strip_comments(const char *src) {
  // naive comment stripper; OK for most codebases
  size_t n = strlen(src);
  char *out = (char *)xmalloc(n + 1);
  size_t i = 0, j = 0;
  while (i < n) {
    if (src[i] == '/' && src[i + 1] == '/') {
      i += 2;
      while (i < n && src[i] != '\n')
        i++;
    } else if (src[i] == '/' && src[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/'))
        i++;
      if (i + 1 < n)
        i += 2;
    } else {
      out[j++] = src[i++];
    }
  }
  out[j] = '\0';
  return out;
}

static size_t extract_brace_block(const char *s, size_t start,
                                  size_t *end_out) {
  size_t i = start;
  while (s[i] && s[i] != '{')
    i++;
  if (!s[i])
    return (size_t)-1;
  int depth = 0;
  size_t j = i;
  for (; s[j]; j++) {
    if (s[j] == '{')
      depth++;
    else if (s[j] == '}') {
      depth--;
      if (depth == 0) {
        if (end_out)
          *end_out = j + 1;
        return i;
      }
    }
  }
  if (end_out)
    *end_out = j;
  return i;
}

static char *slice_lines(const char *raw, int ls, int le) {
  int line = 1;
  const char *p = raw;
  const char *end = raw + strlen(raw);

  while (p < end && line < ls) {
    if (*p++ == '\n')
      line++;
  }
  const char *start = p;

  while (p < end && line <= le) {
    if (*p++ == '\n')
      line++;
  }

  size_t n = (size_t)(p - start);
  char *out = (char *)xmalloc(n + 1);
  memcpy(out, start, n);
  out[n] = '\0';
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
    out[--n] = '\0';
  return out;
}

static bool path_contains(const char *path, const char *needle) {
  return strstr(path, needle) != NULL;
}

static Visibility default_visibility_for_path(const char *relpath) {
  // Prefer public if it lives in include/ or public/
  if (path_contains(relpath, "/include/") ||
      path_contains(relpath, "/public/") ||
      strncmp(relpath, "include/", 8) == 0 ||
      strncmp(relpath, "public/", 7) == 0) {
    return VIS_PUBLIC;
  }
  return VIS_PRIVATE;
}

static Visibility annotation_visibility(const char *raw, int line_start) {
  // Look back up to 6 lines for "@api public/private"
  int lookback = 6;
  int target = line_start;
  int cur = 1;

  // We'll scan raw line-by-line until target; store last few lines in a ring
  const char *p = raw;
  const char *end = raw + strlen(raw);
  const char *ring[6] = {0};
  int ringi = 0;

  while (p < end) {
    const char *ls = p;
    while (p < end && *p != '\n')
      p++;
    size_t n = (size_t)(p - ls);

    char *line = (char *)xmalloc(n + 1);
    memcpy(line, ls, n);
    line[n] = 0;

    ring[ringi % lookback] = line;
    ringi++;

    if (cur == target) {
      // check ring buffer excluding current line
      int start = ringi - 1 - lookback;
      if (start < 0)
        start = 0;
      for (int i = ringi - 2; i >= start; i--) {
        const char *s = ring[i % lookback];
        if (!s)
          continue;
        if (strstr(s, "@api public")) {
          // free ring
          for (int k = 0; k < lookback; k++)
            if (ring[k])
              free((void *)ring[k]);
          return VIS_PUBLIC;
        }
        if (strstr(s, "@api private")) {
          for (int k = 0; k < lookback; k++)
            if (ring[k])
              free((void *)ring[k]);
          return VIS_PRIVATE;
        }
      }
      for (int k = 0; k < lookback; k++)
        if (ring[k])
          free((void *)ring[k]);
      return (Visibility)-1; // no annotation
    }

    if (p < end && *p == '\n')
      p++;
    cur++;
    // keep only last lookback stored; free older overwritten
    if (ringi > lookback) {
      int idx = (ringi - lookback - 1) % lookback;
      if (ring[idx]) {
        free((void *)ring[idx]);
        ring[idx] = 0;
      }
    }
  }

  for (int k = 0; k < lookback; k++)
    if (ring[k])
      free((void *)ring[k]);
  return (Visibility)-1;
}

static char *normalize_first_sigline(const char *snippet) {
  // Return normalized first line (collapse whitespace), for function sig
  // extraction
  const char *nl = strchr(snippet, '\n');
  size_t n = nl ? (size_t)(nl - snippet) : strlen(snippet);
  char *tmp = (char *)xmalloc(n + 1);
  memcpy(tmp, snippet, n);
  tmp[n] = 0;

  // collapse whitespace
  char *out = (char *)xmalloc(n + 1);
  size_t j = 0;
  bool inws = false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)tmp[i];
    if (isspace(c)) {
      if (!inws)
        out[j++] = ' ';
      inws = true;
    } else {
      inws = false;
      out[j++] = (char)c;
    }
  }
  out[j] = 0;

  // trim trailing ; or {
  while (j > 0 && isspace((unsigned char)out[j - 1]))
    out[--j] = 0;
  while (j > 0 && (out[j - 1] == ';' || out[j - 1] == '{'))
    out[--j] = 0;
  while (j > 0 && isspace((unsigned char)out[j - 1]))
    out[--j] = 0;

  free(tmp);
  return out;
}

/* =======================
   String Set (hash set)
   ======================= */

static uint64_t fnv1a(const char *s) {
  uint64_t h = 1469598103934665603ull;
  for (; *s; s++) {
    h ^= (unsigned char)*s;
    h *= 1099511628211ull;
  }
  return h;
}

static void set_init(StrSet *st, size_t cap_pow2) {
  st->cap = cap_pow2;
  st->len = 0;
  st->keys = (char **)calloc(st->cap, sizeof(char *));
  if (!st->keys)
    die("out of memory");
}

static void set_free(StrSet *st) {
  if (!st->keys)
    return;
  for (size_t i = 0; i < st->cap; i++)
    free(st->keys[i]);
  free(st->keys);
  st->keys = NULL;
  st->cap = st->len = 0;
}

static bool set_has(const StrSet *st, const char *key) {
  uint64_t h = fnv1a(key);
  size_t mask = st->cap - 1;
  for (size_t i = 0; i < st->cap; i++) {
    size_t idx = (size_t)(h + i) & mask;
    char *k = st->keys[idx];
    if (!k)
      return false;
    if (strcmp(k, key) == 0)
      return true;
  }
  return false;
}

static void set_grow(StrSet *st);

static void set_add(StrSet *st, const char *key) {
  if (st->len * 2 >= st->cap)
    set_grow(st);
  uint64_t h = fnv1a(key);
  size_t mask = st->cap - 1;
  for (size_t i = 0; i < st->cap; i++) {
    size_t idx = (size_t)(h + i) & mask;
    if (!st->keys[idx]) {
      st->keys[idx] = xstrdup(key);
      st->len++;
      return;
    }
    if (strcmp(st->keys[idx], key) == 0)
      return;
  }
}

static void set_grow(StrSet *st) {
  StrSet nst;
  set_init(&nst, st->cap ? st->cap * 2 : 1024);
  for (size_t i = 0; i < st->cap; i++) {
    if (st->keys[i])
      set_add(&nst, st->keys[i]);
  }
  set_free(st);
  *st = nst;
}

/* =======================
   Scanning
   ======================= */

static void scan_file(const char *path, const char *root, SymVec *out_syms,
                      regex_t *re_fn, regex_t *re_typedef_struct,
                      regex_t *re_struct) {

  size_t raw_len = 0;
  char *raw = read_entire_file(path, &raw_len);
  if (!raw)
    return;

  char *text = strip_comments(raw);

  // relative path
  const char *rel = path;
  size_t root_len = strlen(root);
  if (strncmp(path, root, root_len) == 0) {
    rel = path + root_len;
    if (*rel == '/')
      rel++;
  }

  Visibility file_default_vis = default_visibility_for_path(rel);

  // --- typedef struct ---
  for (size_t pos = 0; text[pos];) {
    regmatch_t m[2];
    if (regexec(re_typedef_struct, text + pos, 2, m, 0) != 0)
      break;

    size_t start = pos + (size_t)m[0].rm_so;
    size_t end_block = 0;
    size_t brace_i = extract_brace_block(text, start, &end_block);
    if (brace_i == (size_t)-1) {
      pos = start + 1;
      continue;
    }

    const char *tail = text + end_block;
    const char *semi = strchr(tail, ';');

    char namebuf[256] = {0};
    bool got = false;
    if (semi) {
      const char *r = semi;
      while (r > tail && !(isalnum((unsigned char)r[-1]) || r[-1] == '_'))
        r--;
      const char *end_id = r;
      while (r > tail && (isalnum((unsigned char)r[-1]) || r[-1] == '_'))
        r--;
      const char *start_id = r;
      size_t n = (size_t)(end_id - start_id);
      if (n > 0 && n < sizeof(namebuf)) {
        memcpy(namebuf, start_id, n);
        namebuf[n] = 0;
        got = true;
      }
    }
    if (!got)
      snprintf(namebuf, sizeof(namebuf), "ANON_TYPEDEF_STRUCT");

    int ls = count_lines_upto(text, start);
    size_t end_off = end_block;
    if (semi)
      end_off = end_block + (size_t)(semi - tail) + 1;
    int le = count_lines_upto(text, end_off);

    Visibility vis = file_default_vis;
    Visibility ann = annotation_visibility(raw, ls);
    if ((int)ann != -1)
      vis = ann;

    Symbol sym = {0};
    sym.kind = SYM_TYPEDEF_STRUCT;
    sym.vis = vis;
    sym.name = xstrdup(namebuf);
    sym.file = xstrdup(rel);
    sym.line_start = ls;
    sym.line_end = le;
    sym.snippet = slice_lines(raw, ls, le);
    sym.sigline = NULL;
    vec_push(out_syms, sym);

    pos = end_off;
  }

  // --- struct ---
  for (size_t pos = 0; text[pos];) {
    regmatch_t m[2];
    if (regexec(re_struct, text + pos, 2, m, 0) != 0)
      break;

    size_t start = pos + (size_t)m[0].rm_so;
    size_t tag_so = pos + (size_t)m[1].rm_so;
    size_t tag_eo = pos + (size_t)m[1].rm_eo;

    char tag[128] = {0};
    size_t tn = tag_eo > tag_so ? (tag_eo - tag_so) : 0;
    if (tn == 0 || tn >= sizeof(tag)) {
      pos = start + 1;
      continue;
    }
    memcpy(tag, text + tag_so, tn);
    tag[tn] = 0;

    size_t end_block = 0;
    if (extract_brace_block(text, start, &end_block) == (size_t)-1) {
      pos = start + 1;
      continue;
    }

    size_t end_off = end_block;
    size_t k = end_block;
    while (text[k] && isspace((unsigned char)text[k]))
      k++;
    if (text[k] == ';')
      end_off = k + 1;

    int ls = count_lines_upto(text, start);
    int le = count_lines_upto(text, end_off);

    Visibility vis = file_default_vis;
    Visibility ann = annotation_visibility(raw, ls);
    if ((int)ann != -1)
      vis = ann;

    Symbol sym = {0};
    sym.kind = SYM_STRUCT;
    sym.vis = vis;
    sym.name = xstrdup(tag);
    sym.file = xstrdup(rel);
    sym.line_start = ls;
    sym.line_end = le;
    sym.snippet = slice_lines(raw, ls, le);
    sym.sigline = NULL;
    vec_push(out_syms, sym);

    pos = end_off;
  }

  // --- functions (single-line sig matcher; defs get brace extraction) ---
  int line = 1;
  const char *p = text;
  const char *ls_ptr = text;
  size_t base_off = 0;

  while (*p) {
    if (*p == '\n') {
      size_t len = (size_t)(p - ls_ptr);
      char *ln = (char *)xmalloc(len + 1);
      memcpy(ln, ls_ptr, len);
      ln[len] = 0;

      regmatch_t m[4];
      if (regexec(re_fn, ln, 4, m, 0) == 0) {
        // group 1 = name, group 2 = tail ; or {
        char name[128] = {0};
        size_t nso = (size_t)m[1].rm_so;
        size_t neo = (size_t)m[1].rm_eo;
        size_t nn = (neo > nso) ? (neo - nso) : 0;
        if (nn > 0 && nn < sizeof(name)) {
          memcpy(name, ln + nso, nn);
          name[nn] = 0;
        }

        char tail = 0;
        if (m[2].rm_so >= 0)
          tail = ln[m[2].rm_so];

        int sym_ls = line;

        Visibility vis = file_default_vis;
        Visibility ann = annotation_visibility(raw, sym_ls);
        if ((int)ann != -1)
          vis = ann;

        if (tail == ';') {
          Symbol sym = {0};
          sym.kind = SYM_FN_PROTO;
          sym.vis = vis;
          sym.name = xstrdup(name);
          sym.file = xstrdup(rel);
          sym.line_start = sym_ls;
          sym.line_end = sym_ls;
          sym.snippet = slice_lines(raw, sym_ls, sym_ls);
          sym.sigline = normalize_first_sigline(sym.snippet);
          vec_push(out_syms, sym);
        } else if (tail == '{') {
          size_t end_block = 0;
          if (extract_brace_block(text, base_off, &end_block) != (size_t)-1) {
            int le = count_lines_upto(text, end_block);
            Symbol sym = {0};
            sym.kind = SYM_FN_DEF;
            sym.vis = vis;
            sym.name = xstrdup(name);
            sym.file = xstrdup(rel);
            sym.line_start = sym_ls;
            sym.line_end = le;
            sym.snippet = slice_lines(raw, sym_ls, le);
            sym.sigline = normalize_first_sigline(sym.snippet);
            vec_push(out_syms, sym);
          }
        }
      }

      free(ln);

      p++;
      line++;
      ls_ptr = p;
      base_off = (size_t)(p - text);
    } else {
      p++;
    }
  }

  free(text);
  free(raw);
}

static void walk_dir(const char *root, const char *path, SymVec *syms,
                     regex_t *re_fn, regex_t *re_typedef_struct,
                     regex_t *re_struct) {

  DIR *d = opendir(path);
  if (!d)
    return;

  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    const char *name = ent->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;

    if (strcmp(name, ".git") == 0 || strcmp(name, "build") == 0 ||
        strcmp(name, "dist") == 0 || strcmp(name, "out") == 0 ||
        strcmp(name, ".cache") == 0 || strcmp(name, ".vscode") == 0) {
      continue;
    }

    char *child = path_join(path, name);
    if (is_dir(child)) {
      walk_dir(root, child, syms, re_fn, re_typedef_struct, re_struct);
    } else if (is_file(child) && has_c_ext(child)) {
      scan_file(child, root, syms, re_fn, re_typedef_struct, re_struct);
    }
    free(child);
  }
  closedir(d);
}

static void free_syms(SymVec *v) {
  for (size_t i = 0; i < v->len; i++) {
    free(v->data[i].name);
    free(v->data[i].file);
    free(v->data[i].snippet);
    free(v->data[i].sigline);
  }
  free(v->data);
  v->data = NULL;
  v->len = v->cap = 0;
}

static void ensure_parent_dir(const char *path) {
  char *dup = xstrdup(path);
  char *slash = strrchr(dup, '/');
  if (slash) {
    *slash = 0;
    if (*dup)
      mkdir(dup, 0755);
  }
  free(dup);
}

/* =======================
   Emit: index.json + api.def
   ======================= */

static void write_index_json(const char *index_path, const SymVec *syms) {
  FILE *f = fopen(index_path, "wb");
  if (!f)
    die("failed to open index output");

  fputc('[', f);
  for (size_t i = 0; i < syms->len; i++) {
    const Symbol *s = &syms->data[i];
    if (i)
      fputc(',', f);
    fputc('\n', f);
    fputs("  {\"kind\":", f);
    json_escape_write(f, kind_str(s->kind));
    fputs(",\"vis\":", f);
    json_escape_write(f, vis_str(s->vis));
    fputs(",\"name\":", f);
    json_escape_write(f, s->name);
    fputs(",\"file\":", f);
    json_escape_write(f, s->file);
    fprintf(f, ",\"line_start\":%d,\"line_end\":%d", s->line_start,
            s->line_end);
    fputs(",\"snippet\":", f);
    json_escape_write(f, s->snippet);
    fputc('}', f);
  }
  fputs("\n]\n", f);
  fclose(f);
}

static bool starts_with(const char *s, const char *prefix) {
  if (!prefix || !*prefix)
    return true;
  size_t n = strlen(prefix);
  return strncmp(s, prefix, n) == 0;
}

static void emit_api_def(const char *out_path, const SymVec *syms,
                         const char *fn_prefix) {
  FILE *f = fopen(out_path, "wb");
  if (!f)
    die("failed to open api.def output");

  fputs("/* AUTO-GENERATED: do not edit by hand */\n", f);
  fputs("/* Generated by api_tool.c */\n\n", f);

  fputs("/* TYPES */\n", f);
  for (size_t i = 0; i < syms->len; i++) {
    const Symbol *s = &syms->data[i];
    if (!(s->kind == SYM_TYPEDEF_STRUCT || s->kind == SYM_STRUCT))
      continue;

    const char *sn = s->snippet;
    const char *lb = strchr(sn, '{');
    const char *rb = strrchr(sn, '}');
    if (!lb || !rb || rb <= lb)
      continue;

    fprintf(f, "API_TYPE(%s, %s,\n", vis_str(s->vis), s->name);

    const char *p = lb + 1;
    while (p < rb) {
      const char *e = strchr(p, '\n');
      if (!e || e > rb)
        e = rb;
      const char *end = e;
      while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        end--;
      fputs("  ", f);
      fwrite(p, 1, (size_t)(end - p), f);
      fputc('\n', f);
      if (e == rb)
        break;
      p = e + 1;
    }

    fputs(")\n\n", f);
  }

  fputs("/* FUNCTIONS (prototypes) */\n", f);
  for (size_t i = 0; i < syms->len; i++) {
    const Symbol *s = &syms->data[i];
    if (s->kind != SYM_FN_PROTO)
      continue;
    if (!starts_with(s->name, fn_prefix))
      continue;
    if (!s->sigline)
      continue;

    // Parse ret + name + args: find '(' then last identifier before it.
    const char *sig = s->sigline;
    const char *lp = strchr(sig, '(');
    if (!lp)
      continue;

    const char *q = lp;
    while (q > sig && isspace((unsigned char)q[-1]))
      q--;
    const char *end_id = q;
    while (q > sig && (isalnum((unsigned char)q[-1]) || q[-1] == '_'))
      q--;
    const char *start_id = q;

    // return type is [sig..start_id)
    size_t ret_len = (size_t)(start_id - sig);
    while (ret_len > 0 && isspace((unsigned char)sig[ret_len - 1]))
      ret_len--;

    char *ret = (char *)xmalloc(ret_len + 1);
    memcpy(ret, sig, ret_len);
    ret[ret_len] = 0;

    fprintf(f, "API_FN(%s, %s, %s, %s)\n", vis_str(s->vis), ret, s->name, lp);

    free(ret);
  }

  fclose(f);
}

/* =======================
   SEARCH (direct scan)
   ======================= */

static bool contains_case(const char *hay, const char *needle) {
  if (!needle || !*needle)
    return true;
  if (!hay)
    return false;
  size_t n = strlen(needle);
  for (const char *p = hay; *p; p++) {
    size_t i = 0;
    while (i < n && p[i] &&
           tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
      i++;
    if (i == n)
      return true;
  }
  return false;
}

static bool kind_match(SymKind k, const char *kind_s) {
  if (!kind_s)
    return true;
  if (strcmp(kind_s, "fn") == 0)
    return k == SYM_FN_PROTO || k == SYM_FN_DEF;
  if (strcmp(kind_s, "fn_proto") == 0)
    return k == SYM_FN_PROTO;
  if (strcmp(kind_s, "fn_def") == 0)
    return k == SYM_FN_DEF;
  if (strcmp(kind_s, "struct") == 0)
    return k == SYM_STRUCT || k == SYM_TYPEDEF_STRUCT;
  if (strcmp(kind_s, "typedef_struct") == 0)
    return k == SYM_TYPEDEF_STRUCT;
  return true;
}

static void do_search(const SymVec *syms, const char *kind_s, const char *name,
                      const char *pattern) {
  for (size_t i = 0; i < syms->len; i++) {
    const Symbol *s = &syms->data[i];
    if (!kind_match(s->kind, kind_s))
      continue;
    if (name && *name && strcmp(s->name, name) != 0)
      continue;
    if (pattern && *pattern) {
      if (!contains_case(s->name, pattern) &&
          !contains_case(s->snippet, pattern))
        continue;
    }
    printf("\n== %s/%s: %s  (%s:%d-%d) ==\n", vis_str(s->vis),
           kind_str(s->kind), s->name, s->file, s->line_start, s->line_end);
    puts(s->snippet);
  }
}

/* =======================
   NEEDS: auto-import generation
   ======================= */

static bool is_ident_start(int c) { return isalpha(c) || c == '_'; }
static bool is_ident_char(int c) { return isalnum(c) || c == '_'; }

static void collect_idents_from_text(const char *text, StrSet *idents) {
  const char *p = text;
  while (*p) {
    if (is_ident_start((unsigned char)*p)) {
      const char *s = p;
      p++;
      while (*p && is_ident_char((unsigned char)*p))
        p++;
      size_t n = (size_t)(p - s);
      if (n < 256) {
        char buf[256];
        memcpy(buf, s, n);
        buf[n] = 0;
        set_add(idents, buf);
      }
    } else {
      p++;
    }
  }
}

static void build_api_name_sets(const SymVec *syms, StrSet *all_names,
                                StrSet *type_names, StrSet *fn_names) {
  set_init(all_names, 2048);
  set_init(type_names, 2048);
  set_init(fn_names, 2048);

  for (size_t i = 0; i < syms->len; i++) {
    const Symbol *s = &syms->data[i];
    set_add(all_names, s->name);
    if (s->kind == SYM_FN_PROTO || s->kind == SYM_FN_DEF)
      set_add(fn_names, s->name);
    if (s->kind == SYM_STRUCT || s->kind == SYM_TYPEDEF_STRUCT)
      set_add(type_names, s->name);
  }
}

static const Symbol *find_symbol(const SymVec *syms, const char *name) {
  for (size_t i = 0; i < syms->len; i++) {
    if (strcmp(syms->data[i].name, name) == 0)
      return &syms->data[i];
  }
  return NULL;
}

static void add_deps_closure(const SymVec *syms, const StrSet *type_names,
                             StrSet *selected) {
  // Fixed-point: if selected symbol's snippet mentions other API type names,
  // select them too. This covers Player -> Vec2, and fn signatures -> types.
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < syms->len; i++) {
      const Symbol *s = &syms->data[i];
      if (!set_has(selected, s->name))
        continue;

      // Collect identifiers from signature/snippet (cheap)
      StrSet ids;
      set_init(&ids, 1024);
      if (s->sigline)
        collect_idents_from_text(s->sigline, &ids);
      collect_idents_from_text(s->snippet, &ids);

      // For every type name identifier present, add it
      for (size_t b = 0; b < ids.cap; b++) {
        if (!ids.keys[b])
          continue;
        const char *id = ids.keys[b];
        if (set_has(type_names, id) && !set_has(selected, id)) {
          set_add(selected, id);
          changed = true;
        }
      }
      set_free(&ids);
    }
  }
}

static Visibility vis_from_arg(const char *s) {
  if (!s || strcmp(s, "public") == 0)
    return VIS_PUBLIC;
  if (strcmp(s, "private") == 0)
    return VIS_PRIVATE; // means "private view" (include priv too)
  return VIS_PUBLIC;
}

static void emit_auto_import(const char *out_path, const SymVec *syms,
                             const char *entry_text,
                             const char *vis_mode /* "public"|"private" */) {

  // Build name sets
  StrSet all_names, type_names, fn_names;
  build_api_name_sets(syms, &all_names, &type_names, &fn_names);

  // Collect identifiers used in entry_text
  StrSet used;
  set_init(&used, 4096);
  collect_idents_from_text(entry_text, &used);

  // Selected imports: intersection(used, api_names), respecting vis_mode
  StrSet selected;
  set_init(&selected, 4096);

  bool include_private = (vis_mode && strcmp(vis_mode, "private") == 0);

  for (size_t i = 0; i < syms->len; i++) {
    const Symbol *sym = &syms->data[i];

    // Visibility filter:
    // - if public mode: only allow PUBLIC symbols
    // - if private mode: allow both
    if (!include_private && sym->vis != VIS_PUBLIC)
      continue;

    if (set_has(&used, sym->name)) {
      set_add(&selected, sym->name);
    }
  }

  // Dependency closure (types referenced by selected symbols)
  add_deps_closure(syms, &type_names, &selected);

  ensure_parent_dir(out_path);
  FILE *f = fopen(out_path, "wb");
  if (!f)
    die("failed to open auto_import output");

  fputs("#pragma once\n", f);
  fputs("#define API_SELECTIVE 1\n", f);
  if (include_private)
    fputs("#define API_VIS_PRIVATE_TOO 1\n", f);
  else
    fputs("#define API_VIS_PRIVATE_TOO 0\n", f);
  fputc('\n', f);

  // Emit IMPORT_ macros
  for (size_t i = 0; i < syms->len; i++) {
    const Symbol *sym = &syms->data[i];
    if (!set_has(&selected, sym->name))
      continue;

    // enforce visibility (again)
    if (!include_private && sym->vis != VIS_PUBLIC)
      continue;

    fprintf(f, "#define IMPORT_%s 1\n", sym->name);
  }

  fputc('\n', f);
  fputs("#include \"framework/api.h\"\n", f);
  fclose(f);

  set_free(&selected);
  set_free(&used);
  set_free(&all_names);
  set_free(&type_names);
  set_free(&fn_names);
}

/* =======================
   Preprocess helper
   ======================= */

static char *read_cmd_output(const char *cmd) {
  FILE *p = popen(cmd, "r");
  if (!p)
    return NULL;

  size_t cap = 1 << 20; // 1MB start
  size_t len = 0;
  char *buf = (char *)xmalloc(cap);

  int c;
  while ((c = fgetc(p)) != EOF) {
    if (len + 1 >= cap) {
      cap *= 2;
      buf = (char *)realloc(buf, cap);
      if (!buf)
        die("out of memory");
    }
    buf[len++] = (char)c;
  }
  buf[len] = 0;
  pclose(p);
  return buf;
}

/* =======================
   Main
   ======================= */

static void usage(void) {
  puts("  gen    --root <dir> --out <api.def> --index <api_index.json> "
       "[--fn_prefix <prefix>] [--backend <sdl|raylib|core>] "
       "[--exclude_backend <name>] [--exclude_path <substr>]\n"
       "  search --root <dir> [--kind ...] [--name <exact>] [--pattern "
       "<substr>] [--backend <sdl|raylib|core>] [--exclude_backend <name>] "
       "[--exclude_path <substr>]\n"
       "  needs  --root <dir> --entry <file.c> --out <auto_import.h> --vis "
       "public|private [--preprocess <cmd>] [--backend <sdl|raylib|core>] "
       "[--exclude_backend <name>] [--exclude_path <substr>]\n");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 1;
  }
  const char *cmd = argv[1];

  const char *root = ".";
  const char *out_def = "framework/api.def";
  const char *out_index = "framework/api_index.json";
  const char *fn_prefix = NULL;

  const char *s_kind = NULL;
  const char *s_name = NULL;
  const char *s_pattern = NULL;

  const char *entry_path = NULL;
  const char *auto_out = "framework/auto_import.h";
  const char *vis_mode = "public";
  const char *pre_cmd = NULL;

  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--root") == 0 && i + 1 < argc)
      root = argv[++i];
    else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
      out_def = argv[++i];
    else if (strcmp(argv[i], "--index") == 0 && i + 1 < argc)
      out_index = argv[++i];
    else if (strcmp(argv[i], "--fn_prefix") == 0 && i + 1 < argc)
      fn_prefix = argv[++i];
    else if (strcmp(argv[i], "--kind") == 0 && i + 1 < argc)
      s_kind = argv[++i];
    else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc)
      s_name = argv[++i];
    else if (strcmp(argv[i], "--pattern") == 0 && i + 1 < argc)
      s_pattern = argv[++i];
    else if (strcmp(argv[i], "--entry") == 0 && i + 1 < argc)
      entry_path = argv[++i];
    else if (strcmp(argv[i], "--auto_out") == 0 && i + 1 < argc)
      auto_out = argv[++i];
    else if (strcmp(argv[i], "--vis") == 0 && i + 1 < argc)
      vis_mode = argv[++i];
    else if (strcmp(argv[i], "--preprocess") == 0 && i + 1 < argc)
      pre_cmd = argv[++i];
  }

  // Compile regexes
  const char *FN_RE =
      "^[[:space:]]*[A-Za-z_][A-Za-z0-9_[:space:]*]*[[:space:]]+"
      "([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\\([^;{}]*\\)[[:space:]]*([;{])[[:"
      "space:]]*$";

  const char *TYPEDEF_STRUCT_RE =
      "^[[:space:]]*typedef[[:space:]]+struct([[:space:]]+[A-Za-z_][A-Za-z0-9_]"
      "*)?[[:space:]]*\\{";

  const char *STRUCT_RE =
      "^[[:space:]]*struct[[:space:]]+([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\\{";

  regex_t re_fn, re_ts, re_s;
  if (regcomp(&re_fn, FN_RE, REG_EXTENDED | REG_NEWLINE) != 0)
    die("regcomp fn failed");
  if (regcomp(&re_ts, TYPEDEF_STRUCT_RE, REG_EXTENDED | REG_NEWLINE) != 0)
    die("regcomp typedef struct failed");
  if (regcomp(&re_s, STRUCT_RE, REG_EXTENDED | REG_NEWLINE) != 0)
    die("regcomp struct failed");

  SymVec syms = {0};
  walk_dir(root, root, &syms, &re_fn, &re_ts, &re_s);

  regfree(&re_fn);
  regfree(&re_ts);
  regfree(&re_s);

  if (strcmp(cmd, "gen") == 0) {
    ensure_parent_dir(out_index);
    ensure_parent_dir(out_def);
    write_index_json(out_index, &syms);
    emit_api_def(out_def, &syms, fn_prefix);
    printf("Wrote %s\nWrote %s\n", out_def, out_index);
    free_syms(&syms);
    return 0;
  }

  if (strcmp(cmd, "search") == 0) {
    do_search(&syms, s_kind, s_name, s_pattern);
    free_syms(&syms);
    return 0;
  }

  if (strcmp(cmd, "needs") == 0) {
    if (!entry_path && !pre_cmd)
      die("needs: provide --entry <file> and/or --preprocess <cmd>");
    char *entry_text = NULL;

    if (pre_cmd && *pre_cmd) {
      entry_text = read_cmd_output(pre_cmd);
      if (!entry_text)
        die("failed to run preprocess command");
    } else {
      size_t n = 0;
      entry_text = read_entire_file(entry_path, &n);
      if (!entry_text)
        die("failed to read entry file");
    }

    emit_auto_import(auto_out, &syms, entry_text, vis_mode);
    printf("Wrote %s\n", auto_out);

    free(entry_text);
    free_syms(&syms);
    return 0;
  }

  usage();
  free_syms(&syms);
  return 1;
}
