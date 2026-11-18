#include "core/skapi.h"
#include <stdlib.h>

static void move_active_window_right(void) { sk_move_active_window_right(); }
static void move_active_window_left(void) { sk_move_active_window_left(); }
static void move_active_window_up(void) { sk_move_active_window_up(); }
static void move_active_window_down(void) { sk_move_active_window_down(); }

static void focus_window_right(void) { sk_focus_window_right(); }
static void focus_window_left(void) { sk_focus_window_left(); }
static void focus_window_up(void) { sk_focus_window_up(); }
static void focus_window_down(void) { sk_focus_window_down(); }

static void close_active_window(void) { sk_close_active_window(); }
static void toggle_fullscreen(void) { sk_toggle_fullscreen_active(); }
static void toggle_minimized(void) { sk_toggle_minimized_active(); }
static void toggle_maximize(void) { sk_toggle_maximize_active(); }
static void toggle_always_on_top(void) { sk_toggle_always_on_top_active(); }
static void raise_active_window(void) { sk_raise_active_window(); }
static void lower_active_window(void) { sk_lower_active_window(); }
static void center_active_window(void) { sk_center_active_window(); }

static void snap_left(void) { sk_snap_active_window(SNAP_LEFT); }
static void snap_right(void) { sk_snap_active_window(SNAP_RIGHT); }
static void snap_top(void) { sk_snap_active_window(SNAP_TOP); }
static void snap_bottom(void) { sk_snap_active_window(SNAP_BOTTOM); }
static void snap_top_left(void) { sk_snap_active_window(SNAP_TOP_LEFT); }
static void snap_top_right(void) { sk_snap_active_window(SNAP_TOP_RIGHT); }
static void snap_bottom_left(void) { sk_snap_active_window(SNAP_BOTTOM_LEFT); }
static void snap_bottom_right(void) {
  sk_snap_active_window(SNAP_BOTTOM_RIGHT);
}

static void move_window_to_next_monitor(void) {
  sk_move_active_window_to_monitor(true);
}
static void move_window_to_prev_monitor(void) {
  sk_move_active_window_to_monitor(false);
}

static void opacity_full(void) { sk_set_active_window_opacity(100); }
static void opacity_80(void) { sk_set_active_window_opacity(80); }
static void opacity_60(void) { sk_set_active_window_opacity(60); }
static void opacity_40(void) { sk_set_active_window_opacity(40); }

static void close_windows_by_title(void) {
  char *needle = sk_popup(.placeholder = "Close windows with matching title");
  if (needle) {
    sk_close_windows_by_title(needle);
    free(needle);
  }
}

static void close_windows_by_id(void) {
  char *needle = sk_popup(.placeholder = "Close windows with matching id");
  if (needle) {
    sk_close_windows_by_id(needle);
    free(needle);
  }
}

#define WS_SWITCH(n)                                                           \
  static void switch_to_workspace_##n(void) { sk_switch_to_workspace(n); }
#define WS_MOVE(n)                                                             \
  static void move_window_to_workspace_##n(void) {                             \
    sk_move_active_window_to_workspace(n);                                     \
  }

WS_SWITCH(1)
WS_SWITCH(2)
WS_SWITCH(3)
WS_SWITCH(4)
WS_SWITCH(5)
WS_SWITCH(6)
WS_SWITCH(7)
WS_SWITCH(8)
WS_SWITCH(9)
WS_MOVE(1)
WS_MOVE(2)
WS_MOVE(3)
WS_MOVE(4)
WS_MOVE(5)
WS_MOVE(6)
WS_MOVE(7)
WS_MOVE(8)
WS_MOVE(9)

typedef struct {
  const char *display_name;
  void (*on_action)(void);
  const char *icon;
} WMEntry;

