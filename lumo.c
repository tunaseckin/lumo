/* Lumo: small, readable, no surprises.
 *
 * Rules: declare with `let`, everything is an expression, `if`/`while`/`and`/`or`
 * need real bools, no implicit conversions, `x |> f(y)` means `f(x, y)`.
 *
 * Single-file bytecode VM with a mark-sweep GC.
 * Build: cc -O2 -o lumo lumo.c -lm
 * Usage: ./lumo [file.lu] [args...]   (no file = REPL)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* strdup, mkdtemp, popen, M_PI, ... on Linux */
#endif
#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <direct.h>
#include <io.h>
#define WIFEXITED(s) 1 /* pclose returns the exit code itself */
#define WEXITSTATUS(s) (s)
#define localtime_r(t, tm) localtime_s(tm, t)
#ifndef popen
#define popen _popen
#define pclose _pclose
#endif
#define CC "gcc"
static char *win_mkdtemp(char *tpl) { return _mktemp(tpl) && _mkdir(tpl) == 0 ? tpl : NULL; }
#define mkdtemp win_mkdtemp /* not in every MinGW */
#else
#include <sys/wait.h>
#include <unistd.h>
#define CC "cc"
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#define LUMO_VERSION "0.2.1"

#ifdef LUMO_HAVE_SRC /* `make` embeds lumo's own source, so `lumo build --fast` can recompile it */
#include "lumo_src.h"
#endif

/* ---------- values & objects ---------- */

typedef enum { V_NIL, V_BOOL, V_NUM, V_OBJ } VType;
typedef struct Obj Obj;
typedef struct { VType t; union { bool b; double n; Obj *o; } as; } Value;

#define NIL_V ((Value){V_NIL, {.n = 0}})
#define BOOL_V(x) ((Value){V_BOOL, {.b = (x)}})
#define NUM_V(x) ((Value){V_NUM, {.n = (x)}})
#define OBJ_V(x) ((Value){V_OBJ, {.o = (Obj *)(x)}})
#define IS_OT(v, ot) ((v).t == V_OBJ && (v).as.o->type == (ot))
#define AS_STR(v) ((Str *)(v).as.o)
#define AS_LIST(v) ((List *)(v).as.o)
#define AS_MAP(v) ((Map *)(v).as.o)

typedef enum { O_STR, O_LIST, O_MAP, O_FN, O_CLOSURE, O_UPVAL, O_NATIVE } OType;
struct Obj { OType type; bool marked; Obj *next; };
typedef struct { Obj obj; int len; uint32_t hash; bool ascii; char chars[]; } Str;
typedef struct { Obj obj; int len, cap; Value *items; } List;
typedef struct { Value key, val; } Entry; /* key nil = deleted */
/* insertion-ordered: entries[] in order, index[] is an open-addressing table into it */
struct Map { Obj obj; int count, used, icap; Entry *entries; int *index; Str *sname; /* struct name, or NULL */ };
typedef struct { uint8_t *code; int *lines; int len, cap; Value *consts; int nconsts, cconsts; } Chunk;
typedef struct Map Map;
typedef struct { Obj obj; int arity, nupvals; Chunk chunk; Str *name, *file; Map *globals; /* its module's */ } Fn;
typedef struct Upval { Obj obj; Value *loc; Value closed; struct Upval *next; } Upval;
typedef struct { Obj obj; Fn *fn; int nup; Upval *up[]; } Closure;
typedef bool (*NativeFn)(int argc, Value *args, Value *out);
typedef struct { Obj obj; NativeFn fn; const char *name, *usage; int min, max; bool plain; /* errors without "name: " */ } Native;

#define FRAMES_MAX 1024
#define STACK_MAX (FRAMES_MAX * 256)
typedef struct { Closure *cl; uint8_t *ip; Value *slots; } Frame;

static struct {
  Frame frames[FRAMES_MAX];
  int nframes;
  Value stack[STACK_MAX];
  Value *sp;
  Map *globals, *builtins, *modules;
  Upval *open;
  Obj *objects;
  size_t bytes, next_gc;
  Obj **gray;
  int ngray, cgray;
  struct { int nframes; Value *sp; uint8_t *ip; } handlers[256]; /* open try blocks */
  int nhandlers;
  Value err, thrown; /* thrown: value passed to error(), picked up by call_value */
  bool has_thrown;
  int err_line;
  Str *err_file;
  Str **strs; /* intern set */
  int nstrs, cstrs;
} vm;

static void *mem(void *p, size_t old, size_t nw) {
  vm.bytes += nw - old;
  if (nw == 0) { free(p); return NULL; }
  void *r = realloc(p, nw);
  if (!r) { fputs("out of memory\n", stderr); exit(1); }
  return r;
}

static Obj *new_obj(size_t size, OType type) {
  Obj *o = mem(NULL, 0, size);
  o->type = type;
  o->marked = false;
  o->next = vm.objects;
  vm.objects = o;
  return o;
}

/* ---------- strings ---------- */

#define TOMB ((Str *)1)

static uint32_t hash_bytes(const char *s, int n) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
  return h;
}

static void grow_strs(void) {
  int live = 0;
  for (int i = 0; i < vm.cstrs; i++)
    if (vm.strs[i] && vm.strs[i] != TOMB) live++;
  int nc = 256;
  while (nc < (live + 1) * 2) nc *= 2;
  Str **ns = calloc(nc, sizeof(Str *));
  for (int i = 0; i < vm.cstrs; i++) {
    Str *s = vm.strs[i];
    if (!s || s == TOMB) continue;
    uint32_t j = s->hash & (nc - 1);
    while (ns[j]) j = (j + 1) & (nc - 1);
    ns[j] = s;
  }
  free(vm.strs);
  vm.strs = ns;
  vm.cstrs = nc;
  vm.nstrs = live;
}

/* every string is interned, so string equality is pointer equality */
static Str *intern(const char *s, int n) {
  uint32_t h = hash_bytes(s, n);
  if (vm.cstrs) {
    for (uint32_t i = h & (vm.cstrs - 1);; i = (i + 1) & (vm.cstrs - 1)) {
      Str *e = vm.strs[i];
      if (!e) break;
      if (e != TOMB && e->hash == h && e->len == n && memcmp(e->chars, s, n) == 0) return e;
    }
  }
  if ((vm.nstrs + 1) * 4 > vm.cstrs * 3) grow_strs();
  Str *str = (Str *)new_obj(sizeof(Str) + n + 1, O_STR);
  str->len = n;
  str->hash = h;
  memcpy(str->chars, s, n);
  str->chars[n] = 0;
  str->ascii = true;
  for (int i = 0; i < n; i++)
    if ((uint8_t)s[i] >= 0x80) { str->ascii = false; break; }
  uint32_t i = h & (vm.cstrs - 1);
  while (vm.strs[i] && vm.strs[i] != TOMB) i = (i + 1) & (vm.cstrs - 1);
  if (!vm.strs[i]) vm.nstrs++;
  vm.strs[i] = str;
  return str;
}

static Str *cstr(const char *s) { return intern(s, (int)strlen(s)); }

static int u8len(char c) {
  uint8_t u = (uint8_t)c;
  return u < 0x80 ? 1 : u >= 0xF0 ? 4 : u >= 0xE0 ? 3 : u >= 0xC0 ? 2 : 1;
}

/* strings are UTF-8; length and indexing count characters, not bytes */
static int str_cplen(Str *s) {
  if (s->ascii) return s->len;
  int n = 0;
  for (int i = 0; i < s->len; i += u8len(s->chars[i])) n++;
  return n;
}

static int str_offset(Str *s, int cp) {
  if (s->ascii) return cp;
  int i = 0;
  while (cp-- > 0 && i < s->len) i += u8len(s->chars[i]);
  return i > s->len ? s->len : i;
}

static int str_cmp(Str *a, Str *b) {
  int n = a->len < b->len ? a->len : b->len;
  int c = memcmp(a->chars, b->chars, n);
  return c ? c : a->len - b->len;
}

/* ---------- lists & maps ---------- */

static List *new_list(void) {
  List *l = (List *)new_obj(sizeof(List), O_LIST);
  l->len = l->cap = 0;
  l->items = NULL;
  return l;
}

static void list_push(List *l, Value v) {
  if (l->len == l->cap) {
    int nc = l->cap ? l->cap * 2 : 8;
    l->items = mem(l->items, sizeof(Value) * l->cap, sizeof(Value) * nc);
    l->cap = nc;
  }
  l->items[l->len++] = v;
}

static Map *new_map(void) {
  Map *m = (Map *)new_obj(sizeof(Map), O_MAP);
  m->count = m->used = m->icap = 0;
  m->entries = NULL;
  m->sname = NULL;
  m->index = NULL;
  return m;
}

static bool valid_key(Value k) { return k.t == V_BOOL || k.t == V_NUM || IS_OT(k, O_STR); }

static uint32_t hash_val(Value k) {
  if (k.t == V_BOOL) return k.as.b ? 1231 : 1237;
  if (k.t == V_NUM) {
    double d = k.as.n == 0 ? 0 : k.as.n;
    uint64_t u;
    memcpy(&u, &d, 8);
    u ^= u >> 33;
    u *= 0xff51afd7ed558ccdULL;
    u ^= u >> 33;
    return (uint32_t)u;
  }
  return AS_STR(k)->hash;
}

static bool key_eq(Value a, Value b) {
  if (a.t != b.t) return false;
  if (a.t == V_NUM) return a.as.n == b.as.n;
  if (a.t == V_BOOL) return a.as.b == b.as.b;
  return a.t == V_OBJ && a.as.o == b.as.o;
}

static int map_find(Map *m, Value k) {
  if (!m->icap) return -1;
  uint32_t mask = m->icap - 1;
  for (uint32_t i = hash_val(k) & mask;; i = (i + 1) & mask) {
    int e = m->index[i];
    if (e == -1) return -1;
    if (key_eq(m->entries[e].key, k)) return e;
  }
}

static void map_rebuild(Map *m) {
  int live = 0;
  for (int i = 0; i < m->used; i++)
    if (m->entries[i].key.t != V_NIL) m->entries[live++] = m->entries[i];
  int cap = 8;
  while (cap < (live + 1) * 2) cap *= 2;
  m->entries = mem(m->entries, sizeof(Entry) * m->icap, sizeof(Entry) * cap);
  m->index = mem(m->index, sizeof(int) * m->icap, sizeof(int) * cap);
  m->icap = cap;
  m->used = live;
  for (int i = 0; i < cap; i++) m->index[i] = -1;
  for (int e = 0; e < live; e++) {
    uint32_t i = hash_val(m->entries[e].key) & (cap - 1);
    while (m->index[i] != -1) i = (i + 1) & (cap - 1);
    m->index[i] = e;
  }
}

static void map_set(Map *m, Value k, Value v) {
  int e = map_find(m, k);
  if (e >= 0) { m->entries[e].val = v; return; }
  if ((m->used + 1) * 4 > m->icap * 3) map_rebuild(m);
  e = m->used++;
  m->entries[e] = (Entry){k, v};
  m->count++;
  uint32_t i = hash_val(k) & (m->icap - 1);
  while (m->index[i] != -1) i = (i + 1) & (m->icap - 1);
  m->index[i] = e;
}

static bool map_remove(Map *m, Value k) {
  int e = map_find(m, k);
  if (e < 0) return false;
  m->entries[e].key = NIL_V; /* index slot stays as a tombstone */
  m->entries[e].val = NIL_V;
  m->count--;
  return true;
}

static List *map_keys(Map *m) {
  List *l = new_list();
  for (int i = 0; i < m->used; i++)
    if (m->entries[i].key.t != V_NIL) list_push(l, m->entries[i].key);
  return l;
}

/* ---------- functions ---------- */

static Map *cur_globals; /* globals of the module being compiled */
static Str *cur_file;    /* path of the file being compiled */
static Map *compiled_fns;         /* functions compiled to C by `lumo build --fast` */
static const char *compiled_file; /* ...which all come from this file */

static Fn *new_fn(Str *name) {
  Fn *f = (Fn *)new_obj(sizeof(Fn), O_FN);
  f->arity = f->nupvals = 0;
  memset(&f->chunk, 0, sizeof(Chunk));
  f->name = name;
  f->globals = cur_globals;
  f->file = cur_file;
  return f;
}

static Closure *new_closure(Fn *fn) {
  Closure *c = (Closure *)new_obj(sizeof(Closure) + sizeof(Upval *) * fn->nupvals, O_CLOSURE);
  c->fn = fn;
  c->nup = fn->nupvals;
  for (int i = 0; i < c->nup; i++) c->up[i] = NULL;
  return c;
}

/* ---------- printing & equality ---------- */

typedef struct { char *s; int len, cap; } Buf;

static void buf_add(Buf *b, const char *s, int n) {
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->len + n + 1) * 2;
    b->s = realloc(b->s, b->cap);
  }
  memcpy(b->s + b->len, s, n);
  b->len += n;
  b->s[b->len] = 0;
}

static int fmt_num(char *out, double n) {
  if (n == 0) n = 0; /* no "-0" */
  if (n == floor(n) && fabs(n) < 1e16) return snprintf(out, 64, "%.0f", n);
  return snprintf(out, 64, "%.14g", n);
}

static const char *type_name(Value v) {
  switch (v.t) {
    case V_NIL: return "nil";
    case V_BOOL: return "bool";
    case V_NUM: return "num";
    case V_OBJ: break;
  }
  switch (v.as.o->type) {
    case O_STR: return "str";
    case O_LIST: return "list";
    case O_MAP: return AS_MAP(v)->sname ? AS_MAP(v)->sname->chars : "map";
    default: return "fn";
  }
}

static void show(Buf *b, Value v, bool nested, int depth) {
  char tmp[64];
  switch (v.t) {
    case V_NIL: buf_add(b, "nil", 3); return;
    case V_BOOL: v.as.b ? buf_add(b, "true", 4) : buf_add(b, "false", 5); return;
    case V_NUM: buf_add(b, tmp, fmt_num(tmp, v.as.n)); return;
    case V_OBJ: break;
  }
  if (depth > 50) { buf_add(b, "...", 3); return; }
  Obj *o = v.as.o;
  switch (o->type) {
    case O_STR: {
      Str *s = (Str *)o;
      if (nested) buf_add(b, "\"", 1);
      buf_add(b, s->chars, s->len);
      if (nested) buf_add(b, "\"", 1);
      return;
    }
    case O_LIST: {
      List *l = (List *)o;
      buf_add(b, "[", 1);
      for (int i = 0; i < l->len; i++) {
        if (i) buf_add(b, ", ", 2);
        show(b, l->items[i], true, depth + 1);
      }
      buf_add(b, "]", 1);
      return;
    }
    case O_MAP: { /* {"a": 1}, or Point(x: 1, y: 2) for a struct */
      Map *m = (Map *)o;
      bool first = true;
      if (m->sname) buf_add(b, m->sname->chars, m->sname->len);
      buf_add(b, m->sname ? "(" : "{", 1);
      for (int i = 0; i < m->used; i++) {
        if (m->entries[i].key.t == V_NIL) continue;
        if (!first) buf_add(b, ", ", 2);
        first = false;
        show(b, m->entries[i].key, !m->sname, depth + 1);
        buf_add(b, ": ", 2);
        show(b, m->entries[i].val, true, depth + 1);
      }
      buf_add(b, m->sname ? ")" : "}", 1);
      return;
    }
    case O_CLOSURE: {
      int n = snprintf(tmp, sizeof tmp, "<fn %s>", ((Closure *)o)->fn->name->chars);
      buf_add(b, tmp, n < (int)sizeof tmp ? n : (int)sizeof tmp - 1);
      return;
    }
    case O_NATIVE: {
      int n = snprintf(tmp, sizeof tmp, "<builtin %s>", ((Native *)o)->name);
      buf_add(b, tmp, n < (int)sizeof tmp ? n : (int)sizeof tmp - 1);
      return;
    }
    default: buf_add(b, "<?>", 3); return;
  }
}

static bool values_equal(Value a, Value b) {
  if (a.t != b.t) return false;
  switch (a.t) {
    case V_NIL: return true;
    case V_BOOL: return a.as.b == b.as.b;
    case V_NUM: return a.as.n == b.as.n;
    case V_OBJ: break;
  }
  if (a.as.o == b.as.o) return true;
  if (a.as.o->type != b.as.o->type) return false;
  if (IS_OT(a, O_LIST)) {
    List *x = AS_LIST(a), *y = AS_LIST(b);
    if (x->len != y->len) return false;
    for (int i = 0; i < x->len; i++)
      if (!values_equal(x->items[i], y->items[i])) return false;
    return true;
  }
  if (IS_OT(a, O_MAP)) {
    Map *x = AS_MAP(a), *y = AS_MAP(b);
    if (x->count != y->count || x->sname != y->sname) return false;
    for (int i = 0; i < x->used; i++) {
      if (x->entries[i].key.t == V_NIL) continue;
      int e = map_find(y, x->entries[i].key);
      if (e < 0 || !values_equal(x->entries[i].val, y->entries[e].val)) return false;
    }
    return true;
  }
  return false; /* strings are interned; functions compare by identity */
}

/* ---------- error helpers ---------- */

/* source of every compiled file, so errors can show the offending line */
static struct { char *path, *src; } sources[256];
static int nsources;

static void remember_source(const char *path, const char *src) {
  for (int i = 0; i < nsources; i++)
    if (!strcmp(sources[i].path, path)) { free(sources[i].src); sources[i].src = strdup(src); return; }
  if (nsources == 256) return;
  sources[nsources].path = strdup(path);
  sources[nsources++].src = strdup(src);
}

static void show_source_line(const char *path, int line) {
  for (int i = 0; i < nsources; i++) {
    if (strcmp(sources[i].path, path)) continue;
    const char *p = sources[i].src;
    for (int l = 1; l < line && p; l++) p = (p = strchr(p, '\n')) ? p + 1 : NULL;
    if (!p || !*p) return;
    while (*p == ' ' || *p == '\t') p++;
    int n = (int)strcspn(p, "\n");
    if (n) fprintf(stderr, "  %4d | %.*s\n", line, n > 120 ? 120 : n, p);
    return;
  }
}

/* edit distance where swapping two neighbouring letters counts as one edit (nmae -> name) */
static int edit_distance(const char *a, int la, const char *b, int lb) {
  if (la > 40 || lb > 40) return 99;
  int d[41][41];
  for (int i = 0; i <= la; i++) d[i][0] = i;
  for (int j = 0; j <= lb; j++) d[0][j] = j;
  for (int i = 1; i <= la; i++)
    for (int j = 1; j <= lb; j++) {
      int best = d[i - 1][j - 1] + (a[i - 1] != b[j - 1]);
      if (d[i - 1][j] + 1 < best) best = d[i - 1][j] + 1;
      if (d[i][j - 1] + 1 < best) best = d[i][j - 1] + 1;
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] && d[i - 2][j - 2] + 1 < best)
        best = d[i - 2][j - 2] + 1;
      d[i][j] = best;
    }
  return d[la][lb];
}

/* collects the closest candidate name for a "did you mean" hint */
typedef struct { const char *name; int len, best_d; char best[64]; } Suggest;
static void suggest_init(Suggest *s, const char *name, int len) { s->name = name; s->len = len; s->best_d = 99; s->best[0] = 0; }
/* lowercase ASCII copy where ç ğ ı ö ş ü (and capitals) become c g i o s u, so `yildiz` finds `yıldız` */
static int fold_name(const char *s, int n, char *out, int cap) {
  int k = 0;
  for (int i = 0; i < n && k < cap - 1; i++) {
    unsigned char c = (unsigned char)s[i], d = i + 1 < n ? (unsigned char)s[i + 1] : 0;
    char r = 0;
    if (c == 0xC3) r = d == 0xA7 || d == 0x87 ? 'c' : d == 0xB6 || d == 0x96 ? 'o' : d == 0xBC || d == 0x9C ? 'u' : 0;
    else if (c == 0xC4) r = d == 0x9F || d == 0x9E ? 'g' : d == 0xB1 || d == 0xB0 ? 'i' : 0;
    else if (c == 0xC5) r = d == 0x9F || d == 0x9E ? 's' : 0;
    if (r) { out[k++] = r; i++; }
    else out[k++] = (char)tolower(c);
  }
  out[k] = 0;
  return k;
}

static void suggest_try(Suggest *s, const char *cand, int clen) {
  if (s->len < 3 || (clen == s->len && !memcmp(cand, s->name, clen))) return; /* short names: too noisy */
  char a[64], b[64];
  int la = fold_name(s->name, s->len, a, sizeof a), lb = fold_name(cand, clen, b, sizeof b);
  int limit = la <= 4 ? 1 : 2;
  int d = edit_distance(a, la, b, lb);
  if (d <= limit && d < s->best_d && clen < 64) {
    s->best_d = d;
    memcpy(s->best, cand, clen);
    s->best[clen] = 0;
  }
}
static const char *suggest_text(Suggest *s) { /* " (did you mean 'x'?)" or "" */
  static char out[96];
  if (!s->best[0]) return "";
  snprintf(out, sizeof out, " (did you mean '%s'?)", s->best);
  return out;
}

/* methods people expect on lists and strings, and Lumo's function for each */
static const char *method_hint(const char *name, int len) {
  static const char *hints[][2] = {
      {"append", "push(xs, x)"}, {"push", "push(xs, x)"}, {"add", "push(xs, x)"}, {"pop", "pop(xs)"},
      {"insert", "insert(xs, i, x)"}, {"length", "len(x)"}, {"len", "len(x)"}, {"size", "len(x)"},
      {"count", "len(x)"}, {"upper", "upper(s)"}, {"lower", "lower(s)"}, {"toUpperCase", "upper(s)"},
      {"toLowerCase", "lower(s)"}, {"strip", "trim(s)"}, {"trim", "trim(s)"}, {"split", "split(s, sep)"},
      {"join", "join(xs, sep)"}, {"replace", "replace(s, old, new)"}, {"startswith", "starts_with(s, x)"},
      {"endswith", "ends_with(s, x)"}, {"find", "find(s, x)"}, {"index", "index_of(xs, x)"},
      {"indexOf", "index_of(xs, x)"}, {"contains", "contains(x, item)"}, {"sort", "sort(xs)"},
      {"reverse", "reverse(xs)"}, {"keys", "keys(m)"}, {"slice", "slice(x, a, b)"}, {"map", "map(xs, f)"},
      {"filter", "filter(xs, f)"}, {"remove", "remove(m, key)"}, {"get", "get(m, key, default)"},
      {"format", "\"...${x}...\""},
  };
  for (int i = 0; i < (int)(sizeof hints / sizeof *hints); i++)
    if ((int)strlen(hints[i][0]) == len && !memcmp(hints[i][0], name, len)) return hints[i][1];
  return NULL;
}

/* names people bring from other languages */
static const char *foreign_hint(const char *name, int len) {
  static const char *hints[][2] = {
      {"null", "use nil"}, {"None", "use nil"}, {"undefined", "use nil"}, {"True", "use true"},
      {"False", "use false"}, {"elif", "use 'else if'"}, {"def", "functions start with fn"},
      {"function", "functions start with fn"}, {"func", "functions start with fn"},
      {"var", "declare variables with let"}, {"const", "declare variables with let"},
      {"length", "use len(x)"}, {"size", "use len(x)"}, {"append", "use push(list, x)"},
      {"println", "use print(...)"}, {"printf", "use print(\"...${x}...\")"}, {"echo", "use print(...)"},
      {"puts", "use print(...)"}, {"console", "use print(...)"}, {"int", "use num(x)"}, {"float", "use num(x)"},
      {"parseInt", "use num(x)"}, {"toString", "use str(x)"}, {"string", "use str(x)"},
      {"self", "Lumo has no methods; pass the value as a parameter"}, {"this", "Lumo has no methods; pass the value as a parameter"},
      {"switch", "use if / else if"}, {"foreach", "use: for x in xs { ... }"}, {"elseif", "use 'else if'"},
      {"pass", "an empty block {} needs nothing inside"}, {"lambda", "write fn(x) { ... }"}, {"is", "use == to compare"},
      {"except", "use catch"}, {"class", "Lumo has no classes: use struct Name { field: type } and functions"}, {"nil?", "use == nil"},
  };
  static char out[96];
  for (int i = 0; i < (int)(sizeof hints / sizeof *hints); i++)
    if ((int)strlen(hints[i][0]) == len && !memcmp(hints[i][0], name, len)) {
      snprintf(out, sizeof out, " (%s)", hints[i][1]);
      return out;
    }
  return NULL;
}

/* ---------- indexing ---------- */

static char nerr_buf[512];
static bool nerr(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(nerr_buf, sizeof nerr_buf, fmt, ap);
  va_end(ap);
  return false;
}

static bool whole(double d) { return d == floor(d) && fabs(d) < 1e15; }

static bool norm_index(Value k, int len, int *out) {
  if (k.t != V_NUM || !whole(k.as.n))
    return nerr("index must be a whole number, got %s", k.t == V_NUM ? "a fraction" : type_name(k));
  double d = k.as.n;
  if (d < -len || d >= len) {
    char t[64];
    fmt_num(t, d);
    if (d == len && len > 0) /* 0..len(xs) includes len */
      return nerr("index %s out of range (length %d; the last item is [%d], and a..b includes b)", t, len, len - 1);
    return nerr("index %s out of range (length %d)", t, len);
  }
  *out = (int)(d < 0 ? d + len : d);
  return true;
}

static bool get_index(Value o, Value k, Value *out) {
  if (IS_OT(o, O_MAP)) {
    if (!valid_key(k)) return nerr("map keys must be str, num or bool, got %s", type_name(k));
    int e = map_find(AS_MAP(o), k);
    if (e < 0) {
      Buf b = {0};
      show(&b, k, !AS_MAP(o)->sname, 0);
      Suggest s;
      suggest_init(&s, IS_OT(k, O_STR) ? AS_STR(k)->chars : "", IS_OT(k, O_STR) ? AS_STR(k)->len : 0);
      for (int i = 0; i < AS_MAP(o)->used; i++)
        if (IS_OT(AS_MAP(o)->entries[i].key, O_STR))
          suggest_try(&s, AS_STR(AS_MAP(o)->entries[i].key)->chars, AS_STR(AS_MAP(o)->entries[i].key)->len);
      if (AS_MAP(o)->sname) nerr("%s has no %s '%s'%s", AS_MAP(o)->sname->chars,
                                 strcmp(AS_MAP(o)->sname->chars, "gfx") ? "field" : "function", b.s, suggest_text(&s));
      else if (s.best[0]) nerr("key %s not found%s", b.s, suggest_text(&s));
      else nerr("key %s not found (get(map, key, default) gives a default instead of an error)", b.s);
      free(b.s);
      return false;
    }
    *out = AS_MAP(o)->entries[e].val;
    return true;
  }
  int i;
  if ((IS_OT(o, O_LIST) || IS_OT(o, O_STR)) && IS_OT(k, O_STR)) { /* xs.append(x), s.upper() */
    const char *m = method_hint(AS_STR(k)->chars, AS_STR(k)->len);
    if (m) return nerr("%s has no .%s: Lumo uses functions, write %s", type_name(o), AS_STR(k)->chars, m);
    return nerr("%s items are numbered, not named: use [0], [1], ...", type_name(o));
  }
  if (IS_OT(o, O_CLOSURE) || IS_OT(o, O_NATIVE)) return nerr("can't index a function (did you forget () to call it?)");
  if (IS_OT(o, O_LIST)) {
    if (!norm_index(k, AS_LIST(o)->len, &i)) return false;
    *out = AS_LIST(o)->items[i];
    return true;
  }
  if (IS_OT(o, O_STR)) {
    Str *s = AS_STR(o);
    if (!norm_index(k, str_cplen(s), &i)) return false;
    int off = str_offset(s, i), n = u8len(s->chars[off]);
    if (off + n > s->len) n = s->len - off;
    *out = OBJ_V(intern(s->chars + off, n));
    return true;
  }
  return nerr("can't index %s", type_name(o));
}

static bool set_index(Value o, Value k, Value v) {
  if (IS_OT(o, O_MAP)) {
    if (!valid_key(k)) return nerr("map keys must be str, num or bool, got %s", type_name(k));
    if (AS_MAP(o)->sname && map_find(AS_MAP(o), k) < 0) { /* structs have fixed fields */
      Buf b = {0};
      show(&b, k, false, 0);
      nerr("%s has no field '%s'", AS_MAP(o)->sname->chars, b.s);
      free(b.s);
      return false;
    }
    map_set(AS_MAP(o), k, v);
    return true;
  }
  if (IS_OT(o, O_LIST)) {
    int i;
    if (!norm_index(k, AS_LIST(o)->len, &i)) return false;
    AS_LIST(o)->items[i] = v;
    return true;
  }
  if (IS_OT(o, O_STR)) return nerr("strings can't be changed");
  return nerr("can't assign into %s", type_name(o));
}

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  Buf b = {0};
  char chunk[8192];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_add(&b, chunk, (int)n);
  fclose(f);
  if (!b.s) b.s = calloc(1, 1);
  return b.s;
}

