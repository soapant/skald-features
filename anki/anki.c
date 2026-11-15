#include "core/skapi.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

typedef enum { CARD_TYPE_BASIC, CARD_TYPE_CLOZE, CARD_TYPE_UNKNOWN } CardType;
typedef struct {
  char *front;
  char *back;
} BasicCard;
typedef struct {
  char *text;
} ClozeCard;
typedef struct {
  CardType type;
  long long id;
  long long due;
  int queue;
  long long due_ts;
  union {
    BasicCard basic;
    ClozeCard cloze;
  } data;
} TYPE_NAME;
typedef struct {
  TYPE_NAME *items;
  size_t count;
  size_t capacity;
} CardList;

typedef enum {
  ANKI_STATE_IDLE = 0,
  ANKI_STATE_QUESTION,
  ANKI_STATE_ANSWER
} AnkiState;

static TYPE_NAME current_card = {0};
static AnkiState anki_state = ANKI_STATE_IDLE;
static SkTimer q_timer = {0};

static SkLibrary g_sqlite_lib = NULL;
static int (*p_sqlite3_open_v2)(const char *, sqlite3 **, int,
                                const char *) = NULL;
static const char *(*p_sqlite3_errmsg)(sqlite3 *) = NULL;
static int (*p_sqlite3_prepare_v2)(sqlite3 *, const char *, int,
                                   sqlite3_stmt **, const char **) = NULL;
static int (*p_sqlite3_step)(sqlite3_stmt *) = NULL;
static const unsigned char *(*p_sqlite3_column_text)(sqlite3_stmt *,
                                                     int) = NULL;
static long long (*p_sqlite3_column_int64)(sqlite3_stmt *, int) = NULL;
static int (*p_sqlite3_finalize)(sqlite3_stmt *) = NULL;
static int (*p_sqlite3_bind_text)(sqlite3_stmt *, int, const char *, int,
                                  void (*)(void *)) = NULL;
static int (*p_sqlite3_close)(sqlite3 *) = NULL;
static int (*p_sqlite3_bind_int64)(sqlite3_stmt *, int, sqlite3_int64) = NULL;

static int time_between_qs = 10;

#define USED_DECK "" // your wanted deck

static char *safe_strdup(const char *s) {
  if (!s)
    return NULL;
  size_t len = strlen(s) + 1;
  char *new_s = malloc(len);
  if (new_s)
    memcpy(new_s, s, len);
  return new_s;
}

static TYPE_NAME card_clone(TYPE_NAME card) {
  TYPE_NAME clone = card;
  if (card.type == CARD_TYPE_BASIC) {
    clone.data.basic.front = safe_strdup(card.data.basic.front);
    clone.data.basic.back = safe_strdup(card.data.basic.back);
  } else if (card.type == CARD_TYPE_CLOZE) {
    clone.data.cloze.text = safe_strdup(card.data.cloze.text);
  }
  return clone;
}

static void free_card_contents(TYPE_NAME *card) {
  if (card->type == CARD_TYPE_BASIC) {
    free(card->data.basic.front);
    free(card->data.basic.back);
    card->data.basic.front = NULL;
    card->data.basic.back = NULL;
  } else if (card->type == CARD_TYPE_CLOZE) {
    free(card->data.cloze.text);
    card->data.cloze.text = NULL;
  }
  card->type = CARD_TYPE_UNKNOWN;
}

