#include "core/skapi.h"
#include "sqlite3.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

#define ANKI_TEXT_CAP 8192
#define ANKI_DECK_NAME 256
#define ANKI_LEARN_STEP_1 60
#define ANKI_LEARN_STEP_2 600
#define ANKI_START_FACTOR 2500
#define ANKI_MIN_FACTOR 1300
#define ANKI_SUB_COLOR sk_color_lerp(COLOR_BG, SK_BLACK, 0.82f)

typedef struct {
  long long id;
  char name[ANKI_DECK_NAME];
  int due;
} AnkiDeck;

typedef struct {
  long long id;
  long long due_ts;
  long long ivl;
  long long factor;
  int queue;
  int reps;
  int lapses;
  int odid;
  int cloze;
  char *flds;
} AnkiCard;

typedef enum { ANKI_VIEW_DECKS, ANKI_VIEW_CARD, ANKI_VIEW_INFO } AnkiView;
typedef enum { ANKI_SIDE_QUESTION, ANKI_SIDE_ANSWER } AnkiSide;

static AnkiView g_view = ANKI_VIEW_DECKS;
static AnkiSide g_side = ANKI_SIDE_QUESTION;
static bool g_focus_mode;
static SkTimer g_session_timer;
static double g_today_ms;
static int g_today_reviewed;
static int g_today_again;
static int g_today_day;
static long long g_crt = 0;

static void stats_check_day(void);

static AnkiDeck *g_decks;
static const char **g_deck_items;
static int g_deck_count;
static int g_deck_cap;

static AnkiCard *g_cards;
static int g_card_count;
static int g_card_cap;
static int g_card_pos;
static char g_deck_name[ANKI_DECK_NAME];

static SkTimer g_answer_timer;
static char g_qtext[ANKI_TEXT_CAP];
static char g_atext[ANKI_TEXT_CAP];
static char g_abody[ANKI_TEXT_CAP];

#define ANKI_CLOZE_SEGS 128
static SkTextSegment g_cloze_segs[ANKI_CLOZE_SEGS];
static int g_cloze_seg_count;
static char g_cloze_pool[ANKI_TEXT_CAP];
static size_t g_cloze_pool_len;
static long long g_last_revlog_ms;
static bool g_revlog_init;

static SkLibrary g_sqlite_lib = NULL;
static int (*q_open)(const char *, sqlite3 **, int, const char *);
static int (*q_close)(sqlite3 *);
static int (*q_exec)(sqlite3 *, const char *,
                     int (*)(void *, int, char **, char **), void *, char **);
static int (*q_prepare)(sqlite3 *, const char *, int, sqlite3_stmt **,
                        const char **);
static int (*q_step)(sqlite3_stmt *);
static int (*q_finalize)(sqlite3_stmt *);
static int (*q_bind_i64)(sqlite3_stmt *, int, sqlite3_int64);
static const unsigned char *(*q_col_text)(sqlite3_stmt *, int);
static sqlite3_int64 (*q_col_i64)(sqlite3_stmt *, int);
static const char *(*q_errmsg)(sqlite3 *);

static bool sqlite_init(void) {
  if (g_sqlite_lib)
    return q_open && q_prepare && q_step && q_finalize && q_exec;
  char buf[1024] = {0};
  char *dir = sk_resolve_feature_path("anki");
  if (!dir)
    return false;
#if defined(_WIN32)
  snprintf(buf, sizeof(buf), "%s/sqlite3.dll", dir);
#elif defined(__APPLE__)
  snprintf(buf, sizeof(buf), "%s/libsqlite3.dylib", dir);
#else
  snprintf(buf, sizeof(buf), "%s/libsqlite3.so", dir);
#endif
  free(dir);
  g_sqlite_lib = sk_library_open(buf);
  if (!g_sqlite_lib)
    return false;
  sk_library_register("sqlite", g_sqlite_lib);
  q_open = (int (*)(const char *, sqlite3 **, int,
                    const char *))sk_library_symbol(g_sqlite_lib,
                                                    "sqlite3_open_v2");
  q_close =
      (int (*)(sqlite3 *))sk_library_symbol(g_sqlite_lib, "sqlite3_close");
  q_exec = (int (*)(sqlite3 *, const char *,
                    int (*)(void *, int, char **, char **), void *,
                    char **))sk_library_symbol(g_sqlite_lib, "sqlite3_exec");
  q_prepare = (int (*)(sqlite3 *, const char *, int, sqlite3_stmt **,
                       const char **))sk_library_symbol(g_sqlite_lib,
                                                        "sqlite3_prepare_v2");
  q_step = (int (*)(sqlite3_stmt *))sk_library_symbol(g_sqlite_lib,
                                                      "sqlite3_step");
  q_finalize = (int (*)(sqlite3_stmt *))sk_library_symbol(g_sqlite_lib,
                                                          "sqlite3_finalize");
  q_bind_i64 =
      (int (*)(sqlite3_stmt *, int, sqlite3_int64))sk_library_symbol(
          g_sqlite_lib, "sqlite3_bind_int64");
  q_col_text =
      (const unsigned char *(*)(sqlite3_stmt *, int))sk_library_symbol(
          g_sqlite_lib, "sqlite3_column_text");
  q_col_i64 =
      (sqlite3_int64(*)(sqlite3_stmt *, int))sk_library_symbol(g_sqlite_lib,
                                                               "sqlite3_column_int64");
  q_errmsg = (const char *(*)(sqlite3 *))sk_library_symbol(g_sqlite_lib,
                                                           "sqlite3_errmsg");
  return q_open && q_close && q_exec && q_prepare && q_step && q_finalize &&
         q_bind_i64 && q_col_text && q_col_i64 && q_errmsg;
}

static bool anki_db_path(char *out, size_t n) {
  const char *home = sk_resolve_user_path();
  if (!home || !home[0])
    return false;
#if defined(_WIN32)
  snprintf(out, n, "%s/AppData/Roaming/Anki2/User 1/collection.anki2", home);
#elif defined(__APPLE__)
  snprintf(out, n,
           "%s/Library/Application Support/Anki2/User 1/collection.anki2",
           home);
#else
  snprintf(out, n, "%s/.local/share/Anki2/User 1/collection.anki2", home);
#endif
  return true;
}

static bool db_open(const char *path, int flags, sqlite3 **out) {
  *out = NULL;
  if (q_open(path, out, flags, NULL) != SQLITE_OK) {
    if (*out)
      q_close(*out);
    *out = NULL;
    return false;
  }
  return true;
}