static const char *path_base(const char *p) { /* what follows the last / (or \ on Windows) */
  const char *b = strrchr(p, '/');
#ifdef _WIN32
  const char *b2 = strrchr(p, '\\');
  if (!b || (b2 && b2 > b)) b = b2;
#endif
  return b ? b + 1 : p;
}

static const char *tmp_dir(void) {
  const char *t = getenv("TMPDIR");
#ifdef _WIN32
  if (!t) t = getenv("TEMP");
  return t ? t : ".";
#else
  return t ? t : "/tmp";
#endif
}

/* an import path is relative to the importing file */
static Str *resolve_import(const char *from, Str *path) {
  const char *base = path_base(from);
  bool absolute = path->chars[0] == '/' || path->chars[0] == '\\' || (path->len > 1 && path->chars[1] == ':');
  if (absolute || base == from) return path;
  int dl = (int)(base - from);
  char *full = malloc(dl + path->len + 1);
  memcpy(full, from, dl);
  memcpy(full + dl, path->chars, path->len + 1);
  Str *s = intern(full, dl + path->len);
  free(full);
  return s;
}

/* `lumo build` appends the program's files to a copy of this executable:
 *   path\0source\0 path\0source\0 ... <u64 length> "LUMOPAK1"
 * The first file is the main program. */
#define PAK_MAGIC "LUMOPAK1"
static char *embedded;
static size_t embedded_len;

static const char *embedded_source(const char *path) {
  for (size_t i = 0; i < embedded_len;) {
    const char *p = embedded + i, *s = p + strlen(p) + 1;
    if (!strcmp(p, path)) return s;
    i = (size_t)(s - embedded) + strlen(s) + 1;
  }
  return NULL;
}

static bool self_path(char *buf, uint32_t size) {
#ifdef __APPLE__
  return _NSGetExecutablePath(buf, &size) == 0;
#elif defined(_WIN32)
  DWORD n = GetModuleFileNameA(NULL, buf, size);
  return n > 0 && n < size;
#else
  ssize_t n = readlink("/proc/self/exe", buf, size - 1);
  if (n <= 0) return false;
  buf[n] = 0;
  return true;
#endif
}

static void load_embedded(void) {
  char self[4096];
  if (!self_path(self, sizeof self)) return;
  FILE *f = fopen(self, "rb");
  if (!f) return;
  char tail[16];
  uint64_t len;
  if (fseek(f, -16, SEEK_END) == 0 && fread(tail, 1, 16, f) == 16 && !memcmp(tail + 8, PAK_MAGIC, 8)) {
    memcpy(&len, tail, 8);
    embedded = malloc(len + 1);
    if (fseek(f, -16 - (long)len, SEEK_END) == 0 && fread(embedded, 1, len, f) == len) embedded_len = len;
    else { free(embedded); embedded = NULL; }
  }
  fclose(f);
}

/* ---------- scanner ---------- */

typedef enum {
  T_NUM, T_STR, T_ISTR /* string part followed by ${expr} */, T_NAME,
  T_LET, T_FN, T_IF, T_ELSE, T_WHILE, T_FOR, T_IN, T_RETURN, T_BREAK, T_CONTINUE,
  T_TRUE, T_FALSE, T_NIL, T_AND, T_OR, T_NOT, T_IMPORT, T_TRY, T_CATCH,
  T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
  T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE, T_ASSIGN, T_OPASSIGN /* += -= *= /= %= */, T_PIPE, T_ARROW,
  T_BAND, T_BOR, T_BXOR, T_BNOT, T_SHL, T_SHR, T_USHR, T_DOTDOT,
  T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
  T_COMMA, T_SEMI, T_COLON, T_DOT, T_EOF
} TT;
static const char *KEYWORDS[] = {"let", "fn", "if", "else", "while", "for", "in", "return", "break", "continue",
                                 "true", "false", "nil", "and", "or", "not", "import", "try", "catch"};

typedef struct { TT type; const char *start; int len, line; } Tok; /* len 0 semicolon = newline */

static jmp_buf compile_jmp;
static char compile_msg[512];
static Tok *toks;
static int ntoks, ctoks, ti, cur_line;
static int interp[64], ninterp, brace_depth; /* open "{...}" inside strings */

static int compile_err_line;
static void compile_error(int line, const char *fmt, ...) {
  compile_err_line = line;
  int n = snprintf(compile_msg, sizeof compile_msg, "line %d: ", line);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(compile_msg + n, sizeof compile_msg - n, fmt, ap);
  va_end(ap);
  longjmp(compile_jmp, 1);
}

static void add_tok(TT t, const char *s, int len, int line) {
  /* these continue the previous line, so drop the newline before them */
  bool cont = t == T_PIPE || t == T_ELSE || t == T_AND || t == T_OR || t == T_CATCH;
  if (cont && ntoks && toks[ntoks - 1].type == T_SEMI && toks[ntoks - 1].len == 0) ntoks--;
  if (ntoks == ctoks) {
    ctoks = ctoks ? ctoks * 2 : 256;
    toks = realloc(toks, sizeof(Tok) * ctoks);
  }
  toks[ntoks++] = (Tok){t, s, len, line};
}

/* a newline ends a statement only after a token that can end one */
static bool ends_stmt(TT t) {
  return t == T_NUM || t == T_STR || t == T_NAME || t == T_RPAREN || t == T_RBRACKET ||
         t == T_RBRACE || t == T_TRUE || t == T_FALSE || t == T_NIL || t == T_RETURN ||
         t == T_BREAK || t == T_CONTINUE;
}

static bool name_char(char c) { return isalnum((unsigned char)c) || c == '_' || (uint8_t)c >= 0x80; }

/* p points at the opening '"' or at the '}' closing an interpolation.
 * The token keeps one delimiter char at each end: "abc$  or  }abc"  ("${" opens an interpolation) */
static const char *string_part(const char *p, int *line) {
  const char *s = p;
  int start_line = *line;
  p++;
  while (*p && *p != '"' && !(*p == '$' && p[1] == '{')) {
    if (*p == '\\' && p[1]) p++;
    if (*p == '\n') compile_error(start_line, "unterminated string (a string ends on the line it starts; write \\n for a line break)");
    p++;
  }
  if (!*p) compile_error(start_line, "unterminated string");
  if (*p == '"') {
    add_tok(T_STR, s, (int)(p - s + 1), start_line);
    return p + 1;
  }
  if (ninterp == 64) compile_error(*line, "strings nested too deeply");
  interp[ninterp++] = brace_depth;
  add_tok(T_ISTR, s, (int)(p - s + 1), start_line);
  return p + 2;
}

static void scan(const char *src) {
  ntoks = ninterp = brace_depth = 0;
  int line = 1;
  const char *p = src;
  while (*p) {
    char c = *p;
    if (c == '\n') {
      if (ntoks && ends_stmt(toks[ntoks - 1].type)) add_tok(T_SEMI, p, 0, line);
      line++;
      p++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') { p++; continue; }
    if (c == '#') { while (*p && *p != '\n') p++; continue; }
    const char *s = p;
    if (c == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) { /* 0xFF */
      p += 2;
      while (isxdigit((unsigned char)*p)) p++;
      add_tok(T_NUM, s, (int)(p - s), line);
      continue;
    }
    if (isdigit((unsigned char)c)) {
      while (isdigit((unsigned char)*p)) p++;
      if (*p == '.' && isdigit((unsigned char)p[1])) {
        p++;
        while (isdigit((unsigned char)*p)) p++;
      }
      if ((*p == 'e' || *p == 'E') && (isdigit((unsigned char)p[1]) ||
                                       ((p[1] == '-' || p[1] == '+') && isdigit((unsigned char)p[2])))) { /* 1e9, 2.5e-3 */
        p += 2;
        while (isdigit((unsigned char)*p)) p++;
      }
      add_tok(T_NUM, s, (int)(p - s), line);
      continue;
    }
    if (name_char(c)) {
      while (name_char(*p)) p++;
      if (p - s == 1 && c == 'f' && *p == '"') compile_error(line, "Lumo has no f-strings: write \"...${x}...\"");
      TT t = T_NAME;
      for (int k = 0; k < (int)(sizeof KEYWORDS / sizeof *KEYWORDS); k++)
        if ((int)strlen(KEYWORDS[k]) == p - s && memcmp(KEYWORDS[k], s, p - s) == 0) t = (TT)(T_LET + k);
      add_tok(t, s, (int)(p - s), line);
      continue;
    }
    if (c == '"' && p[1] == '"' && p[2] == '"') compile_error(line, "Lumo has no triple-quoted strings: use \\n for line breaks");
    if (c == '"') { p = string_part(p, &line); continue; }
    if (c == '}' && ninterp && interp[ninterp - 1] == brace_depth) {
      ninterp--;
      p = string_part(p, &line);
      continue;
    }
    if (c == '{') brace_depth++;
    if (c == '}') brace_depth--;
    if (c == '&' && p[1] == '&') compile_error(line, "unexpected '&&' (use 'and')");
    if (strchr("+-*/%", c) && p[1] == '=') { add_tok(T_OPASSIGN, s, 2, line); p += 2; continue; }
    if ((c == '+' || c == '-') && p[1] == c) compile_error(line, "Lumo has no %c%c: write x %c= 1", c, c, c);
    if (c == '*' && p[1] == '*') compile_error(line, "Lumo has no **: use pow(a, b)");
    if (c == '|' && p[1] == '|') compile_error(line, "unexpected '||' (use 'or')");
    if (c == '>' && p[1] == '>' && p[2] == '>') { add_tok(T_USHR, s, 3, line); p += 3; continue; }
    static const struct { const char *s; TT t; } two[] = {
        {"|>", T_PIPE}, {"->", T_ARROW}, {"==", T_EQ}, {"!=", T_NE}, {"<=", T_LE}, {">=", T_GE},
        {"<<", T_SHL}, {">>", T_SHR}, {"..", T_DOTDOT}};
    bool done = false;
    for (int k = 0; k < 9 && !done; k++)
      if (c == two[k].s[0] && p[1] == two[k].s[1]) {
        add_tok(two[k].t, s, 2, line);
        p += 2;
        done = true;
      }
    if (done) continue;
    const char *ones = "+-*/%<>=(){}[],;:.&|^~";
    static const TT onet[] = {T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_LT, T_GT, T_ASSIGN, T_LPAREN,
                              T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET, T_COMMA, T_SEMI, T_COLON, T_DOT,
                              T_BAND, T_BOR, T_BXOR, T_BNOT};
    const char *hit = strchr(ones, c);
    if (c == '/' && p[1] == '/') {
      bool after_value = ntoks && (toks[ntoks - 1].type == T_NUM || toks[ntoks - 1].type == T_NAME ||
                                   toks[ntoks - 1].type == T_RPAREN || toks[ntoks - 1].type == T_RBRACKET);
      compile_error(line, after_value ? "Lumo has no //: comments start with #, and floor(a / b) divides without the fraction" : "comments start with #, not //");
    }
    if (!hit) {
      if (c == '!') compile_error(line, "unexpected '!' (use 'not')");
      if (c == '`') compile_error(line, "unexpected '`' (if you copied from a guide, leave out the ``` lines)");
      if (c == '\'') compile_error(line, "strings use double quotes: \"like this\"");
      compile_error(line, "unexpected character '%c'", c);
    }
    add_tok(onet[hit - ones], s, 1, line);
    p++;
  }
  if (ninterp) compile_error(line, "unterminated string");
  add_tok(T_SEMI, p, 0, line);
  add_tok(T_EOF, p, 0, line);
}

/* ---------- types ---------- */

/* Gradual typing: `any` fits everything; everything else is checked at compile time. */
typedef enum { TY_ANY, TY_NUM, TY_STR, TY_BOOL, TY_NIL, TY_LIST, TY_MAP, TY_FN, TY_STRUCT } TyKind;
typedef struct Type {
  TyKind k;
  struct Type *a, *b; /* list: a = item; map: a = key, b = value */
  struct Type **params, *ret; /* struct: params/pnames/nparams are its fields */
  int nparams; /* -1: parameters not checked */
  bool mixed;  /* list/map literal whose items have different known types */
  const char *name;
  const char **pnames;
} Type;

static Type T_ANY_ = {.k = TY_ANY}, T_NUM_ = {.k = TY_NUM}, T_STR_ = {.k = TY_STR}, T_BOOL_ = {.k = TY_BOOL},
            T_NIL_ = {.k = TY_NIL};
#define TANY (&T_ANY_)
#define TNUM (&T_NUM_)
#define TSTR (&T_STR_)
#define TBOOL (&T_BOOL_)
#define TNIL (&T_NIL_)

/* ponytail: types are never freed; they are tiny and live as long as the code that uses them */
static Type *ty_new(TyKind k) {
  Type *t = calloc(1, sizeof(Type));
  t->k = k;
  return t;
}
static Type *ty_list(Type *item) { Type *t = ty_new(TY_LIST); t->a = item; return t; }
static Type *ty_map(Type *k, Type *v) { Type *t = ty_new(TY_MAP); t->a = k; t->b = v; return t; }
static bool known(Type *t) { return t->k != TY_ANY; }

static bool ty_eq(Type *x, Type *y) {
  if (x == y) return true;
  if (x->k != y->k) return false;
  if (x->k == TY_STRUCT) return !strcmp(x->name, y->name);
  if (x->k == TY_LIST) return ty_eq(x->a, y->a);
  if (x->k == TY_MAP) return ty_eq(x->a, y->a) && ty_eq(x->b, y->b);
  if (x->k == TY_FN) {
    if (x->nparams != y->nparams || !ty_eq(x->ret, y->ret)) return false;
    for (int i = 0; i < x->nparams; i++)
      if (!ty_eq(x->params[i], y->params[i])) return false;
  }
  return true;
}

/* can a value of type `got` go where `want` is expected? */
static bool compat(Type *want, Type *got) {
  if (!known(want) || !known(got)) return true;
  if (want->k == TY_MAP && got->k == TY_STRUCT) return true; /* a struct is a map with fixed fields */
  if (want->k == TY_STRUCT) return got->k == TY_STRUCT && !strcmp(want->name, got->name);
  if (want->k != got->k) return false;
  if (want->k == TY_LIST) return !(got->mixed && known(want->a)) && compat(want->a, got->a);
  if (want->k == TY_MAP) return !(got->mixed && known(want->b)) && compat(want->a, got->a) && compat(want->b, got->b);
  if (want->k == TY_FN) {
    if (want->nparams < 0 || got->nparams < 0) return true;
    if (want->nparams != got->nparams || !compat(want->ret, got->ret)) return false;
    for (int i = 0; i < want->nparams; i++)
      if (!compat(got->params[i], want->params[i])) return false;
  }
  return true;
}

static Type *join(Type *x, Type *y) {
  if (!x) return y;
  return ty_eq(x, y) ? x : TANY;
}

/* join for literal items, noting when two known types disagree */
static Type *join_items(Type *acc, Type *t, bool *mixed) {
  if (acc && known(acc) && known(t) && !ty_eq(acc, t)) *mixed = true;
  return join(acc, t);
}

static void ty_buf(Type *t, Buf *b) {
  static const char *names[] = {"any", "num", "str", "bool", "nil"};
  if (t->k <= TY_NIL) { buf_add(b, names[t->k], (int)strlen(names[t->k])); return; }
  if (t->k == TY_STRUCT) { buf_add(b, t->name, (int)strlen(t->name)); return; }
  if (t->k == TY_LIST) {
    buf_add(b, "list", 4);
    if (known(t->a)) { buf_add(b, "[", 1); ty_buf(t->a, b); buf_add(b, "]", 1); }
    return;
  }
  if (t->k == TY_MAP) {
    buf_add(b, "map", 3);
    if (known(t->a) || known(t->b)) {
      buf_add(b, "[", 1);
      ty_buf(t->a, b);
      buf_add(b, ", ", 2);
      ty_buf(t->b, b);
      buf_add(b, "]", 1);
    }
    return;
  }
  buf_add(b, "fn(", 3);
  if (t->nparams < 0) buf_add(b, "...", 3);
  for (int i = 0; i < t->nparams; i++) {
    if (i) buf_add(b, ", ", 2);
    ty_buf(t->params[i], b);
  }
  buf_add(b, ")", 1);
  if (known(t->ret)) { buf_add(b, " -> ", 4); ty_buf(t->ret, b); }
}

static const char *tys(Type *t) { /* a few rotating buffers so one message can show several types */
  static char out[4][160];
  static int next;
  Buf b = {0};
  ty_buf(t, &b);
  char *o = out[next++ & 3];
  snprintf(o, 160, "%s", b.s);
  free(b.s);
  return o;
}

static Type *fnty(const char *name, Type *ret, int n, ...) {
  Type *t = ty_new(TY_FN);
  t->name = name;
  t->ret = ret;
  t->nparams = n;
  if (n > 0) {
    t->params = malloc(sizeof(Type *) * n);
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) t->params[i] = va_arg(ap, Type *);
    va_end(ap);
  }
  return t;
}

static Type *builtin_type(const char *name) {
  static struct { const char *n; Type *t; } tab[128];
  static int ntab = -1;
  if (ntab < 0) {
    Type *lnum = ty_list(TNUM), *lstr = ty_list(TSTR), *lany = ty_list(TANY), *many = ty_map(TANY, TANY);
    ntab = 0;
#define SIG(nm, ...) tab[ntab].n = nm, tab[ntab++].t = fnty(nm, __VA_ARGS__)
    SIG("print", TNIL, -1);
    SIG("write", TNIL, -1);
    SIG("len", TNUM, 1, TANY);
    SIG("str", TSTR, 1, TANY);
    SIG("num", TNUM, 1, TANY);
    SIG("type", TSTR, 1, TANY);
    SIG("upper", TSTR, 1, TSTR);
    SIG("lower", TSTR, 1, TSTR);
    SIG("trim", TSTR, 1, TSTR);
    SIG("replace", TSTR, 3, TSTR, TSTR, TSTR);
    SIG("split", lstr, 2, TSTR, TSTR);
    SIG("join", TSTR, 2, lany, TSTR);
    SIG("range", lnum, -1);
    SIG("floor", TNUM, 1, TNUM);
    SIG("ceil", TNUM, 1, TNUM);
    SIG("round", TNUM, 1, TNUM);
    SIG("sqrt", TNUM, 1, TNUM);
    SIG("abs", TNUM, 1, TNUM);
    SIG("min", TNUM, 2, TNUM, TNUM);
    SIG("max", TNUM, 2, TNUM, TNUM);
    SIG("random", TNUM, 0);
    SIG("clock", TNUM, 0);
    SIG("has", TBOOL, 2, many, TANY);
    SIG("remove", TBOOL, 2, many, TANY);
    SIG("contains", TBOOL, 2, TANY, TANY);
    SIG("keys", lany, 1, many);
    SIG("read_file", TSTR, 1, TSTR);
    SIG("write_file", TNIL, 2, TSTR, TSTR);
    SIG("assert", TNIL, -1);
    SIG("starts_with", TBOOL, 2, TSTR, TSTR);
    SIG("ends_with", TBOOL, 2, TSTR, TSTR);
    SIG("find", TNUM, 2, TSTR, TSTR);
    SIG("chars", lstr, 1, TSTR);
    SIG("ord", TNUM, 1, TSTR);
    SIG("chr", TSTR, 1, TNUM);
    SIG("repeat", TSTR, 2, TSTR, TNUM);
    SIG("index_of", TNUM, 2, lany, TANY);
    SIG("pow", TNUM, 2, TNUM, TNUM);
    SIG("trunc", TNUM, 1, TNUM);
    SIG("read_bytes", lnum, 1, TSTR);
    SIG("bytes_str", TSTR, 3, lany, TNUM, TNUM);
    SIG("log", TNUM, 1, TNUM);
    SIG("sin", TNUM, 1, TNUM);
    SIG("cos", TNUM, 1, TNUM);
    SIG("json_encode", TSTR, 1, TANY);
    SIG("now", TNUM, 0);
    SIG("sleep", TNIL, 1, TNUM);
    SIG("format_time", TSTR, -1);
    SIG("exists", TBOOL, 1, TSTR);
    SIG("list_dir", lstr, 1, TSTR);
    SIG("append_file", TNIL, 2, TSTR, TSTR);
    SIG("run", ty_map(TSTR, TANY), 1, TANY);
    tab[ntab].n = "pi", tab[ntab++].t = TNUM;
#undef SIG
  }
  for (int i = 0; i < ntab; i++)
    if (!strcmp(tab[i].n, name)) return tab[i].t;
  return NULL;
}

/* compile-time types of a module's globals (names copied: interned strings may be collected) */
typedef struct { char **names; Type **types; int n, cap; } GTypes;
static GTypes main_types, *cur_types;

static int gtype_find(const char *name, int len) {
  for (int i = 0; i < cur_types->n; i++)
    if ((int)strlen(cur_types->names[i]) == len && !memcmp(cur_types->names[i], name, len)) return i;
  return -1;
}
static int gtype_set(const char *name, int len, Type *t) {
  int i = gtype_find(name, len);
  if (i >= 0) { cur_types->types[i] = t; return i; }
  if (cur_types->n == cur_types->cap) {
    cur_types->cap = cur_types->cap ? cur_types->cap * 2 : 32;
    cur_types->names = realloc(cur_types->names, sizeof(char *) * cur_types->cap);
    cur_types->types = realloc(cur_types->types, sizeof(Type *) * cur_types->cap);
  }
  char *copy = malloc(len + 1);
  memcpy(copy, name, len);
  copy[len] = 0;
  cur_types->names[cur_types->n] = copy;
  cur_types->types[cur_types->n] = t;
  return cur_types->n++;
}

static bool declared_in_main(const char *name) { /* the main program has a let for it somewhere */
  for (int i = 0; i < main_types.n; i++)
    if (!strcmp(main_types.names[i], name)) return true;
  return false;
}

/* ---------- compiler ---------- */

typedef enum {
  OP_CONST, OP_NIL, OP_TRUE, OP_FALSE, OP_POP, OP_POPN, OP_ENDSCOPE, OP_SWAP, OP_DUP2,
  OP_GET_LOCAL, OP_SET_LOCAL, OP_GET_UP, OP_SET_UP, OP_GET_GLOBAL, OP_SET_GLOBAL, OP_DEF_GLOBAL,
  OP_GET_INDEX, OP_SET_INDEX,
  OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG, OP_NOT,
  OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
  OP_JUMP, OP_JUMP_FALSE, OP_AND, OP_OR, OP_CHECK_BOOL, OP_LOOP,
  OP_CALL, OP_CLOSURE, OP_RETURN, OP_LIST, OP_MAP, OP_ITER, OP_FOR_NEXT, OP_IMPORT,
  OP_TOSTR, OP_TRY, OP_END_TRY, OP_POP_HANDLERS, OP_CHECK, OP_CHECK_ARG, OP_STRUCT,
  OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR, OP_USHR, OP_BNOT, OP_RANGE, OP_COUNT
} Op;

/* fixed stack effect of each op; variable ones (POPN, ENDSCOPE, CALL, LIST, MAP) are adjusted by hand */
static const int8_t EFFECT[OP_COUNT] = {
    [OP_CONST] = 1, [OP_NIL] = 1, [OP_TRUE] = 1, [OP_FALSE] = 1, [OP_POP] = -1,
    [OP_GET_LOCAL] = 1, [OP_GET_UP] = 1, [OP_GET_GLOBAL] = 1, [OP_DEF_GLOBAL] = -1,
    [OP_DUP2] = 2, [OP_GET_INDEX] = -1, [OP_SET_INDEX] = -2,
    [OP_ADD] = -1, [OP_SUB] = -1, [OP_MUL] = -1, [OP_DIV] = -1, [OP_MOD] = -1,
    [OP_EQ] = -1, [OP_NE] = -1, [OP_LT] = -1, [OP_LE] = -1, [OP_GT] = -1, [OP_GE] = -1,
    [OP_JUMP_FALSE] = -1, [OP_AND] = -1, [OP_OR] = -1,
    [OP_CLOSURE] = 1, [OP_FOR_NEXT] = 1, [OP_IMPORT] = 1,
    [OP_CHECK] = 0,
    [OP_RANGE] = -1, [OP_BAND] = -1, [OP_BOR] = -1, [OP_BXOR] = -1, [OP_SHL] = -1, [OP_SHR] = -1, [OP_USHR] = -1,
};

typedef struct { const char *name; int len, depth, slot; Type *type; bool loop_var; } Local;
typedef struct { uint8_t index; bool is_local; } UpRef;
typedef struct Loop { struct Loop *outer; int start, base, try_depth, nbreaks; int breaks[256]; } Loop;
typedef struct Comp {
  struct Comp *enclosing;
  Fn *fn;
  Local locals[256];
  int nlocals, depth;
  int height;    /* values on this frame's stack at this point of the code */
  int try_depth; /* open try blocks in this function */
  Type *ret;     /* declared return type, or NULL */
  UpRef ups[256];
  Loop *loop;
} Comp;
static Comp *C;

enum { P_NONE, P_ASSIGN, P_PIPE, P_OR, P_AND, P_NOT, P_CMP, P_RANGE, P_BOR, P_BXOR, P_BAND, P_SHIFT, P_TERM, P_FACTOR, P_UNARY, P_CALL };

static Type *expression(void);
static Type *parse_prec(int prec);
static bool statement(Type **out);
static Type *block(void);
static Type *function(Str *name, Type **slot);
static Type *var_type(Tok *t, bool *checked);
static Type *if_expr(void);
static Type *try_expr(void);

static Tok *peek(void) { return &toks[ti]; }
static bool at(TT t) { return toks[ti].type == t; }
static Tok *advance(void) {
  Tok *t = &toks[ti];
  if (t->type != T_EOF) ti++;
  cur_line = t->line;
  return t;
}
static bool match(TT t) {
  if (!at(t)) return false;
  advance();
  return true;
}
static void describe(Tok *t, char *out, int n) {
  if (t->type == T_EOF) snprintf(out, n, "end of file");
  else if (t->len == 0) snprintf(out, n, "end of line");
  else snprintf(out, n, "'%.*s'", t->len > 40 ? 40 : t->len, t->start);
}
static Tok *expect(TT t, const char *what) {
  if (t == T_NAME && peek()->type >= T_LET && peek()->type <= T_CATCH) /* `let not = 1` */
    compile_error(peek()->line, "'%.*s' is a reserved word in Lumo and can't be used as a name", peek()->len, peek()->start);
  if (!at(t) && at(T_ELSE)) compile_error(peek()->line, "unexpected 'else': close the if block with } before else");
  if (!at(t)) {
    char d[64];
    describe(peek(), d, sizeof d);
    const char *hint = peek()->type == T_NAME ? foreign_hint(peek()->start, peek()->len) : NULL;
    static char h[160];
    Tok *prev = ti > 0 ? &toks[ti - 1] : NULL;
    if (!strcmp(what, "a file path string") && at(T_NAME))
      hint = " (math like sqrt and floor is built in; import loads your own files: import \"file.lu\")";
    else if (!hint && !strcmp(what, "end of statement") && prev && prev->type == T_NAME &&
             (at(T_STR) || at(T_NUM) || at(T_ISTR))) {
      snprintf(h, sizeof h, " (to call %.*s, put parentheses around it: %.*s(...))", prev->len, prev->start, prev->len, prev->start);
      hint = h;
    }
    compile_error(peek()->line, "expected %s, got %s%s", what, d, hint ? hint : "");
  }
  return advance();
}
static void skip_semis(void) { while (at(T_SEMI)) advance(); }

