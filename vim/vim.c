#include "core/skapi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  VIM_MODE_INSERT,
  VIM_MODE_NORMAL,
  VIM_MODE_VISUAL,
  VIM_MODE_ANY
} VimMode;
static void vim_enter_normal_mode(void);
static void vim_set_insert_mode(void);
static void vim_set_normal_mode(void);
static void vim_set_visual_mode(void);
static void vim_line_start(void);
static void vim_line_end(void);
static void vim_refresh_prompt(void);
static void vim_move_left(void);
static void vim_move_right(void);
static void vim_word_forward(void);
static void vim_word_backward(void);

typedef void (*VimMotionCallback)(void);

#define VIM_MAX_MOTIONS 420
#define VIM_MAX_TOKENS 8
#define VIM_BUFFER_TOKENS 16

typedef struct {
  char tokens[VIM_MAX_TOKENS][32];
  int token_count;
  char mods[32];
  VimMode mode;
  VimMotionCallback cb;
} VimMotion;

static VimMode vim_mode = VIM_MODE_INSERT;
static VimMotion motions[VIM_MAX_MOTIONS];
static int motion_count = 0;
static char key_buffer[VIM_BUFFER_TOKENS][32];
static int key_buffer_count = 0;
static volatile int pending_active = 0;
static char pending_char = 0;
static SkMutex pending_mutex = NULL;
static int visual_mark = -1;

static void vim_reset_buffer(void) { key_buffer_count = 0; }

static void vim_sel_clear(void) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->eb)
    return;
  ps->eb->sel_start = -1;
  ps->eb->sel_end = -1;
}

static void vim_sel_sync(void) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->eb || visual_mark < 0)
    return;
  int a = visual_mark, b = (int)ps->eb->cursor;
  if (a > b) {
    int t = a;
    a = b;
    b = t;
  }
  int len = (int)strlen(ps->eb->filter);
  b = b + 1 > len ? len : b + 1;
  ps->eb->sel_start = a;
  ps->eb->sel_end = b;
}

static void vim_set_visual_mode(void) {
  vim_mode = VIM_MODE_VISUAL;
  sk_get_metrics()->edit_cursor_width = (FontSize / 2) + 2;
}

static void vim_visual_enter(void) {
  PromptState *ps = sk_get_active_prompt();
  visual_mark = ps && ps->eb ? (int)ps->eb->cursor : 0;
  vim_set_visual_mode();
  if (ps && ps->eb) {
    ps->eb->sel_start = visual_mark;
    ps->eb->sel_end = visual_mark;
  }
  vim_reset_buffer();
  vim_refresh_prompt();
}

static void vim_visual_exit(void) {
  visual_mark = -1;
  vim_sel_clear();
  vim_set_normal_mode();
  vim_reset_buffer();
  vim_refresh_prompt();
}

static void vim_visual_select_all(void) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->eb)
    return;
  visual_mark = 0;
  ps->eb->cursor = strlen(ps->eb->filter);
  ps->eb->sel_start = 0;
  ps->eb->sel_end = (int)ps->eb->cursor;
  vim_set_visual_mode();
  vim_reset_buffer();
  vim_refresh_prompt();
}

static void vim_visual_left(void) {
  vim_move_left();
  vim_sel_sync();
}
static void vim_visual_right(void) {
  vim_move_right();
  vim_sel_sync();
}
static void vim_visual_word_forward(void) {
  vim_word_forward();
  vim_sel_sync();
}
static void vim_visual_word_backward(void) {
  vim_word_backward();
  vim_sel_sync();
}
static void vim_visual_line_start(void) {
  vim_line_start();
  vim_sel_sync();
}
static void vim_visual_line_end(void) {
  vim_line_end();
  vim_sel_sync();
}

static bool vim_visual_sel_range(int *out_a, int *out_b) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->eb)
    return false;
  int lo = ps->eb->sel_start, hi = ps->eb->sel_end;
  if (lo < 0 || hi <= lo)
    return false;
  *out_a = lo;
  *out_b = hi;
  return true;
}