static void load_crt(sqlite3 *db) {
  sqlite3_stmt *st = NULL;
  if (q_prepare(db, "SELECT crt FROM col;", -1, &st, NULL) != SQLITE_OK)
    return;
  if (q_step(st) == SQLITE_ROW)
    g_crt = (long long)q_col_i64(st, 0);
  q_finalize(st);
}

static long long due_to_ts(long long now, int queue, long long due) {
  if (queue == 1 || queue == 3)
    return due;
  if (queue == 2)
    return g_crt > 0 ? g_crt + due * 86400 : now;
  return now;
}

static void free_decks(void) {
  free(g_decks);
  free(g_deck_items);
  g_decks = NULL;
  g_deck_items = NULL;
  g_deck_count = 0;
  g_deck_cap = 0;
}

static void free_cards(void) {
  for (int i = 0; i < g_card_count; i++)
    free(g_cards[i].flds);
  free(g_cards);
  g_cards = NULL;
  g_card_count = 0;
  g_card_cap = 0;
  g_card_pos = 0;
}

static bool decks_reserve(int needed) {
  if (needed <= g_deck_cap)
    return true;
  int cap = g_deck_cap ? g_deck_cap * 2 : 32;
  while (cap < needed)
    cap *= 2;
  AnkiDeck *d = realloc(g_decks, (size_t)cap * sizeof(*d));
  if (!d)
    return false;
  g_decks = d;
  const char **it = realloc(g_deck_items, (size_t)cap * sizeof(*it));
  if (!it)
    return false;
  g_deck_items = it;
  g_deck_cap = cap;
  return true;
}

static bool cards_reserve(int needed) {
  if (needed <= g_card_cap)
    return true;
  int cap = g_card_cap ? g_card_cap * 2 : 64;
  while (cap < needed)
    cap *= 2;
  AnkiCard *c = realloc(g_cards, (size_t)cap * sizeof(*c));
  if (!c)
    return false;
  g_cards = c;
  g_card_cap = cap;
  return true;
}

static int deck_cmp(const void *a, const void *b) {
  const AnkiDeck *da = (const AnkiDeck *)a;
  const AnkiDeck *db = (const AnkiDeck *)b;
  return strcmp(da->name, db->name);
}

static void load_decks(void) {
  free_decks();
  if (!sqlite_init()) {
    fprintf(stderr, "anki: could not load bundled SQLite library\n");
    return;
  }
  char path[1024];
  if (!anki_db_path(path, sizeof(path))) {
    fprintf(stderr, "anki: could not resolve the home directory\n");
    return;
  }
  sqlite3 *db = NULL;
  if (!db_open(path, SQLITE_OPEN_READONLY, &db)) {
    fprintf(stderr, "anki: could not open collection '%s'\n", path);
    return;
  }
  load_crt(db);
  long long now = (long long)time(NULL);

  sqlite3_stmt *st = NULL;
  if (q_prepare(db, "SELECT id, name FROM decks;", -1, &st, NULL) ==
      SQLITE_OK) {
    while (q_step(st) == SQLITE_ROW) {
      const unsigned char *raw = q_col_text(st, 1);
      if (!raw)
        continue;
      const char *name = (const char *)raw;
      if (strchr(name, '\x1f') || strstr(name, "::"))
        continue;
      if (!decks_reserve(g_deck_count + 1))
        break;
      AnkiDeck *d = &g_decks[g_deck_count++];
      memset(d, 0, sizeof(*d));
      d->id = (long long)q_col_i64(st, 0);
      snprintf(d->name, sizeof(d->name), "%s", name);
    }
    q_finalize(st);
  } else {
    fprintf(stderr, "anki: deck query failed: %s\n", q_errmsg(db));
  }

  st = NULL;
  if (q_prepare(db, "SELECT did, due, queue, odid, odue FROM cards "
                    "WHERE queue >= 0;",
                -1, &st, NULL) == SQLITE_OK) {
    while (q_step(st) == SQLITE_ROW) {
      long long did = (long long)q_col_i64(st, 0);
      long long due = (long long)q_col_i64(st, 1);
      int queue = (int)q_col_i64(st, 2);
      long long odid = (long long)q_col_i64(st, 3);
      long long odue = (long long)q_col_i64(st, 4);
      for (int i = 0; i < g_deck_count; i++) {
        if (g_decks[i].id == did) {
          if (due_to_ts(now, queue, odid != 0 ? odue : due) <= now)
            g_decks[i].due++;
          break;
        }
      }
    }
    q_finalize(st);
  } else {
    fprintf(stderr, "anki: due-count query failed: %s\n", q_errmsg(db));
  }
  if (g_deck_count > 1)
    qsort(g_decks, (size_t)g_deck_count, sizeof(*g_decks), deck_cmp);
  for (int i = 0; i < g_deck_count; i++)
    g_deck_items[i] = g_decks[i].name;
  q_close(db);
}

static int card_cmp(const void *a, const void *b) {
  const AnkiCard *ca = (const AnkiCard *)a;
  const AnkiCard *cb = (const AnkiCard *)b;
  if (ca->due_ts < cb->due_ts)
    return -1;
  if (ca->due_ts > cb->due_ts)
    return 1;
  if (ca->id < cb->id)
    return -1;
  if (ca->id > cb->id)
    return 1;
  return 0;
}