static int sqlite_init(void) {
  if (g_sqlite_lib)
    return 1;
  char buf[1024] = {0};
  char *dir = sk_resolve_feature_path("anki");
#if defined(_WIN32)
  snprintf(buf, sizeof(buf), "%s/sqlite3.dll", dir);
#elif defined(__linux__)
  snprintf(buf, sizeof(buf), "%s/libsqlite3.so", dir);
#endif
  g_sqlite_lib = sk_library_open(buf);
  if (g_sqlite_lib)
    sk_library_register("sqlite", g_sqlite_lib);
  if (!g_sqlite_lib)
    return 0;
  p_sqlite3_open_v2 =
      (int (*)(const char *, sqlite3 **, int, const char *))sk_library_symbol(
          g_sqlite_lib, "sqlite3_open_v2");
  p_sqlite3_errmsg = (const char *(*)(sqlite3 *))sk_library_symbol(
      g_sqlite_lib, "sqlite3_errmsg");
  p_sqlite3_prepare_v2 =
      (int (*)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **))
          sk_library_symbol(g_sqlite_lib, "sqlite3_prepare_v2");
  p_sqlite3_step =
      (int (*)(sqlite3_stmt *))sk_library_symbol(g_sqlite_lib, "sqlite3_step");
  p_sqlite3_column_text =
      (const unsigned char *(*)(sqlite3_stmt *, int))sk_library_symbol(
          g_sqlite_lib, "sqlite3_column_text");
  p_sqlite3_column_int64 =
      (long long (*)(sqlite3_stmt *, int))sk_library_symbol(
          g_sqlite_lib, "sqlite3_column_int64");
  p_sqlite3_finalize = (int (*)(sqlite3_stmt *))sk_library_symbol(
      g_sqlite_lib, "sqlite3_finalize");
  p_sqlite3_bind_text =
      (int (*)(sqlite3_stmt *, int, const char *, int, void (*)(void *)))
          sk_library_symbol(g_sqlite_lib, "sqlite3_bind_text");
  p_sqlite3_close =
      (int (*)(sqlite3 *))sk_library_symbol(g_sqlite_lib, "sqlite3_close");
  p_sqlite3_bind_int64 =
      (int (*)(sqlite3_stmt *, int, long long))sk_library_symbol(
          g_sqlite_lib, "sqlite3_bind_int64");
  return p_sqlite3_open_v2 && p_sqlite3_errmsg && p_sqlite3_prepare_v2 &&
         p_sqlite3_step;
}

static long long anki_crt = 0;

static long long anki_due_ts(long long now, int queue, long long due) {
  if (queue == 1 || queue == 3) {
    return due;
  }
  if (queue == 2) {
    long long days_elapsed = (now - anki_crt) / 86400;
    long long next_day_at = anki_crt + (days_elapsed + 1) * 86400;
    return next_day_at + (due - days_elapsed) * 86400;
  }
  return 0;
}

static int card_due_cmp(const void *a, const void *b) {
  const TYPE_NAME *ca = (const TYPE_NAME *)a;
  const TYPE_NAME *cb = (const TYPE_NAME *)b;
  if (ca->due_ts < cb->due_ts)
    return -1;
  if (ca->due_ts > cb->due_ts)
    return 1;
  return 0;
}

static void card_list_push(CardList *list, TYPE_NAME card) {
  if (list->count == list->capacity) {
    list->capacity = list->capacity == 0 ? 16 : list->capacity * 2;
    list->items = realloc(list->items, list->capacity * sizeof(TYPE_NAME));
  }
  list->items[list->count++] = card;
}

static void free_card_list(CardList *list) {
  for (size_t i = 0; i < list->count; i++) {
    if (list->items[i].type == CARD_TYPE_BASIC) {
      free(list->items[i].data.basic.front);
      free(list->items[i].data.basic.back);
    } else if (list->items[i].type == CARD_TYPE_CLOZE) {
      free(list->items[i].data.cloze.text);
    }
  }
  free(list->items);
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
}