static void vim_visual_delete(void) {
  PromptState *ps = sk_get_active_prompt();
  int a, b;
  if (!ps || !ps->eb)
    return;
  if (!vim_visual_sel_range(&a, &b)) {
    vim_visual_exit();
    return;
  }
  memmove(ps->eb->filter + a, ps->eb->filter + b,
          strlen(ps->eb->filter + b) + 1);
  ps->eb->cursor = (size_t)a;
  visual_mark = -1;
  vim_sel_clear();
  vim_set_normal_mode();
  vim_reset_buffer();
  vim_refresh_prompt();
}

static void vim_visual_yank(void) {
  const PromptState *ps = sk_get_active_prompt();
  int a, b;
  char buf[512];
  if (!ps || !ps->eb)
    return;
  if (!vim_visual_sel_range(&a, &b)) {
    vim_visual_exit();
    return;
  }
  size_t n = (size_t)(b - a);
  if (n >= sizeof(buf))
    n = sizeof(buf) - 1;
  memcpy(buf, ps->eb->filter + a, n);
  buf[n] = '\0';
  sk_copy_to_clipboard(buf);
  vim_visual_exit();
}

static void vim_visual_change(void) {
  vim_visual_delete();
  vim_set_insert_mode();
  vim_reset_buffer();
  vim_refresh_prompt();
}

static void vim_refresh_prompt(void) {
  PromptState *aps = sk_get_active_prompt();
  if (aps && aps->callbacks && aps->callbacks->refresh)
    aps->callbacks->refresh(aps);
}

static void vim_insert_char_silent(char ch) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->eb)
    return;
  size_t len = strlen(ps->eb->filter);
  if (len + 1 >= sizeof(ps->eb->filter))
    return;
  if (ps->eb->cursor > len)
    ps->eb->cursor = len;
  memmove(ps->eb->filter + ps->eb->cursor + 1, ps->eb->filter + ps->eb->cursor,
          len - ps->eb->cursor + 1);
  ps->eb->filter[ps->eb->cursor] = ch;
  ps->eb->cursor++;
}

static void *vim_pending_thread(void *arg) {
  sk_sleep(0.3);
  sk_mutex_lock(pending_mutex);
  if (pending_active) {
    pending_active = 0;
    pending_char = 0;
    sk_mutex_unlock(pending_mutex);
    vim_refresh_prompt();
  } else {
    sk_mutex_unlock(pending_mutex);
  }
  return NULL;
}

static void vim_cancel_pending(void) {
  sk_mutex_lock(pending_mutex);
  pending_active = 0;
  sk_mutex_unlock(pending_mutex);
}

static void vim_start_pending(char ch) {
  sk_mutex_lock(pending_mutex);
  pending_active = 1;
  pending_char = ch;
  sk_mutex_unlock(pending_mutex);
  sk_thread_create(vim_pending_thread, NULL);
}

static void vim_delete_last(int n) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->eb)
    return;
  int len = (int)strlen(ps->eb->filter);
  int cur = ps->eb->cursor;
  if (cur > len)
    cur = len;
  if (n > cur)
    n = cur;
  if (n <= 0)
    return;
  int start = cur - n;
  memmove(ps->eb->filter + start, ps->eb->filter + cur, len - cur + 1);
  ps->eb->cursor = start;
}