static Chunk *chunk(void) { return &C->fn->chunk; }
static void emit(int b) {
  Chunk *c = chunk();
  if (c->len == c->cap) {
    int nc = c->cap ? c->cap * 2 : 64;
    c->code = mem(c->code, c->cap, nc);
    c->lines = mem(c->lines, sizeof(int) * c->cap, sizeof(int) * nc);
    c->cap = nc;
  }
  c->code[c->len] = (uint8_t)b;
  c->lines[c->len++] = cur_line;
}
static void op(int o) { emit(o); C->height += EFFECT[o]; }
static void op_n(int o, int n) { emit(o); emit(n); C->height -= n; } /* POPN / ENDSCOPE / CALL */
static void emit16(int v) { emit((v >> 8) & 0xff); emit(v & 0xff); }
static int make_const(Value v) {
  Chunk *c = chunk();
  if (c->nconsts == 0xffff) compile_error(cur_line, "too many constants in one function");
  if (c->nconsts == c->cconsts) {
    int nc = c->cconsts ? c->cconsts * 2 : 8;
    c->consts = mem(c->consts, sizeof(Value) * c->cconsts, sizeof(Value) * nc);
    c->cconsts = nc;
  }
  c->consts[c->nconsts] = v;
  return c->nconsts++;
}
static void emit_const(Value v) { op(OP_CONST); emit16(make_const(v)); }
static int name_const(Tok *t) { return make_const(OBJ_V(intern(t->start, t->len))); }
static int emit_jump(int o) { op(o); emit(0xff); emit(0xff); return chunk()->len - 2; }
static void patch_jump(int at) {
  int j = chunk()->len - at - 2;
  if (j > 0xffff) compile_error(cur_line, "too much code to jump over");
  chunk()->code[at] = (j >> 8) & 0xff;
  chunk()->code[at + 1] = j & 0xff;
}
static void emit_loop(int start) {
  op(OP_LOOP);
  int off = chunk()->len - start + 2;
  if (off > 0xffff) compile_error(cur_line, "loop body too large");
  emit16(off);
}

/* runtime guard for an annotated boundary when the value's type isn't known statically */
static void check_operands(Type *t, const char *what) {
  emit(t->k);
  emit16(make_const(OBJ_V(cstr(what))));
  if (t->k == TY_STRUCT) emit16(make_const(OBJ_V(cstr(t->name))));
}
static void emit_check(Type *t, const char *what) {
  if (!known(t)) return;
  op(OP_CHECK);
  check_operands(t, what);
}

static Str *str_token(Tok *t) { /* drops the first and last character (the delimiters) */
  char *buf = malloc(t->len);
  int n = 0;
  for (int i = 1; i < t->len - 1; i++) {
    char c = t->start[i];
    if (c == '\\') {
      c = t->start[++i];
      c = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'e' ? '\033' : c == 'a' ? '\a' : c;
    }
    buf[n++] = c;
  }
  Str *s = intern(buf, n);
  free(buf);
  return s;
}

/* type := num | str | bool | nil | any | list[T] | map[K, V] | fn(T, ...) -> T */
static Type *parse_type(void) {
  Tok *t = advance();
  if (t->type == T_NIL) return TNIL;
  if (t->type == T_FN) {
    Type *f = ty_new(TY_FN);
    f->ret = TANY;
    f->params = malloc(sizeof(Type *) * 256);
    expect(T_LPAREN, "'('");
    if (!at(T_RPAREN)) {
      do {
        if (f->nparams == 255) compile_error(t->line, "too many parameters");
        f->params[f->nparams++] = parse_type();
      } while (match(T_COMMA));
    }
    expect(T_RPAREN, "')'");
    if (match(T_ARROW)) f->ret = parse_type();
    return f;
  }
#define IS(s) (t->type == T_NAME && t->len == (int)strlen(s) && !memcmp(t->start, s, t->len))
  if (IS("num")) return TNUM;
  if (IS("str")) return TSTR;
  if (IS("bool")) return TBOOL;
  if (IS("any")) return TANY;
  if (IS("list")) {
    Type *item = TANY;
    if (match(T_LBRACKET)) { item = parse_type(); expect(T_RBRACKET, "']'"); }
    return ty_list(item);
  }
  if (IS("map")) {
    Type *k = TANY, *v = TANY;
    if (match(T_LBRACKET)) {
      k = parse_type();
      expect(T_COMMA, "',' between key and value types");
      v = parse_type();
      expect(T_RBRACKET, "']'");
    }
    return ty_map(k, v);
  }
#undef IS
  if (t->type == T_NAME) { /* a struct's name is its type */
    bool checked;
    Type *v = var_type(t, &checked);
    if (v->k == TY_FN && v->ret && v->ret->k == TY_STRUCT && (int)strlen(v->ret->name) == t->len &&
        !memcmp(v->ret->name, t->start, t->len))
      return v->ret;
  }
  static const char *aliases[][2] = {{"int", "num"}, {"float", "num"}, {"double", "num"}, {"number", "num"},
                                     {"integer", "num"}, {"string", "str"}, {"String", "str"}, {"boolean", "bool"},
                                     {"array", "list"}, {"List", "list"}, {"dict", "map"}, {"object", "map"},
                                     {"Map", "map"}, {"void", "nil"}};
  for (int i = 0; i < (int)(sizeof aliases / sizeof *aliases); i++)
    if (t->type == T_NAME && (int)strlen(aliases[i][0]) == t->len && !memcmp(aliases[i][0], t->start, t->len))
      compile_error(t->line, "unknown type '%s' (use %s)", aliases[i][0], aliases[i][1]);
  char d[64];
  describe(t, d, sizeof d);
  compile_error(t->line, "unknown type %s (types: num, str, bool, nil, any, list[T], map[K, V], fn(T) -> T)", d);
  return TANY;
}

static void comp_init(Comp *c, Str *name) {
  c->enclosing = C;
  c->fn = new_fn(name);
  c->nlocals = 1; /* slot 0 holds the function itself */
  c->depth = 0;
  c->height = 1;
  c->try_depth = 0;
  c->ret = NULL;
  c->loop = NULL;
  c->locals[0] = (Local){"", 0, 0, 0, TANY, false};
  C = c;
}

static bool is_global_scope(void) { return C->enclosing == NULL && C->depth == 0; }

static Local *add_local_named(const char *name, int len, int slot, Type *type) {
  for (int i = C->nlocals - 1; i >= 0 && C->locals[i].depth == C->depth; i--)
    if (C->locals[i].len == len && memcmp(C->locals[i].name, name, len) == 0)
      compile_error(cur_line, "'%.*s' is already declared here", len, name);
  if (C->nlocals == 256 || slot > 255) compile_error(cur_line, "too many local variables");
  C->locals[C->nlocals] = (Local){name, len, C->depth, slot, type, false};
  return &C->locals[C->nlocals++];
}
static Local *add_local(Tok *t, int slot, Type *type) { return add_local_named(t->start, t->len, slot, type); }

/* let-style definition: a global at the top of a file, otherwise a local in the value's slot */
static void define(Tok *n, Type *type) {
  if (is_global_scope()) {
    gtype_set(n->start, n->len, type);
    op(OP_DEF_GLOBAL);
    emit16(name_const(n));
  } else {
    add_local(n, C->height - 1, type);
  }
}

static void begin_scope(void) { C->depth++; }
/* a scope's locals sit contiguously on top of the stack (under the value, if kept) */
static void end_scope(bool keep_value) {
  C->depth--;
  int n = 0;
  while (C->nlocals > 0 && C->locals[C->nlocals - 1].depth > C->depth) { C->nlocals--; n++; }
  if (n) op_n(keep_value ? OP_ENDSCOPE : OP_POPN, n);
}

static int find_local(Comp *c, Tok *t) {
  for (int i = c->nlocals - 1; i >= 0; i--)
    if (c->locals[i].len == t->len && memcmp(c->locals[i].name, t->start, t->len) == 0) return i;
  return -1;
}
static int resolve_local(Comp *c, Tok *t) {
  int i = find_local(c, t);
  return i < 0 ? -1 : c->locals[i].slot;
}
static int add_upvalue(Comp *c, int index, bool is_local) {
  for (int i = 0; i < c->fn->nupvals; i++)
    if (c->ups[i].index == index && c->ups[i].is_local == is_local) return i;
  if (c->fn->nupvals == 256) compile_error(cur_line, "too many captured variables");
  c->ups[c->fn->nupvals] = (UpRef){(uint8_t)index, is_local};
  return c->fn->nupvals++;
}
static int resolve_upvalue(Comp *c, Tok *t) {
  if (!c->enclosing) return -1;
  int l = resolve_local(c->enclosing, t);
  if (l != -1) return add_upvalue(c, l, true);
  int u = resolve_upvalue(c->enclosing, t);
  if (u != -1) return add_upvalue(c, u, false);
  return -1;
}

/* static type of a name; `checked` is false for names the compiler can't vouch for (builtins, unknown) */
static Type *var_type(Tok *t, bool *checked) {
  *checked = true;
  for (Comp *c = C; c; c = c->enclosing) {
    int i = find_local(c, t);
    if (i >= 0) return c->locals[i].type;
  }
  int g = gtype_find(t->start, t->len);
  if (g >= 0) return cur_types->types[g];
  *checked = false;
  char name[64];
  snprintf(name, sizeof name, "%.*s", t->len, t->start);
  Type *b = builtin_type(name);
  return b ? b : TANY;
}

/* is `name` declared anywhere in this file (it may be defined below its first use)? */
static bool declared_in_file(Tok *t) {
  for (int i = 0; i + 1 < ntoks; i++) {
    Tok *a = &toks[i], *b = &toks[i + 1];
    bool decl = ((a->type == T_LET || a->type == T_FN) && b->type == T_NAME) ||
                (a->type == T_NAME && b->type == T_NAME && a->len == 6 && !memcmp(a->start, "struct", 6)) ||
                (a->type == T_NAME && b->type == T_NAME && a->len == 2 && !memcmp(a->start, "as", 2));
    if (decl && b->len == t->len && !memcmp(b->start, t->start, t->len)) return true;
    if (a->type == T_IMPORT && b->type == T_STR) { /* import "lib/geo.lu" binds geo */
      const char *s = b->start + 1, *e = b->start + b->len - 1, *base = s;
      for (const char *p = s; p < e; p++) if (*p == '/') base = p + 1;
      const char *dot = e;
      for (const char *p = base; p < e; p++) if (*p == '.') dot = p;
      if (dot - base == t->len && !memcmp(base, t->start, t->len)) return true;
    }
  }
  return false;
}

static void undefined_name(Tok *t) {
  const char *hint = foreign_hint(t->start, t->len);
  if (!hint && (toks[ti].type == T_ASSIGN || toks[ti].type == T_OPASSIGN)) /* `yaş = 15` before any let */
    compile_error(t->line, "'%.*s' doesn't exist yet; to create it write: let %.*s = ...", t->len, t->start, t->len, t->start);
  if (!hint) {
    Suggest s;
    suggest_init(&s, t->start, t->len);
    for (Comp *c = C; c; c = c->enclosing)
      for (int i = 0; i < c->nlocals; i++)
        if (c->locals[i].name[0] != '(') suggest_try(&s, c->locals[i].name, c->locals[i].len);
    for (int i = 0; i < cur_types->n; i++) suggest_try(&s, cur_types->names[i], (int)strlen(cur_types->names[i]));
    for (int i = 0; i < vm.builtins->used; i++)
      if (IS_OT(vm.builtins->entries[i].key, O_STR))
        suggest_try(&s, AS_STR(vm.builtins->entries[i].key)->chars, AS_STR(vm.builtins->entries[i].key)->len);
    for (int k = 0; k < (int)(sizeof KEYWORDS / sizeof *KEYWORDS); k++) suggest_try(&s, KEYWORDS[k], (int)strlen(KEYWORDS[k]));
    hint = s.best[0] ? suggest_text(&s)
         : toks[ti].type == T_LPAREN ? " (there's no function by that name; the README lists the built-in ones)"
         : " (declare it with let, or put it in quotes if you meant text)";
  }
  compile_error(t->line, "'%.*s' is not defined%s", t->len, t->start, hint);
}

static const int BINOPS[] = {[T_PLUS] = OP_ADD, [T_MINUS] = OP_SUB, [T_STAR] = OP_MUL, [T_SLASH] = OP_DIV,
                             [T_PERCENT] = OP_MOD, [T_EQ] = OP_EQ, [T_NE] = OP_NE, [T_LT] = OP_LT,
                             [T_LE] = OP_LE, [T_GT] = OP_GT, [T_GE] = OP_GE, [T_BAND] = OP_BAND,
                             [T_BOR] = OP_BOR, [T_BXOR] = OP_BXOR, [T_SHL] = OP_SHL, [T_SHR] = OP_SHR,
                             [T_USHR] = OP_USHR};

/* the operator of a += -= *= /= %= token */
static TT opassign_op(Tok *t) {
  return t->start[0] == '+' ? T_PLUS : t->start[0] == '-' ? T_MINUS : t->start[0] == '*' ? T_STAR
       : t->start[0] == '/' ? T_SLASH : T_PERCENT;
}

static Type *arith(TT tt, Type *l, Type *r, int line);

/* the value of `= v` or `op= v`; for op= the current value (cur) is already on the stack */
static Type *assigned_value(Tok *opt, Type *cur, int line) {
  TT tt = opt->type == T_ASSIGN ? T_ASSIGN : opassign_op(opt);
  Type *v = expression();
  if (tt == T_ASSIGN) return v;
  op(BINOPS[tt]);
  return arith(tt, cur, v, line);
}

static Type *named_var(Tok *t, bool can_assign) {
  int arg, get, set;
  bool wide = false, checked;
  Type *ty = var_type(t, &checked);
  if ((arg = resolve_local(C, t)) != -1) { get = OP_GET_LOCAL; set = OP_SET_LOCAL; }
  else if ((arg = resolve_upvalue(C, t)) != -1) { get = OP_GET_UP; set = OP_SET_UP; }
  else {
    if (gtype_find(t->start, t->len) < 0 && map_find(vm.builtins, OBJ_V(intern(t->start, t->len))) < 0 &&
        !declared_in_file(t))
      undefined_name(t); /* caught before the program runs */
    arg = name_const(t);
    get = OP_GET_GLOBAL;
    set = OP_SET_GLOBAL;
    wide = true;
  }
  int o = get;
  if (can_assign && (at(T_ASSIGN) || at(T_OPASSIGN))) {
    Tok *opt = advance();
    int line = cur_line;
    int li = get == OP_GET_LOCAL ? find_local(C, t) : -1;
    if (li >= 0 && C->locals[li].loop_var)
      compile_error(line, "'%.*s' is the loop variable; changing it doesn't change the list. "
                          "To change the list, loop over positions: for i in 0..len(xs) - 1 { xs[i] = ... }", t->len, t->start);
    if (opt->type == T_OPASSIGN) {
      op(get);
      if (wide) emit16(arg); else emit(arg);
    }
    Type *v = assigned_value(opt, ty, line);
    if (checked && !compat(ty, v))
      compile_error(line, "'%.*s' is %s, can't assign %s", t->len, t->start, tys(ty), tys(v));
    o = set;
  }
  op(o);
  if (wide) emit16(arg); else emit(arg);
  return ty;
}

static int infix_prec(TT t) {
  switch (t) {
    case T_PIPE: return P_PIPE;
    case T_OR: return P_OR;
    case T_AND: return P_AND;
    case T_EQ: case T_NE: case T_LT: case T_LE: case T_GT: case T_GE: return P_CMP;
    case T_DOTDOT: return P_RANGE;
    case T_BOR: return P_BOR;
    case T_BXOR: return P_BXOR;
    case T_BAND: return P_BAND;
    case T_SHL: case T_SHR: case T_USHR: return P_SHIFT;
    case T_PLUS: case T_MINUS: return P_TERM;
    case T_STAR: case T_SLASH: case T_PERCENT: return P_FACTOR;
    case T_LPAREN: case T_LBRACKET: case T_DOT: return P_CALL;
    default: return P_NONE;
  }
}

/* comma-separated expressions; records up to `max` types, and/or their join */
static int items(TT close, const char *what, Type **types, int max, Type **joined, bool *mixed) {
  int n = 0, open_line = cur_line;
  if (close == T_RBRACKET) /* [x * 2 for x in xs]: say so before complaining about x */
    for (int i = ti, depth = 0; i < ntoks && toks[i].type != T_EOF; i++) {
      TT k = toks[i].type;
      if (k == T_LBRACKET || k == T_LPAREN || k == T_LBRACE) depth++;
      else if (k == T_RBRACKET || k == T_RPAREN || k == T_RBRACE) { if (depth-- == 0) break; }
      else if (k == T_FOR && depth == 0)
        compile_error(toks[i].line, "Lumo has no list comprehensions: use map(xs, fn(x) { ... }) or a for loop");
    }
  skip_semis();
  while (!at(close)) {
    if (close == T_RPAREN && at(T_NAME) && toks[ti + 1].type == T_ASSIGN)
      compile_error(peek()->line, "Lumo has no named arguments: pass the values in order, like f(\"Ali\", 3)");
    Type *t = expression();
    if (at(T_FOR)) compile_error(peek()->line, "Lumo has no list comprehensions: use map(xs, fn(x) { ... }) or a for loop");
    if (n < max) types[n] = t;
    if (joined) *joined = join_items(*joined, t, mixed);
    if (++n > 0xffff) compile_error(cur_line, "too many items");
    bool newline = at(T_SEMI);
    skip_semis();
    if (!at(close) && (at(T_RBRACE) || at(T_EOF) || at(T_LET) || at(T_FN) || (newline && !at(T_COMMA))))
      compile_error(open_line, "%s is missing: the bracket opened on this line is never closed", what);
    if (!at(close)) { expect(T_COMMA, "','"); skip_semis(); }
  }
  expect(close, what);
  return n;
}

static Type *check_call(Type *f, Type **args, int argc, int line) {
  if (argc > 255) compile_error(line, "too many arguments");
  op_n(OP_CALL, argc);
  if (!known(f)) return TANY;
  if (f->k != TY_FN) compile_error(line, "%s is not a function", tys(f));
  const char *name = f->name ? f->name : "fn";
  if (f->nparams >= 0) {
    if (argc != f->nparams)
      compile_error(line, "%s takes %d args, got %d%s", name, f->nparams, argc,
                    !strcmp(name, "random") ? " (random() gives a number from 0 to 1; a dice roll is floor(random() * 6) + 1)" : "");
    for (int i = 0; i < argc; i++)
      if (!compat(f->params[i], args[i])) {
        const char *h = f->params[i]->k == TY_STR && args[i]->k == TY_NUM ? " (str(x) turns a number into text)"
                      : f->params[i]->k == TY_NUM && args[i]->k == TY_STR ? " (num(s) turns text into a number)" : "";
        if (f->pnames) compile_error(line, "%s: %s should be %s, got %s%s", name, f->pnames[i], tys(f->params[i]), tys(args[i]), h);
        compile_error(line, "%s: argument %d should be %s, got %s%s", name, i + 1, tys(f->params[i]), tys(args[i]), h);
      }
  }
  return f->ret;
}

static Type *map_literal(void) {
  int n = 0;
  bool kmixed = false, vmixed = false;
  Type *kj = NULL, *vj = NULL;
  skip_semis();
  while (!at(T_RBRACE)) {
    if (at(T_NAME) && toks[ti + 1].type == T_COLON) { /* {name: 1} means {"name": 1} */
      Tok *k = advance();
      emit_const(OBJ_V(intern(k->start, k->len)));
      kj = join_items(kj, TSTR, &kmixed);
    } else {
      kj = join_items(kj, expression(), &kmixed);
    }
    expect(T_COLON, "':'");
    skip_semis();
    vj = join_items(vj, expression(), &vmixed);
    if (++n > 0xffff) compile_error(cur_line, "too many items");
    skip_semis();
    if (!at(T_RBRACE)) { expect(T_COMMA, "','"); skip_semis(); }
  }
  expect(T_RBRACE, "'}'");
  op(OP_MAP);
  emit16(n);
  C->height += 1 - 2 * n;
  Type *m = ty_map(kj ? kj : TANY, vj ? vj : TANY);
  m->mixed = vmixed;
  return m;
}

/* "a${x}b${y}c" compiles to "a" + str(x) + "b" + str(y) + "c" */
static Type *interpolation(Tok *first) {
  emit_const(OBJ_V(str_token(first)));
  for (;;) {
    expression();
    op(OP_TOSTR);
    op(OP_ADD);
    Tok *part = advance();
    if (part->type != T_STR && part->type != T_ISTR) {
      char d[64];
      describe(part, d, sizeof d);
      compile_error(part->line, "expected '}' to end ${...} in string, got %s", d);
    }
    emit_const(OBJ_V(str_token(part)));
    op(OP_ADD);
    if (part->type == T_STR) return TSTR;
  }
}

static void need_bool(Type *t, int line, const char *what) {
  if (t->k == TY_FN) compile_error(line, "%s needs a bool, got a function (did you forget () to call it?)", what);
  if (known(t) && t->k != TY_BOOL) compile_error(line, "%s needs a bool, got %s", what, tys(t));
}

static Type *prefix(Tok *t, bool can_assign) {
  switch (t->type) {
    case T_NUM: {
      char b[64];
      int n = t->len < 63 ? t->len : 63;
      memcpy(b, t->start, n);
      b[n] = 0;
      emit_const(NUM_V(strtod(b, NULL)));
      return TNUM;
    }
    case T_STR: emit_const(OBJ_V(str_token(t))); return TSTR;
    case T_ISTR: return interpolation(t);
    case T_TRUE: op(OP_TRUE); return TBOOL;
    case T_FALSE: op(OP_FALSE); return TBOOL;
    case T_NIL: op(OP_NIL); return TNIL;
    case T_NAME: return named_var(t, can_assign);
    case T_LPAREN: {
      skip_semis();
      Type *e = expression();
      skip_semis();
      expect(T_RPAREN, "')'");
      return e;
    }
    case T_LBRACKET: {
      Type *item = NULL;
      bool mixed = false;
      int n = items(T_RBRACKET, "']'", NULL, 0, &item, &mixed);
      op(OP_LIST);
      emit16(n);
      C->height += 1 - n;
      Type *l = ty_list(item ? item : TANY);
      l->mixed = mixed;
      return l;
    }
    case T_LBRACE: return map_literal();
    case T_BNOT: {
      Type *e = parse_prec(P_UNARY);
      if (known(e) && e->k != TY_NUM) compile_error(t->line, "can't do ~%s", tys(e));
      op(OP_BNOT);
      return TNUM;
    }
    case T_MINUS: {
      Type *e = parse_prec(P_UNARY);
      if (known(e) && e->k != TY_NUM) compile_error(t->line, "can't negate %s", tys(e));
      op(OP_NEG);
      return TNUM;
    }
    case T_NOT: { /* not a == b  means  not (a == b) */
      need_bool(parse_prec(P_CMP), t->line, "not");
      op(OP_NOT);
      return TBOOL;
    }
    case T_FN: return function(cstr("fn"), NULL);
    case T_IF: return if_expr();
    case T_TRY: return try_expr();
    default: {
      char d[64];
      describe(t, d, sizeof d);
      compile_error(t->line, "unexpected %s", d);
      return TANY;
    }
  }
}

/* after obj and key are compiled: either obj[key] or obj[key] = value */
static Type *index_tail(bool can_assign, Type *obj, Type *key, Tok *field, int line) {
  Type *item = TANY;
  if (obj->k == TY_STRUCT) {
    if (field) {
      int i = 0;
      while (i < obj->nparams && ((int)strlen(obj->pnames[i]) != field->len || memcmp(obj->pnames[i], field->start, field->len))) i++;
      if (i == obj->nparams) {
        Suggest s;
        suggest_init(&s, field->start, field->len);
        for (int j = 0; j < obj->nparams; j++) suggest_try(&s, obj->pnames[j], (int)strlen(obj->pnames[j]));
        compile_error(line, "%s has no field '%.*s'%s", obj->name, field->len, field->start, suggest_text(&s));
      }
      item = obj->params[i];
    } else if (!compat(TSTR, key)) {
      compile_error(line, "%s fields are named by str, got %s", obj->name, tys(key));
    }
    if (can_assign && (at(T_ASSIGN) || at(T_OPASSIGN))) {
      Tok *opt = advance();
      if (opt->type == T_OPASSIGN) { op(OP_DUP2); op(OP_GET_INDEX); }
      Type *v = assigned_value(opt, item, line);
      if (!compat(item, v)) {
        if (field) compile_error(line, "%s.%.*s is %s, can't assign %s", obj->name, field->len, field->start, tys(item), tys(v));
        compile_error(line, "can't assign %s to a field of %s", tys(v), obj->name);
      }
      op(OP_SET_INDEX);
      return v;
    }
    op(OP_GET_INDEX);
    return item;
  }
  if (obj->k == TY_LIST || obj->k == TY_STR) {
    if (field) {
      const char *m = method_hint(field->start, field->len);
      if (m) compile_error(line, "%s has no .%.*s: Lumo uses functions, write %s", tys(obj), field->len, field->start, m);
      compile_error(line, "%s items are numbered, not named: use [0], [1], ...", tys(obj));
    }
    if (known(key) && key->k != TY_NUM) compile_error(line, "index must be a num, got %s", tys(key));
    item = obj->k == TY_STR ? TSTR : obj->a;
  } else if (obj->k == TY_MAP) {
    if (!compat(obj->a, key)) compile_error(line, "%s keys are %s, got %s", tys(obj), tys(obj->a), tys(key));
    item = obj->b;
  } else if (known(obj) && obj->k != TY_STR) {
    compile_error(line, "can't index %s", tys(obj));
  }
  if (can_assign && (at(T_ASSIGN) || at(T_OPASSIGN))) {
    Tok *opt = advance();
    if (opt->type == T_OPASSIGN) { op(OP_DUP2); op(OP_GET_INDEX); }
    Type *v = assigned_value(opt, item, line);
    if (!compat(item, v)) compile_error(line, "%s holds %s, can't put %s in it", tys(obj), tys(item), tys(v));
    op(OP_SET_INDEX);
    return v;
  }
  op(OP_GET_INDEX);
  return item;
}

static Type *arith(TT tt, Type *l, Type *r, int line) {
  static const char *sym[] = {[T_PLUS] = "+", [T_MINUS] = "-", [T_STAR] = "*", [T_SLASH] = "/", [T_PERCENT] = "%",
                              [T_LT] = "<", [T_LE] = "<=", [T_GT] = ">", [T_GE] = ">="};
  if (tt == T_EQ || tt == T_NE) return TBOOL;
  if (tt >= T_BAND && tt <= T_USHR) { /* bitwise: 32-bit integers */
    static const char *bsym[] = {"&", "|", "^", "~", "<<", ">>", ">>>"};
    if ((known(l) && l->k != TY_NUM) || (known(r) && r->k != TY_NUM))
      compile_error(line, "can't do %s %s %s", tys(l), bsym[tt - T_BAND], tys(r));
    return TNUM;
  }
  bool cmp = tt == T_LT || tt == T_LE || tt == T_GT || tt == T_GE;
  bool ok;
  if (known(l) && known(r)) {
    ok = l->k == r->k && (l->k == TY_NUM || (l->k == TY_STR && (cmp || tt == T_PLUS)) || (l->k == TY_LIST && tt == T_PLUS));
  } else {
    Type *k = known(l) ? l : r; /* the other side is any */
    ok = !known(k) || k->k == TY_NUM || (k->k == TY_STR && (cmp || tt == T_PLUS)) || (k->k == TY_LIST && tt == T_PLUS);
  }
  if (!ok && tt == T_PLUS && ((l->k == TY_STR && r->k == TY_NUM) || (l->k == TY_NUM && r->k == TY_STR)))
    compile_error(line, "can't do %s + %s (to put a value into text, write \"...${x}...\" or use str(x))", tys(l), tys(r));
  if (!ok) compile_error(line, "can't do %s %s %s", tys(l), sym[tt], tys(r));
  if (cmp) return TBOOL;
  if (tt != T_PLUS) return TNUM;
  if (known(l) && known(r)) return l->k == TY_LIST ? ty_list(join(l->a, r->a)) : l;
  Type *k = known(l) ? l : r;
  return !known(k) ? TANY : k->k == TY_LIST ? ty_list(TANY) : k;
}