static CardList get_cards_in_deck(const char *db_path,
                                  const char *target_deck_name) {
  CardList list = {0};
  if (!sqlite_init()) {
    fprintf(stderr, "sqlite lib not found\n");
    return list;
  }
  sqlite3 *db = NULL;
  sqlite3_stmt *stmt = NULL;
  if (p_sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) !=
      SQLITE_OK) {
    fprintf(stderr, "Failed to open DB: %s\n", p_sqlite3_errmsg(db));
    return list;
  }
  {
    sqlite3_stmt *cs = NULL;
    if (p_sqlite3_prepare_v2(db, "SELECT crt FROM col;", -1, &cs, NULL) ==
        SQLITE_OK) {
      if (p_sqlite3_step(cs) == SQLITE_ROW)
        anki_crt = p_sqlite3_column_int64(cs, 0);
      p_sqlite3_finalize(cs);
    }
  }
  char *deck_id = NULL;
  if (p_sqlite3_prepare_v2(db, "SELECT id, name FROM decks;", -1, &stmt,
                           NULL) == SQLITE_OK) {
    while (p_sqlite3_step(stmt) == SQLITE_ROW) {
      long long id = p_sqlite3_column_int64(stmt, 0);
      const char *name = (const char *)p_sqlite3_column_text(stmt, 1);
      if (name && strcmp(name, target_deck_name) == 0) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%lld", id);
        deck_id = safe_strdup(tmp);
        break;
      }
    }
    p_sqlite3_finalize(stmt);
    stmt = NULL;
  }
  if (!deck_id) {
    fprintf(stderr, "Deck '%s' not found in collection.\n", target_deck_name);
    p_sqlite3_close(db);
    return list;
  }
  const char *query = "SELECT c.id, n.flds, n.mid, c.due, c.queue FROM notes n "
                      "JOIN cards c ON n.id = c.nid WHERE c.did = ?;";
  if (p_sqlite3_prepare_v2(db, query, -1, &stmt, NULL) == SQLITE_OK) {
    p_sqlite3_bind_text(stmt, 1, deck_id, -1, SQLITE_TRANSIENT);
    while (p_sqlite3_step(stmt) == SQLITE_ROW) {
      const char *flds = (const char *)p_sqlite3_column_text(stmt, 1);
      if (!flds)
        continue;
      TYPE_NAME card = {0};
      card.id = p_sqlite3_column_int64(stmt, 0);
      card.due = p_sqlite3_column_int64(stmt, 3);
      card.queue = (int)p_sqlite3_column_int64(stmt, 4);
      card.due_ts = anki_due_ts((long long)time(NULL), card.queue, card.due);
      if (strstr(flds, "{{c") != NULL) {
        card.type = CARD_TYPE_CLOZE;
        char *csep = strchr(flds, '\x1f');
        size_t clen = csep ? (size_t)(csep - flds) : strlen(flds);
        card.data.cloze.text = malloc(clen + 1);
        strncpy(card.data.cloze.text, flds, clen);
        card.data.cloze.text[clen] = '\0';
      } else {
        card.type = CARD_TYPE_BASIC;
        char *sep = strchr(flds, '\x1f');
        if (sep) {
          size_t front_len = sep - flds;
          char *front = malloc(front_len + 1);
          strncpy(front, flds, front_len);
          front[front_len] = '\0';
          card.data.basic.front = front;
          card.data.basic.back = safe_strdup(sep + 1);
        } else {
          card.data.basic.front = safe_strdup(flds);
          card.data.basic.back = safe_strdup("");
        }
      }
      card_list_push(&list, card);
    }
    p_sqlite3_finalize(stmt);
    stmt = NULL;
  } else {
    fprintf(stderr, "Failed to prepare query: %s\n", p_sqlite3_errmsg(db));
  }
  qsort(list.items, list.count, sizeof(TYPE_NAME), card_due_cmp);
  free(deck_id);
  p_sqlite3_close(db);
  return list;
}

static void display_card_question(TYPE_NAME card) {
  char question[1024];
  question[0] = '\0';
  if (card.type == CARD_TYPE_BASIC) {
    snprintf(question, sizeof(question), "%s", card.data.basic.front);
  } else if (card.type == CARD_TYPE_CLOZE) {
    const char *src = card.data.cloze.text;
    size_t src_len = strlen(src);
    size_t o = 0;

    size_t i = 0;
    while (i < src_len && o + 1 < sizeof(question)) {
      if (src[i] == '{') {
        size_t j = i + 1;
        while (j < src_len && src[j] != '}')
          j++;

        if (j < src_len) {
          const char *replacement = "[...]";
          for (size_t k = 0; replacement[k] && o + 1 < sizeof(question); k++) {
            question[o++] = replacement[k];
          }
          i = j + 2;
          continue;
        }
      }
      question[o++] = src[i++];
    }
    question[o] = '\0';
  }
  q_timer = sk_start_timer();
  sk_message(.name = "question", .message = question, .slot = TOP_CENTER,
             .timeout = 10000.0);
}