static void load_cards(const char *deck_name) {
  free_cards();
  g_deck_name[0] = '\0';
  snprintf(g_deck_name, sizeof(g_deck_name), "%s", deck_name ? deck_name : "");
  if (!sqlite_init()) {
    fprintf(stderr, "anki: could not load bundled SQLite library\n");
    return;
  }
  char path[1024];
  if (!anki_db_path(path, sizeof(path))) {
    fprintf(stderr, "anki: could not resolve the home directory\n");
    return;
  }
  sqlite3 *db = NULL;
  if (!db_open(path, SQLITE_OPEN_READONLY, &db)) {
    fprintf(stderr, "anki: could not open collection '%s'\n", path);
    return;
  }
  load_crt(db);

  long long deck_id = 0;
  bool found = false;
  sqlite3_stmt *st = NULL;
  if (q_prepare(db, "SELECT id, name FROM decks;", -1, &st, NULL) ==
      SQLITE_OK) {
    while (q_step(st) == SQLITE_ROW) {
      const unsigned char *name = q_col_text(st, 1);
      if (name && strcmp((const char *)name, g_deck_name) == 0) {
        deck_id = (long long)q_col_i64(st, 0);
        found = true;
        break;
      }
    }
    q_finalize(st);
  } else {
    fprintf(stderr, "anki: deck lookup failed: %s\n", q_errmsg(db));
  }
  if (!found) {
    q_close(db);
    return;
  }

  long long now = (long long)time(NULL);
  st = NULL;
  if (q_prepare(db, "SELECT c.id, n.flds, c.due, c.queue, c.ivl, c.factor, "
                    "c.reps, c.lapses, c.odid, c.odue FROM notes n JOIN "
                    "cards c ON n.id = c.nid WHERE c.did = ? AND "
                    "c.queue >= 0;",
                -1, &st, NULL) == SQLITE_OK) {
    q_bind_i64(st, 1, deck_id);
    while (q_step(st) == SQLITE_ROW) {
      int queue = (int)q_col_i64(st, 3);
      long long odid = (long long)q_col_i64(st, 8);
      long long odue = (long long)q_col_i64(st, 9);
      long long due = odid != 0 ? odue : (long long)q_col_i64(st, 2);
      long long ts = due_to_ts(now, queue, due);
      if (ts > now)
        continue;
      const unsigned char *flds = q_col_text(st, 1);
      if (!flds)
        continue;
      if (!cards_reserve(g_card_count + 1))
        break;
      AnkiCard *c = &g_cards[g_card_count++];
      memset(c, 0, sizeof(*c));
      c->id = (long long)q_col_i64(st, 0);
      c->due_ts = ts;
      c->ivl = (long long)q_col_i64(st, 4);
      c->factor = (long long)q_col_i64(st, 5);
      c->queue = queue;
      c->reps = (int)q_col_i64(st, 6);
      c->lapses = (int)q_col_i64(st, 7);
      c->odid = (int)odid;
      c->cloze = strstr((const char *)flds, "{{c") != NULL;
      size_t flen = strlen((const char *)flds) + 1;
      c->flds = malloc(flen);
      if (!c->flds) {
        g_card_count--;
        continue;
      }
      memcpy(c->flds, flds, flen);
    }
    q_finalize(st);
  } else {
    fprintf(stderr, "anki: card query failed: %s\n", q_errmsg(db));
  }
  q_close(db);
  if (g_card_count > 1)
    qsort(g_cards, (size_t)g_card_count, sizeof(*g_cards), card_cmp);
}

static void append_n(const char *s, size_t n, char *out, size_t cap,
                     size_t *o) {
  for (size_t i = 0; i < n && *o + 1 < cap; i++)
    out[(*o)++] = s[i];
}

static bool ci_starts(const char *s, size_t n, const char *prefix) {
  size_t len = strlen(prefix);
  if (n < len)
    return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char a = (unsigned char)s[i];
    unsigned char b = (unsigned char)prefix[i];
    if (tolower(a) != tolower(b))
      return false;
  }
  return true;
}

static bool tag_is_break(const char *t, size_t n) {
  size_t k = 0;
  while (k < n && (t[k] == '/' || isspace((unsigned char)t[k])))
    k++;
  if (k >= n)
    return false;
  static const char *const breaks[] = {"br", "p",  "div", "li", "tr", "ul",
                                       "ol", "h1", "h2",  "h3", "h4", "h5",
                                       "h6"};
  for (size_t i = 0; i < sizeof(breaks) / sizeof(breaks[0]); i++) {
    size_t len = strlen(breaks[i]);
    if (ci_starts(t + k, n - k, breaks[i]) &&
        (k + len >= n || !isalnum((unsigned char)t[k + len])))
      return true;
  }
  return false;
}

static void cloze_append(const char *inner, size_t n, bool answer, char *out,
                         size_t cap, size_t *o) {
  size_t p = 0;
  while (p < n && (inner[p] == 'c' || inner[p] == 'C'))
    p++;
  while (p < n && isdigit((unsigned char)inner[p]))
    p++;
  size_t sep1 = SIZE_MAX;
  for (size_t i = p; i + 1 < n; i++) {
    if (inner[i] == ':' && inner[i + 1] == ':') {
      sep1 = i + 2;
      break;
    }
  }
  size_t sep2 = SIZE_MAX;
  if (sep1 != SIZE_MAX) {
    for (size_t i = sep1; i + 1 < n; i++) {
      if (inner[i] == ':' && inner[i + 1] == ':') {
        sep2 = i;
        break;
      }
    }
  }
  if (answer) {
    size_t start = sep1 != SIZE_MAX ? sep1 : p;
    size_t end = sep2 != SIZE_MAX ? sep2 : n;
    append_n(inner + start, end - start, out, cap, o);
  } else if (sep2 != SIZE_MAX && sep2 + 2 < n) {
    append_n(inner + sep2 + 2, n - sep2 - 2, out, cap, o);
  } else {
    append_n("...", 3, out, cap, o);
  }
}

static void clean_copy(const char *src, char *out, size_t cap, size_t *o,
                       int cloze_mode) {
  size_t len = strlen(src);
  for (size_t i = 0; i < len && *o + 1 < cap;) {
    if (cloze_mode && i + 1 < len && src[i] == '{' && src[i + 1] == '{') {
      size_t j = i + 2;
      while (j + 1 < len && !(src[j] == '}' && src[j + 1] == '}'))
        j++;
      if (j + 1 < len) {
        cloze_append(src + i + 2, j - i - 2, cloze_mode == 2, out, cap, o);
        i = j + 2;
        continue;
      }
    }
    if (src[i] == '[' && i + 7 <= len &&
        strncmp(src + i, "[sound:", 7) == 0) {
      const char *end = strchr(src + i, ']');
      i = end ? (size_t)(end - src) + 1 : len;
      continue;
    }
    if (src[i] == '<') {
      size_t j = i + 1;
      while (j < len && src[j] != '>')
        j++;
      if (j < len) {
        if (tag_is_break(src + i + 1, j - i - 1) && *o + 1 < cap)
          out[(*o)++] = '\n';
        i = j + 1;
        continue;
      }
    }
    if (src[i] == '&') {
      static const char *const names[] = {"nbsp;", "amp;",  "lt;",
                                          "gt;",   "quot;", "amp;"};
      static const char vals[] = {' ', '&', '<', '>', '"', '&'};
      bool matched = false;
      for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
        size_t nl = strlen(names[k]);
        if (i + 1 + nl <= len && strncmp(src + i + 1, names[k], nl) == 0) {
          out[(*o)++] = vals[k];
          i += 1 + nl;
          matched = true;
          break;
        }
      }
      if (matched)
        continue;
    }
    out[(*o)++] = src[i++];
  }
  out[*o] = '\0';
}