static Type *infix(Tok *t, bool can_assign, Type *left) {
  switch (t->type) {
    case T_AND: case T_OR: {
      const char *what = t->type == T_AND ? "and" : "or";
      need_bool(left, t->line, what);
      int j = emit_jump(t->type == T_AND ? OP_AND : OP_OR);
      need_bool(parse_prec(infix_prec(t->type) + 1), t->line, what);
      op(OP_CHECK_BOOL);
      patch_jump(j);
      return TBOOL;
    }
    case T_PIPE: { /* x |> f(a)  ==  f(x, a) */
      Type *f = named_var(expect(T_NAME, "function name after '|>'"), false);
      while (match(T_DOT)) {
        Tok *fld = expect(T_NAME, "field name after '.'");
        emit_const(OBJ_V(intern(fld->start, fld->len)));
        op(OP_GET_INDEX);
        f = f->k == TY_MAP ? f->b : TANY; /* ponytail: struct fields in pipes are any */
      }
      op(OP_SWAP);
      Type *args[256];
      args[0] = left;
      int argc = 1;
      if (match(T_LPAREN)) argc += items(T_RPAREN, "')'", args + 1, 255, NULL, NULL);
      return check_call(f, args, argc, t->line);
    }
    case T_DOTDOT: { /* 1..5 is [1, 2, 3, 4, 5]; empty when the end is before the start */
      Type *right = parse_prec(P_RANGE + 1);
      if ((known(left) && left->k != TY_NUM) || (known(right) && right->k != TY_NUM))
        compile_error(t->line, "can't do %s..%s (ranges are made of nums)", tys(left), tys(right));
      op(OP_RANGE);
      return ty_list(TNUM);
    }
    case T_LPAREN: {
      Type *args[256];
      int argc = items(T_RPAREN, "')'", args, 256, NULL, NULL);
      return check_call(left, args, argc, t->line);
    }
    case T_LBRACKET: {
      skip_semis();
      if (at(T_COLON)) compile_error(t->line, "Lumo has no [a:b] slices: use slice(xs, a, b)");
      Type *key = expression();
      skip_semis();
      if (at(T_COLON)) compile_error(t->line, "Lumo has no [a:b] slices: use slice(xs, a, b)");
      expect(T_RBRACKET, "']'");
      return index_tail(can_assign, left, key, NULL, t->line);
    }
    case T_DOT: { /* m.name  means  m["name"] */
      Tok *f = expect(T_NAME, "field name after '.'");
      emit_const(OBJ_V(intern(f->start, f->len)));
      return index_tail(can_assign, left, TSTR, f, t->line);
    }
    default: {
      Type *right = parse_prec(infix_prec(t->type) + 1);
      op(BINOPS[t->type]);
      return arith(t->type, left, right, t->line);
    }
  }
}

static Type *parse_prec(int prec) {
  bool can_assign = prec <= P_ASSIGN;
  Type *t = prefix(advance(), can_assign);
  while (prec <= infix_prec(peek()->type)) t = infix(advance(), can_assign, t);
  if ((at(T_NOT) && toks[ti + 1].type == T_IN) || (at(T_IN) && prec <= P_CMP))
    compile_error(peek()->line, "Lumo has no '%sin' operator: use %scontains(xs, x)", at(T_NOT) ? "not " : "", at(T_NOT) ? "not " : "");
  if (can_assign && (at(T_ASSIGN) || at(T_OPASSIGN))) compile_error(peek()->line, "can't assign to that");
  return t;
}

static Type *expression(void) { return parse_prec(P_ASSIGN); }

/* an if/while condition; `=` there is almost always a typo for `==` */
static void condition(int line) {
  Type *t = parse_prec(P_PIPE);
  if (at(T_ASSIGN)) compile_error(peek()->line, "use == to compare (= assigns a value)");
  need_bool(t, line, "condition");
}

static Type *if_expr(void) {
  int line = cur_line;
  condition(line);
  int jf = emit_jump(OP_JUMP_FALSE);
  int h = C->height;
  Type *then = block(), *other;
  int je = emit_jump(OP_JUMP);
  patch_jump(jf);
  C->height = h;
  if (match(T_ELSE)) {
    other = match(T_IF) ? if_expr() : block();
  } else {
    op(OP_NIL);
    other = TNIL;
  }
  patch_jump(je);
  return join(then, other);
}

/* try { ... } catch e { ... } : the value of whichever block ran */
static Type *try_expr(void) {
  int h = C->height;
  int jt = emit_jump(OP_TRY);
  C->try_depth++;
  Type *body = block();
  C->try_depth--;
  op(OP_END_TRY);
  int je = emit_jump(OP_JUMP);
  patch_jump(jt);
  C->height = h + 1; /* the VM pushes the error here */
  expect(T_CATCH, "'catch' after try block");
  begin_scope();
  if (at(T_NAME)) add_local(advance(), C->height - 1, TANY);
  else add_local_named("(err)", 5, C->height - 1, TANY);
  Type *handler = block();
  end_scope(true);
  patch_jump(je);
  return join(body, handler);
}

/* a block's value is its last expression; the block leaves exactly one value */
static Type *block(void) {
  if (at(T_COLON)) compile_error(peek()->line, "blocks use { ... }, not ':'");
  expect(T_LBRACE, "'{'");
  int open_line = cur_line;
  begin_scope();
  bool has_value = false;
  Type *last = TNIL;
  skip_semis();
  while (!at(T_RBRACE) && !at(T_EOF)) {
    if (has_value) op(OP_POP);
    has_value = statement(&last);
    if (!at(T_RBRACE)) expect(T_SEMI, "end of statement");
    skip_semis();
  }
  if (at(T_EOF)) compile_error(open_line, "this { is never closed (add a } where the block ends)");
  expect(T_RBRACE, "'}'");
  if (!has_value) { op(OP_NIL); last = TNIL; }
  end_scope(true);
  return last;
}

/* fn name(a: T, b) -> R { ... } ; *slot receives the signature before the body, for recursion */
static Type *function(Str *name, Type **slot) {
  Comp c;
  comp_init(&c, name);
  begin_scope();
  Type *ft = ty_new(TY_FN);
  ft->name = name->chars;
  ft->ret = TANY;
  ft->params = malloc(sizeof(Type *) * 256);
  ft->pnames = malloc(sizeof(char *) * 256);
  bool typed_params[256] = {0};
  expect(T_LPAREN, "'('");
  if (!at(T_RPAREN)) {
    do {
      Tok *p = expect(T_NAME, "parameter name");
      if (ft->nparams == 255) compile_error(cur_line, "too many parameters");
      Type *pt = TANY;
      if (match(T_COLON)) { pt = parse_type(); typed_params[ft->nparams] = true; }
      char *pname = malloc(p->len + 1);
      memcpy(pname, p->start, p->len);
      pname[p->len] = 0;
      ft->pnames[ft->nparams] = pname;
      ft->params[ft->nparams++] = pt;
      add_local(p, c.height++, pt); /* the caller pushed it */
    } while (match(T_COMMA));
  }
  c.fn->arity = ft->nparams;
  expect(T_RPAREN, "')'");
  if (match(T_ARROW)) c.ret = ft->ret = parse_type();
  if (slot) *slot = ft;
  for (int i = 0; i < ft->nparams; i++) /* callers may pass anything through `any` */
    if (typed_params[i] && known(ft->params[i])) {
      op(OP_CHECK_ARG);
      emit(i + 1);
      check_operands(ft->params[i], ft->pnames[i]);
    }
  Type *body = block();
  if (c.ret) {
    if (!compat(c.ret, body))
      compile_error(cur_line, "%s should return %s, got %s", name->chars, tys(c.ret), tys(body));
    if (!known(body)) {
      char what[96];
      snprintf(what, sizeof what, "return value of %s", name->chars);
      emit_check(c.ret, what);
    }
  }
  op(OP_RETURN);
  C = c.enclosing;
  op(OP_CLOSURE);
  emit16(make_const(OBJ_V(c.fn)));
  for (int i = 0; i < c.fn->nupvals; i++) {
    emit(c.ups[i].is_local);
    emit(c.ups[i].index);
  }
  return ft;
}

static void patch_breaks(Loop *l) {
  for (int i = 0; i < l->nbreaks; i++) patch_jump(l->breaks[i]);
}

static Type *item_type(Type *seq, int line) {
  if (seq->k == TY_LIST) return seq->a;
  if (seq->k == TY_STR) return TSTR;
  if (seq->k == TY_MAP) return seq->a;
  if (seq->k == TY_STRUCT) return TSTR;
  if (known(seq)) compile_error(line, "can't loop over %s", tys(seq));
  return TANY;
}

/* struct Point { x: num, y: num }  defines Point(x, y), which builds a map with exactly these fields */
static void struct_decl(void) {
  advance();
  Tok *n = advance();
  Str *name = intern(n->start, n->len);
  Type *st = ty_new(TY_STRUCT), *ctor = ty_new(TY_FN);
  st->name = ctor->name = name->chars;
  st->params = malloc(sizeof(Type *) * 256);
  st->pnames = malloc(sizeof(char *) * 256);
  ctor->ret = st;
  /* registered first, so fields can be of this same struct type (linked lists, trees) */
  if (is_global_scope()) gtype_set(n->start, n->len, ctor);
  else add_local(n, C->height, ctor);
  Tok *fields[256];
  expect(T_LBRACE, "'{'");
  skip_semis();
  while (!at(T_RBRACE)) {
    Tok *f = expect(T_NAME, "field name");
    for (int i = 0; i < st->nparams; i++)
      if ((int)strlen(st->pnames[i]) == f->len && !memcmp(st->pnames[i], f->start, f->len))
        compile_error(f->line, "duplicate field '%.*s'", f->len, f->start);
    if (st->nparams == 255) compile_error(f->line, "too many fields");
    char *fname = malloc(f->len + 1);
    memcpy(fname, f->start, f->len);
    fname[f->len] = 0;
    fields[st->nparams] = f;
    st->pnames[st->nparams] = fname;
    st->params[st->nparams++] = match(T_COLON) ? parse_type() : TANY;
    bool newline = at(T_SEMI);
    skip_semis();
    if (!at(T_RBRACE) && !newline) { expect(T_COMMA, "',' or a new line between fields"); skip_semis(); }
  }
  expect(T_RBRACE, "'}'");
  ctor->params = st->params;
  ctor->pnames = st->pnames;
  ctor->nparams = st->nparams;

  Comp c; /* the constructor: fn Point(x, y) { {x: x, y: y} tagged as Point } */
  comp_init(&c, name);
  begin_scope();
  for (int i = 0; i < st->nparams; i++) add_local(fields[i], c.height++, st->params[i]);
  c.fn->arity = st->nparams;
  for (int i = 0; i < st->nparams; i++)
    if (known(st->params[i])) {
      op(OP_CHECK_ARG);
      emit(i + 1);
      check_operands(st->params[i], st->pnames[i]);
    }
  for (int i = 0; i < st->nparams; i++) {
    emit_const(OBJ_V(cstr(st->pnames[i])));
    op(OP_GET_LOCAL);
    emit(i + 1);
  }
  op(OP_MAP);
  emit16(st->nparams);
  c.height += 1 - 2 * st->nparams;
  op(OP_STRUCT);
  emit16(make_const(OBJ_V(name)));
  op(OP_RETURN);
  C = c.enclosing;
  op(OP_CLOSURE);
  emit16(make_const(OBJ_V(c.fn)));
  if (is_global_scope()) { op(OP_DEF_GLOBAL); emit16(name_const(n)); }
}

/* returns true if the statement left a value on the stack, and its type in *out */
static bool statement(Type **out) {
  *out = TANY;
  if (match(T_LET)) {
    Tok *n = expect(T_NAME, "variable name");
    Type *decl = match(T_COLON) ? parse_type() : NULL;
    expect(T_ASSIGN, "'='");
    int line = cur_line;
    Type *v = expression();
    char name[64];
    snprintf(name, sizeof name, "%.*s", n->len, n->start);
    if (decl) {
      if (!compat(decl, v)) compile_error(line, "%s is declared %s, got %s", name, tys(decl), tys(v));
      if (!known(v)) emit_check(decl, name);
    }
    if (C->loop && !is_global_scope()) {
      bool exists = gtype_find(n->start, n->len) >= 0;
      for (Comp *c = C; c && !exists; c = c->enclosing) exists = find_local(c, n) >= 0;
      if (exists)
        compile_error(line, "'%s' already exists; inside a loop, let makes a new one every time around. "
                            "To change the existing one, write %s = ...", name, name);
    }
    /* without an annotation the variable takes the value's type; nil means "decide later" */
    define(n, decl ? decl : v->k == TY_NIL ? TANY : v);
    return false;
  }
  if (at(T_NAME) && peek()->len == 6 && !memcmp(peek()->start, "struct", 6) && toks[ti + 1].type == T_NAME &&
      toks[ti + 2].type == T_LBRACE) {
    struct_decl();
    return false;
  }
  if (at(T_FN) && toks[ti + 1].type == T_NAME) {
    advance();
    Tok *n = advance();
    Str *name = intern(n->start, n->len);
    if (is_global_scope()) {
      int g = gtype_set(n->start, n->len, TANY);
      function(name, &cur_types->types[g]); /* nothing in a body adds globals, so this stays valid */
      if (compiled_fns && compiled_file && !strcmp(cur_file->chars, compiled_file)) {
        int e = map_find(compiled_fns, OBJ_V(name));
        if (e >= 0) { /* lumo build --fast compiled this one to C */
          op(OP_POP);
          emit_const(compiled_fns->entries[e].val);
        }
      }
      op(OP_DEF_GLOBAL);
      emit16(name_const(n));
    } else {
      Local *l = add_local(n, C->height, TANY); /* declared first so it can call itself */
      function(name, &l->type);
    }
    return false;
  }
  if (match(T_RETURN)) {
    int line = cur_line;
    if (!C->enclosing) compile_error(line, "return outside a function");
    Type *v = TNIL;
    if (at(T_SEMI) || at(T_RBRACE)) op(OP_NIL);
    else v = expression();
    if (C->ret) {
      if (!compat(C->ret, v)) compile_error(line, "%s should return %s, got %s", C->fn->name->chars, tys(C->ret), tys(v));
      if (!known(v)) {
        char what[96];
        snprintf(what, sizeof what, "return value of %s", C->fn->name->chars);
        emit_check(C->ret, what);
      }
    }
    op(OP_RETURN); /* the VM drops any open try blocks of this call */
    return true;
  }
  if (at(T_BREAK) || at(T_CONTINUE)) {
    Tok *t = advance();
    Loop *l = C->loop;
    if (!l) compile_error(t->line, "%s outside a loop", t->type == T_BREAK ? "break" : "continue");
    int h = C->height;
    if (C->try_depth > l->try_depth) { op(OP_POP_HANDLERS); emit(C->try_depth - l->try_depth); }
    if (h > l->base) op_n(OP_POPN, h - l->base);
    if (t->type == T_CONTINUE) {
      emit_loop(l->start);
    } else {
      if (l->nbreaks == 256) compile_error(t->line, "too many breaks in one loop");
      l->breaks[l->nbreaks++] = emit_jump(OP_JUMP);
    }
    C->height = h + 1; /* unreachable from here; pretend it made a value */
    return true;
  }
  if (match(T_WHILE)) {
    int line = cur_line;
    Loop l = {.outer = C->loop, .start = chunk()->len, .base = C->height, .try_depth = C->try_depth};
    condition(line);
    int exit = emit_jump(OP_JUMP_FALSE);
    C->loop = &l;
    block();
    C->loop = l.outer;
    op(OP_POP);
    emit_loop(l.start);
    patch_jump(exit);
    patch_breaks(&l);
    op(OP_NIL);
    *out = TNIL;
    return true;
  }
  if (match(T_FOR)) {
    Tok *var = expect(T_NAME, "loop variable");
    expect(T_IN, "'in'");
    int line = cur_line;
    begin_scope();
    Type *it = item_type(expression(), line);
    op(OP_ITER);
    int seq = C->height - 1;
    add_local_named("(seq)", 5, seq, TANY);
    emit_const(NUM_V(0));
    add_local_named("(i)", 3, seq + 1, TANY);
    Loop l = {.outer = C->loop, .start = chunk()->len, .base = C->height, .try_depth = C->try_depth};
    op(OP_FOR_NEXT);
    emit(seq);
    emit16(0xffff);
    int exit = chunk()->len - 2;
    begin_scope();
    add_local(var, C->height - 1, it)->loop_var = true; /* fresh per iteration, so closures capture each value */
    C->loop = &l;
    block();
    C->loop = l.outer;
    op(OP_POP);
    end_scope(false);
    emit_loop(l.start);
    patch_jump(exit);
    patch_breaks(&l);
    end_scope(false);
    op(OP_NIL);
    *out = TNIL;
    return true;
  }
  if (match(T_IMPORT)) { /* import "lib/geo.lu" [as name]  binds the module's globals as a map */
    Tok *p = expect(T_STR, "a file path string");
    Str *path = str_token(p);
    const char *base = strrchr(path->chars, '/');
    base = base ? base + 1 : path->chars;
    const char *dot = strrchr(base, '.');
    Tok name = {T_NAME, base, dot ? (int)(dot - base) : (int)strlen(base), p->line};
    if (at(T_NAME) && peek()->len == 2 && !memcmp(peek()->start, "as", 2)) {
      advance();
      name = *expect(T_NAME, "module name after 'as'");
    } else {
      bool ok = name.len > 0 && !isdigit((unsigned char)base[0]);
      for (int i = 0; i < name.len && ok; i++) ok = name_char(base[i]);
      for (int k = 0; k < (int)(sizeof KEYWORDS / sizeof *KEYWORDS) && ok; k++)
        ok = !((int)strlen(KEYWORDS[k]) == name.len && !memcmp(KEYWORDS[k], base, name.len));
      if (!ok) compile_error(p->line, "can't name a module '%.*s'; write: import \"%s\" as name", name.len, base, path->chars);
    }
    path = resolve_import(cur_file->chars, path);
    op(OP_IMPORT);
    emit16(make_const(OBJ_V(path)));
    define(&name, ty_map(TSTR, TANY));
    return false;
  }
  *out = expression();
  return true;
}

/* a module returns its globals map; a script returns its last value */
static Fn *compile(const char *src, Str *name, Map *globals, bool module) {
  Comp c;
  GTypes mod_types = {0};
  C = NULL;
  cur_globals = globals;
  cur_file = name;
  cur_types = module ? &mod_types : &main_types;
  remember_source(name->chars, src);
  if (setjmp(compile_jmp)) {
    C = NULL;
    fprintf(stderr, "error: %s\n", compile_msg);
    show_source_line(name->chars, compile_err_line);
    return NULL;
  }
  scan(src);
  ti = 0;
  cur_line = 1;
  comp_init(&c, name);
  bool has_value = false;
  Type *last;
  skip_semis();
  while (!at(T_EOF)) {
    if (has_value) op(OP_POP);
    if (at(T_RBRACE)) compile_error(peek()->line, "unexpected '}': there's one } more than {, check the blocks above");
    has_value = statement(&last);
    if (at(T_RBRACE)) compile_error(peek()->line, "unexpected '}': there's one } more than {, check the blocks above");
    if (!at(T_EOF)) expect(T_SEMI, "end of statement");
    skip_semis();
  }
  if (module) {
    if (has_value) op(OP_POP);
    emit_const(OBJ_V(globals));
  } else if (!has_value) {
    op(OP_NIL);
  }
  op(OP_RETURN);
  C = NULL;
  for (int i = 0; i < mod_types.n; i++) free(mod_types.names[i]);
  free(mod_types.names);
  free(mod_types.types);
  return c.fn;
}

/* ---------- native compiler: lumo build --fast ---------- */

/* Generated names: lcf_<fn> (function), lcw_<fn> (VM wrapper), lcrt_* (helpers), v_<name> (locals).
 * Top-level functions whose parameters are num, bool or list (of nums) and whose result is num or bool
 * are translated to C here, compiled by the system C compiler together with this file, and called
 * instead of their bytecode. Anything outside that subset leaves the function interpreted. */

typedef enum { CT_NIL, CT_NUM, CT_BOOL, CT_LIST } CTy;
typedef struct { const char *name; int len; CTy ty; } CVar;
typedef struct {
  Tok *name;
  int fn_tok, nparams;
  CTy params[32];
  Tok *pnames[32];
  CTy ret;
  bool ok;
  char why[120];
} CFn;

static CFn cfns[256];
static int ncfns, ncvars, cg_loops;
static CVar cvars[512];
static CFn *cg_fn;
static char cg_why[120];

static void cg_fail(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(cg_why, sizeof cg_why, fmt, ap);
  va_end(ap);
  longjmp(compile_jmp, 1);
}

static void bputs(Buf *b, const char *s) { buf_add(b, s, (int)strlen(s)); }
static void bbuf(Buf *b, Buf *x) { if (x->len) buf_add(b, x->s, x->len); }
static const char *cty(CTy t) { return t == CT_BOOL ? "bool" : t == CT_LIST ? "List *" : "double"; }
static const char *cty_name(CTy t) { return t == CT_BOOL ? "bool" : t == CT_LIST ? "list" : t == CT_NUM ? "num" : "nothing"; }

/* Lumo names may contain any UTF-8 letter; C names may not */
static void cg_ident(Buf *b, const char *prefix, const char *s, int n) {
  bputs(b, prefix);
  for (int i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (isalnum(c) || c == '_') buf_add(b, (char *)&c, 1);
    else {
      char t[8];
      buf_add(b, t, snprintf(t, sizeof t, "_x%02X", c));
    }
  }
}
static void cg_var(Buf *b, const char *s, int n) { cg_ident(b, "v_", s, n); }

static int cg_find(Tok *t) {
  for (int i = ncvars - 1; i >= 0; i--)
    if (cvars[i].len == t->len && !memcmp(cvars[i].name, t->start, t->len)) return i;
  return -1;
}

static void cg_push(Tok *t, CTy ty) {
  if (ncvars == 512) cg_fail("has too many locals");
  cvars[ncvars++] = (CVar){t->start, t->len, ty};
}

static CTy cg_type(void) { /* num, bool, list, list[num] */
  Tok *t = advance();
  if (t->type == T_NAME && t->len == 3 && !memcmp(t->start, "num", 3)) return CT_NUM;
  if (t->type == T_NAME && t->len == 4 && !memcmp(t->start, "bool", 4)) return CT_BOOL;
  if (t->type == T_NAME && t->len == 4 && !memcmp(t->start, "list", 4)) {
    if (match(T_LBRACKET)) {
      Tok *e = advance();
      if (!(e->type == T_NAME && e->len == 3 && !memcmp(e->start, "num", 3))) cg_fail("uses a list of something other than nums");
      expect(T_RBRACKET, "']'");
    }
    return CT_LIST;
  }
  cg_fail("uses the type %.*s", t->len, t->start);
  return CT_NIL;
}

static bool cg_signature(CFn *f) { /* fn name(a: num, ...) -> num { */
  cg_why[0] = 0;
  if (setjmp(compile_jmp)) {
    snprintf(f->why, sizeof f->why, "%s", cg_why[0] ? cg_why : compile_msg);
    return false;
  }
  ti = f->fn_tok;
  advance();
  advance();
  expect(T_LPAREN, "'('");
  while (!at(T_RPAREN)) {
    Tok *p = expect(T_NAME, "parameter");
    if (!match(T_COLON)) cg_fail("parameter '%.*s' has no type", p->len, p->start);
    if (f->nparams == 32) cg_fail("has too many parameters");
    f->pnames[f->nparams] = p;
    f->params[f->nparams++] = cg_type();
    match(T_COMMA);
  }
  expect(T_RPAREN, "')'");
  if (!match(T_ARROW)) cg_fail("has no return type (-> num or -> bool)");
  f->ret = cg_type();
  if (f->ret == CT_LIST) cg_fail("returns a list");
  return true;
}

static CTy cg_prec(Buf *b, int prec);
static CTy cg_block(Buf *b);
static CTy cg_expr(Buf *b) { return cg_prec(b, P_PIPE); }

static CTy cg_call(Buf *b, Tok *name) {
  Buf args[32];
  CTy aty[32];
  int argc = 0;
  memset(args, 0, sizeof args);
  skip_semis();
  while (!at(T_RPAREN)) {
    if (argc == 32) cg_fail("passes too many arguments");
    aty[argc] = cg_expr(&args[argc]);
    argc++;
    skip_semis();
    if (!at(T_RPAREN)) { expect(T_COMMA, "','"); skip_semis(); }
  }
  expect(T_RPAREN, "')'");
#define IS(s) (name->len == (int)strlen(s) && !memcmp(name->start, s, name->len))
  static const char *math[][2] = {{"sqrt", "sqrt"}, {"floor", "floor"}, {"ceil", "ceil"}, {"round", "round"},
                                  {"trunc", "trunc"}, {"abs", "fabs"}, {"log", "log"}, {"sin", "sin"}, {"cos", "cos"}};
  for (int i = 0; i < (int)(sizeof math / sizeof *math); i++)
    if (IS(math[i][0]) && argc == 1 && aty[0] == CT_NUM) {
      bputs(b, math[i][1]);
      bputs(b, "(");
      bbuf(b, &args[0]);
      bputs(b, ")");
      return CT_NUM;
    }
  if ((IS("min") || IS("max") || IS("pow")) && argc == 2 && aty[0] == CT_NUM && aty[1] == CT_NUM) {
    bputs(b, "({ double _a = ");
    bbuf(b, &args[0]);
    bputs(b, ", _b = ");
    bbuf(b, &args[1]);
    bputs(b, IS("pow") ? "; pow(_a, _b); })" : IS("min") ? "; _a <= _b ? _a : _b; })" : "; _a >= _b ? _a : _b; })");
    return CT_NUM;
  }
  if (IS("len") && argc == 1 && aty[0] == CT_LIST) {
    bputs(b, "((double)(");
    bbuf(b, &args[0]);
    bputs(b, ")->len)");
    return CT_NUM;
  }
#undef IS
  for (int i = 0; i < ncfns; i++) {
    CFn *f = &cfns[i];
    if (!f->ok || f->name->len != name->len || memcmp(f->name->start, name->start, name->len)) continue;
    if (argc != f->nparams) cg_fail("calls %.*s with %d args", name->len, name->start, argc);
    for (int k = 0; k < argc; k++)
      if (aty[k] != f->params[k]) cg_fail("passes a %s to %.*s", cty_name(aty[k]), name->len, name->start);
    bputs(b, "({ ");
    bputs(b, cty(f->ret));
    bputs(b, " _r = ");
    cg_ident(b, "lcf_", name->start, name->len);
    bputs(b, "(");
    for (int k = 0; k < argc; k++) {
      if (k) bputs(b, ", ");
      bbuf(b, &args[k]);
    }
    bputs(b, "); if (__builtin_expect(lcrt_failed, 0)) return 0; _r; })");
    return f->ret;
  }
  cg_fail("calls %.*s", name->len, name->start);
  return CT_NIL;
}

static CTy cg_if(Buf *b) {
  Buf c = {0}, tb = {0}, eb = {0};
  if (cg_expr(&c) != CT_BOOL) cg_fail("has an if whose condition isn't a bool");
  CTy tt = cg_block(&tb), et = CT_NIL;
  bool has_else = match(T_ELSE);
  if (has_else) et = match(T_IF) ? cg_if(&eb) : cg_block(&eb);
  if (has_else && tt == et && tt != CT_NIL) {
    bputs(b, "(");
    bbuf(b, &c);
    bputs(b, " ? ");
    bbuf(b, &tb);
    bputs(b, " : ");
    bbuf(b, &eb);
    bputs(b, ")");
    return tt;
  }
  bputs(b, "({ if (");
  bbuf(b, &c);
  bputs(b, ") { (void)");
  bbuf(b, &tb);
  bputs(b, "; }");
  if (has_else) {
    bputs(b, " else { (void)");
    bbuf(b, &eb);
    bputs(b, "; }");
  }
  bputs(b, " 0; })");
  return CT_NIL;
}

static CTy cg_prefix(Buf *b) {
  Tok *t = advance();
  Buf e = {0};
  switch (t->type) {
    case T_NUM: {
      char lit[64], num[64];
      int n = t->len < 63 ? t->len : 63;
      memcpy(lit, t->start, n);
      lit[n] = 0;
      snprintf(num, sizeof num, "((double)%.17g)", strtod(lit, NULL));
      bputs(b, num);
      return CT_NUM;
    }
    case T_TRUE: bputs(b, "true"); return CT_BOOL;
    case T_FALSE: bputs(b, "false"); return CT_BOOL;
    case T_NAME: {
      if (match(T_LPAREN)) return cg_call(b, t);
      int i = cg_find(t);
      if (i < 0) cg_fail("uses '%.*s' from outside the function", t->len, t->start);
      cg_var(b, t->start, t->len);
      return cvars[i].ty;
    }
    case T_LPAREN: {
      skip_semis();
      CTy ty = cg_expr(&e);
      skip_semis();
      expect(T_RPAREN, "')'");
      bputs(b, "(");
      bbuf(b, &e);
      bputs(b, ")");
      return ty;
    }
    case T_MINUS:
      if (cg_prec(&e, P_UNARY) != CT_NUM) cg_fail("negates something that isn't a num");
      bputs(b, "(-");
      bbuf(b, &e);
      bputs(b, ")");
      return CT_NUM;
    case T_NOT:
      if (cg_prec(&e, P_CMP) != CT_BOOL) cg_fail("uses not on something that isn't a bool");
      bputs(b, "(!");
      bbuf(b, &e);
      bputs(b, ")");
      return CT_BOOL;
    case T_BNOT:
      if (cg_prec(&e, P_UNARY) != CT_NUM) cg_fail("uses ~ on something that isn't a num");
      bputs(b, "((double)~lcrt_i32(");
      bbuf(b, &e);
      bputs(b, "))");
      return CT_NUM;
    case T_IF: return cg_if(b);
    default: {
      char d[64];
      describe(t, d, sizeof d);
      cg_fail("uses %s", d);
      return CT_NIL;
    }
  }
}

