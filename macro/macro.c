#include "core/skapi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool recording = false;
static SkThread thread = NULL;

#define WIDTH sk_get_screen_width()
#define HEIGHT sk_get_screen_height()

#define MACRO_MAX_EVENTS 32768 * 2
#define DEAD_TIME_LIMIT 250.0

#define MACRO_RECORD_VK 'R'
#define MACRO_PLAY_VK 'P'

#define MACRO_OVERLAY_NAME "macro_overlay"

#define EVT_KEY 1
#define EVT_MOUSE 2
#define EVT_MOVE 3

typedef struct {
  int type;
  int a;
  int b;
  int x;
  int y;
  int mods;
  bool down;
  bool repeat;
  unsigned long long t_us;
} MacroEvent;

static MacroEvent events[MACRO_MAX_EVENTS];
static int event_count = 0;
static bool overflow = false;

static void recording_render(void *ud) {
  int max_x = WIDTH - 1;
  int max_y = HEIGHT - 1;

  sk_draw_line(.x1 = 0, .y1 = 0, .x2 = max_x, .y2 = 0, .color = SK_RED);
  sk_draw_line(.x1 = max_x, .y1 = 0, .x2 = max_x, .y2 = max_y, .color = SK_RED);
  sk_draw_line(.x1 = max_x, .y1 = max_y, .x2 = 0, .y2 = max_y, .color = SK_RED);
  sk_draw_line(.x1 = 0, .y1 = max_y, .x2 = 0, .y2 = 0, .color = SK_RED);
}

static void playback_render(void *ud) {
  int max_x = WIDTH - 1;
  int max_y = HEIGHT - 1;

  sk_draw_line(.x1 = 0, .y1 = 0, .x2 = max_x, .y2 = 0, .color = SK_BLUE);
  sk_draw_line(.x1 = max_x, .y1 = 0, .x2 = max_x, .y2 = max_y,
               .color = SK_BLUE);
  sk_draw_line(.x1 = max_x, .y1 = max_y, .x2 = 0, .y2 = max_y,
               .color = SK_BLUE);
  sk_draw_line(.x1 = 0, .y1 = max_y, .x2 = 0, .y2 = 0, .color = SK_BLUE);
}

static SkTimer macro_clock;
static bool macro_clock_valid = false;

static void macro_clock_reset(void) {
  macro_clock = sk_start_timer();
  macro_clock_valid = true;
}

static unsigned long macro_now_us(void) {
  if (!macro_clock_valid)
    macro_clock_reset();
  double ms = sk_end_timer(macro_clock);
  if (ms < 0.0)
    ms = 0.0;
  return (unsigned long long)(ms * 1000.0);
}

static void record_push(const MacroEvent *m) {
  if (event_count >= MACRO_MAX_EVENTS) {
    overflow = true;
    return;
  }
  events[event_count++] = *m;
}

static void record_push_key(const SkKeyEvent *e) {
  MacroEvent m;
  memset(&m, 0, sizeof(m));
  m.type = EVT_KEY;
  m.a = e->vk;
  m.b = e->scancode;
  m.mods = e->mods;
  m.down = e->down;
  m.repeat = e->repeat;
  m.t_us = macro_now_us();
  record_push(&m);
}

static void record_push_mouse(const SkMouseEvent *e) {
  MacroEvent m;
  memset(&m, 0, sizeof(m));
  if (e->button == 0) {
    m.type = EVT_MOVE;
  } else {
    m.type = EVT_MOUSE;
    m.a = e->button;
    m.mods = e->mods;
    m.down = e->down;
    m.repeat = e->repeat;
  }
  m.x = e->x;
  m.y = e->y;
  m.t_us = macro_now_us();
  record_push(&m);
}

static bool macro_event_less(const MacroEvent *a, const MacroEvent *b) {
  if (a->t_us != b->t_us)
    return a->t_us < b->t_us;
  return a->type < b->type;
}

static void sort_events(void) {
  for (int i = 1; i < event_count; i++) {
    MacroEvent key = events[i];
    int j = i - 1;
    while (j >= 0 && macro_event_less(&key, &events[j])) {
      events[j + 1] = events[j];
      j--;
    }
    events[j + 1] = key;
  }
}

static int macro_last_mx = -1, macro_last_my = -1;