static void build_text(const AnkiCard *card, bool answer, char *out,
                       size_t cap) {
  out[0] = '\0';
  if (!card || !card->flds)
    return;
  char f0[ANKI_TEXT_CAP];
  char f1[ANKI_TEXT_CAP];
  const char *sep = strchr(card->flds, '\x1f');
  size_t n0 = sep ? (size_t)(sep - card->flds) : strlen(card->flds);
  if (n0 >= sizeof(f0))
    n0 = sizeof(f0) - 1;
  memcpy(f0, card->flds, n0);
  f0[n0] = '\0';
  f1[0] = '\0';
  if (sep) {
    size_t n1 = strlen(sep + 1);
    if (n1 >= sizeof(f1))
      n1 = sizeof(f1) - 1;
    memcpy(f1, sep + 1, n1);
    f1[n1] = '\0';
  }
  size_t o = 0;
  clean_copy(f0, out, cap, &o, card->cloze ? (answer ? 2 : 1) : 0);
  if (!card->cloze && answer && f1[0]) {
    if (o + 2 < cap) {
      out[o++] = '\n';
      out[o++] = '\n';
      out[o] = '\0';
    }
    clean_copy(f1, out, cap, &o, 0);
  }
  out[o] = '\0';
}

static void build_answer_only(const AnkiCard *card, char *out, size_t cap) {
  out[0] = '\0';
  if (!card || !card->flds)
    return;
  const char *sep = strchr(card->flds, '\x1f');
  size_t n0 = sep ? (size_t)(sep - card->flds) : strlen(card->flds);
  if (n0 >= ANKI_TEXT_CAP)
    n0 = ANKI_TEXT_CAP - 1;
  char f0[ANKI_TEXT_CAP];
  memcpy(f0, card->flds, n0);
  f0[n0] = '\0';
  size_t o = 0;
  if (card->cloze) {
    clean_copy(f0, out, cap, &o, 2);
  } else if (sep) {
    char f1[ANKI_TEXT_CAP];
    size_t n1 = strlen(sep + 1);
    if (n1 >= sizeof(f1))
      n1 = sizeof(f1) - 1;
    memcpy(f1, sep + 1, n1);
    f1[n1] = '\0';
    clean_copy(f1, out, cap, &o, 0);
  }
  out[o] = '\0';
}

static void cloze_add_piece(const char *s, size_t len, ColorRef color) {
  if (g_cloze_seg_count >= ANKI_CLOZE_SEGS || !s || !len)
    return;
  char raw[ANKI_TEXT_CAP];
  char clean[ANKI_TEXT_CAP];
  size_t n = len < sizeof(raw) - 1 ? len : sizeof(raw) - 1;
  memcpy(raw, s, n);
  raw[n] = '\0';
  size_t o = 0;
  clean_copy(raw, clean, sizeof(clean), &o, 0);
  if (o == 0)
    return;
  if (o + 1 > sizeof(g_cloze_pool) - g_cloze_pool_len)
    return;
  memcpy(g_cloze_pool + g_cloze_pool_len, clean, o + 1);
  g_cloze_segs[g_cloze_seg_count].text = g_cloze_pool + g_cloze_pool_len;
  g_cloze_segs[g_cloze_seg_count].color = color;
  g_cloze_seg_count++;
  g_cloze_pool_len += o + 1;
}

static void build_cloze_segments(const AnkiCard *card) {
  g_cloze_seg_count = 0;
  g_cloze_pool_len = 0;
  if (!card || !card->flds)
    return;
  const char *sep = strchr(card->flds, '\x1f');
  size_t n0 = sep ? (size_t)(sep - card->flds) : strlen(card->flds);
  if (n0 > ANKI_TEXT_CAP - 1)
    n0 = ANKI_TEXT_CAP - 1;
  char f0[ANKI_TEXT_CAP];
  memcpy(f0, card->flds, n0);
  f0[n0] = '\0';

  size_t pos = 0;
  while (pos < n0) {
    const char *open = strstr(f0 + pos, "{{c");
    if (!open) {
      cloze_add_piece(f0 + pos, n0 - pos, ANKI_SUB_COLOR);
      break;
    }
    size_t start = (size_t)(open - f0);
    if (start > pos)
      cloze_add_piece(f0 + pos, start - pos, ANKI_SUB_COLOR);
    const char *close = strstr(open, "}}");
    if (!close) {
      cloze_add_piece(f0 + start, n0 - start, ANKI_SUB_COLOR);
      break;
    }
    size_t inner_len = (size_t)(close - (open + 2));
    const char *inner = open + 2;
    size_t p = 0;
    while (p < inner_len && (inner[p] == 'c' || inner[p] == 'C'))
      p++;
    while (p < inner_len && isdigit((unsigned char)inner[p]))
      p++;
    size_t sep1 = SIZE_MAX;
    size_t sep2 = SIZE_MAX;
    for (size_t i = p; i + 1 < inner_len; i++) {
      if (inner[i] == ':' && inner[i + 1] == ':') {
        sep1 = i + 2;
        break;
      }
    }
    if (sep1 != SIZE_MAX) {
      for (size_t i = sep1; i + 1 < inner_len; i++) {
        if (inner[i] == ':' && inner[i + 1] == ':') {
          sep2 = i;
          break;
        }
      }
    }
    size_t astart = sep1 != SIZE_MAX ? sep1 : p;
    size_t aend = sep2 != SIZE_MAX ? sep2 : inner_len;
    if (aend > astart)
      cloze_add_piece(inner + astart, aend - astart, 0);
    pos = (size_t)(close - f0) + 2;
  }
}