static void vim_motion(const char *seq, const char *mods, VimMode mode,
                       VimMotionCallback cb) {
  if (!seq || !cb || motion_count >= VIM_MAX_MOTIONS)
    return;
  VimMotion *m = &motions[motion_count++];
  m->mode = mode;
  m->cb = cb;
  m->token_count = 0;
  if (strchr(seq, ' ')) {
    char tmp[256];
    strncpy(tmp, seq, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *save = NULL;
    const char *tok = strtok_r(tmp, " ", &save);
    while (tok && m->token_count < VIM_MAX_TOKENS) {
      strncpy(m->tokens[m->token_count], tok, 31);
      m->tokens[m->token_count][31] = '\0';
      m->token_count++;
      tok = strtok_r(NULL, " ", &save);
    }
  } else if (strchr(seq, '+')) {
    strncpy(m->tokens[0], seq, 31);
    m->tokens[0][31] = '\0';
    m->token_count = 1;
  } else {
    for (const char *p = seq; *p && m->token_count < VIM_MAX_TOKENS; p++) {
      m->tokens[m->token_count][0] = *p;
      m->tokens[m->token_count][1] = '\0';
      m->token_count++;
    }
  }

  if (mods)
    strncpy(m->mods, mods, 31);
  else
    m->mods[0] = '\0';
  m->mods[31] = '\0';

  if (m->mods[0]) {
    for (int i = 0; i < m->token_count; i++) {
      char tmp[64];
      snprintf(tmp, sizeof(tmp), "%s+%s", m->mods, m->tokens[i]);
      snprintf(m->tokens[i], sizeof(m->tokens[i]), "%.*s",
               (int)sizeof(m->tokens[i]) - 1, tmp);
    }
  }
}

static bool vim_key_to_token(const PopupKeyEvent *key, char *out,
                             size_t outsz) {
  if (!key || !out || outsz == 0)
    return false;
  char ch = 0;
  if (key->vk >= 'A' && key->vk <= 'Z')
    ch = (char)(key->vk + 32);
  else if (key->vk >= 'a' && key->vk <= 'z')
    ch = (char)key->vk;
  else
    return false;
  size_t pos = 0;
  if (key->ctrl) {
    memcpy(out + pos, "ctrl+", 5);
    pos += 5;
  }
  if (key->shift) {
    memcpy(out + pos, "shift+", 6);
    pos += 6;
  }
  if (pos + 1 >= outsz)
    return false;
  out[pos++] = ch;
  out[pos] = '\0';
  return true;
}

static bool vim_key_interceptor(const PopupKeyEvent *key) {
  if (!key)
    return false;
  if (key->vk == VK_ESCAPE) {
    if (vim_mode == VIM_MODE_INSERT) {
      vim_set_normal_mode();
      vim_reset_buffer();
      return true;
    }
    if (vim_mode == VIM_MODE_VISUAL) {
      vim_visual_exit();
      return true;
    }
    return false;
  }
  if (vim_mode == VIM_MODE_INSERT) {
    char token[32];
    int has_token = vim_key_to_token(key, token, sizeof(token));

    sk_mutex_lock(pending_mutex);
    int was_pending = pending_active;
    char pending = pending_char;
    sk_mutex_unlock(pending_mutex);

    if (was_pending) {
      int continues = 0;
      for (int i = 0; i < motion_count; i++) {
        const VimMotion *m = &motions[i];
        if (!(m->mode == vim_mode || m->mode == VIM_MODE_ANY))
          continue;
        if (m->token_count < 2)
          continue;
        if (m->tokens[0][0] == pending && m->tokens[0][1] == '\0' &&
            strcmp(m->tokens[1], token) == 0) {
          continues = 1;
          break;
        }
      }
      if (continues) {
        vim_cancel_pending();
        vim_delete_last(1);
        for (int i = 0; i < motion_count; i++) {
          VimMotion *m = &motions[i];
          if (!(m->mode == vim_mode || m->mode == VIM_MODE_ANY))
            continue;
          if (m->token_count < 2)
            continue;
          if (m->tokens[0][0] != pending || m->tokens[0][1] != '\0')
            continue;
          if (strcmp(m->tokens[1], token) != 0)
            continue;
          if (m->cb)
            m->cb();
          vim_refresh_prompt();
          return true;
        }
      }
      vim_cancel_pending();
    }

    if (has_token) {
      for (int i = 0; i < motion_count; i++) {
        const VimMotion *m = &motions[i];
        if (!(m->mode == vim_mode || m->mode == VIM_MODE_ANY))
          continue;
        if (m->token_count < 2)
          continue;
        if (strcmp(m->tokens[0], token) == 0) {
          vim_insert_char_silent(token[0]);
          vim_start_pending(token[0]);
          return true;
        }
      }
    }
  }

  char token[32];
  if (vim_key_to_token(key, token, sizeof(token))) {
    if (key_buffer_count >= VIM_BUFFER_TOKENS)
      key_buffer_count = 0;
    snprintf(key_buffer[key_buffer_count], sizeof(key_buffer[key_buffer_count]),
             "%s", token);
    key_buffer_count++;

    for (int i = 0; i < motion_count; i++) {
      VimMotion *m = &motions[i];
      if (!(m->mode == vim_mode || m->mode == VIM_MODE_ANY))
        continue;
      if (m->token_count == 0 || m->token_count > key_buffer_count)
        continue;
      bool match = true;
      for (int t = 0; t < m->token_count; t++) {
        if (strcmp(key_buffer[key_buffer_count - m->token_count + t],
                   m->tokens[t]) != 0) {
          match = false;
          break;
        }
      }
      if (match) {
        if (vim_mode == VIM_MODE_INSERT)
          vim_delete_last(m->token_count);
        vim_reset_buffer();
        if (m->cb)
          m->cb();
        PromptState *aps = sk_get_active_prompt();
        if (aps && aps->callbacks && aps->callbacks->refresh)
          aps->callbacks->refresh(aps);
        return true;
      }
    }
  } else {
    vim_reset_buffer();
  }

  if (vim_mode == VIM_MODE_VISUAL) {
    if (!key->ctrl && !key->alt &&
        ((key->vk >= 'A' && key->vk <= 'Z') ||
         (key->vk >= 'a' && key->vk <= 'z'))) {
      vim_refresh_prompt();
      return true;
    }
    return false;
  }
  if (vim_mode == VIM_MODE_NORMAL) {
    if (key->vk == 'I' || key->vk == 'i') {
      PromptState *ps = sk_get_active_prompt();
      if (ps && ps->eb && key->shift)
        ps->eb->cursor = 0;
      vim_set_insert_mode();
      vim_reset_buffer();
      vim_refresh_prompt();
      return true;
    }
    if (key->vk == '0') {
      vim_line_start();
      return true;
    }
    if (key->vk == '$') {
      vim_line_end();
      return true;
    }
    if (!key->ctrl && !key->alt &&
        ((key->vk >= 'A' && key->vk <= 'Z') ||
         (key->vk >= 'a' && key->vk <= 'z'))) {
      vim_refresh_prompt();
      return true;
    }
    return false;
  }

  return false;
}

static void vim_move(int amount, bool up) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->lb)
    return;

  if (up) {
    if (ps->lb->selected_index >= amount) {
      ps->lb->selected_index -= amount;
    } else {
      ps->lb->selected_index = 0;
    }
  } else {
    int max_index = ps->lb->item_count - 1;
    if (max_index < 0)
      max_index = 0;

    if (ps->lb->selected_index + amount <= max_index) {
      ps->lb->selected_index += amount;
    } else {
      ps->lb->selected_index = max_index;
    }
  }
  vim_refresh_prompt();
}