static void *recorder_loop(void *arg) {
  while (recording) {
    SkKeyEvent ke;
    while (sk_poll_key(&ke))
      record_push_key(&ke);

    SkMouseEvent me;
    while (sk_poll_mouse(&me))
      record_push_mouse(&me);

    {
      SkPos _mp = sk_get_cursor_pos();
      if (_mp.x != macro_last_mx || _mp.y != macro_last_my) {
        SkMouseEvent _me;
        memset(&_me, 0, sizeof(_me));
        _me.button = 0;
        _me.x = _mp.x;
        _me.y = _mp.y;
        _me.t_us = macro_now_us();
        record_push_mouse(&_me);
        macro_last_mx = _mp.x;
        macro_last_my = _mp.y;
      }
    }
    sk_sleep(0.005);
  }

  thread = NULL;
  return NULL;
}

static void reset_recording(void) {
  event_count = 0;
  overflow = false;
  macro_last_mx = -1;
  macro_last_my = -1;
  macro_clock_reset();
}

static bool playing = false;
static SkThread play_thread = NULL;
#define MACRO_MAX_HELD 64
static int held_keys[MACRO_MAX_HELD];
static int held_count = 0;

static void held_add(int vk) {
  if (vk == 0)
    return;
  for (int i = 0; i < held_count; i++)
    if (held_keys[i] == vk)
      return;
  if (held_count < MACRO_MAX_HELD)
    held_keys[held_count++] = vk;
}

static void held_remove(int vk) {
  for (int i = 0; i < held_count; i++) {
    if (held_keys[i] == vk) {
      held_keys[i] = held_keys[--held_count];
      return;
    }
  }
}

static void release_everything(void) {
  for (int i = 0; i < held_count; i++)
    sk_emit_key(held_keys[i], false);
  held_count = 0;
  sk_emit_key(VK_SHIFT, false);
  sk_emit_key(VK_CONTROL, false);
  sk_emit_key(VK_MENU, false);
  sk_emit_key(VK_LWIN, false);
  sk_emit_key(VK_RWIN, false);
  for (int b = 1; b <= 5; b++)
    sk_emit_mouse_button(b, false);
}

static bool is_macro_control_chord(int vk, int mods) {
  if (mods != (HOTKEY_MOD_CTRL | HOTKEY_MOD_ALT))
    return false;
  return vk == MACRO_RECORD_VK || vk == MACRO_PLAY_VK;
}

static void *playback_loop(void *arg) {
  (void)arg;
  if (event_count == 0) {
    playing = false;
    play_thread = NULL;
    return NULL;
  }
  while (playing) {
    sort_events();
    if (event_count == 0)
      break;
    unsigned long long t0 = events[0].t_us;
    SkTimer start = sk_start_timer();
    held_count = 0;
    for (int i = 0; i < event_count && playing; i++) {
      const MacroEvent *m = &events[i];
      double target_ms = (double)(m->t_us - t0) / 1000.0;
      double cur_ms = sk_end_timer(start);
      double wait_ms = target_ms - cur_ms;
      if (sk_config_get_setting_bool("Macro.Remove Dead Time") &&
          wait_ms > DEAD_TIME_LIMIT)
        wait_ms = 0.0;
      if (wait_ms > 0.0)
        sk_sleep(wait_ms / 1000.0);
      if (m->type == EVT_KEY) {
        if (m->repeat)
          continue;
        if (m->down && is_macro_control_chord(m->a, m->mods))
          continue;
        if (m->down && sk_replay_hotkey(m->a, m->mods))
          continue;
        sk_emit_key(m->a, m->down);
        if (m->down)
          held_add(m->a);
        else
          held_remove(m->a);
      } else if (m->type == EVT_MOUSE) {
        if (m->a != 0)
          sk_emit_mouse_button(m->a, m->down);
      } else {
        SkPos p = {m->x, m->y};
        sk_move_to_pos(p);
      }
    }
    release_everything();
    if (playing && sk_config_get_setting_bool("Macro.Infinite Playback"))
      continue;
    break;
  }
  release_everything();
  sk_dismiss_overlay(MACRO_OVERLAY_NAME);
  playing = false;
  play_thread = NULL;
  return NULL;
}

static ListOfFiles macro_load_files;
static const char *macro_file_name_ptrs[MAX_VISIBLE_ITEMS + 1];