static int measure_main_text(int max_w, int line_h, int fs,
                             bool cloze_answer, bool is_question) {
  if (cloze_answer)
    return sk_draw_text_wrapped(.segments = g_cloze_segs,
                                .count = g_cloze_seg_count, .y = 0,
                                .max_width = max_w, .line_height = line_h,
                                .font_size = fs, .measure_only = true);
  int height = sk_draw_text_wrapped(.text = g_qtext, .y = 0,
                                    .max_width = max_w, .line_height = line_h,
                                    .font_size = fs, .measure_only = true);
  if (!is_question && g_abody[0])
    height += fs + sk_draw_text_wrapped(.text = g_abody, .y = 0,
                                        .max_width = max_w,
                                        .line_height = line_h,
                                        .font_size = fs,
                                        .measure_only = true);
  return height;
}

static void init_revlog_id(sqlite3 *db) {
  if (g_revlog_init)
    return;
  g_revlog_init = true;
  sqlite3_stmt *st = NULL;
  if (q_prepare(db, "SELECT MAX(id) FROM revlog;", -1, &st, NULL) == SQLITE_OK) {
    if (q_step(st) == SQLITE_ROW)
      g_last_revlog_ms = (long long)q_col_i64(st, 0);
    q_finalize(st);
  }
}

static void grade_card(int grade, double elapsed_ms) {
  if (!g_cards || g_card_pos < 0 || g_card_pos >= g_card_count)
    return;
  AnkiCard *c = &g_cards[g_card_pos];
  if (!sqlite_init())
    return;
  char path[1024];
  if (!anki_db_path(path, sizeof(path)))
    return;
  sqlite3 *db = NULL;
  if (!db_open(path, SQLITE_OPEN_READWRITE, &db))
    return;
  load_crt(db);
  init_revlog_id(db);

  long long ivl = c->ivl;
  long long factor = c->factor > 0 ? c->factor : ANKI_START_FACTOR;
  int reps = c->reps + 1;
  int lapses = c->lapses;
  int queue = c->queue;
  sqlite3_stmt *cur = NULL;
  if (q_prepare(db, "SELECT ivl, factor, reps, lapses, queue, odid FROM "
                    "cards WHERE id = ?;",
                -1, &cur, NULL) == SQLITE_OK) {
    q_bind_i64(cur, 1, c->id);
    if (q_step(cur) == SQLITE_ROW) {
      ivl = (long long)q_col_i64(cur, 0);
      factor = (long long)q_col_i64(cur, 1);
      reps = (int)q_col_i64(cur, 2) + 1;
      lapses = (int)q_col_i64(cur, 3);
      queue = (int)q_col_i64(cur, 4);
      c->odid = (int)q_col_i64(cur, 5);
    }
    q_finalize(cur);
  }
  if (factor <= 0)
    factor = ANKI_START_FACTOR;
  int rtype = queue == 0 || queue == 1 ? 0 : (queue == 2 ? 1 : 2);
  long long now = (long long)time(NULL);
  long long today = g_crt > 0 ? (now - g_crt) / 86400 : 0;
  long long new_ivl;
  long long due_value;
  int new_queue;

  if (rtype != 1) {
    if (grade == 1) {
      new_queue = rtype;
      new_ivl = -ANKI_LEARN_STEP_1;
      due_value = now + ANKI_LEARN_STEP_1;
    } else if (grade == 2) {
      long long step = ANKI_LEARN_STEP_2;
      new_queue = rtype;
      new_ivl = -step;
      due_value = now + step;
    } else {
      new_queue = 2;
      if (grade == 4) {
        factor += 150;
        new_ivl = 4;
      } else {
        new_ivl = 1;
      }
      due_value = today + new_ivl;
    }
  } else if (grade == 1) {
    lapses++;
    factor -= 200;
    if (factor < ANKI_MIN_FACTOR)
      factor = ANKI_MIN_FACTOR;
    new_queue = 3;
    new_ivl = -ANKI_LEARN_STEP_1;
    due_value = now + ANKI_LEARN_STEP_1;
  } else {
    double mult;
    if (grade == 2) {
      factor -= 150;
      mult = 1.2;
    } else {
      mult = (double)factor / 1000.0;
      if (grade == 4) {
        factor += 150;
        mult *= 1.3;
      }
    }
    if (factor < ANKI_MIN_FACTOR)
      factor = ANKI_MIN_FACTOR;
    long long base = ivl > 0 ? ivl : 1;
    new_ivl = (long long)(base * mult);
    if (new_ivl < 1)
      new_ivl = 1;
    new_queue = 2;
    due_value = today + new_ivl;
  }

  if (q_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
    fprintf(stderr, "anki: cannot begin transaction: %s\n", q_errmsg(db));
    q_close(db);
    return;
  }

  char sql[256];
  snprintf(sql, sizeof(sql),
           "UPDATE cards SET ivl=?, factor=?, reps=?, lapses=?, queue=?, "
           "%s=?, mod=? WHERE id=?;",
           c->odid != 0 ? "odue" : "due");
  sqlite3_stmt *st = NULL;
  bool ok = q_prepare(db, sql, -1, &st, NULL) == SQLITE_OK;
  if (ok) {
    q_bind_i64(st, 1, new_ivl);
    q_bind_i64(st, 2, factor);
    q_bind_i64(st, 3, reps);
    q_bind_i64(st, 4, lapses);
    q_bind_i64(st, 5, new_queue);
    q_bind_i64(st, 6, due_value);
    q_bind_i64(st, 7, now);
    q_bind_i64(st, 8, c->id);
    ok = q_step(st) == SQLITE_DONE;
    q_finalize(st);
  }
  if (!ok)
    fprintf(stderr, "anki: card update failed: %s\n", q_errmsg(db));

  long long rid = now * 1000;
  if (rid <= g_last_revlog_ms)
    rid = g_last_revlog_ms + 1;
  g_last_revlog_ms = rid;
  long long ms = elapsed_ms;
  if (ms < 0)
    ms = 0;
  if (ms > 86400000)
    ms = 86400000;
  st = NULL;
  if (ok && q_prepare(db, "INSERT INTO revlog (id, cid, usn, ease, ivl, "
                          "lastIvl, factor, time, type) VALUES "
                          "(?,?,?,?,?,?,?,?,?);",
                      -1, &st, NULL) == SQLITE_OK) {
    q_bind_i64(st, 1, rid);
    q_bind_i64(st, 2, c->id);
    q_bind_i64(st, 3, -1);
    q_bind_i64(st, 4, grade);
    q_bind_i64(st, 5, new_ivl);
    q_bind_i64(st, 6, ivl);
    q_bind_i64(st, 7, factor);
    q_bind_i64(st, 8, ms);
    q_bind_i64(st, 9, rtype);
    ok = q_step(st) == SQLITE_DONE;
    q_finalize(st);
  }
  if (!ok)
    fprintf(stderr, "anki: revlog insert failed: %s\n", q_errmsg(db));

  st = NULL;
  if (ok && q_prepare(db, "UPDATE col SET mod=?;", -1, &st, NULL) ==
                SQLITE_OK) {
    q_bind_i64(st, 1, now);
    ok = q_step(st) == SQLITE_DONE;
    q_finalize(st);
  }
  if (!ok)
    fprintf(stderr, "anki: collection update failed: %s\n", q_errmsg(db));

  if (!ok || q_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
    q_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    fprintf(stderr, "anki: transaction failed: %s\n", q_errmsg(db));
    q_close(db);
    return;
  }
  q_close(db);

  c->queue = new_queue;
  c->ivl = new_ivl;
  c->factor = factor;
  c->reps = reps;
  c->lapses = lapses;
  c->due_ts = due_to_ts(now, new_queue, due_value);
}