static void vim_move_down(void) { vim_move(1, false); }

static void vim_move_down5(void) { vim_move(5, false); }

static void vim_move_up(void) { vim_move(1, true); }

static void vim_move_up5(void) { vim_move(5, true); }

static void vim_set_insert_mode(void) {
  vim_mode = VIM_MODE_INSERT;
  sk_get_metrics()->edit_cursor_width = 0;
}

static void vim_set_normal_mode(void) {
  vim_mode = VIM_MODE_NORMAL;
  sk_get_metrics()->edit_cursor_width = (FontSize / 2) + 2;
}

static void vim_enter_normal_mode(void) { vim_set_normal_mode(); }

static void vim_close_window(void) { sk_close_window(); }

static void vim_on_prompt_open(void) {
  vim_set_insert_mode();
  vim_reset_buffer();
  vim_cancel_pending();
  visual_mark = -1;
  vim_sel_clear();
}
static void vim_go_to_bottom(void) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->lb)
    return;
  ps->lb->selected_index = ps->lb->item_count - 1;
}
static void vim_go_to_top(void) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->lb)
    return;
  ps->lb->selected_index = 0;
}
static void vim_ctrlenter(void) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->lb)
    return;
  if (ps->callbacks->ctrl_enter)
    ps->callbacks->ctrl_enter(ps);
}
static void vim_edit_text(void) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->lb)
    return;
  if (ps->callbacks->edit)
    ps->callbacks->edit(ps);
}
static void vim_move_left(void) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->eb)
    return;
  if (ps->eb->cursor > 0)
    ps->eb->cursor--;
}