static bool macro_save(const char *name) {
  if (event_count == 0)
    return false;
  if (!name || !name[0])
    name = "last.macro";
  char filename[256];
  snprintf(filename, sizeof(filename), "%s", name);
  size_t ln = strlen(filename);
  bool has_ext =
      (ln >= 4 && filename[ln - 4] == '.' && filename[ln - 3] == 't' &&
       filename[ln - 2] == 'x' && filename[ln - 1] == 't');
  if (!has_ext) {
    size_t l = strlen(filename);
    if (l + 4 < sizeof(filename))
      strcat(filename, ".txt");
  }

  size_t cap = (size_t)event_count * 128 + 256;
  char *buf = (char *)malloc(cap);
  if (!buf)
    return false;
  size_t off = 0;
  off += (size_t)snprintf(buf + off, cap - off, "%d\n", event_count);
  for (int i = 0; i < event_count; i++) {
    off += (size_t)snprintf(
        buf + off, cap - off, "%d %d %d %d %d %d %d %d %llu\n", events[i].type,
        events[i].a, events[i].b, events[i].x, events[i].y, events[i].mods,
        events[i].down ? 1 : 0, events[i].repeat ? 1 : 0, events[i].t_us);
  }
  bool ok = sk_write_string_to_file(filename, buf);
  free(buf);
  return ok;
}

static int macro_load(const char *name) {
  if (!name || !name[0])
    name = "last.macro";
  char filename[256];
  snprintf(filename, sizeof(filename), "%s", name);
  size_t ln = strlen(filename);
  bool has_ext2 =
      (ln >= 4 && filename[ln - 4] == '.' && filename[ln - 3] == 't' &&
       filename[ln - 2] == 'x' && filename[ln - 1] == 't');
  if (!has_ext2) {
    if (ln + 4 < sizeof(filename))
      strcat(filename, ".txt");
  }
  char *path = sk_resolve_data_path(filename);
  if (!path)
    return 0;
  size_t len = 0;
  char *data = sk_read_file_to_string(path, &len);
  free(path);
  if (!data)
    return 0;
  int count = 0;
  char *cursor = data;
  if (sscanf(cursor, "%d", &count) != 1 || count < 0 ||
      count > MACRO_MAX_EVENTS) {
    free(data);
    return 0;
  }
  while (*cursor && *cursor != '\n')
    cursor++;
  if (*cursor == '\n')
    cursor++;
  event_count = 0;
  for (int i = 0; i < count; i++) {
    MacroEvent m;
    memset(&m, 0, sizeof(m));
    int down = 0, repeat = 0;
    unsigned long long t = 0;
    if (sscanf(cursor, "%d %d %d %d %d %d %d %d %llu", &m.type, &m.a, &m.b,
               &m.x, &m.y, &m.mods, &down, &repeat, &t) != 9)
      break;
    m.down = down != 0;
    m.repeat = repeat != 0;
    m.t_us = t;
    record_push(&m);
    while (*cursor && *cursor != '\n')
      cursor++;
    if (*cursor == '\n')
      cursor++;
  }
  free(data);
  return event_count;
}

static void macro_refresh_cache(void) {
  macro_load_files.count = 0;
  char *resolved = sk_resolve_data_path("last.macro");
  if (resolved) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", resolved);
    char *slash = strrchr(dir, '/');
    char *back = strrchr(dir, '\\');
    if (back && (!slash || back > slash))
      slash = back;
    if (slash)
      *slash = '\0';
    sk_dir_scan(.dir_path = dir, .out = &macro_load_files, .max_depth = 1);
    free(resolved);
  }
}
static void init_macros(void) {
  macro_refresh_cache();
  int n = (int)macro_load_files.count;
  if (n > MAX_VISIBLE_ITEMS)
    n = MAX_VISIBLE_ITEMS;
  for (int i = 0; i < n; i++) {
    macro_file_name_ptrs[i] = macro_load_files.files[i].name;
  }
}

static void macro_refresh(PromptState *state) {
  sk_populate_listbox(.lb = state->lb, .filter = state->eb->filter,
                      .items = macro_file_name_ptrs,
                      .count = macro_load_files.count);
}

static bool macro_load_enter(PromptState *state) {
  if (state->lb->selected_text)
    macro_load(state->lb->selected_text);
  return true;
}

