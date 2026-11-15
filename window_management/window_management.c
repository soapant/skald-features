#include "core/skapi.h"

static void move_active_window_right(void) { sk_move_active_window_right(); }
static void move_active_window_left(void) { sk_move_active_window_left(); }
static void focus_window_right(void) { sk_focus_window_right(); }
static void focus_window_left(void) { sk_focus_window_left(); }
static void close_active_window(void) { sk_close_active_window(); }
static void toggle_fullscreen(void) { sk_toggle_fullscreen_active(); }
static void toggle_minimized(void) { sk_toggle_minimized_active(); }

SKALD_FEATURE_INIT {
  sk_add_to_cc(.display_name = "MoveActiveWindowRight",
               .on_action = move_active_window_right,
               .type = "WindowManagement", .icon = "/arrow-right.png");
  sk_add_to_cc(.display_name = "MoveActiveWindowLeft",
               .on_action = move_active_window_left, .type = "WindowManagement",
               .icon = "/arrow-left.png");
  sk_add_to_cc(.display_name = "FocusWindowRight",
               .on_action = focus_window_right, .type = "WindowManagement",
               .icon = "/arrow-right.png");
  sk_add_to_cc(.display_name = "FocusWindowLeft",
               .on_action = focus_window_left, .type = "WindowManagement",
               .icon = "/arrow-left.png");
  sk_add_to_cc(.display_name = "CloseActiveWindow",
               .on_action = close_active_window, .type = "WindowManagement",
               .icon = "/x.png");
  sk_add_to_cc(.display_name = "ToggleFullscreen",
               .on_action = toggle_fullscreen, .type = "WindowManagement",
               .icon = "/maximize-2.png");
  sk_add_to_cc(.display_name = "ToggleMinimized", .on_action = toggle_minimized,
               .type = "WindowManagement", .icon = "/minimize-2.png");
}