static CTy cg_infix(Tok *t, Buf *l, CTy lt) {
  Buf r = {0}, out = {0};
  CTy rt, res = CT_NUM;
  TT k = t->type;
  if (k == T_LBRACKET) {
    if (lt != CT_LIST) cg_fail("indexes something that isn't a list");
    skip_semis();
    if (cg_expr(&r) != CT_NUM) cg_fail("indexes a list with something that isn't a num");
    skip_semis();
    expect(T_RBRACKET, "']'");
    bputs(&out, "({ double _v; if (!lcrt_get(");
    bbuf(&out, l);
    bputs(&out, ", ");
    bbuf(&out, &r);
    bputs(&out, ", &_v)) LCRT_FAIL(); _v; })");
  } else if (k == T_AND || k == T_OR) {
    rt = cg_prec(&r, infix_prec(k) + 1);
    if (lt != CT_BOOL || rt != CT_BOOL) cg_fail("uses and/or on something that isn't a bool");
    bputs(&out, "(");
    bbuf(&out, l);
    bputs(&out, k == T_AND ? " && " : " || ");
    bbuf(&out, &r);
    bputs(&out, ")");
    res = CT_BOOL;
  } else if (k == T_PLUS || k == T_MINUS || k == T_STAR || k == T_SLASH || k == T_PERCENT || k == T_LT || k == T_LE ||
             k == T_GT || k == T_GE || k == T_EQ || k == T_NE || (k >= T_BAND && k <= T_USHR && k != T_BNOT)) {
    rt = cg_prec(&r, infix_prec(k) + 1);
    bool eq = k == T_EQ || k == T_NE;
    if (eq ? (lt != rt || lt == CT_LIST || lt == CT_NIL) : (lt != CT_NUM || rt != CT_NUM))
      cg_fail("does %s %.*s %s", cty_name(lt), t->len, t->start, cty_name(rt));
    if (k == T_SLASH || k == T_PERCENT) {
      bputs(&out, "({ double _a = ");
      bbuf(&out, l);
      bputs(&out, ", _b = ");
      bbuf(&out, &r);
      bputs(&out, k == T_SLASH ? "; if (_b == 0) { nerr(\"division by zero\"); LCRT_FAIL(); } _a / _b; })"
                               : "; if (_b == 0) { nerr(\"division by zero\"); LCRT_FAIL(); } lcrt_mod(_a, _b); })");
    } else if (k >= T_BAND && k <= T_USHR) {
      static const char *fmt[][3] = {
          {"((double)(lcrt_i32(", ") & lcrt_i32(", ")))"},
          {"((double)(lcrt_i32(", ") | lcrt_i32(", ")))"},
          {"((double)(lcrt_i32(", ") ^ lcrt_i32(", ")))"},
          {"", "", ""},
          {"((double)(int32_t)((uint32_t)lcrt_i32(", ") << (lcrt_i32(", ") & 31)))"},
          {"((double)(lcrt_i32(", ") >> (lcrt_i32(", ") & 31)))"},
          {"((double)((uint32_t)lcrt_i32(", ") >> (lcrt_i32(", ") & 31)))"},
      };
      bputs(&out, fmt[k - T_BAND][0]);
      bbuf(&out, l);
      bputs(&out, fmt[k - T_BAND][1]);
      bbuf(&out, &r);
      bputs(&out, fmt[k - T_BAND][2]);
    } else {
      bputs(&out, "(");
      bbuf(&out, l);
      bputs(&out, " ");
      buf_add(&out, t->start, t->len);
      bputs(&out, " ");
      bbuf(&out, &r);
      bputs(&out, ")");
      if (k != T_PLUS && k != T_MINUS && k != T_STAR) res = CT_BOOL;
    }
  } else {
    cg_fail("uses %.*s", t->len, t->start);
  }
  free(l->s);
  free(r.s);
  *l = out;
  return res;
}

static CTy cg_prec(Buf *b, int prec) {
  Buf l = {0};
  CTy ty = cg_prefix(&l);
  while (prec <= infix_prec(peek()->type)) ty = cg_infix(advance(), &l, ty);
  bbuf(b, &l);
  free(l.s);
  return ty;
}

/* returns true for an expression statement, whose code (without ';') is in b */
static bool cg_stmt(Buf *b, CTy *ty) {
  Buf e = {0}, k = {0}, body = {0};
  *ty = CT_NIL;
  if (match(T_LET)) {
    Tok *n = expect(T_NAME, "name");
    CTy decl = match(T_COLON) ? cg_type() : CT_NIL;
    expect(T_ASSIGN, "'='");
    CTy et = cg_expr(&e);
    if (et == CT_NIL || (decl != CT_NIL && decl != et)) cg_fail("has a local that isn't a num, bool or list");
    bputs(b, cty(et));
    bputs(b, " ");
    cg_var(b, n->start, n->len);
    bputs(b, " = ");
    bbuf(b, &e);
    bputs(b, ";\n");
    cg_push(n, et);
    return false;
  }
  if (match(T_RETURN)) {
    if (at(T_SEMI) || at(T_RBRACE)) cg_fail("returns nothing");
    if (cg_expr(&e) != cg_fn->ret) cg_fail("returns something that isn't a %s", cty_name(cg_fn->ret));
    bputs(b, "({ return ");
    bbuf(b, &e);
    bputs(b, "; (");
    bputs(b, cty(cg_fn->ret));
    bputs(b, ")0; })");
    *ty = cg_fn->ret; /* never reached, but lets `return x` end a block of that type */
    return true;
  }
  if (at(T_BREAK) || at(T_CONTINUE)) {
    Tok *t = advance();
    if (!cg_loops) cg_fail("uses break outside a loop");
    bputs(b, t->type == T_BREAK ? "break;\n" : "continue;\n");
    return false;
  }
  if (match(T_WHILE)) {
    if (cg_expr(&e) != CT_BOOL) cg_fail("has a while condition that isn't a bool");
    cg_loops++;
    cg_block(&body);
    cg_loops--;
    bputs(b, "while (");
    bbuf(b, &e);
    bputs(b, ") { (void)");
    bbuf(b, &body);
    bputs(b, "; }\n");
    return false;
  }
  if (match(T_FOR)) {
    Tok *v = expect(T_NAME, "loop variable");
    expect(T_IN, "'in'");
    CTy seq = cg_prec(&e, P_RANGE + 1);
    bool range = match(T_DOTDOT);
    if (range && (seq != CT_NUM || cg_prec(&k, P_RANGE + 1) != CT_NUM)) cg_fail("has a range that isn't made of nums");
    if (!range && seq != CT_LIST) cg_fail("loops over something other than a range or a list");
    int mark = ncvars;
    cg_push(v, CT_NUM);
    cg_loops++;
    cg_block(&body);
    cg_loops--;
    ncvars = mark;
    if (range) { /* the loop variable is a fresh copy, so changing it doesn't change the loop */
      bputs(b, "{ double _a = ");
      bbuf(b, &e);
      bputs(b, ", _b = ");
      bbuf(b, &k);
      bputs(b, "; if (_a != floor(_a) || _b != floor(_b)) { nerr(\"ranges need whole numbers\"); LCRT_FAIL(); }\n"
               "for (double _k = _a; _k <= _b; _k++) { double ");
    } else {
      bputs(b, "{ List *_l = ");
      bbuf(b, &e);
      bputs(b, "; for (int _i = 0; _i < _l->len; _i++) {\n"
               "if (_l->items[_i].t != V_NUM) { nerr(\"expected a list of nums, got a %s\", type_name(_l->items[_i])); LCRT_FAIL(); }\n"
               "double _k = _l->items[_i].as.n; double ");
    }
    cg_var(b, v->start, v->len);
    bputs(b, " = _k; (void)");
    bbuf(b, &body);
    bputs(b, "; } }\n");
    return false;
  }
  if (at(T_NAME) && (toks[ti + 1].type == T_ASSIGN || toks[ti + 1].type == T_OPASSIGN)) {
    Tok *n = advance();
    Tok *opt = advance();
    int i = cg_find(n);
    if (i < 0) cg_fail("changes '%.*s' from outside the function", n->len, n->start);
    CTy et = cg_expr(&e);
    if (opt->type == T_OPASSIGN) { /* x op= e  as  x = x op (e) */
      if (cvars[i].ty != CT_NUM || et != CT_NUM) cg_fail("does %.*s on something that isn't a num", opt->len, opt->start);
      char c = opt->start[0];
      bputs(b, c == '/' || c == '%' ? "{ double _b = " : "");
      if (c == '/' || c == '%') {
        bbuf(b, &e);
        bputs(b, "; if (_b == 0) { nerr(\"division by zero\"); LCRT_FAIL(); } ");
      }
      cg_var(b, n->start, n->len);
      bputs(b, " = ");
      if (c == '%') bputs(b, "lcrt_mod(");
      cg_var(b, n->start, n->len);
      if (c == '/' || c == '%') bputs(b, c == '/' ? " / _b; }\n" : ", _b); }\n");
      else {
        buf_add(b, (char[]){' ', c, ' ', '('}, 4);
        bbuf(b, &e);
        bputs(b, ");\n");
      }
      return false;
    }
    if (et != cvars[i].ty) cg_fail("changes the type of '%.*s'", n->len, n->start);
    cg_var(b, n->start, n->len);
    bputs(b, " = ");
    bbuf(b, &e);
    bputs(b, ";\n");
    return false;
  }
  if (at(T_NAME) && toks[ti + 1].type == T_LBRACKET) { /* xs[i] = v ? */
    int depth = 0, j = ti + 1;
    for (; j < ntoks; j++) {
      if (toks[j].type == T_LBRACKET) depth++;
      if (toks[j].type == T_RBRACKET && --depth == 0) break;
    }
    if (j + 1 < ntoks && toks[j + 1].type == T_OPASSIGN) cg_fail("uses %.*s on a list item", toks[j + 1].len, toks[j + 1].start);
    if (j + 1 < ntoks && toks[j + 1].type == T_ASSIGN) {
      Tok *n = advance();
      int i = cg_find(n);
      if (i < 0 || cvars[i].ty != CT_LIST) cg_fail("writes into '%.*s', which isn't a local list", n->len, n->start);
      advance();
      if (cg_expr(&k) != CT_NUM) cg_fail("indexes a list with something that isn't a num");
      expect(T_RBRACKET, "']'");
      advance();
      if (cg_expr(&e) != CT_NUM) cg_fail("puts something that isn't a num into a list");
      bputs(b, "if (!lcrt_set(");
      cg_var(b, n->start, n->len);
      bputs(b, ", ");
      bbuf(b, &k);
      bputs(b, ", ");
      bbuf(b, &e);
      bputs(b, ")) LCRT_FAIL();\n");
      return false;
    }
  }
  *ty = cg_expr(b);
  return true;
}

/* a block is a GNU statement expression: ({ stmt; stmt; value; }) */
static CTy cg_block(Buf *b) {
  expect(T_LBRACE, "'{'");
  int mark = ncvars;
  Buf pend = {0};
  CTy pty = CT_NIL;
  bool has = false;
  bputs(b, "({ ");
  skip_semis();
  while (!at(T_RBRACE) && !at(T_EOF)) {
    if (has) {
      bputs(b, "(void)");
      bbuf(b, &pend);
      bputs(b, ";\n");
      free(pend.s);
      pend = (Buf){0};
      has = false;
    }
    Buf s = {0};
    CTy sty;
    if (cg_stmt(&s, &sty)) {
      pend = s;
      pty = sty;
      has = true;
    } else {
      bbuf(b, &s);
      free(s.s);
    }
    if (!at(T_RBRACE)) expect(T_SEMI, "end of statement");
    skip_semis();
  }
  expect(T_RBRACE, "'}'");
  CTy ty = CT_NIL;
  if (has) {
    bbuf(b, &pend);
    bputs(b, ";");
    ty = pty;
    free(pend.s);
  } else {
    bputs(b, "0;");
  }
  bputs(b, " })");
  ncvars = mark;
  return ty;
}

static void cg_header(Buf *b, CFn *f) {
  bputs(b, "static ");
  bputs(b, cty(f->ret));
  bputs(b, " ");
  cg_ident(b, "lcf_", f->name->start, f->name->len);
  bputs(b, "(");
  for (int i = 0; i < f->nparams; i++) {
    if (i) bputs(b, ", ");
    bputs(b, cty(f->params[i]));
    bputs(b, " ");
    cg_var(b, f->pnames[i]->start, f->pnames[i]->len);
  }
  if (!f->nparams) bputs(b, "void");
  bputs(b, ")");
}

static Buf cg_code; /* static: survives the longjmp out of a failed function */

static bool cg_function(CFn *f) {
  static size_t start;
  start = (size_t)cg_code.len;
  cg_why[0] = 0;
  if (setjmp(compile_jmp)) {
    snprintf(f->why, sizeof f->why, "%s", cg_why[0] ? cg_why : compile_msg);
    cg_code.len = (int)start;
    return false;
  }
  ti = f->fn_tok;
  while (!at(T_LBRACE)) advance(); /* skip the signature, already read */
  cg_fn = f;
  ncvars = cg_loops = 0;
  for (int i = 0; i < f->nparams; i++) cg_push(f->pnames[i], f->params[i]);
  Buf body = {0};
  CTy bt = cg_block(&body);
  if (bt != f->ret) cg_fail("doesn't always end with a %s", cty_name(f->ret));
  cg_header(&cg_code, f);
  bputs(&cg_code, " {\n  return ");
  bbuf(&cg_code, &body);
  bputs(&cg_code, ";\n}\n\n");
  free(body.s);
  return true;
}

/* translates the main file's eligible functions (its tokens are in toks); returns how many */
static int cgen_program(const char *path, Buf *out, bool quiet) {
  ncfns = 0;
  int depth = 0;
  for (int i = 0; i + 1 < ntoks; i++) {
    TT t = toks[i].type;
    if (t == T_LBRACE) depth++;
    else if (t == T_RBRACE) depth--;
    else if (t == T_FN && depth == 0 && toks[i + 1].type == T_NAME && ncfns < 256) {
      CFn *f = &cfns[ncfns++];
      memset(f, 0, sizeof *f);
      f->fn_tok = i;
      f->name = &toks[i + 1];
      f->ok = cg_signature(f);
    }
  }
  for (bool changed = true; changed;) { /* drop functions that can't be compiled, and their callers */
    changed = false;
    cg_code.len = 0;
    for (int i = 0; i < ncfns; i++)
      if (cfns[i].ok && !cg_function(&cfns[i])) { cfns[i].ok = false; changed = true; }
  }

  int n = 0;
  bputs(out, "\n/* ---------- generated by lumo build --fast ---------- */\n"
             "static bool lcrt_failed; /* set (with nerr) when compiled code hits a runtime error */\n"
             "#define LCRT_FAIL() do { lcrt_failed = true; return 0; } while (0)\n"
             "static int32_t lcrt_i32(double d) { return to_i32(d); }\n"
             "static double lcrt_mod(double a, double b) { return floored_mod(a, b); }\n"
             "static bool lcrt_index(List *l, double k, int *i) {\n"
             "  if (k != floor(k)) return nerr(\"index must be a whole number, got a fraction\");\n"
             "  if (k < -l->len || k >= l->len) return nerr(\"index %g out of range (length %d)\", k, l->len);\n"
             "  *i = (int)(k < 0 ? k + l->len : k); return true;\n}\n"
             "static bool lcrt_get(List *l, double k, double *out) {\n"
             "  int i; if (!lcrt_index(l, k, &i)) return false;\n"
             "  if (l->items[i].t != V_NUM) return nerr(\"expected a num in the list, got %s\", type_name(l->items[i]));\n"
             "  *out = l->items[i].as.n; return true;\n}\n"
             "static bool lcrt_set(List *l, double k, double v) {\n"
             "  int i; if (!lcrt_index(l, k, &i)) return false; l->items[i] = NUM_V(v); return true;\n}\n\n");
  for (int i = 0; i < ncfns; i++)
    if (cfns[i].ok) {
      cg_header(out, &cfns[i]);
      bputs(out, ";\n");
    }
  bbuf(out, &cg_code);
  for (int i = 0; i < ncfns; i++) { /* wrappers the VM calls: check the arguments, convert values */
    CFn *f = &cfns[i];
    if (!f->ok) continue;
    n++;
    bputs(out, "static bool ");
    cg_ident(out, "lcw_", f->name->start, f->name->len);
    bputs(out, "(int argc, Value *args, Value *out) {\n  (void)argc;\n");
    for (int k = 0; k < f->nparams; k++) {
      char line[256];
      const char *test = f->params[k] == CT_NUM ? "args[%d].t != V_NUM" : f->params[k] == CT_BOOL ? "args[%d].t != V_BOOL" : "!IS_OT(args[%d], O_LIST)";
      char cond[64];
      snprintf(cond, sizeof cond, test, k);
      snprintf(line, sizeof line, "  if (%s) return nerr(\"%.*s should be %s, got %%s\", type_name(args[%d]));\n", cond,
               f->pnames[k]->len, f->pnames[k]->start, cty_name(f->params[k]), k);
      bputs(out, line);
    }
    bputs(out, "  lcrt_failed = false;\n  ");
    bputs(out, cty(f->ret));
    bputs(out, " r = ");
    cg_ident(out, "lcf_", f->name->start, f->name->len);
    bputs(out, "(");
    for (int k = 0; k < f->nparams; k++) {
      char a[48];
      snprintf(a, sizeof a, f->params[k] == CT_NUM ? "%sargs[%d].as.n" : f->params[k] == CT_BOOL ? "%sargs[%d].as.b" : "%sAS_LIST(args[%d])", k ? ", " : "", k);
      bputs(out, a);
    }
    bputs(out, f->ret == CT_NUM ? ");\n  if (lcrt_failed) return false;\n  *out = NUM_V(r);\n  return true;\n}\n" : ");\n  if (lcrt_failed) return false;\n  *out = BOOL_V(r);\n  return true;\n}\n");
  }
  bputs(out, "\nstatic void register_compiled(void) {\n  compiled_file = \"");
  for (const char *p = path; *p; p++) { /* as a C string literal */
    if (*p == '"' || *p == '\\') buf_add(out, "\\", 1);
    buf_add(out, p, 1);
  }
  bputs(out, "\";\n");
  for (int i = 0; i < ncfns; i++) {
    CFn *f = &cfns[i];
    if (!f->ok) continue;
    char line[160];
    snprintf(line, sizeof line, "  def_compiled(\"%.*s\", ", f->name->len, f->name->start);
    bputs(out, line);
    cg_ident(out, "lcw_", f->name->start, f->name->len);
    snprintf(line, sizeof line, ", %d);\n", f->nparams);
    bputs(out, line);
  }
  bputs(out, "}\n");

  for (int i = 0; i < ncfns && !quiet; i++)
    if (cfns[i].ok) printf("  native:      %.*s\n", cfns[i].name->len, cfns[i].name->start);
  for (int i = 0; i < ncfns && !quiet; i++)
    if (!cfns[i].ok) printf("  interpreted: %.*s (it %s)\n", cfns[i].name->len, cfns[i].name->start, cfns[i].why);
  return n;
}

/* ---------- garbage collector ---------- */

static void mark_obj(Obj *o) {
  if (!o || o->marked) return;
  o->marked = true;
  if (vm.ngray == vm.cgray) {
    vm.cgray = vm.cgray ? vm.cgray * 2 : 256;
    vm.gray = realloc(vm.gray, sizeof(Obj *) * vm.cgray);
  }
  vm.gray[vm.ngray++] = o;
}
static void mark_val(Value v) { if (v.t == V_OBJ) mark_obj(v.as.o); }

static void blacken(Obj *o) {
  switch (o->type) {
    case O_LIST: {
      List *l = (List *)o;
      for (int i = 0; i < l->len; i++) mark_val(l->items[i]);
      break;
    }
    case O_MAP: {
      Map *m = (Map *)o;
      mark_obj((Obj *)m->sname);
      for (int i = 0; i < m->used; i++) { mark_val(m->entries[i].key); mark_val(m->entries[i].val); }
      break;
    }
    case O_FN: {
      Fn *f = (Fn *)o;
      mark_obj((Obj *)f->name);
      mark_obj((Obj *)f->globals);
      mark_obj((Obj *)f->file);
      for (int i = 0; i < f->chunk.nconsts; i++) mark_val(f->chunk.consts[i]);
      break;
    }
    case O_CLOSURE: {
      Closure *c = (Closure *)o;
      mark_obj((Obj *)c->fn);
      for (int i = 0; i < c->nup; i++) mark_obj((Obj *)c->up[i]);
      break;
    }
    case O_UPVAL: mark_val(((Upval *)o)->closed); break;
    default: break;
  }
}

static void free_obj(Obj *o) {
  switch (o->type) {
    case O_STR: mem(o, sizeof(Str) + ((Str *)o)->len + 1, 0); break;
    case O_LIST: {
      List *l = (List *)o;
      mem(l->items, sizeof(Value) * l->cap, 0);
      mem(o, sizeof(List), 0);
      break;
    }
    case O_MAP: {
      Map *m = (Map *)o;
      mem(m->entries, sizeof(Entry) * m->icap, 0);
      mem(m->index, sizeof(int) * m->icap, 0);
      mem(o, sizeof(Map), 0);
      break;
    }
    case O_FN: {
      Chunk *c = &((Fn *)o)->chunk;
      mem(c->code, c->cap, 0);
      mem(c->lines, sizeof(int) * c->cap, 0);
      mem(c->consts, sizeof(Value) * c->cconsts, 0);
      mem(o, sizeof(Fn), 0);
      break;
    }
    case O_CLOSURE: mem(o, sizeof(Closure) + sizeof(Upval *) * ((Closure *)o)->nup, 0); break;
    case O_UPVAL: mem(o, sizeof(Upval), 0); break;
    case O_NATIVE: mem(o, sizeof(Native), 0); break;
  }
}

/* only called at safe points in the VM loop, where every live value is reachable from a root */
static void collect(void) {
  for (Value *v = vm.stack; v < vm.sp; v++) mark_val(*v);
  for (int i = 0; i < vm.nframes; i++) mark_obj((Obj *)vm.frames[i].cl);
  for (Upval *u = vm.open; u; u = u->next) mark_obj((Obj *)u);
  mark_obj((Obj *)vm.globals);
  mark_obj((Obj *)vm.builtins);
  mark_obj((Obj *)vm.modules);
  mark_obj((Obj *)compiled_fns);
  while (vm.ngray) blacken(vm.gray[--vm.ngray]);
  for (int i = 0; i < vm.cstrs; i++) {
    Str *s = vm.strs[i];
    if (s && s != TOMB && !s->obj.marked) vm.strs[i] = TOMB;
  }
  Obj **p = &vm.objects;
  while (*p) {
    Obj *o = *p;
    if (o->marked) { o->marked = false; p = &o->next; }
    else { *p = o->next; free_obj(o); }
  }
  vm.next_gc = vm.bytes * 2 < (1u << 20) ? (1u << 20) : vm.bytes * 2;
}

/* ---------- virtual machine ---------- */

static inline void push(Value v) { *vm.sp++ = v; }
static inline Value pop(void) { return *--vm.sp; }
static inline Value peekv(int d) { return vm.sp[-1 - d]; }

static void close_upvalues(Value *last) {
  while (vm.open && vm.open->loc >= last) {
    Upval *u = vm.open;
    u->closed = *u->loc;
    u->loc = &u->closed;
    vm.open = u->next;
  }
}

static Upval *capture(Value *loc) {
  Upval *prev = NULL, *u = vm.open;
  while (u && u->loc > loc) { prev = u; u = u->next; }
  if (u && u->loc == loc) return u;
  Upval *n = (Upval *)new_obj(sizeof(Upval), O_UPVAL);
  n->loc = loc;
  n->closed = NIL_V;
  n->next = u;
  if (prev) prev->next = n;
  else vm.open = n;
  return n;
}

static int frame_line(Frame *f) {
  Chunk *c = &f->cl->fn->chunk;
  int i = (int)(f->ip - c->code) - 1;
  return c->lines[i < 0 ? 0 : i];
}

/* errors are values: set vm.err here, then the VM unwinds to the nearest catch */
static void throw_value(Value v) {
  vm.err = v;
  vm.err_line = frame_line(&vm.frames[vm.nframes - 1]);
  vm.err_file = vm.frames[vm.nframes - 1].cl->fn->file;
}

