#include "core/skapi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *get_zoxide_match(const char *query) {
  char clean_query[512];
  strncpy(clean_query, query, sizeof(clean_query) - 1);
  clean_query[sizeof(clean_query) - 1] = '\0';
  clean_query[strcspn(clean_query, "\r\n")] = '\0';

  const char *real_home = sk_resolve_user_path();

  char command[1024];
#if defined(_WIN32)
  snprintf(command, sizeof(command),
           "cmd.exe /c \"set \"HOME=%s\" & set "
           "\"_ZO_DATA_DIR=%s\\AppData\\Local\\zoxide\" & zoxide query -- "
           "\"%s\" 2>&1\"",
           real_home, real_home, clean_query);
#elif defined(__linux__)
  snprintf(command, sizeof(command),
           "export HOME=\"%s\"; "
           "export _ZO_DATA_DIR=\"%s/.local/share/zoxide\"; "
           "export PATH=\"%s/.local/bin:%s/.cargo/bin:/usr/local/bin:$PATH\"; "
           "zoxide query -- \"%s\" 2>&1",
           real_home, real_home, real_home, real_home, clean_query);
#endif

  char *out = sk_run_terminal_command(command);
  if (!out) {
    return NULL;
  }
  out[strcspn(out, "\r\n")] = '\0';

  if (strstr(out, "no match found") || strlen(out) == 0) {
    return NULL;
  }
  return out;
}

const char *TerminalZoxideFilePath = "TerminalZoxide.txt";
static void zoxide_refresh(PromptState *state) {
  SkStringList files = sk_load_file_list(TerminalZoxideFilePath);
  sk_populate_listbox(.lb = state->lb, .filter = state->eb->filter,
                      .items = (const char **)files.items,
                      .count = (int)files.count);
  sk_free_string_list(&files);
}
static void zoxide_edit(PromptState *state) {
  int idx = -1;
  char *edited = sk_edit_text_popup(.initial_text = state->lb->selected_text,
                                    .lb = state->lb, .out_index = &idx);
  if (!edited)
    return;
  if (idx >= 0)
    sk_replace_line_in_file(TerminalZoxideFilePath, idx, edited);
  free(edited);
  zoxide_refresh(state);
}
static bool zoxide_enter(PromptState *state) {
  const char *text =
      state->lb->selected_text ? state->lb->selected_text : state->eb->filter;
  if (!sk_is_selected(state->lb)) {
    sk_append_unique_line(TerminalZoxideFilePath, text);
  }
  char *path = get_zoxide_match(text);
  sk_launch_terminal_at_path(path, NULL);
  free(path);
  return true;
}
static void trigger_zoxide(void) {
  SkStringList files = sk_load_file_list(TerminalZoxideFilePath);
  sk_window(.title = "Zoxide", .items = (const char **)files.items,
            .refresh = zoxide_refresh, .enter = zoxide_enter,
            .edit = zoxide_edit);
  sk_free_string_list(&files);
}

SKALD_FEATURE_INIT {
  sk_register_hotkey(.hotkey = "Alt+W", .cb = trigger_zoxide);

  sk_add_feature(.display_name = "Zoxide", .on_action = trigger_zoxide,
                 .icon = "/folder.png");
}