static void vim_move_right(void) {
  PromptState *ps = sk_get_active_prompt();
  if (ps->eb->cursor < strlen(ps->eb->filter)) {
    ps->eb->cursor++;
  }
}
static void vim_delete_line(void) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->eb)
    return;
  ps->eb->filter[0] = '\0';
  ps->eb->cursor = 0;
}

static int vim_is_word_char(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') || c == '_';
}

static void vim_word_forward(void) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->eb)
    return;
  const char *s = ps->eb->filter;
  int len = (int)strlen(s);
  int i = ps->eb->cursor;
  if (i > len)
    i = len;
  while (i < len && vim_is_word_char(s[i]))
    i++;
  while (i < len && !vim_is_word_char(s[i]))
    i++;
  ps->eb->cursor = i;
  vim_refresh_prompt();
}

static void vim_word_backward(void) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->eb)
    return;
  const char *s = ps->eb->filter;
  int len = (int)strlen(s);
  int i = ps->eb->cursor;
  if (i > len)
    i = len;
  if (i > 0)
    i--;
  while (i > 0 && !vim_is_word_char(s[i]))
    i--;
  while (i > 0 && vim_is_word_char(s[i - 1]))
    i--;
  ps->eb->cursor = i;
  vim_refresh_prompt();
}

static void vim_line_start(void) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->eb)
    return;
  ps->eb->cursor = 0;
  vim_refresh_prompt();
}

static void vim_line_end(void) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->eb)
    return;
  ps->eb->cursor = (int)strlen(ps->eb->filter);
  vim_refresh_prompt();
}

static void vim_delete_to_end(void) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->eb)
    return;
  size_t len = strlen(ps->eb->filter);
  if (ps->eb->cursor > len)
    ps->eb->cursor = len;
  ps->eb->filter[ps->eb->cursor] = '\0';
  vim_refresh_prompt();
}

static void vim_delete_char(void) {
  PromptState *ps = sk_get_active_prompt();
  if (!ps || !ps->eb)
    return;
  size_t len = strlen(ps->eb->filter);
  if (ps->eb->cursor >= len)
    return;
  memmove(ps->eb->filter + ps->eb->cursor, ps->eb->filter + ps->eb->cursor + 1,
          len - ps->eb->cursor);
  vim_refresh_prompt();
}

static void vim_append(void) {
  PromptState *ps = sk_get_active_prompt();
  if (ps && ps->eb) {
    size_t len = strlen(ps->eb->filter);
    if (ps->eb->cursor < len)
      ps->eb->cursor++;
  }
  vim_set_insert_mode();
  vim_reset_buffer();
  vim_refresh_prompt();
}

static void vim_append_end(void) {
  PromptState *ps = sk_get_active_prompt();
  if (ps && ps->eb)
    ps->eb->cursor = (int)strlen(ps->eb->filter);
  vim_set_insert_mode();
  vim_reset_buffer();
  vim_refresh_prompt();
}