static bool macro_load_ctrlenter(PromptState *state) {
  if (state->lb->selected_text && state->lb->selected_text[0]) {
    char *path = sk_resolve_data_path(state->lb->selected_text);
    if (path) {
      remove(path);
      free(path);
    }
    macro_refresh(state);
  }
  return false;
}

static void SaveMacro(void) {
  if (event_count == 0) {
    sk_message(.message = "No recording in memory", .timeout = 3.0);
    return;
  }
  char *name = sk_popup(.placeholder = "Macro file name");
  char chosen[256];
  if (name && name[0]) {
    snprintf(chosen, sizeof(chosen), "%s", name);
    free(name);
  } else {
    if (name)
      free(name);
    snprintf(chosen, sizeof(chosen), "%s", "last.macro");
  }
  macro_save(chosen);
}

static void LoadMacro(void) {
  sk_window(.title = "Macro Load", .refresh = macro_refresh,
            .enter = macro_load_enter, .ctrl_enter = macro_load_ctrlenter,
            .auto_scale = true);
}

static void macro_show_overlay(void (*render)(void *ud)) {
  sk_overlay(.name = MACRO_OVERLAY_NAME, .render = render, .w = WIDTH,
             .h = HEIGHT, .x = WIDTH / 2, .y = HEIGHT / 2, .no_bg = true);
}

static void PlayMacro(void) {
  if (!playing) {
    if (event_count == 0) {
      sk_message(.message = "No macro loaded", .timeout = 3.0);
    }
    macro_show_overlay(playback_render);
    sk_clear_key_queue();
    sk_clear_mouse_queue();
    playing = true;
    if (!play_thread)
      play_thread = sk_thread_create(playback_loop, NULL);
    return;
  }
  sk_dismiss_overlay(MACRO_OVERLAY_NAME);
  playing = false;
  release_everything();
}

static void macro_toggle_setting(const char *key) {
  sk_config_set_setting(key,
                        sk_config_get_setting_bool(key) ? "false" : "true");
}

static void infinite_playback_toggle(void) {
  macro_toggle_setting("Macro.Infinite Playback");
}

static void remove_dead_time_toggle(void) {
  macro_toggle_setting("Macro.Remove Dead Time");
}

static void record_toggle(void) {
  if (!recording) {
    macro_show_overlay(recording_render);
    sk_clear_key_queue();
    sk_clear_mouse_queue();
    reset_recording();
    recording = true;
    if (!thread)
      thread = sk_thread_create(recorder_loop, NULL);
    return;
  }
  sk_dismiss_overlay(MACRO_OVERLAY_NAME);
  recording = false;
}

static int macro_refresh_options(const char ***out_options) {
  *out_options = macro_file_name_ptrs;
  return (int)macro_load_files.count;
}

static void macro_select_current(const char *option, int index) {
  (void)index;
  if (option)
    macro_load(option);
}

SKALD_FEATURE_INIT {
  init_macros();
  sk_register_hotkey(.hotkey = "Ctrl+Alt+R", .cb = record_toggle);
  sk_register_hotkey(.hotkey = "Ctrl+Alt+P", .cb = PlayMacro);
  sk_add_to_cc(.display_name = "Save Macro", .on_action = SaveMacro,
               .type = "Macro", .icon = "/save.png");
  sk_add_to_cc(.display_name = "Load Macro", .on_action = LoadMacro,
               .type = "Macro", .icon = "/load.png");
  sk_add_to_cc(.display_name = "Infinite Playback",
               .on_action = infinite_playback_toggle, .type = "Macro",
               .icon = "/repeat.png");
  sk_add_to_cc(.display_name = "Remove Dead Time",
               .on_action = remove_dead_time_toggle, .type = "Macro",
               .icon = "/clock-off.png");

  SkSettingsEntry *macro =
      sk_add_settings_entry(.name = "Macro", .icon = "/save.png",
                            .desc = "Configure Macro Settings");

  sk_settings_entry_add_checkbox(macro, .title = "Infinite Playback",
                                 .sub = "Toggle infinite playback");

  sk_settings_entry_add_checkbox(
      macro, .title = "Remove Dead Time",
     .sub = "Toggle removal of dead time (more than 250ms by default)");

  sk_settings_entry_add_dropdown(
      macro, .title = "Current Macro In Use",
      .sub = "The current macro that the playback will use",
      .refresh_options = macro_refresh_options,
      .on_select = macro_select_current);
}