static const WMEntry WM_ENTRIES[] = {
    {.display_name = "Move Active Window Right",
     .on_action = move_active_window_right,
     .icon = "/arrow-right.png"},
    {.display_name = "Move Active Window Left",
     .on_action = move_active_window_left,
     .icon = "/arrow-left.png"},
    {.display_name = "Move Active Window Up",
     .on_action = move_active_window_up,
     .icon = "/arrow-up.png"},
    {.display_name = "Move Active Window Down",
     .on_action = move_active_window_down,
     .icon = "/arrow-down.png"},
    {.display_name = "Focus Window Right",
     .on_action = focus_window_right,
     .icon = "/arrow-right.png"},
    {.display_name = "Focus Window Left",
     .on_action = focus_window_left,
     .icon = "/arrow-left.png"},
    {.display_name = "Focus Window Up",
     .on_action = focus_window_up,
     .icon = "/arrow-up.png"},
    {.display_name = "Focus Window Down",
     .on_action = focus_window_down,
     .icon = "/arrow-down.png"},
    {.display_name = "Close Active Window",
     .on_action = close_active_window,
     .icon = "/x.png"},
    {.display_name = "Toggle Fullscreen",
     .on_action = toggle_fullscreen,
     .icon = "/maximize-2.png"},
    {.display_name = "Toggle Minimized",
     .on_action = toggle_minimized,
     .icon = "/minimize-2.png"},
    {.display_name = "Toggle Maximize",
     .on_action = toggle_maximize,
     .icon = "/expand.png"},
    {.display_name = "Toggle Always On Top",
     .on_action = toggle_always_on_top,
     .icon = "/pin.png"},
    {.display_name = "Raise Active Window",
     .on_action = raise_active_window,
     .icon = "/chevrons-up.png"},
    {.display_name = "Lower Active Window",
     .on_action = lower_active_window,
     .icon = "/chevrons-down.png"},
    {.display_name = "Center Active Window",
     .on_action = center_active_window,
     .icon = "/crosshair.png"},
    {.display_name = "Snap Left",
     .on_action = snap_left,
     .icon = "/panel-left.png"},
    {.display_name = "Snap Right",
     .on_action = snap_right,
     .icon = "/panel-right.png"},
    {.display_name = "Snap Top",
     .on_action = snap_top,
     .icon = "/panel-top.png"},
    {.display_name = "Snap Bottom",
     .on_action = snap_bottom,
     .icon = "/panel-bottom.png"},
    {.display_name = "Snap Top Left",
     .on_action = snap_top_left,
     .icon = "/arrow-up-left.png"},
    {.display_name = "Snap Top Right",
     .on_action = snap_top_right,
     .icon = "/arrow-up-right.png"},
    {.display_name = "Snap Bottom Left",
     .on_action = snap_bottom_left,
     .icon = "/arrow-down-left.png"},
    {.display_name = "Snap Bottom Right",
     .on_action = snap_bottom_right,
     .icon = "/arrow-down-right.png"},
    {.display_name = "Move Window To Next Monitor",
     .on_action = move_window_to_next_monitor,
     .icon = "/move-horizontal.png"},
    {.display_name = "Move Window To Previous Monitor",
     .on_action = move_window_to_prev_monitor,
     .icon = "/move-horizontal.png"},
    {.display_name = "Switch To Workspace 1",
     .on_action = switch_to_workspace_1,
     .icon = "/ws1.png"},
    {.display_name = "Switch To Workspace 2",
     .on_action = switch_to_workspace_2,
     .icon = "/ws2.png"},
    {.display_name = "Switch To Workspace 3",
     .on_action = switch_to_workspace_3,
     .icon = "/ws3.png"},
    {.display_name = "Switch To Workspace 4",
     .on_action = switch_to_workspace_4,
     .icon = "/ws4.png"},
    {.display_name = "Switch To Workspace 5",
     .on_action = switch_to_workspace_5,
     .icon = "/ws5.png"},
    {.display_name = "Switch To Workspace 6",
     .on_action = switch_to_workspace_6,
     .icon = "/ws6.png"},
    {.display_name = "Switch To Workspace 7",
     .on_action = switch_to_workspace_7,
     .icon = "/ws7.png"},
    {.display_name = "Switch To Workspace 8",
     .on_action = switch_to_workspace_8,
     .icon = "/ws8.png"},
    {.display_name = "Switch To Workspace 9",
     .on_action = switch_to_workspace_9,
     .icon = "/ws9.png"},
    {.display_name = "Move Window To Workspace 1",
     .on_action = move_window_to_workspace_1,
     .icon = "/mws1.png"},
    {.display_name = "Move Window To Workspace 2",
     .on_action = move_window_to_workspace_2,
     .icon = "/mws2.png"},
    {.display_name = "Move Window To Workspace 3",
     .on_action = move_window_to_workspace_3,
     .icon = "/mws3.png"},
    {.display_name = "Move Window To Workspace 4",
     .on_action = move_window_to_workspace_4,
     .icon = "/mws4.png"},
    {.display_name = "Move Window To Workspace 5",
     .on_action = move_window_to_workspace_5,
     .icon = "/mws5.png"},
    {.display_name = "Move Window To Workspace 6",
     .on_action = move_window_to_workspace_6,
     .icon = "/mws6.png"},
    {.display_name = "Move Window To Workspace 7",
     .on_action = move_window_to_workspace_7,
     .icon = "/mws7.png"},
    {.display_name = "Move Window To Workspace 8",
     .on_action = move_window_to_workspace_8,
     .icon = "/mws8.png"},
    {.display_name = "Move Window To Workspace 9",
     .on_action = move_window_to_workspace_9,
     .icon = "/mws9.png"},
    {.display_name = "Set Opacity Full",
     .on_action = opacity_full,
     .icon = "/percent.png"},
    {.display_name = "Set Opacity 80",
     .on_action = opacity_80,
     .icon = "/percent.png"},
    {.display_name = "Set Opacity 60",
     .on_action = opacity_60,
     .icon = "/percent.png"},
    {.display_name = "Set Opacity 40",
     .on_action = opacity_40,
     .icon = "/percent.png"},
    {.display_name = "Close Windows By Title",
     .on_action = close_windows_by_title,
     .icon = "/x.png"},
    {.display_name = "Close Windows By Id",
     .on_action = close_windows_by_id,
     .icon = "/box.png"},
};

SKALD_FEATURE_INIT {
  for (size_t i = 0; i < sizeof(WM_ENTRIES) / sizeof(WM_ENTRIES[0]); i++) {
    const WMEntry *e = &WM_ENTRIES[i];
    sk_add_to_cc(.display_name = e->display_name, .on_action = e->on_action,
                 .type = "Window Management", .icon = e->icon);
  }
}