static void runtime_error(const char *fmt, ...) {
  char msg[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  throw_value(OBJ_V(cstr(msg)));
}

static void report_uncaught(void) {
  Buf b = {0};
  show(&b, vm.err, false, 0);
  fprintf(stderr, "error: line %d: %s\n", vm.err_line, b.s);
  if (vm.err_file) show_source_line(vm.err_file->chars, vm.err_line);
  free(b.s);
  if (vm.nframes > 1)
    for (int i = vm.nframes - 1; i >= 0; i--)
      fprintf(stderr, "  in %s (line %d)\n", vm.frames[i].cl->fn->name->chars, frame_line(&vm.frames[i]));
  vm.sp = vm.stack;
  vm.nframes = 0;
  vm.open = NULL;
  vm.nhandlers = 0;
}

static bool call_value(Value callee, int argc) {
  if (IS_OT(callee, O_CLOSURE)) {
    Closure *cl = (Closure *)callee.as.o;
    if (argc != cl->fn->arity) {
      runtime_error("%s takes %d args, got %d", cl->fn->name->chars, cl->fn->arity, argc);
      return false;
    }
    if (vm.nframes == FRAMES_MAX || vm.sp - vm.stack > STACK_MAX - 1024) {
      runtime_error("too much recursion");
      return false;
    }
    Frame *f = &vm.frames[vm.nframes++];
    f->cl = cl;
    f->ip = cl->fn->chunk.code;
    f->slots = vm.sp - argc - 1;
    return true;
  }
  if (IS_OT(callee, O_NATIVE)) {
    Native *n = (Native *)callee.as.o;
    if (argc < n->min || argc > n->max) {
      char use[160] = "";
      if (n->usage) snprintf(use, sizeof use, " (usage: %s)", n->usage);
      if (n->max == 255) runtime_error("%s takes at least %d args, got %d%s", n->name, n->min, argc, use);
      else if (n->min == n->max) runtime_error("%s takes %d args, got %d%s", n->name, n->min, argc, use);
      else runtime_error("%s takes %d to %d args, got %d%s", n->name, n->min, n->max, argc, use);
      return false;
    }
    Value out = NIL_V;
    if (!n->fn(argc, vm.sp - argc, &out)) {
      if (vm.has_thrown) { vm.has_thrown = false; throw_value(vm.thrown); }
      else if (n->plain || !strcmp(n->name, "assert")) runtime_error("%s", nerr_buf);
      else runtime_error("%s: %s", n->name, nerr_buf);
      return false;
    }
    vm.sp -= argc + 1;
    push(out);
    return true;
  }
  runtime_error("%s is not a function", type_name(callee));
  return false;
}

/* floored modulo like Python (-1 % 3 == 2); whole numbers take the fast integer path */
static inline double floored_mod(double a, double b) {
  if (a == (double)(int64_t)a && b == (double)(int64_t)b && fabs(a) < 9e15 && fabs(b) < 9e15) {
    int64_t r = (int64_t)a % (int64_t)b;
    if (r != 0 && ((r < 0) != (b < 0))) r += (int64_t)b;
    return (double)r;
  }
  double r = fmod(a, b);
  if (r != 0 && (r < 0) != (b < 0)) r += b;
  return r;
}

/* bitwise operators work on 32-bit integers, wrapping like C and JavaScript */
static int32_t to_i32(double d) {
  if (!isfinite(d)) return 0;
  double m = fmod(trunc(d), 4294967296.0);
  if (m < 0) m += 4294967296.0;
  return (int32_t)(uint32_t)m;
}

static Str *concat(Str *a, Str *b) {
  char *buf = malloc(a->len + b->len + 1);
  memcpy(buf, a->chars, a->len);
  memcpy(buf + a->len, b->chars, b->len);
  Str *s = intern(buf, a->len + b->len);
  free(buf);
  return s;
}

static bool run(int base) {
  Frame *f = &vm.frames[vm.nframes - 1];
  uint8_t *ip = f->ip;
#define RB() (*ip++)
#define R16() (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))
#define RC() (f->cl->fn->chunk.consts[R16()])
#define GLOBALS (f->cl->fn->globals)
#define ERR(...) do { f->ip = ip; runtime_error(__VA_ARGS__); goto on_error; } while (0)
#define NUMS(a, b, opname) \
  if ((a).t != V_NUM || (b).t != V_NUM) ERR("can't do %s %s %s", type_name(a), opname, type_name(b))
  for (;;) {
    switch (RB()) {
      case OP_CONST: push(RC()); break;
      case OP_NIL: push(NIL_V); break;
      case OP_TRUE: push(BOOL_V(true)); break;
      case OP_FALSE: push(BOOL_V(false)); break;
      case OP_POP: vm.sp--; break;
      case OP_POPN: {
        int n = RB();
        close_upvalues(vm.sp - n);
        vm.sp -= n;
        break;
      }
      case OP_ENDSCOPE: { /* drop n locals under the block's value */
        int n = RB();
        Value v = pop();
        close_upvalues(vm.sp - n);
        vm.sp -= n;
        push(v);
        break;
      }
      case OP_SWAP: {
        Value a = vm.sp[-1];
        vm.sp[-1] = vm.sp[-2];
        vm.sp[-2] = a;
        break;
      }
      case OP_DUP2: /* obj, key -> obj, key, obj, key (for xs[i] += v) */
        push(vm.sp[-2]);
        push(vm.sp[-2]);
        break;
      case OP_GET_LOCAL: push(f->slots[RB()]); break;
      case OP_SET_LOCAL: f->slots[RB()] = peekv(0); break;
      case OP_GET_UP: push(*f->cl->up[RB()]->loc); break;
      case OP_SET_UP: *f->cl->up[RB()]->loc = peekv(0); break;
      case OP_DEF_GLOBAL: {
        Value name = RC();
        if (map_find(GLOBALS, name) >= 0) ERR("'%s' is already declared", AS_STR(name)->chars);
        map_set(GLOBALS, name, peekv(0));
        vm.sp--;
        break;
      }
      case OP_GET_GLOBAL: {
        Value name = RC();
        Map *m = GLOBALS;
        int e = map_find(m, name);
        if (e < 0) e = map_find(m = vm.builtins, name);
        if (e < 0 && GLOBALS == vm.globals && declared_in_main(AS_STR(name)->chars))
          ERR("'%s' is used before its let runs (move `let %s = ...` above this line)", AS_STR(name)->chars, AS_STR(name)->chars);
        if (e < 0) ERR("'%s' is not defined (declare it with let)", AS_STR(name)->chars);
        push(m->entries[e].val);
        break;
      }
      case OP_SET_GLOBAL: {
        Value name = RC();
        int e = map_find(GLOBALS, name);
        if (e < 0) {
          if (map_find(vm.builtins, name) >= 0)
            ERR("'%s' is a builtin; use let to make your own", AS_STR(name)->chars);
          ERR("'%s' is not defined (declare it with let)", AS_STR(name)->chars);
        }
        GLOBALS->entries[e].val = peekv(0);
        break;
      }
      case OP_GET_INDEX: {
        Value k = pop(), o = pop(), out;
        if (!get_index(o, k, &out)) ERR("%s", nerr_buf);
        push(out);
        break;
      }
      case OP_SET_INDEX: {
        Value v = pop(), k = pop(), o = pop();
        if (!set_index(o, k, v)) ERR("%s", nerr_buf);
        push(v);
        break;
      }
      case OP_ADD: {
        Value b = pop(), a = pop();
        if (a.t == V_NUM && b.t == V_NUM) push(NUM_V(a.as.n + b.as.n));
        else if (IS_OT(a, O_STR) && IS_OT(b, O_STR)) push(OBJ_V(concat(AS_STR(a), AS_STR(b))));
        else if (IS_OT(a, O_LIST) && IS_OT(b, O_LIST)) {
          List *l = new_list();
          for (int i = 0; i < AS_LIST(a)->len; i++) list_push(l, AS_LIST(a)->items[i]);
          for (int i = 0; i < AS_LIST(b)->len; i++) list_push(l, AS_LIST(b)->items[i]);
          push(OBJ_V(l));
        } else if ((IS_OT(a, O_STR) && b.t == V_NUM) || (a.t == V_NUM && IS_OT(b, O_STR))) {
          ERR("can't do %s + %s (to put a value into text, write \"...${x}...\" or use str(x))", type_name(a), type_name(b));
        } else ERR("can't do %s + %s", type_name(a), type_name(b));
        break;
      }
      case OP_SUB: { Value b = pop(), a = pop(); NUMS(a, b, "-"); push(NUM_V(a.as.n - b.as.n)); break; }
      case OP_MUL: { Value b = pop(), a = pop(); NUMS(a, b, "*"); push(NUM_V(a.as.n * b.as.n)); break; }
      case OP_DIV: {
        Value b = pop(), a = pop();
        NUMS(a, b, "/");
        if (b.as.n == 0) ERR("division by zero");
        push(NUM_V(a.as.n / b.as.n));
        break;
      }
      case OP_MOD: { /* floored, like Python: -1 % 3 == 2 */
        Value b = pop(), a = pop();
        NUMS(a, b, "%");
        if (b.as.n == 0) ERR("division by zero");
        push(NUM_V(floored_mod(a.as.n, b.as.n)));
        break;
      }
      case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: case OP_USHR: {
        int bop = ip[-1];
        static const char *bnames[] = {"&", "|", "^", "<<", ">>", ">>>"};
        Value b = pop(), a = pop();
        if (a.t != V_NUM || b.t != V_NUM) ERR("can't do %s %s %s", type_name(a), bnames[bop - OP_BAND], type_name(b));
        int32_t x = to_i32(a.as.n), y = to_i32(b.as.n);
        double r = bop == OP_BAND ? x & y : bop == OP_BOR ? x | y : bop == OP_BXOR ? x ^ y
                 : bop == OP_SHL ? (int32_t)((uint32_t)x << (y & 31)) : bop == OP_SHR ? x >> (y & 31)
                 : (double)((uint32_t)x >> (y & 31));
        push(NUM_V(r));
        break;
      }
      case OP_RANGE: {
        Value b = pop(), a = pop();
        if (a.t != V_NUM || b.t != V_NUM) ERR("can't do %s..%s (ranges are made of nums)", type_name(a), type_name(b));
        if (!whole(a.as.n) || !whole(b.as.n)) ERR("ranges need whole numbers");
        double n = b.as.n - a.as.n + 1;
        if (n > 1e8) ERR("range too big");
        List *l = new_list();
        if (n > 0) {
          l->items = mem(NULL, 0, sizeof(Value) * (size_t)n);
          l->cap = (int)n;
          for (double x = a.as.n; x <= b.as.n; x++) l->items[l->len++] = NUM_V(x);
        }
        push(OBJ_V(l));
        break;
      }
      case OP_BNOT: {
        Value v = pop();
        if (v.t != V_NUM) ERR("can't do ~%s", type_name(v));
        push(NUM_V(~to_i32(v.as.n)));
        break;
      }
      case OP_NEG: {
        Value v = pop();
        if (v.t != V_NUM) ERR("can't negate %s", type_name(v));
        push(NUM_V(-v.as.n));
        break;
      }
      case OP_NOT: {
        Value v = pop();
        if (v.t != V_BOOL) ERR("not needs a bool, got %s", type_name(v));
        push(BOOL_V(!v.as.b));
        break;
      }
      case OP_EQ: { Value b = pop(), a = pop(); push(BOOL_V(values_equal(a, b))); break; }
      case OP_NE: { Value b = pop(), a = pop(); push(BOOL_V(!values_equal(a, b))); break; }
      case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
        int op = ip[-1];
        static const char *names[] = {"<", "<=", ">", ">="};
        Value b = pop(), a = pop();
        int c;
        if (a.t == V_NUM && b.t == V_NUM) c = (a.as.n > b.as.n) - (a.as.n < b.as.n);
        else if (IS_OT(a, O_STR) && IS_OT(b, O_STR)) c = str_cmp(AS_STR(a), AS_STR(b));
        else ERR("can't do %s %s %s", type_name(a), names[op - OP_LT], type_name(b));
        bool r = op == OP_LT ? c < 0 : op == OP_LE ? c <= 0 : op == OP_GT ? c > 0 : c >= 0;
        if (a.t == V_NUM && (isnan(a.as.n) || isnan(b.as.n))) r = false;
        push(BOOL_V(r));
        break;
      }
      case OP_JUMP: { uint16_t o = R16(); ip += o; break; }
      case OP_JUMP_FALSE: {
        uint16_t o = R16();
        Value c = pop();
        if (IS_OT(c, O_CLOSURE) || IS_OT(c, O_NATIVE)) ERR("condition needs a bool, got a function (did you forget () to call it?)");
        if (c.t != V_BOOL) ERR("condition needs a bool, got %s", type_name(c));
        if (!c.as.b) ip += o;
        break;
      }
      case OP_AND: case OP_OR: {
        bool is_and = ip[-1] == OP_AND;
        uint16_t o = R16();
        Value c = peekv(0);
        if (c.t != V_BOOL) ERR("%s needs a bool, got %s", is_and ? "and" : "or", type_name(c));
        if (c.as.b != is_and) ip += o; /* short-circuit, keep the value */
        else vm.sp--;
        break;
      }
      case OP_CHECK_BOOL:
        if (peekv(0).t != V_BOOL) ERR("and/or needs a bool, got %s", type_name(peekv(0)));
        break;
      case OP_LOOP: {
        uint16_t o = R16();
        ip -= o;
        if (vm.bytes > vm.next_gc) collect();
        break;
      }
      case OP_CALL: {
        int argc = RB();
        f->ip = ip;
        if (vm.bytes > vm.next_gc) collect();
        if (!call_value(peekv(argc), argc)) goto on_error;
        f = &vm.frames[vm.nframes - 1];
        ip = f->ip;
        break;
      }
      case OP_CLOSURE: {
        Fn *fn = (Fn *)RC().as.o;
        Closure *cl = new_closure(fn);
        push(OBJ_V(cl));
        for (int i = 0; i < fn->nupvals; i++) {
          int is_local = RB(), idx = RB();
          cl->up[i] = is_local ? capture(f->slots + idx) : f->cl->up[idx];
        }
        break;
      }
      case OP_RETURN: {
        Value r = pop();
        close_upvalues(f->slots);
        vm.nframes--;
        while (vm.nhandlers && vm.handlers[vm.nhandlers - 1].nframes > vm.nframes) vm.nhandlers--;
        vm.sp = f->slots;
        push(r);
        if (vm.nframes == base) return true;
        f = &vm.frames[vm.nframes - 1];
        ip = f->ip;
        break;
      }
      case OP_LIST: {
        int n = R16();
        List *l = new_list();
        if (n) {
          l->items = mem(NULL, 0, sizeof(Value) * n);
          l->cap = l->len = n;
          memcpy(l->items, vm.sp - n, sizeof(Value) * n);
        }
        vm.sp -= n;
        push(OBJ_V(l));
        break;
      }
      case OP_MAP: {
        int n = R16();
        Map *m = new_map();
        for (Value *p = vm.sp - 2 * n; p < vm.sp; p += 2) {
          if (!valid_key(p[0])) ERR("map keys must be str, num or bool, got %s", type_name(p[0]));
          map_set(m, p[0], p[1]);
        }
        vm.sp -= 2 * n;
        push(OBJ_V(m));
        break;
      }
      case OP_ITER: {
        Value v = peekv(0);
        if (IS_OT(v, O_MAP)) vm.sp[-1] = OBJ_V(map_keys(AS_MAP(v)));
        else if (!IS_OT(v, O_LIST) && !IS_OT(v, O_STR)) ERR("can't loop over %s", type_name(v));
        break;
      }
      case OP_FOR_NEXT: { /* (i) is a list index, or a byte offset into a string */
        int s = RB();
        uint16_t o = R16();
        Value seq = f->slots[s];
        int i = (int)f->slots[s + 1].as.n;
        if (IS_OT(seq, O_LIST)) {
          if (i >= AS_LIST(seq)->len) { ip += o; break; }
          push(AS_LIST(seq)->items[i]);
          f->slots[s + 1].as.n = i + 1;
        } else {
          Str *st = AS_STR(seq);
          if (i >= st->len) { ip += o; break; }
          int n = u8len(st->chars[i]);
          if (i + n > st->len) n = st->len - i;
          push(OBJ_V(intern(st->chars + i, n)));
          f->slots[s + 1].as.n = i + n;
        }
        break;
      }
      case OP_IMPORT: {
        Value path = RC();
        int e = map_find(vm.modules, path);
        if (e >= 0) { push(vm.modules->entries[e].val); break; } /* each module runs once */
        const char *packed = embedded_source(AS_STR(path)->chars);
        char *src = packed ? strdup(packed) : read_file(AS_STR(path)->chars);
        if (!src) ERR("can't read '%s'", AS_STR(path)->chars);
        Map *globals = new_map();
        Fn *fn = compile(src, AS_STR(path), globals, true);
        free(src);
        if (!fn) ERR("in import '%s'", AS_STR(path)->chars);
        map_set(vm.modules, path, OBJ_V(globals)); /* before running, so circular imports see it */
        Closure *cl = new_closure(fn);
        push(OBJ_V(cl));
        f->ip = ip;
        if (!call_value(OBJ_V(cl), 0)) goto on_error;
        f = &vm.frames[vm.nframes - 1];
        ip = f->ip;
        break;
      }
      case OP_TOSTR:
        if (!IS_OT(peekv(0), O_STR)) {
          Buf b = {0};
          show(&b, peekv(0), false, 0);
          vm.sp[-1] = OBJ_V(intern(b.s, b.len));
          free(b.s);
        }
        break;
      case OP_TRY: {
        uint16_t o = R16();
        if (vm.nhandlers == 256) ERR("too many nested try blocks");
        vm.handlers[vm.nhandlers].nframes = vm.nframes;
        vm.handlers[vm.nhandlers].sp = vm.sp;
        vm.handlers[vm.nhandlers++].ip = ip + o;
        break;
      }
      case OP_END_TRY: vm.nhandlers--; break;
      case OP_STRUCT: AS_MAP(peekv(0))->sname = AS_STR(RC()); break;
      case OP_CHECK:
      case OP_CHECK_ARG: { /* a value reaching an annotated boundary through `any` */
        Value v = ip[-1] == OP_CHECK ? peekv(0) : f->slots[*ip++];
        int k = RB();
        Value what = RC();
        static const char *names[] = {"any", "num", "str", "bool", "nil", "list", "map", "fn"};
        Str *sname = k == 8 ? AS_STR(RC()) : NULL; /* struct: also compare the name */
        bool ok = k == 1 ? v.t == V_NUM : k == 2 ? IS_OT(v, O_STR) : k == 3 ? v.t == V_BOOL : k == 4 ? v.t == V_NIL
                : k == 5 ? IS_OT(v, O_LIST) : k == 6 ? IS_OT(v, O_MAP) : k == 7 ? IS_OT(v, O_CLOSURE) || IS_OT(v, O_NATIVE)
                : k == 8 ? IS_OT(v, O_MAP) && AS_MAP(v)->sname == sname
                : true;
        if (!ok) ERR("%s should be %s, got %s", AS_STR(what)->chars, sname ? sname->chars : names[k], type_name(v));
        break;
      }
      case OP_POP_HANDLERS: vm.nhandlers -= RB(); break;
    }
    continue;

  on_error: /* unwind to the innermost try, or stop */
    if (!vm.nhandlers) {
      report_uncaught();
      return false;
    }
    vm.nhandlers--;
    close_upvalues(vm.handlers[vm.nhandlers].sp);
    vm.nframes = vm.handlers[vm.nhandlers].nframes;
    vm.sp = vm.handlers[vm.nhandlers].sp;
    push(vm.err);
    f = &vm.frames[vm.nframes - 1];
    ip = vm.handlers[vm.nhandlers].ip;
  }
#undef RB
#undef R16
#undef RC
#undef GLOBALS
#undef ERR
#undef NUMS
}

/* ---------- builtins ---------- */

#define NEED(i, ot, tname) \
  if (!IS_OT(args[i], ot)) return nerr("expected " tname ", got %s%s", type_name(args[i]), \
                                       ot == O_STR && args[i].t == V_NUM ? " (str(x) turns a number into text)" : "")
#define NEED_NUM(i) \
  if (args[i].t != V_NUM) return nerr("expected num, got %s", type_name(args[i]))

static bool n_write(int argc, Value *args, Value *out) { /* like print, without the newline */
  Buf b = {0};
  for (int i = 0; i < argc; i++) show(&b, args[i], false, 0);
  if (b.len) fwrite(b.s, 1, b.len, stdout);
  fflush(stdout);
  free(b.s);
  (void)out;
  return true;
}

static bool n_print(int argc, Value *args, Value *out) {
  Buf b = {0};
  for (int i = 0; i < argc; i++) {
    if (i) buf_add(&b, " ", 1);
    show(&b, args[i], false, 0);
  }
  buf_add(&b, "\n", 1);
  fwrite(b.s, 1, b.len, stdout);
  free(b.s);
  (void)out;
  return true;
}

static bool n_len(int argc, Value *args, Value *out) {
  (void)argc;
  if (IS_OT(args[0], O_STR)) *out = NUM_V(str_cplen(AS_STR(args[0])));
  else if (IS_OT(args[0], O_LIST)) *out = NUM_V(AS_LIST(args[0])->len);
  else if (IS_OT(args[0], O_MAP)) *out = NUM_V(AS_MAP(args[0])->count);
  else return nerr("expected str, list or map, got %s", type_name(args[0]));
  return true;
}

static bool n_push(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_LIST, "list");
  list_push(AS_LIST(args[0]), args[1]);
  *out = args[0];
  return true;
}

static bool n_pop(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_LIST, "list");
  List *l = AS_LIST(args[0]);
  if (!l->len) return nerr("list is empty");
  *out = l->items[--l->len];
  return true;
}

static int clamp_index(double d, int len) {
  if (d < 0) d += len;
  if (d < 0) d = 0;
  if (d > len) d = len;
  return (int)d;
}

static bool n_slice(int argc, Value *args, Value *out) {
  bool is_list = IS_OT(args[0], O_LIST);
  if (!is_list && !IS_OT(args[0], O_STR)) return nerr("expected list or str, got %s", type_name(args[0]));
  for (int k = 1; k < argc; k++)
    if (args[k].t != V_NUM || !whole(args[k].as.n)) return nerr("indexes must be whole numbers");
  int len = is_list ? AS_LIST(args[0])->len : str_cplen(AS_STR(args[0]));
  int i = clamp_index(args[1].as.n, len), j = argc > 2 ? clamp_index(args[2].as.n, len) : len;
  if (j < i) j = i;
  if (is_list) {
    List *l = new_list();
    for (int k = i; k < j; k++) list_push(l, AS_LIST(args[0])->items[k]);
    *out = OBJ_V(l);
  } else {
    Str *s = AS_STR(args[0]);
    int a = str_offset(s, i), b = str_offset(s, j);
    *out = OBJ_V(intern(s->chars + a, b - a));
  }
  return true;
}

static bool n_contains(int argc, Value *args, Value *out) {
  (void)argc;
  if (IS_OT(args[0], O_LIST)) {
    List *l = AS_LIST(args[0]);
    bool found = false;
    for (int i = 0; i < l->len && !found; i++) found = values_equal(l->items[i], args[1]);
    *out = BOOL_V(found);
  } else if (IS_OT(args[0], O_STR)) {
    NEED(1, O_STR, "str");
    *out = BOOL_V(strstr(AS_STR(args[0])->chars, AS_STR(args[1])->chars) != NULL);
  } else if (IS_OT(args[0], O_MAP)) {
    *out = BOOL_V(valid_key(args[1]) && map_find(AS_MAP(args[0]), args[1]) >= 0);
  } else {
    return nerr("expected list, str or map, got %s", type_name(args[0]));
  }
  return true;
}

static int cmp_vals(const void *x, const void *y) {
  const Value *a = x, *b = y;
  if (a->t == V_NUM) return (a->as.n > b->as.n) - (a->as.n < b->as.n);
  return str_cmp(AS_STR(*a), AS_STR(*b));
}

static bool n_sort(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_LIST, "list");
  List *l = AS_LIST(args[0]);
  for (int i = 0; i < l->len; i++) {
    bool ok = l->len && ((l->items[0].t == V_NUM && l->items[i].t == V_NUM) ||
                         (IS_OT(l->items[0], O_STR) && IS_OT(l->items[i], O_STR)));
    if (!ok) return nerr("can only sort a list of all nums or all strs");
  }
  qsort(l->items, l->len, sizeof(Value), cmp_vals);
  *out = args[0];
  return true;
}

static bool n_join(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_LIST, "list");
  NEED(1, O_STR, "str");
  List *l = AS_LIST(args[0]);
  Str *sep = AS_STR(args[1]);
  Buf b = {0};
  for (int i = 0; i < l->len; i++) {
    if (!IS_OT(l->items[i], O_STR)) {
      free(b.s);
      return nerr("item %d is a %s, not a str (use map(xs, str) first)", i, type_name(l->items[i]));
    }
    if (i) buf_add(&b, sep->chars, sep->len);
    buf_add(&b, AS_STR(l->items[i])->chars, AS_STR(l->items[i])->len);
  }
  *out = OBJ_V(intern(b.s ? b.s : "", b.len));
  free(b.s);
  return true;
}

static bool n_split(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_STR, "str");
  NEED(1, O_STR, "str");
  Str *s = AS_STR(args[0]), *sep = AS_STR(args[1]);
  if (!sep->len) return nerr("separator can't be empty");
  List *l = new_list();
  const char *p = s->chars, *end = s->chars + s->len;
  for (;;) {
    const char *q = strstr(p, sep->chars);
    if (!q) { list_push(l, OBJ_V(intern(p, (int)(end - p)))); break; }
    list_push(l, OBJ_V(intern(p, (int)(q - p))));
    p = q + sep->len;
  }
  *out = OBJ_V(l);
  return true;
}

static bool n_replace(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_STR, "str");
  NEED(1, O_STR, "str");
  NEED(2, O_STR, "str");
  Str *s = AS_STR(args[0]), *old = AS_STR(args[1]), *nw = AS_STR(args[2]);
  if (!old->len) return nerr("text to replace can't be empty");
  Buf b = {0};
  const char *p = s->chars, *end = s->chars + s->len, *q;
  while ((q = strstr(p, old->chars))) {
    buf_add(&b, p, (int)(q - p));
    buf_add(&b, nw->chars, nw->len);
    p = q + old->len;
  }
  buf_add(&b, p, (int)(end - p));
  *out = OBJ_V(intern(b.s, b.len));
  free(b.s);
  return true;
}

static bool change_case(Value *args, Value *out, int (*fn)(int)) {
  NEED(0, O_STR, "str");
  Str *s = AS_STR(args[0]);
  char *buf = malloc(s->len + 1);
  /* ponytail: ASCII only; non-ASCII letters (ğ, ü, ...) stay as they are */
  for (int i = 0; i < s->len; i++) buf[i] = (uint8_t)s->chars[i] < 0x80 ? (char)fn(s->chars[i]) : s->chars[i];
  *out = OBJ_V(intern(buf, s->len));
  free(buf);
  return true;
}
static bool n_upper(int argc, Value *args, Value *out) { (void)argc; return change_case(args, out, toupper); }
static bool n_lower(int argc, Value *args, Value *out) { (void)argc; return change_case(args, out, tolower); }

static bool n_trim(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_STR, "str");
  Str *s = AS_STR(args[0]);
  int a = 0, b = s->len;
  while (a < b && isspace((unsigned char)s->chars[a])) a++;
  while (b > a && isspace((unsigned char)s->chars[b - 1])) b--;
  *out = OBJ_V(intern(s->chars + a, b - a));
  return true;
}

static bool n_keys(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_MAP, "map");
  *out = OBJ_V(map_keys(AS_MAP(args[0])));
  return true;
}

static bool n_has(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_MAP, "map");
  *out = BOOL_V(valid_key(args[1]) && map_find(AS_MAP(args[0]), args[1]) >= 0);
  return true;
}

static bool n_get(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_MAP, "map");
  int e = valid_key(args[1]) ? map_find(AS_MAP(args[0]), args[1]) : -1;
  *out = e >= 0 ? AS_MAP(args[0])->entries[e].val : args[2];
  return true;
}

static bool n_remove(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_MAP, "map");
  if (AS_MAP(args[0])->sname) return nerr("can't remove fields from %s", AS_MAP(args[0])->sname->chars);
  *out = BOOL_V(valid_key(args[1]) && map_remove(AS_MAP(args[0]), args[1]));
  return true;
}

static bool n_str(int argc, Value *args, Value *out) {
  (void)argc;
  Buf b = {0};
  show(&b, args[0], false, 0);
  *out = OBJ_V(intern(b.s, b.len));
  free(b.s);
  return true;
}

static bool n_num(int argc, Value *args, Value *out) {
  (void)argc;
  if (args[0].t == V_NUM) { *out = args[0]; return true; }
  NEED(0, O_STR, "str");
  const char *s = AS_STR(args[0])->chars;
  char *end;
  double d = strtod(s, &end);
  while (isspace((unsigned char)*end)) end++;
  if (end == s || *end) return nerr("can't turn \"%s\" into a num", s);
  *out = NUM_V(d);
  return true;
}

static bool n_type(int argc, Value *args, Value *out) {
  (void)argc;
  *out = OBJ_V(cstr(type_name(args[0])));
  return true;
}

static bool n_range(int argc, Value *args, Value *out) {
  for (int i = 0; i < argc; i++)
    if (args[i].t != V_NUM || !whole(args[i].as.n)) return nerr("expected whole numbers");
  double a = argc > 1 ? args[0].as.n : 0, b = argc > 1 ? args[1].as.n : args[0].as.n;
  double step = argc > 2 ? args[2].as.n : 1;
  if (step == 0) return nerr("step can't be 0");
  double n = ceil((b - a) / step);
  if (n > 1e8) return nerr("range too big");
  List *l = new_list();
  if (n > 0) {
    l->items = mem(NULL, 0, sizeof(Value) * (size_t)n);
    l->cap = (int)n;
    for (double x = a; step > 0 ? x < b : x > b; x += step) l->items[l->len++] = NUM_V(x);
  }
  *out = OBJ_V(l);
  return true;
}

static bool math1(Value *args, Value *out, double (*fn)(double)) {
  NEED_NUM(0);
  *out = NUM_V(fn(args[0].as.n));
  return true;
}
static bool n_floor(int argc, Value *args, Value *out) { (void)argc; return math1(args, out, floor); }
static bool n_ceil(int argc, Value *args, Value *out) { (void)argc; return math1(args, out, ceil); }
static bool n_round(int argc, Value *args, Value *out) { (void)argc; return math1(args, out, round); }
static bool n_sqrt(int argc, Value *args, Value *out) { (void)argc; return math1(args, out, sqrt); }
static bool n_abs(int argc, Value *args, Value *out) { (void)argc; return math1(args, out, fabs); }

static bool n_min(int argc, Value *args, Value *out) {
  (void)argc;
  NEED_NUM(0);
  NEED_NUM(1);
  *out = args[0].as.n <= args[1].as.n ? args[0] : args[1];
  return true;
}
static bool n_max(int argc, Value *args, Value *out) {
  (void)argc;
  NEED_NUM(0);
  NEED_NUM(1);
  *out = args[0].as.n >= args[1].as.n ? args[0] : args[1];
  return true;
}

static bool n_random(int argc, Value *args, Value *out) {
  (void)argc;
  (void)args;
  *out = NUM_V(rand() / (RAND_MAX + 1.0));
  return true;
}

static bool n_clock(int argc, Value *args, Value *out) {
  (void)argc;
  (void)args;
  *out = NUM_V((double)clock() / CLOCKS_PER_SEC);
  return true;
}

static bool n_input(int argc, Value *args, Value *out) {
  if (argc) {
    NEED(0, O_STR, "str");
    fputs(AS_STR(args[0])->chars, stdout);
    fflush(stdout);
  }
  Buf b = {0};
  int c;
  while ((c = getchar()) != EOF && c != '\n') {
    char ch = (char)c;
    buf_add(&b, &ch, 1);
  }
  *out = c == EOF && !b.len ? NIL_V : OBJ_V(intern(b.s ? b.s : "", b.len));
  free(b.s);
  return true;
}

static bool n_read_file(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_STR, "str");
  char *s = read_file(AS_STR(args[0])->chars);
  if (!s) return nerr("can't read '%s' (exists(path) checks whether a file is there)", AS_STR(args[0])->chars);
  *out = OBJ_V(cstr(s));
  free(s);
  return true;
}