static AnkiDeck *find_deck(const char *name) {
  if (!name)
    return NULL;
  for (int i = 0; i < g_deck_count; i++) {
    if (strcmp(g_decks[i].name, name) == 0)
      return &g_decks[i];
  }
  return NULL;
}

static void drain_grade_keys(void) {
  sk_is_key_pressed(SK_KEY_SPACE);
  sk_is_key_pressed(SK_KEY_1);
  sk_is_key_pressed(SK_KEY_2);
  sk_is_key_pressed(SK_KEY_3);
  sk_is_key_pressed(SK_KEY_4);
}

static void reveal_card(void) {
  if (g_view != ANKI_VIEW_CARD || g_card_pos >= g_card_count)
    return;
  g_side = ANKI_SIDE_ANSWER;
  g_answer_timer = sk_timer_start();
  build_text(&g_cards[g_card_pos], true, g_atext, sizeof(g_atext));
  build_answer_only(&g_cards[g_card_pos], g_abody, sizeof(g_abody));
  if (g_cards[g_card_pos].cloze)
    build_cloze_segments(&g_cards[g_card_pos]);
  drain_grade_keys();
}

static void next_card(void) {
  if (g_card_pos >= g_card_count) {
    char name[ANKI_DECK_NAME];
    snprintf(name, sizeof(name), "%s", g_deck_name);
    load_cards(name);
    g_card_pos = 0;
    if (g_card_count == 0) {
      g_view = ANKI_VIEW_DECKS;
      load_decks();
      drain_grade_keys();
      return;
    }
  }
  g_side = ANKI_SIDE_QUESTION;
  build_text(&g_cards[g_card_pos], false, g_qtext, sizeof(g_qtext));
  drain_grade_keys();
}

static void handle_grade_keys(void) {
  if (g_view != ANKI_VIEW_CARD || g_card_pos >= g_card_count)
    return;
  if (g_side == ANKI_SIDE_QUESTION) {
    if (sk_is_key_pressed(SK_KEY_SPACE) || sk_button_clicked("show"))
      reveal_card();
    return;
  }
  int grade = 0;
  if (sk_is_key_pressed(SK_KEY_1) || sk_button_clicked("again"))
    grade = 1;
  else if (sk_is_key_pressed(SK_KEY_2) || sk_button_clicked("good"))
    grade = 2;
  else if (sk_is_key_pressed(SK_KEY_3) || sk_button_clicked("hard"))
    grade = 3;
  else if (sk_is_key_pressed(SK_KEY_4) || sk_button_clicked("easy"))
    grade = 4;
  else if (sk_is_key_pressed(SK_KEY_SPACE))
    grade = 3;
  if (grade == 0)
    return;
  stats_check_day();
  g_today_reviewed++;
  if (grade == 1)
    g_today_again++;
  double elapsed = sk_timer_end(g_answer_timer);
  grade_card(grade, elapsed);
  g_card_pos++;
  next_card();
  sk_focus_clear();
}

static void count_remaining(int *out_new, int *out_learning, int *out_due) {
  int new_count = 0;
  int learning = 0;
  int due = 0;
  for (int i = 0; i < g_card_count; i++) {
    int q = g_cards[i].queue;
    if (q == 0)
      new_count++;
    else if (q == 1 || q == 3)
      learning++;
    else if (q == 2)
      due++;
  }
  if (out_new)
    *out_new = new_count;
  if (out_learning)
    *out_learning = learning;
  if (out_due)
    *out_due = due;
}

static void predict_interval(const AnkiCard *c, int grade, char *out,
                             size_t n) {
  long long ivl = c->ivl;
  long long factor = c->factor > 0 ? c->factor : ANKI_START_FACTOR;
  int rtype = c->queue == 0 || c->queue == 1 ? 0 : (c->queue == 2 ? 1 : 2);
  long long new_ivl;

  if (rtype != 1) {
    if (grade == 1)
      new_ivl = -ANKI_LEARN_STEP_1;
    else if (grade == 2)
      new_ivl = -ANKI_LEARN_STEP_2;
    else if (grade == 4)
      new_ivl = 4;
    else
      new_ivl = 1;
  } else if (grade == 1) {
    new_ivl = -ANKI_LEARN_STEP_1;
  } else {
    double mult;
    if (grade == 2) {
      mult = 1.2;
    } else {
      mult = (double)factor / 1000.0;
      if (grade == 4)
        mult *= 1.3;
    }
    long long base = ivl > 0 ? ivl : 1;
    new_ivl = (long long)(base * mult);
    if (new_ivl < 1)
      new_ivl = 1;
  }

  if (new_ivl < 0) {
    long long sec = -new_ivl;
    if (sec < 60)
      snprintf(out, n, "%llds", sec);
    else if (sec < 3600)
      snprintf(out, n, "%lldm", sec / 60);
    else
      snprintf(out, n, "%lldh", sec / 3600);
  } else if (new_ivl < 30) {
    snprintf(out, n, "%lldd", new_ivl);
  } else if (new_ivl < 365) {
    snprintf(out, n, "%lldmo", new_ivl / 30);
  } else {
    snprintf(out, n, "%lldy", new_ivl / 365);
  }
}

