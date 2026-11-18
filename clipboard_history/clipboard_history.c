#include "core/skapi.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define CH_FILE "clipboard_history.txt"
#define CH_MAX_STORED 1000
#define CH_MAX_TEXT 4096
#define CH_PREVIEW_LEN 128

static char **ch_items = NULL;
static int ch_count = 0;

static void ch_free_items(void) {
  if (!ch_items) {
    ch_count = 0;
    return;
  }
  for (int i = 0; i < ch_count; i++)
    free(ch_items[i]);
  free(ch_items);
  ch_items = NULL;
  ch_count = 0;
}

static size_t ch_escape(const char *src, char *dst, size_t cap) {
  size_t o = 0;
  for (const char *p = src ? src : ""; *p && o + 2 < cap; p++) {
    if (*p == '\\') {
      dst[o++] = '\\';
      dst[o++] = '\\';
    } else if (*p == '\n') {
      dst[o++] = '\\';
      dst[o++] = 'n';
    } else if (*p == '\r') {
      dst[o++] = '\\';
      dst[o++] = 'r';
    } else
      dst[o++] = *p;
  }
  dst[o] = '\0';
  return o;
}

static size_t ch_unescape(const char *src, char *dst, size_t cap) {
  size_t o = 0;
  for (const char *p = src ? src : ""; *p && o + 1 < cap; p++) {
    if (*p == '\\' && *(p + 1) == 'n') {
      dst[o++] = '\n';
      p++;
    } else if (*p == '\\' && *(p + 1) == 'r') {
      dst[o++] = '\r';
      p++;
    } else if (*p == '\\' && *(p + 1) == '\\') {
      dst[o++] = '\\';
      p++;
    } else
      dst[o++] = *p;
  }
  dst[o] = '\0';
  return o;
}

static void ch_preview(const char *src, char *dst, size_t cap) {
  size_t o = 0;
  for (const char *p = src ? src : ""; *p && o + 1 < cap && o < CH_PREVIEW_LEN;
       p++) {
    if (*p == '\n' || *p == '\r' || *p == '\t')
      dst[o++] = ' ';
    else
      dst[o++] = *p;
  }
  dst[o] = '\0';
}

static void ch_remember_locked(const char *text) {
  if (!text || !text[0])
    return;
  for (int i = 0; i < ch_count; i++) {
    if (strcmp(ch_items[i], text) == 0) {
      free(ch_items[i]);
      for (int k = i; k + 1 < ch_count; k++)
        ch_items[k] = ch_items[k + 1];
      ch_count--;
      break;
    }
  }
  if (ch_count >= CH_MAX_STORED) {
    free(ch_items[0]);
    for (int k = 0; k + 1 < ch_count; k++)
      ch_items[k] = ch_items[k + 1];
    ch_count--;
  }
  char *copy = malloc(CH_MAX_TEXT);
  if (!copy)
    return;
  strncpy(copy, text, CH_MAX_TEXT - 1);
  copy[CH_MAX_TEXT - 1] = '\0';
  char **tmp = realloc(ch_items, (size_t)(ch_count + 1) * sizeof(char *));
  if (!tmp) {
    free(copy);
    return;
  }
  ch_items = tmp;
  ch_items[ch_count++] = copy;
}

static void ch_save(void) {
  char **lines = NULL;
  int count = 0;
  if (ch_count > 0) {
    lines = malloc((size_t)ch_count * sizeof(char *));
    if (lines) {
      for (int i = 0; i < ch_count; i++) {
        lines[i] = malloc(CH_MAX_TEXT * 2);
        if (!lines[i])
          break;
        ch_escape(ch_items[i], lines[i], CH_MAX_TEXT * 2);
        count++;
      }
    }
  }
  if (lines && count > 0)
    sk_write_to_file(CH_FILE, (const char **)lines, count);
  if (lines) {
    for (int i = 0; i < count; i++)
      free(lines[i]);
    free(lines);
  }
}

static void ch_load(void) {
  SkStringList lines = sk_load_file_list(CH_FILE);
  for (size_t i = 0; i < lines.count; i++) {
    if (!lines.items[i] || !lines.items[i][0])
      continue;
    char buf[CH_MAX_TEXT];
    ch_unescape(lines.items[i], buf, sizeof(buf));
    ch_remember_locked(buf);
  }
  sk_free_string_list(&lines);
}

static bool ch_sample_once(void) {
  char *cur = sk_get_last_clipboard_entry();
  bool changed = false;
  if (cur && cur[0] &&
      (ch_count == 0 || strcmp(ch_items[ch_count - 1], cur) != 0)) {
    ch_remember_locked(cur);
    changed = true;
  }
  if (cur)
    free(cur);
  if (changed)
    ch_save();
  return changed;
}

static void ch_tick(void) { ch_sample_once(); }

static void clipboard_history_refresh(PromptState *state) {
  int count = ch_count;
  const char **items = NULL;
  if (count > 0) {
    items = (const char **)malloc((size_t)count * sizeof(char *));
    for (int i = 0; i < count && items; i++) {
      char *buffer = malloc(CH_PREVIEW_LEN + 1);
      if (!buffer)
        break;
      ch_preview(ch_items[count - 1 - i], buffer, CH_PREVIEW_LEN + 1);
      items[i] = buffer;
    }
  }
  sk_populate_listbox(.lb = state->lb, .filter = state->eb->filter,
                      .items = items, .count = count);
  sk_free_list(items, count);
}

static bool clipboard_history_enter(PromptState *state) {
  (void)state->lb->selected_text;
  if (!sk_is_selected(state->lb))
    return true;
  int sel = state->lb->selected_index;
  if (sel < 0 || sel >= state->lb->total_matches)
    return true;
  int idx = state->lb->matching_indices[sel];
  char *copy = NULL;
  if (idx >= 0 && idx < ch_count && ch_items[ch_count - 1 - idx]) {
    copy = strdup(ch_items[ch_count - 1 - idx]);
    if (copy)
      ch_remember_locked(copy);
  }
  if (copy) {
    sk_copy_to_clipboard(copy);
    free(copy);
    ch_save();
  }
  return true;
}

static void trigger_clipboard_history(void) {
  sk_window(.title = "Clipboard History", .refresh = clipboard_history_refresh,
            .enter = clipboard_history_enter);
}

SKALD_FEATURE_SHUTDOWN {
  ch_save();
  ch_free_items();
}

SKALD_FEATURE_INIT {
  ch_load();
  sk_run_every(2.0, ch_tick);
  sk_register_hotkey(.hotkey = "Alt+Shift+R", .cb = trigger_clipboard_history);
  sk_add_feature(.display_name = "Clipboard History",
                 .on_action = trigger_clipboard_history,
                 .icon = "/clipboard.png");
}