static void vim_change_to_end(void) {
  vim_delete_to_end();
  vim_set_insert_mode();
  vim_reset_buffer();
  vim_refresh_prompt();
}

static void vim_substitute_char(void) {
  vim_delete_char();
  vim_set_insert_mode();
  vim_reset_buffer();
  vim_refresh_prompt();
}

SKALD_FEATURE_INIT {
  pending_mutex = sk_mutex_create();
  vim_set_insert_mode();
  vim_motion("jk", "", VIM_MODE_INSERT, vim_enter_normal_mode);
  vim_motion("jq", "", VIM_MODE_INSERT, vim_close_window);
  vim_motion("jc", "", VIM_MODE_INSERT, vim_ctrlenter);
  vim_motion("v", "", VIM_MODE_NORMAL, vim_visual_enter);
  vim_motion("l", "shift", VIM_MODE_NORMAL, vim_ctrlenter);
  vim_motion("j", "", VIM_MODE_NORMAL, vim_move_down);
  vim_motion("e", "", VIM_MODE_NORMAL, vim_edit_text);
  vim_motion("k", "", VIM_MODE_NORMAL, vim_move_up);
  vim_motion("h", "", VIM_MODE_NORMAL, vim_move_left);
  vim_motion("l", "", VIM_MODE_NORMAL, vim_move_right);
  vim_motion("r", "shift", VIM_MODE_NORMAL, vim_move_up5);
  vim_motion("u", "shift", VIM_MODE_NORMAL, vim_move_down5);
  vim_motion("q", "", VIM_MODE_NORMAL, vim_close_window);
  vim_motion("gg", "", VIM_MODE_NORMAL, vim_go_to_top);
  vim_motion("dd", "", VIM_MODE_NORMAL, vim_delete_line);
  vim_motion("g", "shift", VIM_MODE_NORMAL, vim_go_to_bottom);
  vim_motion("a", "", VIM_MODE_NORMAL, vim_append);
  vim_motion("a", "shift", VIM_MODE_NORMAL, vim_append_end);
  vim_motion("w", "", VIM_MODE_NORMAL, vim_word_forward);
  vim_motion("b", "", VIM_MODE_NORMAL, vim_word_backward);
  vim_motion("d", "shift", VIM_MODE_NORMAL, vim_delete_to_end);
  vim_motion("c", "shift", VIM_MODE_NORMAL, vim_change_to_end);
  vim_motion("x", "", VIM_MODE_NORMAL, vim_delete_char);
  vim_motion("v", "", VIM_MODE_VISUAL, vim_visual_exit);
  vim_motion("v", "shift", VIM_MODE_VISUAL, vim_visual_select_all);
  vim_motion("h", "", VIM_MODE_VISUAL, vim_visual_left);
  vim_motion("l", "", VIM_MODE_VISUAL, vim_visual_right);
  vim_motion("w", "", VIM_MODE_VISUAL, vim_visual_word_forward);
  vim_motion("b", "", VIM_MODE_VISUAL, vim_visual_word_backward);
  vim_motion("0", "", VIM_MODE_VISUAL, vim_visual_line_start);
  vim_motion("5", "shift", VIM_MODE_VISUAL, vim_visual_line_end);
  vim_motion("d", "", VIM_MODE_VISUAL, vim_visual_delete);
  vim_motion("x", "", VIM_MODE_VISUAL, vim_visual_delete);
  vim_motion("y", "", VIM_MODE_VISUAL, vim_visual_yank);
  vim_motion("c", "", VIM_MODE_VISUAL, vim_visual_change);
  vim_motion("s", "", VIM_MODE_VISUAL, vim_visual_change);
  vim_motion("s", "", VIM_MODE_NORMAL, vim_substitute_char);
  sk_register_key_interceptor(vim_key_interceptor);
  sk_register_event(EVENT_PROMPT_OPEN, vim_on_prompt_open);
}