static void anki_apply_elapsed(double elapsed_ms) {
  double elapsed_s = elapsed_ms / 1000.0;

  if (elapsed_s > 20)
    return;

  char buf[300];
#if defined(_WIN32)
  snprintf(buf, sizeof(buf), "%s/AppData/Roaming/Anki2/User 1/collection.anki2",
           sk_resolve_user_path());
#elif defined(__linux__)
  snprintf(buf, sizeof(buf), "%s/.local/share/Anki2/User 1/collection.anki2",
           sk_resolve_user_path());
#endif

  if (!sqlite_init())
    return;

  sqlite3 *db = NULL;
  if (p_sqlite3_open_v2(buf, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
    if (db)
      p_sqlite3_close(db);
    return;
  }

  long long current_ivl = 0;
  long long current_reps = 0;
  int current_queue = 1;

  sqlite3_stmt *select_stmt = NULL;
  if (p_sqlite3_prepare_v2(db,
                           "SELECT ivl, reps, queue FROM cards WHERE id = ?;",
                           -1, &select_stmt, NULL) == SQLITE_OK) {
    p_sqlite3_bind_int64(select_stmt, 1, current_card.id);
    if (p_sqlite3_step(select_stmt) == SQLITE_ROW) {
      current_ivl = p_sqlite3_column_int64(select_stmt, 0);
      current_reps = p_sqlite3_column_int64(select_stmt, 1);
      current_queue = (int)p_sqlite3_column_int64(select_stmt, 2);
    }
    p_sqlite3_finalize(select_stmt);
  }

  long long new_ivl = current_ivl;
  long long new_reps = current_reps + 1;
  int new_queue = current_queue;
  long long due_offset_seconds = 0;

  if (elapsed_s <= 3.0) {
    if (current_queue == 1 || current_ivl < 0) {
      if (new_reps >= 2) {
        new_queue = 2;
        new_ivl = 1;
        due_offset_seconds = 86400;
      } else {
        new_ivl = -600;
        due_offset_seconds = 600;
      }
    } else {
      long long calculated_ivl = (long long)(current_ivl * 2.5);
      new_ivl = calculated_ivl < 1 ? 1 : calculated_ivl;
      due_offset_seconds = new_ivl * 86400;
    }
  } else {
    if (current_queue == 1 || current_ivl < 0) {
      new_ivl = -300;
      due_offset_seconds = 300;
    } else {
      new_ivl = current_ivl + 1;
      due_offset_seconds = new_ivl * 86400;
    }
  }

  long long new_due = (long long)time(NULL) + due_offset_seconds;

  sqlite3_stmt *update_stmt = NULL;
  const char *sql =
      "UPDATE cards SET ivl = ?, reps = ?, queue = ?, due = ? WHERE id = ?;";
  if (p_sqlite3_prepare_v2(db, sql, -1, &update_stmt, NULL) == SQLITE_OK) {
    p_sqlite3_bind_int64(update_stmt, 1, new_ivl);
    p_sqlite3_bind_int64(update_stmt, 2, new_reps);
    p_sqlite3_bind_int64(update_stmt, 3, new_queue);
    p_sqlite3_bind_int64(update_stmt, 4, new_due);
    p_sqlite3_bind_int64(update_stmt, 5, current_card.id);

    if (p_sqlite3_step(update_stmt) != SQLITE_DONE) {
      fprintf(stderr, "Failed to execute update: %s\n", p_sqlite3_errmsg(db));
    }

    p_sqlite3_finalize(update_stmt);
  } else {
    fprintf(stderr, "Failed to prepare update: %s\n", p_sqlite3_errmsg(db));
  }

  p_sqlite3_close(db);
}
static void display_card_answer(void) {
  char answer[1024];
  answer[0] = '\0';

  TYPE_NAME card = current_card;

  if (card.type == CARD_TYPE_BASIC) {
    snprintf(answer, sizeof(answer), "%s\n---\n%s", card.data.basic.front,
             card.data.basic.back);
  } else if (card.type == CARD_TYPE_CLOZE) {
    const char *src = card.data.cloze.text;
    size_t src_len = strlen(src);
    size_t o = 0;

    size_t i = 0;
    while (i < src_len && o + 1 < sizeof(answer)) {
      if (src[i] == '{') {
        size_t j = i + 1;
        while (j < src_len && src[j] != '}')
          j++;

        if (j < src_len) {
          const char *content_start = src + i + 1;
          size_t content_len = j - i - 1;

          const char *sep = NULL;
          for (size_t k = 0; k + 1 < content_len; k++) {
            if (content_start[k] == ':' && content_start[k + 1] == ':') {
              sep = content_start + k;
              break;
            }
          }

          const char *ans = sep ? sep + 2 : content_start;
          size_t ans_len =
              sep ? (size_t)((content_start + content_len) - ans) : content_len;

          for (size_t k = 0; k < ans_len && o + 1 < sizeof(answer); k++) {
            answer[o++] = ans[k];
          }
          i = j + 2;
          continue;
        }
      }
      answer[o++] = src[i++];
    }
    answer[o] = '\0';
  }

  double elapsed_ms = sk_end_timer(q_timer);
  fflush(stdout);
  anki_apply_elapsed(elapsed_ms);

  anki_state = ANKI_STATE_ANSWER;

  sk_dismiss_message("question");
  sk_message(.name = "answer", .message = answer, .slot = TOP_CENTER,
             .timeout = 1000.0);
}

static void print_due_card(void) {
  if (sk_string_contains_ci(sk_get_active_window_title(), "Anki"))
    return;
  sk_dismiss_message("question");
  sk_dismiss_message("answer");
  char buf[300];
#if defined(_WIN32)
  snprintf(buf, sizeof(buf), "%s/AppData/Roaming/Anki2/User 1/collection.anki2",
           sk_resolve_user_path());
#elif defined(__linux__)
  snprintf(buf, sizeof(buf), "%s/.local/share/Anki2/User 1/collection.anki2",
           sk_resolve_user_path());
#endif
  CardList list = get_cards_in_deck(buf, USED_DECK);
  long long now = (long long)time(NULL);
  size_t *due = malloc(list.count * sizeof(size_t));
  size_t due_count = 0;
  for (size_t i = 0; i < list.count; i++) {
    long long left = list.items[i].due_ts - now;
    if (left <= 0)
      due[due_count++] = i;
  }
  if (due_count == 0) {
    printf("No due cards.\n");
    free(due);
    free_card_list(&list);
    return;
  }
  int random = sk_rand_num_between(0, (int)(due_count - 1));
  size_t pick = due[random];
  TYPE_NAME card = list.items[pick];

  free_card_contents(&current_card);
  current_card = card_clone(list.items[pick]);
  anki_state = ANKI_STATE_QUESTION;

  display_card_question(card);
  free(due);
  free_card_list(&list);
}

static void handle_anki(void) {
  AnkiState current_state = anki_state;

  if (current_state == ANKI_STATE_QUESTION) {
    display_card_answer();
  } else if (current_state == ANKI_STATE_ANSWER) {
    anki_state = ANKI_STATE_IDLE;
    sk_dismiss_message("answer");
  }
}

static bool should_swallow_anki(void) {
  bool active = (anki_state != ANKI_STATE_IDLE);
  return active;
}

SKALD_FEATURE_INIT {
  srand((time(NULL)));
  // sk_run_every(time_between_qs, print_due_card);
  // sk_register_hotkey(.hotkey = "Shift+Space", .cb = handle_anki,
  //                    .should_swallow = should_swallow_anki);
}