static void anki_refresh(PromptState *state) {
  if (!state || !state->lb)
    return;
  if (g_view != ANKI_VIEW_DECKS) {
    if (state->lb->item_count > 0)
      sk_populate_listbox(.lb = state->lb, .filter = NULL, .items = NULL,
                          .count = 0);
    return;
  }
  sk_populate_listbox(.lb = state->lb, .filter = state->eb->filter,
                      .items = g_deck_items, .count = g_deck_count);
}

static int stats_day_key(void) {
  time_t now = time(NULL);
  struct tm *local = localtime(&now);
  if (!local)
    return 0;
  return (local->tm_year + 1900) * 10000 + (local->tm_mon + 1) * 100 +
         local->tm_mday;
}

static void stats_check_day(void) {
  int day = stats_day_key();
  if (g_today_day == day)
    return;
  g_today_day = day;
  g_today_ms = 0;
  g_today_reviewed = 0;
  g_today_again = 0;
}

static void stats_format_duration(double ms, char *out, size_t n) {
  long long total = ms > 0 ? (long long)(ms / 1000.0) : 0;
  long long hours = total / 3600;
  long long minutes = (total % 3600) / 60;
  long long seconds = total % 60;
  if (hours > 0)
    snprintf(out, n, "%lldh %lldm %llds", hours, minutes, seconds);
  else if (minutes > 0)
    snprintf(out, n, "%lldm %llds", minutes, seconds);
  else
    snprintf(out, n, "%llds", seconds);
}

static void anki_info_render(PromptState *state) {
  (void)state;
  stats_check_day();
  int fs = FontSize + 6;
  int lh = fs + 14;
  int y = 80;
  int x = 20;
  sk_draw_rect(.x=x - 5,.y=y - 5,.w=420,.h=270,.filled=true,.color=sk_color_lerp(COLOR_BG,SK_BLACK,0.6f),.radius=6);
  sk_draw_text(.text = "Today:", .x = x, .y = y, .font_size = FontSize + 12,
               .color = sk_color_lerp(COLOR_BG,SK_BLUE,0.5f), .align = "LEFT");
  y += lh + 10;

  char time_buf[64], reviewed_buf[32], again_buf[32], pct_buf[32],
      per_card_buf[64];
  stats_format_duration(g_today_ms, time_buf, sizeof(time_buf));
  snprintf(reviewed_buf, sizeof(reviewed_buf), "%d", g_today_reviewed);
  snprintf(again_buf, sizeof(again_buf), "%d", g_today_again);
  int correct = g_today_reviewed > 0
                    ? (g_today_reviewed - g_today_again) * 100 /
                          g_today_reviewed
                    : 0;
  snprintf(pct_buf, sizeof(pct_buf), "%d%%", correct);
  if (g_today_reviewed > 0)
    stats_format_duration(g_today_ms / g_today_reviewed, per_card_buf,
                          sizeof(per_card_buf));
  else
    snprintf(per_card_buf, sizeof(per_card_buf), "-");

  struct {
    const char *label;
    const char *value;
  } rows[] = {
      {"Time Spent:", time_buf},
      {"Cards Reviewed:", reviewed_buf},
      {"Again:", again_buf},
      {"Correct:", pct_buf},
      {"Time Per Card:", per_card_buf},
  };
  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    sk_draw_text(.text = rows[i].label, .x = x, .y = y, .font_size = fs,
                 .color = sk_color_lerp(COLOR_BG,SK_WHITE,0.6f), .align = "LEFT");
    sk_draw_text(.text = rows[i].value, .x = 340, .y = y, .font_size = fs, .align = "LEFT");
    y += lh;
  }
}

static void anki_go_to_decks(PromptState *state) {
  free_cards();
  load_decks();
  g_view = ANKI_VIEW_DECKS;
  g_focus_mode = false;
  sk_focus_clear();
  drain_grade_keys();
  memset(state->eb->filter, 0, sizeof(state->eb->filter));
  sk_request_refresh();
}