static bool n_write_file(int argc, Value *args, Value *out) {
  (void)argc;
  (void)out;
  NEED(0, O_STR, "str");
  NEED(1, O_STR, "str");
  FILE *f = fopen(AS_STR(args[0])->chars, "wb");
  if (!f) return nerr("can't write '%s'", AS_STR(args[0])->chars);
  fwrite(AS_STR(args[1])->chars, 1, AS_STR(args[1])->len, f);
  fclose(f);
  return true;
}

static bool n_assert(int argc, Value *args, Value *out) {
  (void)out;
  if (args[0].t != V_BOOL) return nerr("assert needs a bool, got %s", type_name(args[0]));
  if (args[0].as.b) return true;
  if (argc > 1) {
    Buf b = {0};
    show(&b, args[1], false, 0);
    nerr("assertion failed: %s", b.s);
    free(b.s);
    return false;
  }
  return nerr("assertion failed");
}

static bool n_error(int argc, Value *args, Value *out) { /* throws any value */
  (void)argc;
  (void)out;
  vm.thrown = args[0];
  vm.has_thrown = true;
  return false;
}

static bool n_exit(int argc, Value *args, Value *out) {
  (void)out;
  fflush(stdout);
  exit(argc && args[0].t == V_NUM ? (int)args[0].as.n : 0);
}

/* --- strings --- */

static int utf8_enc(uint32_t cp, char *out) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) { out[0] = (char)(0xC0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | cp >> 12);
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | cp >> 18);
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

static uint32_t utf8_dec(const char *s, int n) {
  uint8_t c = (uint8_t)s[0];
  int len = u8len(s[0]);
  if (len > n) len = 1;
  uint32_t cp = len == 1 ? c : len == 2 ? c & 0x1F : len == 3 ? c & 0x0F : c & 0x07;
  for (int i = 1; i < len; i++) cp = cp << 6 | ((uint8_t)s[i] & 0x3F);
  return cp;
}

static bool n_starts_with(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_STR, "str");
  NEED(1, O_STR, "str");
  Str *s = AS_STR(args[0]), *p = AS_STR(args[1]);
  *out = BOOL_V(p->len <= s->len && !memcmp(s->chars, p->chars, p->len));
  return true;
}

static bool n_ends_with(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_STR, "str");
  NEED(1, O_STR, "str");
  Str *s = AS_STR(args[0]), *p = AS_STR(args[1]);
  *out = BOOL_V(p->len <= s->len && !memcmp(s->chars + s->len - p->len, p->chars, p->len));
  return true;
}

static bool n_find(int argc, Value *args, Value *out) { /* character index, or -1 */
  (void)argc;
  NEED(0, O_STR, "str");
  NEED(1, O_STR, "str");
  Str *s = AS_STR(args[0]);
  const char *hit = strstr(s->chars, AS_STR(args[1])->chars);
  if (!hit) { *out = NUM_V(-1); return true; }
  int cp = 0;
  for (const char *p = s->chars; p < hit; p += u8len(*p)) cp++;
  *out = NUM_V(cp);
  return true;
}

static bool n_chars(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_STR, "str");
  Str *s = AS_STR(args[0]);
  List *l = new_list();
  for (int i = 0; i < s->len;) {
    int n = u8len(s->chars[i]);
    if (i + n > s->len) n = s->len - i;
    list_push(l, OBJ_V(intern(s->chars + i, n)));
    i += n;
  }
  *out = OBJ_V(l);
  return true;
}

static bool n_ord(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_STR, "str");
  Str *s = AS_STR(args[0]);
  if (!s->len) return nerr("empty string");
  *out = NUM_V(utf8_dec(s->chars, s->len));
  return true;
}

static bool n_chr(int argc, Value *args, Value *out) {
  (void)argc;
  NEED_NUM(0);
  double d = args[0].as.n;
  if (!whole(d) || d < 0 || d > 0x10FFFF) return nerr("not a character code: %g", d);
  char buf[4];
  *out = OBJ_V(intern(buf, utf8_enc((uint32_t)d, buf)));
  return true;
}

static bool n_repeat(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_STR, "str");
  NEED_NUM(1);
  Str *s = AS_STR(args[0]);
  double n = args[1].as.n;
  if (!whole(n) || n < 0) return nerr("count must be a whole number >= 0");
  if (n * s->len > 1e8) return nerr("result too big");
  Buf b = {0};
  for (int i = 0; i < (int)n; i++) buf_add(&b, s->chars, s->len);
  *out = OBJ_V(intern(b.s ? b.s : "", b.len));
  free(b.s);
  return true;
}

/* --- lists --- */

static bool n_reverse(int argc, Value *args, Value *out) { /* a new list or string */
  (void)argc;
  if (IS_OT(args[0], O_LIST)) {
    List *src = AS_LIST(args[0]), *l = new_list();
    for (int i = src->len - 1; i >= 0; i--) list_push(l, src->items[i]);
    *out = OBJ_V(l);
    return true;
  }
  NEED(0, O_STR, "list or str");
  Str *s = AS_STR(args[0]);
  char *buf = malloc(s->len + 1);
  for (int i = 0; i < s->len;) {
    int n = u8len(s->chars[i]);
    if (i + n > s->len) n = s->len - i;
    memcpy(buf + s->len - i - n, s->chars + i, n);
    i += n;
  }
  *out = OBJ_V(intern(buf, s->len));
  free(buf);
  return true;
}

static bool n_insert(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_LIST, "list");
  NEED_NUM(1);
  List *l = AS_LIST(args[0]);
  double d = args[1].as.n;
  if (!whole(d)) return nerr("index must be a whole number");
  if (d < 0) d += l->len;
  if (d < 0 || d > l->len) return nerr("index %g out of range (length %d)", args[1].as.n, l->len);
  list_push(l, NIL_V);
  int i = (int)d;
  memmove(l->items + i + 1, l->items + i, sizeof(Value) * (l->len - 1 - i));
  l->items[i] = args[2];
  *out = args[0];
  return true;
}

static bool n_index_of(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_LIST, "list");
  List *l = AS_LIST(args[0]);
  *out = NUM_V(-1);
  for (int i = 0; i < l->len; i++)
    if (values_equal(l->items[i], args[1])) { *out = NUM_V(i); break; }
  return true;
}

/* --- math --- */

static bool n_pow(int argc, Value *args, Value *out) {
  (void)argc;
  NEED_NUM(0);
  NEED_NUM(1);
  *out = NUM_V(pow(args[0].as.n, args[1].as.n));
  return true;
}
static bool n_log(int argc, Value *args, Value *out) { (void)argc; return math1(args, out, log); }
static bool n_trunc(int argc, Value *args, Value *out) { (void)argc; return math1(args, out, trunc); }

/* binary files as lists of byte values 0-255 */
static bool n_read_bytes(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_STR, "str");
  FILE *f = fopen(AS_STR(args[0])->chars, "rb");
  if (!f) return nerr("can't read '%s'", AS_STR(args[0])->chars);
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *buf = malloc(n > 0 ? n : 1);
  size_t got = fread(buf, 1, n, f);
  fclose(f);
  List *l = new_list();
  l->items = mem(NULL, 0, sizeof(Value) * (got ? got : 1));
  l->cap = got ? (int)got : 1;
  for (size_t i = 0; i < got; i++) l->items[l->len++] = NUM_V(buf[i]);
  free(buf);
  *out = OBJ_V(l);
  return true;
}

static bool n_bytes_str(int argc, Value *args, Value *out) { /* bytes_str(bytes, start, len): text up to the first 0 */
  (void)argc;
  NEED(0, O_LIST, "list");
  NEED_NUM(1);
  NEED_NUM(2);
  List *l = AS_LIST(args[0]);
  int start = (int)args[1].as.n, len = (int)args[2].as.n;
  if (start < 0 || len < 0 || start + len > l->len) return nerr("range out of bounds (length %d)", l->len);
  char buf[256];
  int n = 0;
  for (int i = 0; i < len && n < 255; i++) {
    Value v = l->items[start + i];
    int b = v.t == V_NUM ? (int)v.as.n & 255 : 0;
    if (!b) break;
    buf[n++] = (char)b;
  }
  *out = OBJ_V(intern(buf, n));
  return true;
}
static bool n_sin(int argc, Value *args, Value *out) { (void)argc; return math1(args, out, sin); }
static bool n_cos(int argc, Value *args, Value *out) { (void)argc; return math1(args, out, cos); }

/* --- JSON --- */

static void json_str(Buf *b, Str *s) {
  buf_add(b, "\"", 1);
  for (int i = 0; i < s->len; i++) {
    unsigned char c = (unsigned char)s->chars[i];
    char esc[8];
    if (c == '"') buf_add(b, "\\\"", 2);
    else if (c == '\\') buf_add(b, "\\\\", 2);
    else if (c == '\n') buf_add(b, "\\n", 2);
    else if (c == '\r') buf_add(b, "\\r", 2);
    else if (c == '\t') buf_add(b, "\\t", 2);
    else if (c < 0x20) buf_add(b, esc, snprintf(esc, sizeof esc, "\\u%04x", c));
    else buf_add(b, (char *)&c, 1);
  }
  buf_add(b, "\"", 1);
}

static bool json_enc(Buf *b, Value v, int depth) {
  char tmp[64];
  if (depth > 200) return nerr("nested too deeply");
  switch (v.t) {
    case V_NIL: buf_add(b, "null", 4); return true;
    case V_BOOL: v.as.b ? buf_add(b, "true", 4) : buf_add(b, "false", 5); return true;
    case V_NUM:
      if (!isfinite(v.as.n)) return nerr("can't encode %g", v.as.n);
      buf_add(b, tmp, fmt_num(tmp, v.as.n));
      return true;
    case V_OBJ: break;
  }
  if (IS_OT(v, O_STR)) { json_str(b, AS_STR(v)); return true; }
  if (IS_OT(v, O_LIST)) {
    List *l = AS_LIST(v);
    buf_add(b, "[", 1);
    for (int i = 0; i < l->len; i++) {
      if (i) buf_add(b, ",", 1);
      if (!json_enc(b, l->items[i], depth + 1)) return false;
    }
    buf_add(b, "]", 1);
    return true;
  }
  if (IS_OT(v, O_MAP)) {
    Map *m = AS_MAP(v);
    bool first = true;
    buf_add(b, "{", 1);
    for (int i = 0; i < m->used; i++) {
      Entry *e = &m->entries[i];
      if (e->key.t == V_NIL) continue;
      if (!IS_OT(e->key, O_STR)) return nerr("object keys must be str, got %s", type_name(e->key));
      if (!first) buf_add(b, ",", 1);
      first = false;
      json_str(b, AS_STR(e->key));
      buf_add(b, ":", 1);
      if (!json_enc(b, e->val, depth + 1)) return false;
    }
    buf_add(b, "}", 1);
    return true;
  }
  return nerr("can't encode %s", type_name(v));
}

static bool n_json_encode(int argc, Value *args, Value *out) {
  (void)argc;
  Buf b = {0};
  bool ok = json_enc(&b, args[0], 0);
  if (ok) *out = OBJ_V(intern(b.s, b.len));
  free(b.s);
  return ok;
}

typedef struct { const char *start, *p; } JP;
static bool jfail(JP *j) { return nerr("invalid JSON at position %d", (int)(j->p - j->start)); }
static void jws(JP *j) { while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') j->p++; }

static int hex4(const char *p) {
  int v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0';
    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    else return -1;
  }
  return v;
}

static bool jstr(JP *j, Value *out) {
  Buf b = {0};
  j->p++;
  while (*j->p != '"') {
    unsigned char c = (unsigned char)*j->p;
    if (c < 0x20) { free(b.s); return jfail(j); } /* includes end of input */
    if (c != '\\') { buf_add(&b, j->p++, 1); continue; }
    char e = *++j->p, ch = e;
    if (e == 'u') {
      int cp = hex4(j->p + 1);
      if (cp < 0) { free(b.s); return jfail(j); }
      j->p += 4;
      if (cp >= 0xD800 && cp < 0xDC00 && j->p[1] == '\\' && j->p[2] == 'u') { /* surrogate pair */
        int lo = hex4(j->p + 3);
        if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); j->p += 6; }
      }
      char u[4];
      buf_add(&b, u, utf8_enc((uint32_t)cp, u));
      j->p++;
      continue;
    }
    switch (e) {
      case '"': case '\\': case '/': break;
      case 'b': ch = '\b'; break;
      case 'f': ch = '\f'; break;
      case 'n': ch = '\n'; break;
      case 'r': ch = '\r'; break;
      case 't': ch = '\t'; break;
      default: free(b.s); return jfail(j);
    }
    buf_add(&b, &ch, 1);
    j->p++;
  }
  j->p++;
  *out = OBJ_V(intern(b.s ? b.s : "", b.len));
  free(b.s);
  return true;
}

static bool jval(JP *j, Value *out, int depth) {
  jws(j);
  if (depth > 200) return nerr("nested too deeply");
  char c = *j->p;
  if (c == '"') return jstr(j, out);
  if (c == '{') {
    Map *m = new_map();
    *out = OBJ_V(m);
    j->p++;
    jws(j);
    if (*j->p == '}') { j->p++; return true; }
    for (;;) {
      Value k, v;
      jws(j);
      if (*j->p != '"' || !jstr(j, &k)) return jfail(j);
      jws(j);
      if (*j->p != ':') return jfail(j);
      j->p++;
      if (!jval(j, &v, depth + 1)) return false;
      map_set(m, k, v);
      jws(j);
      if (*j->p == ',') { j->p++; continue; }
      if (*j->p == '}') { j->p++; return true; }
      return jfail(j);
    }
  }
  if (c == '[') {
    List *l = new_list();
    *out = OBJ_V(l);
    j->p++;
    jws(j);
    if (*j->p == ']') { j->p++; return true; }
    for (;;) {
      Value v;
      if (!jval(j, &v, depth + 1)) return false;
      list_push(l, v);
      jws(j);
      if (*j->p == ',') { j->p++; continue; }
      if (*j->p == ']') { j->p++; return true; }
      return jfail(j);
    }
  }
  if (!strncmp(j->p, "true", 4)) { j->p += 4; *out = BOOL_V(true); return true; }
  if (!strncmp(j->p, "false", 5)) { j->p += 5; *out = BOOL_V(false); return true; }
  if (!strncmp(j->p, "null", 4)) { j->p += 4; *out = NIL_V; return true; }
  if (c == '-' || isdigit((unsigned char)c)) {
    char *end;
    double d = strtod(j->p, &end);
    if (end == j->p) return jfail(j);
    j->p = end;
    *out = NUM_V(d);
    return true;
  }
  return jfail(j);
}

static bool n_json_decode(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_STR, "str");
  JP j = {AS_STR(args[0])->chars, AS_STR(args[0])->chars};
  if (!jval(&j, out, 0)) return false;
  jws(&j);
  return *j.p ? jfail(&j) : true;
}

/* --- time --- */

static bool n_now(int argc, Value *args, Value *out) {
  (void)argc;
  (void)args;
#ifdef _WIN32
  FILETIME ft; /* 100 ns steps since 1601 */
  GetSystemTimeAsFileTime(&ft);
  *out = NUM_V(((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 1e7 - 11644473600.0);
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  *out = NUM_V(ts.tv_sec + ts.tv_nsec / 1e9);
#endif
  return true;
}

static bool n_sleep(int argc, Value *args, Value *out) {
  (void)argc;
  (void)out;
  NEED_NUM(0);
  double s = args[0].as.n;
  if (s < 0) return nerr("can't sleep a negative time");
  fflush(stdout);
#ifdef _WIN32
  Sleep((DWORD)(s * 1000));
#else
  struct timespec ts = {(time_t)s, (long)((s - floor(s)) * 1e9)};
  nanosleep(&ts, NULL);
#endif
  return true;
}

static bool n_format_time(int argc, Value *args, Value *out) { /* strftime, local time */
  NEED_NUM(0);
  const char *fmt = "%Y-%m-%d %H:%M:%S";
  if (argc > 1) { NEED(1, O_STR, "str"); fmt = AS_STR(args[1])->chars; }
  time_t t = (time_t)args[0].as.n;
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[256];
  size_t n = strftime(buf, sizeof buf, fmt, &tm);
  *out = OBJ_V(intern(buf, (int)n));
  return true;
}

/* --- files & processes --- */

static bool n_exists(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_STR, "str");
  struct stat st;
  *out = BOOL_V(stat(AS_STR(args[0])->chars, &st) == 0);
  return true;
}

static bool n_list_dir(int argc, Value *args, Value *out) { /* sorted names, without . and .. */
  (void)argc;
  NEED(0, O_STR, "str");
  DIR *d = opendir(AS_STR(args[0])->chars);
  if (!d) return nerr("can't open directory '%s'", AS_STR(args[0])->chars);
  List *l = new_list();
  struct dirent *e;
  while ((e = readdir(d)))
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) list_push(l, OBJ_V(cstr(e->d_name)));
  closedir(d);
  qsort(l->items, l->len, sizeof(Value), cmp_vals);
  *out = OBJ_V(l);
  return true;
}

static bool n_append_file(int argc, Value *args, Value *out) {
  (void)argc;
  (void)out;
  NEED(0, O_STR, "str");
  NEED(1, O_STR, "str");
  FILE *f = fopen(AS_STR(args[0])->chars, "ab");
  if (!f) return nerr("can't write '%s'", AS_STR(args[0])->chars);
  fwrite(AS_STR(args[1])->chars, 1, AS_STR(args[1])->len, f);
  fclose(f);
  return true;
}

#ifdef _WIN32
/* Windows passes one command line that the program splits again, so each argument is quoted
 * the way the C runtime unquotes it: backslashes only matter before a quote */
static bool win_start(const char *dir, char *const argv[], HANDLE out, PROCESS_INFORMATION *pi) {
  Buf cmd = {0};
  for (int i = 0; argv[i]; i++) {
    if (i) buf_add(&cmd, " ", 1);
    if (*argv[i] && !strpbrk(argv[i], " \t\"")) { /* plain words stay unquoted: cmd /c mangles quotes */
      buf_add(&cmd, argv[i], (int)strlen(argv[i]));
      continue;
    }
    buf_add(&cmd, "\"", 1);
    for (const char *p = argv[i];; p++) {
      int slashes = 0;
      while (*p == '\\') slashes++, p++;
      if (*p == '"' || !*p) slashes *= 2; /* doubled before a quote (ours or theirs) */
      for (int k = 0; k < slashes + (*p == '"'); k++) buf_add(&cmd, "\\", 1);
      if (!*p) break;
      buf_add(&cmd, p, 1);
    }
    buf_add(&cmd, "\"", 1);
  }
  STARTUPINFOA si = {0};
  si.cb = sizeof si;
  if (out) {
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  }
  BOOL ok = CreateProcessA(NULL, cmd.s, NULL, NULL, TRUE, 0, NULL, dir, &si, pi);
  free(cmd.s);
  return ok;
}

static int win_wait(PROCESS_INFORMATION *pi) {
  DWORD code = 0;
  WaitForSingleObject(pi->hProcess, INFINITE);
  GetExitCodeProcess(pi->hProcess, &code);
  CloseHandle(pi->hProcess);
  CloseHandle(pi->hThread);
  return (int)code;
}
#endif

/* runs argv without a shell (in dir, if given); returns its exit status, 127 if it couldn't start */
static int spawn_wait(const char *dir, char *const argv[]) {
  fflush(stdout);
#ifdef _WIN32
  PROCESS_INFORMATION pi;
  return win_start(dir, argv, NULL, &pi) ? win_wait(&pi) : 127;
#else
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    if (dir && chdir(dir)) _exit(126);
    execvp(argv[0], argv);
    _exit(127);
  }
  int status;
  if (waitpid(pid, &status, 0) < 0) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static Value run_result(Buf *b, int code) { /* {output, code} */
  Map *m = new_map();
  map_set(m, OBJ_V(cstr("output")), OBJ_V(intern(b->s ? b->s : "", b->len)));
  map_set(m, OBJ_V(cstr("code")), NUM_V(code));
  free(b->s);
  return OBJ_V(m);
}

/* run(["prog", "arg", ...]): starts prog directly, so arguments are never parsed by a shell */
static bool run_argv(List *l, Value *out) {
  if (!l->len || l->len > 255) return nerr("give the program and its arguments: run([\"ls\", \"-l\"])");
  char *argv[256];
  for (int i = 0; i < l->len; i++) {
    if (!IS_OT(l->items[i], O_STR)) return nerr("arguments must be strs, got %s", type_name(l->items[i]));
    argv[i] = AS_STR(l->items[i])->chars;
  }
  argv[l->len] = NULL;
  Buf b = {0};
  char chunk[4096];
#ifdef _WIN32
  SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
  HANDLE rd, wr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return nerr("can't run '%s'", argv[0]);
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  fflush(stdout);
  PROCESS_INFORMATION pi;
  bool started = win_start(NULL, argv, wr, &pi);
  CloseHandle(wr);
  DWORD n;
  while (started && ReadFile(rd, chunk, sizeof chunk, &n, NULL) && n > 0) buf_add(&b, chunk, (int)n);
  CloseHandle(rd);
  *out = run_result(&b, started ? win_wait(&pi) : 127);
  return true;
#else
  int fds[2];
  if (pipe(fds)) return nerr("can't run '%s'", argv[0]);
  fflush(stdout);
  pid_t pid = fork();
  if (pid < 0) { close(fds[0]); close(fds[1]); return nerr("can't run '%s'", argv[0]); }
  if (pid == 0) {
    dup2(fds[1], 1);
    close(fds[0]);
    close(fds[1]);
    execvp(argv[0], argv);
    _exit(127);
  }
  close(fds[1]);
  ssize_t n;
  while ((n = read(fds[0], chunk, sizeof chunk)) > 0) buf_add(&b, chunk, (int)n);
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  *out = run_result(&b, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  return true;
#endif
}

/* run("shell command") or run([prog, args...]): {output: str, code: num} */
static bool n_run(int argc, Value *args, Value *out) {
  (void)argc;
  if (IS_OT(args[0], O_LIST)) return run_argv(AS_LIST(args[0]), out);
  NEED(0, O_STR, "str or list");
  fflush(stdout);
  FILE *p = popen(AS_STR(args[0])->chars, "r");
  if (!p) return nerr("can't run '%s'", AS_STR(args[0])->chars);
  Buf b = {0};
  char chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, p)) > 0) buf_add(&b, chunk, (int)n);
  int status = pclose(p);
  *out = run_result(&b, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  return true;
}

static bool n_env(int argc, Value *args, Value *out) {
  (void)argc;
  NEED(0, O_STR, "str");
  const char *v = getenv(AS_STR(args[0])->chars);
  *out = v ? OBJ_V(cstr(v)) : NIL_V;
  return true;
}

/* --- graphics: gfx.* (gfx_macos.m on macOS, gfx.c on Windows and Linux; plain `cc lumo.c` builds get stubs that say so) --- */

bool gfx_window(int w, int h, const char *title);
bool gfx_frame(void);
void gfx_clear(const double *c);
void gfx_rect(double x, double y, double w, double h, double radius, const double *c);
void gfx_circle(double x, double y, double r, const double *c);
void gfx_line(double x1, double y1, double x2, double y2, double width, const double *c);
void gfx_text(const char *s, double x, double y, double size, const double *c);
double gfx_text_width(const char *s, double size);
bool gfx_key(const char *name);
bool gfx_pressed(const char *name);
void gfx_mouse(double *x, double *y, bool *down, bool *clicked);
double gfx_delta(void);
bool gfx_sound(const char *name);
bool gfx_screenshot(const char *path);
void gfx_pixels(const unsigned char *rgba, int w, int h);

#ifndef LUMO_GFX /* never called: gfx.window refuses to open, so nothing else runs */
bool gfx_window(int w, int h, const char *title) { (void)w; (void)h; (void)title; return false; }
bool gfx_frame(void) { return false; }
void gfx_clear(const double *c) { (void)c; }
void gfx_rect(double x, double y, double w, double h, double radius, const double *c) { (void)x; (void)y; (void)w; (void)h; (void)radius; (void)c; }
void gfx_circle(double x, double y, double r, const double *c) { (void)x; (void)y; (void)r; (void)c; }
void gfx_line(double x1, double y1, double x2, double y2, double width, const double *c) { (void)x1; (void)y1; (void)x2; (void)y2; (void)width; (void)c; }
void gfx_text(const char *s, double x, double y, double size, const double *c) { (void)s; (void)x; (void)y; (void)size; (void)c; }
double gfx_text_width(const char *s, double size) { (void)s; (void)size; return 0; }
bool gfx_key(const char *name) { (void)name; return false; }
bool gfx_pressed(const char *name) { (void)name; return false; }
void gfx_mouse(double *x, double *y, bool *down, bool *clicked) { *x = *y = 0; *down = *clicked = false; }
double gfx_delta(void) { return 0; }
bool gfx_sound(const char *name) { (void)name; return false; }
bool gfx_screenshot(const char *path) { (void)path; return false; }
void gfx_pixels(const unsigned char *rgba, int w, int h) { (void)rgba; (void)w; (void)h; }
#endif

static bool gfx_open;
static long gfx_frames;

static void gfx_note_no_frames(void) { /* window opened, program ended without a game loop */
  if (gfx_open && !gfx_frames)
    fprintf(stderr, "note: the window closed right away; draw inside a loop: while gfx.frame() { ... }\n");
}
#define NEED_WINDOW() if (!gfx_open) return nerr("open a window first: gfx.window(800, 600)")

/* colors: a name ("red"), "#rrggbb" / "#rrggbbaa", or [r, g, b] / [r, g, b, a] with 0-255 parts */
static bool parse_color(Value v, double c[4]) {
  static const struct { const char *name; uint32_t rgb; } names[] = {
      {"black", 0x000000}, {"white", 0xffffff}, {"red", 0xef4444}, {"orange", 0xf97316}, {"yellow", 0xfacc15},
      {"green", 0x22c55e}, {"lime", 0x84cc16}, {"teal", 0x14b8a6}, {"cyan", 0x06b6d4}, {"blue", 0x3b82f6},
      {"navy", 0x1e3a8a}, {"purple", 0xa855f7}, {"magenta", 0xd946ef}, {"pink", 0xec4899}, {"brown", 0x92400e},
      {"gold", 0xf59e0b}, {"silver", 0xcbd5e1}, {"gray", 0x6b7280}, {"grey", 0x6b7280}, {"dark", 0x1f2937},
  };
  c[3] = 1;
  if (IS_OT(v, O_LIST)) {
    List *l = AS_LIST(v);
    if (l->len < 3 || l->len > 4) return nerr("a color list needs 3 or 4 numbers: [r, g, b] or [r, g, b, a]");
    for (int i = 0; i < l->len; i++) {
      if (l->items[i].t != V_NUM) return nerr("color parts must be numbers from 0 to 255");
      c[i] = fmin(fmax(l->items[i].as.n, 0), 255) / 255;
    }
    return true;
  }
  if (!IS_OT(v, O_STR)) return nerr("expected a color, got %s", type_name(v));
  const char *s = AS_STR(v)->chars;
  if (s[0] == '#' && (strlen(s) == 7 || strlen(s) == 9)) {
    unsigned long x = strtoul(s + 1, NULL, 16);
    if (strlen(s) == 7) x = x << 8 | 0xff;
    for (int i = 0; i < 4; i++) c[i] = ((x >> (24 - 8 * i)) & 0xff) / 255.0;
    return true;
  }
  for (int i = 0; i < (int)(sizeof names / sizeof *names); i++)
    if (!strcmp(names[i].name, s)) {
      for (int k = 0; k < 3; k++) c[k] = ((names[i].rgb >> (16 - 8 * k)) & 0xff) / 255.0;
      return true;
    }
  static const char *turkish[][2] = {{"kırmızı", "red"}, {"mavi", "blue"}, {"yeşil", "green"}, {"sarı", "yellow"},
                                     {"siyah", "black"}, {"beyaz", "white"}, {"turuncu", "orange"}, {"mor", "purple"},
                                     {"pembe", "pink"}, {"gri", "gray"}, {"lacivert", "navy"}, {"kahverengi", "brown"}};
  for (int i = 0; i < (int)(sizeof turkish / sizeof *turkish); i++)
    if (!strcmp(turkish[i][0], s)) return nerr("unknown color \"%s\" (color names are English: \"%s\")", s, turkish[i][1]);
  char low[32];
  int n = 0;
  for (; s[n] && n < 31; n++) low[n] = (char)tolower((unsigned char)s[n]);
  low[n] = 0;
  for (int i = 0; i < (int)(sizeof names / sizeof *names); i++)
    if (!strcmp(names[i].name, low)) return nerr("unknown color \"%s\" (color names are lowercase: \"%s\")", s, low);
  return nerr("unknown color \"%s\" (try \"red\", \"#ff8800\" or [255, 136, 0])", s);
}

static bool need_nums(Value *args, int from, int to) {
  for (int i = from; i < to; i++)
    if (args[i].t != V_NUM) return nerr("argument %d should be a num, got %s", i + 1, type_name(args[i]));
  return true;
}

static bool g_window(int argc, Value *args, Value *out) {
  (void)out;
  if (!need_nums(args, 0, 2)) return false;
  if (argc > 2) NEED(2, O_STR, "str");
  if (gfx_open) return nerr("a window is already open");
  if (args[0].as.n < 1 || args[1].as.n < 1 || args[0].as.n > 8000 || args[1].as.n > 8000) return nerr("window size out of range");
#ifndef LUMO_GFX
  return nerr("this lumo was built without graphics (build it with: make)");
#endif
  if (!gfx_window((int)args[0].as.n, (int)args[1].as.n, argc > 2 ? AS_STR(args[2])->chars : "Lumo"))
    return nerr("couldn't open a window");
  gfx_open = true;
  return true;
}

static bool g_frame(int argc, Value *args, Value *out) {
  (void)argc;
  (void)args;
  NEED_WINDOW();
  gfx_frames++;
  *out = BOOL_V(gfx_frame());
  return true;
}

static bool g_clear(int argc, Value *args, Value *out) {
  (void)argc;
  (void)out;
  double c[4];
  NEED_WINDOW();
  if (!parse_color(args[0], c)) return false;
  gfx_clear(c);
  return true;
}

static bool g_rect(int argc, Value *args, Value *out) { /* rect(x, y, w, h, color, radius?) */
  (void)out;
  double c[4];
  NEED_WINDOW();
  if (!need_nums(args, 0, 4) || !parse_color(args[4], c)) return false;
  if (argc > 5 && !need_nums(args, 5, 6)) return false;
  gfx_rect(args[0].as.n, args[1].as.n, args[2].as.n, args[3].as.n, argc > 5 ? args[5].as.n : 0, c);
  return true;
}

static bool g_circle(int argc, Value *args, Value *out) { /* circle(x, y, r, color) */
  (void)argc;
  (void)out;
  double c[4];
  NEED_WINDOW();
  if (!need_nums(args, 0, 3) || !parse_color(args[3], c)) return false;
  gfx_circle(args[0].as.n, args[1].as.n, args[2].as.n, c);
  return true;
}

static bool g_line(int argc, Value *args, Value *out) { /* line(x1, y1, x2, y2, color, width?) */
  (void)out;
  double c[4];
  NEED_WINDOW();
  if (!need_nums(args, 0, 4) || !parse_color(args[4], c)) return false;
  if (argc > 5 && !need_nums(args, 5, 6)) return false;
  gfx_line(args[0].as.n, args[1].as.n, args[2].as.n, args[3].as.n, argc > 5 ? args[5].as.n : 2, c);
  return true;
}

static bool g_text(int argc, Value *args, Value *out) { /* text(value, x, y, size?, color?) */
  (void)out;
  double c[4] = {1, 1, 1, 1};
  NEED_WINDOW();
  if (!need_nums(args, 1, argc > 3 ? 4 : 3)) return false;
  if (argc > 4 && !parse_color(args[4], c)) return false;
  Buf b = {0};
  show(&b, args[0], false, 0);
  gfx_text(b.s, args[1].as.n, args[2].as.n, argc > 3 ? args[3].as.n : 20, c);
  free(b.s);
  return true;
}

static bool g_text_width(int argc, Value *args, Value *out) { /* text_width(value, size?) */
  NEED_WINDOW();
  if (argc > 1 && !need_nums(args, 1, 2)) return false;
  Buf b = {0};
  show(&b, args[0], false, 0);
  *out = NUM_V(gfx_text_width(b.s, argc > 1 ? args[1].as.n : 20));
  free(b.s);
  return true;
}

/* key names: arrows, a few named keys, or one lowercase letter/digit/symbol */
static bool check_key(const char *k) {
  static const char *named[] = {"left", "right", "up", "down", "space", "enter", "escape", "backspace", "tab"};
  for (int i = 0; i < 9; i++)
    if (!strcmp(named[i], k)) return true;
  if (strlen(k) == 1 && !isupper((unsigned char)k[0]) && isgraph((unsigned char)k[0])) return true;
  static const char *alias[][2] = {{"sol", "left"}, {"sağ", "right"}, {"yukarı", "up"}, {"aşağı", "down"},
                                   {"boşluk", "space"}, {"esc", "escape"}, {"return", "enter"}, {"arrowleft", "left"},
                                   {"arrowright", "right"}, {"arrowup", "up"}, {"arrowdown", "down"}, {" ", "space"}};
  char low[32];
  int n = 0;
  for (; k[n] && n < 31; n++) low[n] = (char)tolower((unsigned char)k[n]);
  low[n] = 0;
  for (int i = 0; i < (int)(sizeof alias / sizeof *alias); i++)
    if (!strcmp(alias[i][0], k) || !strcmp(alias[i][0], low)) return nerr("unknown key \"%s\" (use \"%s\")", k, alias[i][1]);
  if (strcmp(low, k) && check_key(low)) return nerr("unknown key \"%s\" (key names are lowercase: \"%s\")", k, low);
  return nerr("unknown key \"%s\" (keys: \"left\", \"right\", \"up\", \"down\", \"space\", \"enter\", \"escape\", or a letter like \"a\")", k);
}

static bool g_key(int argc, Value *args, Value *out) { /* held down: "left", "space", "a", ... */
  (void)argc;
  NEED_WINDOW();
  NEED(0, O_STR, "str");
  if (!check_key(AS_STR(args[0])->chars)) return false;
  *out = BOOL_V(gfx_key(AS_STR(args[0])->chars));
  return true;
}

static bool g_pressed(int argc, Value *args, Value *out) { /* went down since the last frame */
  (void)argc;
  NEED_WINDOW();
  NEED(0, O_STR, "str");
  if (!check_key(AS_STR(args[0])->chars)) return false;
  *out = BOOL_V(gfx_pressed(AS_STR(args[0])->chars));
  return true;
}

static bool g_mouse(int argc, Value *args, Value *out) { /* {x, y, down, clicked} */
  (void)argc;
  (void)args;
  NEED_WINDOW();
  double x, y;
  bool down, clicked;
  gfx_mouse(&x, &y, &down, &clicked);
  Map *m = new_map();
  map_set(m, OBJ_V(cstr("x")), NUM_V(x));
  map_set(m, OBJ_V(cstr("y")), NUM_V(y));
  map_set(m, OBJ_V(cstr("down")), BOOL_V(down));
  map_set(m, OBJ_V(cstr("clicked")), BOOL_V(clicked));
  *out = OBJ_V(m);
  return true;
}

static bool g_delta(int argc, Value *args, Value *out) { /* seconds since the previous frame */
  (void)argc;
  (void)args;
  NEED_WINDOW();
  *out = NUM_V(gfx_delta());
  return true;
}

static bool g_sound(int argc, Value *args, Value *out) {
  (void)argc;
  (void)out;
  NEED_WINDOW();
  NEED(0, O_STR, "str");
  char name[64];
  snprintf(name, sizeof name, "%s", AS_STR(args[0])->chars);
  for (int i = 0; name[i]; i++) name[i] = (char)(i ? tolower((unsigned char)name[i]) : toupper((unsigned char)name[i]));
  if (!gfx_sound(name)) /* system sound names are capitalised: Pop, Glass */
    return nerr("unknown sound \"%s\" (try Pop, Tink, Glass, Basso, Funk, Hero, Ping, Purr, Submarine)", AS_STR(args[0])->chars);
  return true;
}

static bool g_screenshot(int argc, Value *args, Value *out) {
  (void)argc;
  (void)out;
  NEED_WINDOW();
  NEED(0, O_STR, "str");
  if (!gfx_screenshot(AS_STR(args[0])->chars)) return nerr("can't write '%s'", AS_STR(args[0])->chars);
  return true;
}

/* pixels(buf, w, h, palette?): draw a w*h image stretched over the whole window.
 * buf holds 0xRRGGBB colors, or indexes into palette (a list of 256 0xRRGGBB colors). */
static bool g_pixels(int argc, Value *args, Value *out) {
  (void)out;
  NEED_WINDOW();
  NEED(0, O_LIST, "list");
  if (!need_nums(args, 1, 3)) return false;
  int w = (int)args[1].as.n, h = (int)args[2].as.n;
  List *buf = AS_LIST(args[0]), *pal = NULL;
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return nerr("bad image size %dx%d", w, h);
  if (buf->len < w * h) return nerr("need %d pixels for %dx%d, got %d", w * h, w, h, buf->len);
  if (argc > 3) {
    NEED(3, O_LIST, "list");
    pal = AS_LIST(args[3]);
    if (pal->len < 256) return nerr("a palette needs 256 colors, got %d", pal->len);
  }
  unsigned char *rgba = malloc((size_t)w * h * 4);
  for (int i = 0; i < w * h; i++) {
    Value v = buf->items[i];
    uint32_t c = v.t == V_NUM ? (uint32_t)v.as.n : 0;
    if (pal) {
      Value pv = pal->items[c & 255];
      c = pv.t == V_NUM ? (uint32_t)pv.as.n : 0;
    }
    rgba[i * 4] = (c >> 16) & 255;
    rgba[i * 4 + 1] = (c >> 8) & 255;
    rgba[i * 4 + 2] = c & 255;
    rgba[i * 4 + 3] = 255;
  }
  gfx_pixels(rgba, w, h);
  free(rgba);
  return true;
}

static void def_gfx(Map *gfx, const char *name, NativeFn fn, int min, int max, const char *usage) {
  Native *n = (Native *)new_obj(sizeof(Native), O_NATIVE);
  n->fn = fn;
  n->name = name;
  n->min = min;
  n->max = max;
  n->plain = false;
  n->usage = NULL;
  n->usage = usage;
  map_set(gfx, OBJ_V(cstr(name + 4)), OBJ_V(n)); /* "gfx.rect" -> key "rect" */
}

static void def_graphics(void) {
  Map *gfx = new_map();
  gfx->sname = cstr("gfx"); /* fixed names: gfx.circel says "gfx has no function 'circel' (did you mean 'circle'?)" */
  map_set(vm.builtins, OBJ_V(cstr("gfx")), OBJ_V(gfx));
  def_gfx(gfx, "gfx.window", g_window, 2, 3, "gfx.window(width, height, title)");
  def_gfx(gfx, "gfx.frame", g_frame, 0, 0, "while gfx.frame() { ... }");
  def_gfx(gfx, "gfx.clear", g_clear, 1, 1, "gfx.clear(color)");
  def_gfx(gfx, "gfx.rect", g_rect, 5, 6, "gfx.rect(x, y, width, height, color, corner_radius)");
  def_gfx(gfx, "gfx.circle", g_circle, 4, 4, "gfx.circle(x, y, radius, color)");
  def_gfx(gfx, "gfx.line", g_line, 5, 6, "gfx.line(x1, y1, x2, y2, color, width)");
  def_gfx(gfx, "gfx.text", g_text, 3, 5, "gfx.text(value, x, y, size, color)");
  def_gfx(gfx, "gfx.text_width", g_text_width, 1, 2, "gfx.text_width(value, size)");
  def_gfx(gfx, "gfx.key", g_key, 1, 1, "gfx.key(\"left\")");
  def_gfx(gfx, "gfx.pressed", g_pressed, 1, 1, "gfx.pressed(\"space\")");
  def_gfx(gfx, "gfx.mouse", g_mouse, 0, 0, "gfx.mouse().x");
  def_gfx(gfx, "gfx.delta", g_delta, 0, 0, "gfx.delta()");
  def_gfx(gfx, "gfx.sound", g_sound, 1, 1, "gfx.sound(\"Pop\")");
  def_gfx(gfx, "gfx.screenshot", g_screenshot, 1, 1, "gfx.screenshot(\"shot.png\")");
  def_gfx(gfx, "gfx.pixels", g_pixels, 3, 4, "gfx.pixels(buf, width, height, palette)");
}

#ifdef LUMO_COMPILED
static void def_compiled(const char *name, NativeFn fn, int arity) {
  Native *n = (Native *)new_obj(sizeof(Native), O_NATIVE);
  n->fn = fn;
  n->name = name;
  n->min = n->max = arity;
  n->plain = true; /* same messages as the interpreted version */
  n->usage = NULL;
  map_set(compiled_fns, OBJ_V(cstr(name)), OBJ_V(n));
}
static void register_compiled(void); /* generated */
#endif

static void def_native(const char *name, NativeFn fn, int min, int max) {
  Native *n = (Native *)new_obj(sizeof(Native), O_NATIVE);
  n->fn = fn;
  n->name = name;
  n->min = min;
  n->max = max;
  n->plain = false;
  n->usage = NULL;
  map_set(vm.builtins, OBJ_V(cstr(name)), OBJ_V(n));
}

/* the rest of the standard library is written in Lumo itself */
static const char *PRELUDE =
    "fn map(xs, f) {\n  let out = []\n  for x in xs { push(out, f(x)) }\n  out\n}\n"
    "fn filter(xs, f) {\n  let out = []\n  for x in xs { if f(x) { push(out, x) } }\n  out\n}\n"
    "fn reduce(xs, start, f) {\n  let acc = start\n  for x in xs { acc = f(acc, x) }\n  acc\n}\n"
    "fn sum(xs) { reduce(xs, 0, fn(a, b) { a + b }) }\n"
    "fn any(xs, f) {\n  for x in xs { if f(x) { return true } }\n  false\n}\n"
    "fn all(xs, f) {\n  for x in xs { if not f(x) { return false } }\n  true\n}\n";

/* ---------- entry ---------- */

static bool interpret(const char *src, const char *name, Value *result) {
  Fn *fn = compile(src, cstr(name), vm.globals, false);
  if (!fn) return false;
  Closure *cl = new_closure(fn);
  push(OBJ_V(cl));
  call_value(OBJ_V(cl), 0);
  if (!run(0)) return false;
  Value r = pop();
  if (result) *result = r;
  return true;
}

/* lumo build app.lu [-o app]: one executable with the program and everything it imports */
static bool write_whole(const char *path, const void *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, len, f) == len;
  return fclose(f) == 0 && ok;
}

/* writes lumo's source plus the generated C to a fresh private temp dir and compiles it there */
static char *compile_native(Buf *gen, long *len) {
#ifndef LUMO_HAVE_SRC
  (void)gen;
  (void)len;
  fprintf(stderr, "error: --fast needs lumo built with `make` (it carries its own source)\n");
  return NULL;
#else
  const char *tmp = tmp_dir();
  char dir[1024], path[1200];
  snprintf(dir, sizeof dir, "%s/lumo_fast_XXXXXX", tmp);
  if (!mkdtemp(dir)) { fprintf(stderr, "error: can't make a temp directory in %s\n", tmp); return NULL; }
  Buf prog = {0};
  buf_add(&prog, (const char *)lumo_c, (int)lumo_c_len);
  buf_add(&prog, gen->s, gen->len);
  struct { const char *name; const void *data; size_t len; } files[] = {
      {"prog.c", prog.s, (size_t)prog.len},
#if defined(LUMO_GFX) && defined(__APPLE__)
      {"gfx_macos.m", gfx_macos_m, gfx_macos_m_len},
#elif defined(LUMO_GFX)
      {"gfx.c", gfx_c, gfx_c_len},
      {"gfx_font.h", gfx_font_h, gfx_font_h_len},
#endif
  };
  int nfiles = (int)(sizeof files / sizeof *files);
  bool ok = true;
  for (int i = 0; i < nfiles; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, files[i].name);
    ok = ok && write_whole(path, files[i].data, files[i].len);
  }
  if (getenv("LUMO_KEEP_C")) write_whole(getenv("LUMO_KEEP_C"), prog.s, prog.len); /* to read or debug it */
  free(prog.s);
  char *argv[16] = {CC, "-O2", "-w", "-DLUMO_COMPILED", "-o", "prog.exe", "prog.c"}; /* .exe: Windows needs it, it's harmless elsewhere */
  int n = 7;
#if defined(LUMO_GFX) && defined(__APPLE__)
  char *gfx[] = {"-DLUMO_GFX", "gfx_macos.m", "-framework", "Cocoa", "-framework", "QuartzCore", "-framework", "ImageIO", NULL};
#elif defined(LUMO_GFX) && defined(_WIN32)
  char *gfx[] = {"-DLUMO_GFX", "gfx.c", "-lgdi32", "-lwinmm", NULL};
#elif defined(LUMO_GFX)
  char *gfx[] = {"-DLUMO_GFX", "gfx.c", "-ldl", NULL};
#else
  char *gfx[] = {NULL};
#endif
  for (int i = 0; gfx[i]; i++) argv[n++] = gfx[i];
  argv[n++] = "-lm";
  argv[n] = NULL;
  int rc = ok ? spawn_wait(dir, argv) : -1;
  char *exe = NULL;
  FILE *f;
  snprintf(path, sizeof path, "%s/prog.exe", dir);
  if (rc == 0 && (f = fopen(path, "rb"))) {
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fclose(f);
    exe = read_file(path);
  } else {
    fprintf(stderr, "error: the C compiler failed%s\n", rc == 127 ? " (is `" CC "` installed?)" : "");
  }
  unlink(path);
  for (int i = 0; i < nfiles; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, files[i].name);
    unlink(path);
  }
  rmdir(dir);
  return exe;
#endif
}

static int build(const char *main_path, const char *out, bool fast, bool quiet) {
  char self[4096];
  if (!self_path(self, sizeof self)) { fprintf(stderr, "error: can't find the lumo executable\n"); return 1; }
  const char *files[256];
  int nfiles = 0;
  files[nfiles++] = main_path;
  Buf pak = {0}, native = {0};
  for (int i = 0; i < nfiles; i++) {
    char *src = read_file(files[i]);
    if (!src) { fprintf(stderr, "error: can't read '%s'\n", files[i]); return 1; }
    /* compile it now so mistakes show up at build time, not on someone else's computer */
    if (!compile(src, cstr(files[i]), new_map(), i > 0)) { free(src); return 1; }
    if (fast && i == 0) cgen_program(files[0], &native, quiet); /* the main file's typed functions become C */
    buf_add(&pak, files[i], (int)strlen(files[i]) + 1);
    buf_add(&pak, src, (int)strlen(src) + 1);
    for (int t = 0; t + 1 < ntoks; t++) { /* compile() left this file's tokens in toks (they point into src) */
      if (toks[t].type != T_IMPORT || toks[t + 1].type != T_STR) continue;
      const char *dep = resolve_import(files[i], str_token(&toks[t + 1]))->chars;
      bool seen = false;
      for (int k = 0; k < nfiles; k++) seen = seen || !strcmp(files[k], dep);
      if (!seen) {
        if (nfiles == 256) { fprintf(stderr, "error: too many imported files\n"); return 1; }
        files[nfiles++] = strdup(dep);
      }
    }
    free(src);
  }
  long exe_len = 0;
  char *exe = NULL;
  if (fast) {
    if (!(exe = compile_native(&native, &exe_len))) return 1;
  } else {
    exe = read_file(self);
    FILE *in = fopen(self, "rb");
    if (!exe || !in) { fprintf(stderr, "error: can't read '%s'\n", self); return 1; }
    fseek(in, 0, SEEK_END);
    exe_len = ftell(in);
    fclose(in);
  }
  if (exe_len >= 16 && !memcmp(exe + exe_len - 8, PAK_MAGIC, 8)) { /* built from a built program: strip its pak */
    uint64_t old;
    memcpy(&old, exe + exe_len - 16, 8);
    exe_len -= 16 + (long)old;
  }
  FILE *o = fopen(out, "wb");
  if (!o) { fprintf(stderr, "error: can't write '%s'\n", out); return 1; }
  uint64_t len = (uint64_t)pak.len;
  fwrite(exe, 1, exe_len, o);
  fwrite(pak.s, 1, pak.len, o);
  fwrite(&len, 1, 8, o);
  fwrite(PAK_MAGIC, 1, 8, o);
  fclose(o);
#ifndef _WIN32
  chmod(out, 0755);
#endif
  if (!quiet) printf("built %s (%d file%s, %ld KB)\n", out, nfiles, nfiles == 1 ? "" : "s", (exe_len + pak.len + 1023) / 1024);
  free(native.s);
  free(exe);
  free(pak.s);
  return 0;
}

static void usage(void) {
  puts("lumo " LUMO_VERSION "\n"
       "usage:\n"
       "  lumo                      start the interactive REPL\n"
       "  lumo file.lu [args...]    run a program\n"
       "  lumo build file.lu [-o name]\n"
       "                            make a standalone executable (Lumo not needed to run it)\n"
       "  lumo build --fast file.lu [-o name]\n"
       "                            same, with typed functions compiled to machine code\n"
       "  lumo --fast file.lu [args...]\n"
       "                            run with typed functions compiled to machine code\n"
       "  lumo --version");
}

static int brace_balance(const char *s) {
  int d = 0;
  for (; *s; s++) {
    if (*s == '"') {
      for (s++; *s && *s != '"'; s++)
        if (*s == '\\' && s[1]) s++;
      if (!*s) return 1;
    } else if (*s == '#') {
      while (*s && *s != '\n') s++;
      if (!*s) break;
    } else if (strchr("{[(", *s)) d++;
    else if (strchr("}])", *s)) d--;
  }
  return d;
}

int main(int argc, char **argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8); /* Lumo strings are UTF-8: ç, ş, ğ print as themselves */
  SetConsoleCP(CP_UTF8);
#endif
  vm.sp = vm.stack;
  vm.next_gc = 1u << 20;
  vm.globals = new_map();
  vm.builtins = new_map();
  vm.modules = new_map();
  srand((unsigned)time(NULL));

  def_native("print", n_print, 0, 255);
  def_native("write", n_write, 0, 255);
  def_native("len", n_len, 1, 1);
  def_native("push", n_push, 2, 2);
  def_native("pop", n_pop, 1, 1);
  def_native("slice", n_slice, 2, 3);
  def_native("contains", n_contains, 2, 2);
  def_native("sort", n_sort, 1, 1);
  def_native("join", n_join, 2, 2);
  def_native("split", n_split, 2, 2);
  def_native("replace", n_replace, 3, 3);
  def_native("upper", n_upper, 1, 1);
  def_native("lower", n_lower, 1, 1);
  def_native("trim", n_trim, 1, 1);
  def_native("keys", n_keys, 1, 1);
  def_native("has", n_has, 2, 2);
  def_native("get", n_get, 3, 3);
  def_native("remove", n_remove, 2, 2);
  def_native("str", n_str, 1, 1);
  def_native("num", n_num, 1, 1);
  def_native("type", n_type, 1, 1);
  def_native("range", n_range, 1, 3);
  def_native("floor", n_floor, 1, 1);
  def_native("ceil", n_ceil, 1, 1);
  def_native("round", n_round, 1, 1);
  def_native("sqrt", n_sqrt, 1, 1);
  def_native("abs", n_abs, 1, 1);
  def_native("min", n_min, 2, 2);
  def_native("max", n_max, 2, 2);
  def_native("random", n_random, 0, 0);
  def_native("clock", n_clock, 0, 0);
  def_native("input", n_input, 0, 1);
  def_native("read_file", n_read_file, 1, 1);
  def_native("write_file", n_write_file, 2, 2);
  def_native("assert", n_assert, 1, 2);
  def_native("error", n_error, 1, 1);
  def_native("exit", n_exit, 0, 1);
  def_native("starts_with", n_starts_with, 2, 2);
  def_native("ends_with", n_ends_with, 2, 2);
  def_native("find", n_find, 2, 2);
  def_native("chars", n_chars, 1, 1);
  def_native("ord", n_ord, 1, 1);
  def_native("chr", n_chr, 1, 1);
  def_native("repeat", n_repeat, 2, 2);
  def_native("reverse", n_reverse, 1, 1);
  def_native("insert", n_insert, 3, 3);
  def_native("index_of", n_index_of, 2, 2);
  def_native("pow", n_pow, 2, 2);
  def_native("trunc", n_trunc, 1, 1);
  def_native("read_bytes", n_read_bytes, 1, 1);
  def_native("bytes_str", n_bytes_str, 3, 3);
  def_native("log", n_log, 1, 1);
  def_native("sin", n_sin, 1, 1);
  def_native("cos", n_cos, 1, 1);
  def_native("json_encode", n_json_encode, 1, 1);
  def_native("json_decode", n_json_decode, 1, 1);
  def_native("now", n_now, 0, 0);
  def_native("sleep", n_sleep, 1, 1);
  def_native("format_time", n_format_time, 1, 2);
  def_native("exists", n_exists, 1, 1);
  def_native("list_dir", n_list_dir, 1, 1);
  def_native("append_file", n_append_file, 2, 2);
  def_native("run", n_run, 1, 1);
  def_native("env", n_env, 1, 1);
  def_graphics();
  compiled_fns = new_map();
#ifdef LUMO_COMPILED
  register_compiled();
#endif
  map_set(vm.builtins, OBJ_V(cstr("pi")), NUM_V(M_PI));
  load_embedded();
  List *script_args = new_list(); /* a built program gets all its arguments */
  for (int i = embedded ? 1 : 2; i < argc; i++) list_push(script_args, OBJ_V(cstr(argv[i])));
  map_set(vm.builtins, OBJ_V(cstr("args")), OBJ_V(script_args));

  if (!interpret(PRELUDE, "prelude", NULL)) return 70;
  for (int i = 0; i < vm.globals->used; i++)
    if (vm.globals->entries[i].key.t != V_NIL)
      map_set(vm.builtins, vm.globals->entries[i].key, vm.globals->entries[i].val);
  vm.globals = new_map(); /* user code may shadow builtins with let */

  if (embedded) {
    bool ok = interpret(embedded + strlen(embedded) + 1, embedded, NULL);
    if (ok) gfx_note_no_frames();
    return ok ? 0 : 70;
  }
  if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) { usage(); return 0; }
  if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) { puts("lumo " LUMO_VERSION); return 0; }
  if (argc > 2 && !strcmp(argv[1], "--fast")) { /* build to a temp file, then run it in our place */
    char out[1024];
    snprintf(out, sizeof out, "%s/lumo_run_XXXXXX", tmp_dir());
#ifdef _WIN32
    if (!_mktemp(out)) { fprintf(stderr, "error: can't make a temp file\n"); return 70; }
    strcat(out, ".exe");
#else
    int fd = mkstemp(out); /* a fresh file nobody else can swap out */
    if (fd < 0) { fprintf(stderr, "error: can't make a temp file\n"); return 70; }
    close(fd);
#endif
    if (build(argv[2], out, true, true)) { unlink(out); return 70; }
    argv[2] = out;
    int rc = spawn_wait(NULL, argv + 2);
    unlink(out);
    return rc < 0 ? 70 : rc;
  }
  if (argc > 2 && !strcmp(argv[1], "build")) {
    bool fast = !strcmp(argv[2], "--fast");
    int a = fast ? 3 : 2;
    if (argc <= a) { usage(); return 64; }
    char out[1024];
    if (argc > a + 2 && !strcmp(argv[a + 1], "-o")) {
      snprintf(out, sizeof out, "%s", argv[a + 2]);
    } else { /* examples/todo.lu -> todo */
      snprintf(out, sizeof out, "%s", path_base(argv[a]));
      char *dot = strrchr(out, '.');
      if (dot && dot != out) *dot = 0;
      if (!strcmp(out, argv[a])) strcat(out, ".out");
    }
#ifdef _WIN32
    size_t ol = strlen(out); /* Windows only runs programs whose name ends in .exe */
    if ((ol < 4 || _stricmp(out + ol - 4, ".exe")) && ol + 4 < sizeof out) strcat(out, ".exe");
#endif
    return build(argv[a], out, fast, false);
  }
  if (argc > 1) {
    char *src = read_file(argv[1]);
    if (!src) {
      fprintf(stderr, "error: can't read '%s'\n", argv[1]);
      return 74;
    }
    bool ok = interpret(src, argv[1], NULL);
    if (ok) gfx_note_no_frames();
    free(src);
    return ok ? 0 : 70;
  }

  printf("lumo — ctrl-d to quit\n");
  Buf buf = {0};
  char line[4096];
  for (;;) {
    fputs(buf.len ? "... " : "> ", stdout);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin)) { putchar('\n'); break; }
    buf_add(&buf, line, (int)strlen(line));
    if (brace_balance(buf.s) > 0) continue;
    Value r;
    if (interpret(buf.s, "<repl>", &r) && r.t != V_NIL) {
      Buf out = {0};
      show(&out, r, true, 0);
      puts(out.s);
      free(out.s);
    }
    buf.len = 0;
    buf.s[0] = 0;
  }
  free(buf.s);
  return 0;
}