static void anki_render(PromptState *state) {
  if (!state)
    return;
  if (g_view == ANKI_VIEW_CARD && sk_is_key_pressed(SK_KEY_F))
    g_focus_mode = !g_focus_mode;
  if (g_view == ANKI_VIEW_INFO) {
    bool decks_clicked = sk_button_clicked("decks");
    sk_place_button(.name="decks",.text="Decks",.x=24,.y=24,.w=100,.h=40);
    anki_info_render(state);
    if (decks_clicked || sk_is_key_pressed(SK_KEY_D)) {
      anki_go_to_decks(state);
      return;
    }
    return;
  }
  if (g_view == ANKI_VIEW_CARD) {
    if (sk_is_key_pressed(SK_KEY_I)) {
      g_view = ANKI_VIEW_INFO;
      g_focus_mode = false;
      sk_focus_clear();
      sk_request_refresh();
      return;
    }
    if (sk_is_key_pressed(SK_KEY_D)) {
      anki_go_to_decks(state);
      return;
    }
    bool decks_clicked = sk_button_clicked("decks");
    bool info_clicked = sk_button_clicked("info");
    if (!g_focus_mode) {
      sk_place_button(.name="decks",.text="Decks",.x=24,.y=24,.w=100,.h=40);
      sk_place_button(.name="info",.text="Info",.x=132,.y=24,.w=100,.h=40);
    }
    if (decks_clicked) {
      anki_go_to_decks(state);
      return;
    }
    if (info_clicked) {
      g_view = ANKI_VIEW_INFO;
      g_focus_mode = false;
      sk_focus_clear();
      sk_request_refresh();
      return;
    }
  }
  handle_grade_keys();
  if (g_view == ANKI_VIEW_DECKS) {
    sk_place_editbox();
    if (!sk_listbox_begin(.lb = state->lb, .item_height = FontSize * 2))
      return;
    SkListItemCtx ctx;
    while (sk_listbox_next(&ctx)) {
      sk_draw_text(.text = ctx.text, .x = ctx.x + 12, .y = ctx.y + 4,
                   .font_size = FontSize, .align = "LEFT");
      const AnkiDeck *d = find_deck(ctx.text);
      if (d) {
        char count[64];
        snprintf(count, sizeof(count), "%d due", d->due);
        sk_draw_text(.text = count, .x = state->width - 16, .y = ctx.y,
                     .font_size = 23, .align = "RIGHT", .color = ANKI_SUB_COLOR);
      }
    }
    sk_listbox_end();
    return;
  }

  bool is_question = g_side == ANKI_SIDE_QUESTION;
  bool cloze_answer = !is_question && g_card_pos < g_card_count &&
                      g_cards[g_card_pos].cloze;
  int max_w = state->width - 48;
  int top_margin = 16;
  int bottom_margin = g_focus_mode ? 16 : 130;
  int avail_h = state->height - top_margin - bottom_margin;
  if (avail_h < 40)
    avail_h = 40;
  int main_fs = FontSize + 16 + (g_focus_mode ? 10 : 0);
  int text_h = measure_main_text(max_w, main_fs, main_fs, cloze_answer,
                                 is_question);
  while (text_h > avail_h && main_fs > 12) {
    main_fs -= 2;
    text_h = measure_main_text(max_w, main_fs, main_fs, cloze_answer,
                               is_question);
  }
  int body_y = top_margin;
  if (text_h < avail_h)
    body_y += (avail_h - text_h) / 2;
  if (cloze_answer) {
    sk_draw_text_wrapped(.segments = g_cloze_segs, .count = g_cloze_seg_count,
                         .x = 24, .y = body_y, .max_width = max_w,
                         .line_height = main_fs, .font_size = main_fs,
                         .color = COLOR_TEXT, .align = "LEFT");
  } else {
    int next_y = sk_draw_text_wrapped(.text = g_qtext, .x = 24, .y = body_y,
                                      .max_width = max_w,
                                      .line_height = main_fs,
                                      .font_size = main_fs,
                                      .color = ANKI_SUB_COLOR, .align = "LEFT");
    if (!is_question && g_abody[0])
      sk_draw_text_wrapped(.text = g_abody, .x = 24, .y = next_y + main_fs,
                           .max_width = max_w, .line_height = main_fs,
                           .font_size = main_fs, .color = COLOR_TEXT,
                           .align = "LEFT");
  }
  if (!g_focus_mode) {
    if (g_side == ANKI_SIDE_ANSWER) {
      sk_place_button(.name="again",.text="Again",.x=154,.h=50,.y=state->height-76);
      sk_place_button(.name="hard",.text="Hard",.x=304,.h=50,.y=state->height-76);
      sk_place_button(.name="good",.text="Good",.x=454,.h=50,.y=state->height-76);
      sk_place_button(.name="easy",.text="Easy",.x=604,.h=50,.y=state->height-76);

      char iv_again[16] = "", iv_hard[16] = "", iv_good[16] = "",
           iv_easy[16] = "";
      if (g_card_pos < g_card_count) {
        const AnkiCard *card = &g_cards[g_card_pos];
        predict_interval(card, 1, iv_again, sizeof(iv_again));
        predict_interval(card, 2, iv_hard, sizeof(iv_hard));
        predict_interval(card, 3, iv_good, sizeof(iv_good));
        predict_interval(card, 4, iv_easy, sizeof(iv_easy));
      }
      sk_draw_text(.text = iv_again, .x = 184, .y = state->height - 110);
      sk_draw_text(.text = iv_hard, .x = 344, .y = state->height - 110);
      sk_draw_text(.text = iv_good, .x = 494, .y = state->height - 110);
      sk_draw_text(.text = iv_easy, .x = 644, .y = state->height - 110);
    } else {
      int new_count = 0, learning = 0, due = 0;
      count_remaining(&new_count, &learning, &due);
      char new_buf[16], learning_buf[16], due_buf[16];
      snprintf(new_buf, sizeof(new_buf), "%d", new_count);
      snprintf(learning_buf, sizeof(learning_buf), "%d", learning);
      snprintf(due_buf, sizeof(due_buf), "%d", due);
      SkTextSegment segments[5] = {
          {new_buf, SK_BLUE},      {" + ", ANKI_SUB_COLOR}, {learning_buf, SK_RED},
          {" + ", ANKI_SUB_COLOR}, {due_buf, SK_GREEN},
      };
      sk_draw_text_segments(.segments = segments, .count = 5, .x = 435,
                            .y = state->height - 108, .font_size = 20,
                            .color = ANKI_SUB_COLOR, .align = "CENTER");
      sk_place_button(.name="show",.text="Show Answer",.x=360,.h=50,.w=150,.y=state->height-76);
    }
  }
}

static bool anki_enter(PromptState *state) {
  if (!state)
    return false;
  if (g_view == ANKI_VIEW_CARD) {
    reveal_card();
    return false;
  }
  if (g_view == ANKI_VIEW_INFO)
    return false;
  if (!sk_is_selected(state->lb) || !state->lb->selected_text)
    return false;
  const char *sel = state->lb->selected_text;
  load_cards(sel);
  g_view = ANKI_VIEW_CARD;
  g_focus_mode = false;
  g_card_pos = 0;
  if (g_card_count == 0) {
    sk_message(.message = "No due cards in this deck.", .slot = TOP_CENTER,
               .timeout = 2.0);
    g_view = ANKI_VIEW_DECKS;
    load_decks();
    return false;
  }
  g_side = ANKI_SIDE_QUESTION;
  build_text(&g_cards[0], false, g_qtext, sizeof(g_qtext));
  sk_set_edit_text("");
  if (state->lb->item_count)
    sk_populate_listbox(.lb = state->lb, .filter = NULL, .items = NULL,
                        .count = 0);
  drain_grade_keys();
  return false;
}

static void anki_on_close(void) {
  stats_check_day();
  double ms = sk_timer_end(g_session_timer);
  if (ms > 0)
    g_today_ms += ms;
  g_view = ANKI_VIEW_DECKS;
  g_focus_mode = false;
  free_cards();
  free_decks();
}

static void open_anki(void) {
  load_decks();
  if (sk_process_running("anki")) {
    sk_message(.slot=TOP_CENTER,.message="Anki is currently running please close it and try again"); 
    return;
  } 

  stats_check_day();
  g_session_timer = sk_timer_start();
  sk_window(.title = "Anki", .placeholder = "Decks", .refresh = anki_refresh,
            .render = anki_render, .enter = anki_enter,
            .on_close = anki_on_close);
}

SKALD_FEATURE_SHUTDOWN {
  free_cards();
  free_decks();
  if (g_sqlite_lib) {
    sk_library_close(g_sqlite_lib);
    g_sqlite_lib = NULL;
  }
}

SKALD_FEATURE_INIT {
  sk_register_hotkey(.hotkey="Alt+N",.cb=open_anki);
  sk_add_feature(.display_name = "Anki", .on_action = open_anki,
                 .icon = "/anki.png");
}
