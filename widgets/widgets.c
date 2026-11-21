#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include "core/skapi.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sys/statvfs.h>
#endif

#define NO_BG true

static int s_widget_fps = 1;
static int s_widget_font_size = 14;
static char s_fps_buf[16] = "1";
static char s_font_buf[16] = "14";
static char s_net_buf[24] = "500";
static double s_net_sample_interval_ms = 500.0;

static int widgets_parse_int(const char *text, int fallback) {
  if (!text || !text[0])
    return fallback;
  char *end = NULL;
  long n = strtol(text, &end, 10);
  return (end && *end == '\0') ? (int)n : fallback;
}

static int widgets_setting_int(const char *key, int fallback) {
  char *v = sk_config_get_setting(key);
  int out = widgets_parse_int(v, fallback);
  free(v);
  return out;
}

static double widgets_setting_double(const char *key, double fallback) {
  if (!sk_config_has_setting(key))
    return fallback;
  return (double)sk_config_get_setting_float(key);
}

static bool read_disk(unsigned long long *used_out,
                      unsigned long long *total_out) {
  *used_out = 0;
  *total_out = 0;
#if defined(__linux__)
  struct statvfs vfs;
  if (statvfs("/", &vfs) != 0)
    return false;
  unsigned long long block_size = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
  unsigned long long total = vfs.f_blocks * block_size;
  if (total == 0)
    return false;
  *used_out = total - vfs.f_bfree * block_size;
  *total_out = total;
  return true;
#elif defined(_WIN32)
  char root[MAX_PATH] = {0};
  DWORD n = GetCurrentDirectoryA(sizeof(root) - 1, root);
  if (n == 0 || n < 3)
    return false;
  if (root[1] != ':')
    return false;
  root[2] = '\\';
  root[3] = 0;

  ULARGE_INTEGER avail, total, free_total;
  if (!GetDiskFreeSpaceExA(root, &avail, &total, &free_total))
    return false;
  if (total.QuadPart == 0)
    return false;
  *used_out = (unsigned long long)(total.QuadPart - free_total.QuadPart);
  *total_out = (unsigned long long)total.QuadPart;
  return true;
#endif
}

static bool read_mem(unsigned long long *used_out,
                     unsigned long long *total_out) {
  *used_out = 0;
  *total_out = 0;
#if defined(__linux__)
  FILE *fp = fopen("/proc/meminfo", "r");
  if (!fp)
    return false;
  unsigned long long total_kb = 0, avail_kb = 0;
  char line[256];
  while (fgets(line, sizeof(line), fp)) {
    if (strncmp(line, "MemTotal:", 9) == 0) {
      sscanf(line + 9, "%llu", &total_kb);
    } else if (strncmp(line, "MemAvailable:", 13) == 0) {
      sscanf(line + 13, "%llu", &avail_kb);
    }
    if (total_kb && avail_kb)
      break;
  }
  fclose(fp);
  if (total_kb == 0)
    return false;
  *used_out = (total_kb - avail_kb) * 1024ULL;
  *total_out = total_kb * 1024ULL;
  return true;
#elif defined(_WIN32)
  MEMORYSTATUSEX ms;
  ms.dwLength = sizeof(ms);
  if (!GlobalMemoryStatusEx(&ms))
    return false;
  if (ms.ullTotalPhys == 0)
    return false;
  *used_out = (unsigned long long)(ms.ullTotalPhys - ms.ullAvailPhys);
  *total_out = (unsigned long long)ms.ullTotalPhys;
  return true;
#endif
}

static bool sample_cpu(unsigned long long *busy_out,
                       unsigned long long *total_out) {
#if defined(__linux__)
  FILE *fp = fopen("/proc/stat", "r");
  if (!fp)
    return false;
  char label[16];
  unsigned long long user = 0, nice = 0, sys = 0, idle = 0, iowait = 0;
  unsigned long long irq = 0, softirq = 0, steal = 0;
  int got = fscanf(fp, "%15s %llu %llu %llu %llu %llu %llu %llu %llu", label,
                   &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal);
  fclose(fp);
  if (got < 5)
    return false;

  unsigned long long busy = user + nice + sys + irq + softirq + steal;
  unsigned long long total = busy + idle + iowait;
  *busy_out = busy;
  *total_out = total;
  return true;
#elif defined(_WIN32)
  FILETIME idle_ft, kernel_ft, user_ft;
  if (!GetSystemTimes(&idle_ft, &kernel_ft, &user_ft))
    return false;

  ULARGE_INTEGER idle, kernel, user;
  idle.LowPart = idle_ft.dwLowDateTime;
  idle.HighPart = idle_ft.dwHighDateTime;
  kernel.LowPart = kernel_ft.dwLowDateTime;
  kernel.HighPart = kernel_ft.dwHighDateTime;
  user.LowPart = user_ft.dwLowDateTime;
  user.HighPart = user_ft.dwHighDateTime;

  unsigned long long busy = (kernel.QuadPart - idle.QuadPart) + user.QuadPart;
  unsigned long long total = busy + idle.QuadPart;
  *busy_out = busy;
  *total_out = total;
  return true;
#endif
}

static bool read_cpu(unsigned long long *used_out,
                     unsigned long long *total_out) {
  static unsigned long long prev_busy = 0, prev_total = 0;
  static bool seeded = false;

  *used_out = 0;
  *total_out = 0;

  unsigned long long busy = 0, total = 0;
  if (!sample_cpu(&busy, &total))
    return false;

  if (!seeded) {
    seeded = true;
    prev_busy = busy;
    prev_total = total;
    *total_out = 1;
    return true;
  }
  unsigned long long d_total = total - prev_total;
  *used_out = busy - prev_busy;
  *total_out = d_total;
  if (d_total == 0)
    *used_out = 0;
  prev_busy = busy;
  prev_total = total;
  return d_total > 0;
}

static ColorRef usage_color(unsigned long long used, unsigned long long total) {
  if (total == 0)
    return RGB(255, 255, 255);
  unsigned int pct = (unsigned int)((used * 100ULL) / total);
  if (pct >= 80)
    return RGB(255, 0, 0);
  if (pct >= 60)
    return RGB(255, 165, 0);
  if (pct >= 40)
    return RGB(0, 100, 0);
  return RGB(0, 150, 0);
}

static void draw_usage(const char *label, unsigned long long used,
                       unsigned long long total) {
  char buf[128];
  if (total == 0) {
    snprintf(buf, sizeof(buf), "%s??", label);
  } else {
    char *used_str = sk_format_bytes(used);
    char *total_str = sk_format_bytes(total);
    snprintf(buf, sizeof(buf), "%s%s/%s", label, used_str ? used_str : "?",
             total_str ? total_str : "?");
    free(used_str);
    free(total_str);
  }
  sk_draw_text(.text = buf, .x = 10, .y = 6, .font_size = s_widget_font_size,
               .color = usage_color(used, total), .align = "LEFT");
}

static void widgets_disk_usage_render(SkWidgetState *state) {
  unsigned long long used = 0, total = 0;
  read_disk(&used, &total);
  draw_usage("Disk Usage:", used, total);
}

static void widgets_memory_usage_render(SkWidgetState *state) {
  unsigned long long used = 0, total = 0;
  read_mem(&used, &total);
  draw_usage("Mem Usage:", used, total);
}

static void widgets_cpu_usage_render(SkWidgetState *state) {
  unsigned long long used = 0, total = 0;
  read_cpu(&used, &total);
  char buf[128];
  if (total == 0) {
    snprintf(buf, sizeof(buf), "CPU Usage:??");
    sk_draw_text(.text = buf, .x = 10, .y = 6, .font_size = s_widget_font_size,
                 .color = RGB(255, 255, 255), .align = "LEFT");
    return;
  }
  snprintf(buf, sizeof(buf), "CPU Usage:%3llu%%", (used * 100ULL) / total);
  sk_draw_text(.text = buf, .x = 10, .y = 6, .font_size = s_widget_font_size,
               .color = usage_color(used, total), .align = "LEFT");
}

static void widgets_date_and_time_render(SkWidgetState *state) {
  char buf[128];
  time_t t = time(NULL);
  struct tm *tm = localtime(&t);
  if (!tm)
    return;

  strftime(buf, sizeof(buf), "%A %B %d %H:%M:%S", tm);

  sk_draw_text(.text = buf, .x = 10, .y = 6, .font_size = s_widget_font_size,
               .color = RGB(220, 220, 220), .align = "LEFT");
}

static void net_usage_tick(void);

#define NET_ICON_SIZE 16

static void widgets_internet_render(SkWidgetState *state) {
  net_usage_tick();

  SkNetStatus status = sk_net_status();
  const char *icon = NULL;
  switch (status) {
  case SK_NET_OFFLINE:
    icon = "/net_offline.png";
    break;
  case SK_NET_ETHERNET:
    icon = "/net_ethernet.png";
    break;
  case SK_NET_WIFI:
    icon = "/net_wifi.png";
    break;
  }
  if (!icon)
    return;
  sk_draw_image(.path = icon, .x = 40, .y = 2, .w = NET_ICON_SIZE,
                .h = NET_ICON_SIZE);
}

static SkWidget *disk = NULL;
static SkWidget *mem = NULL;
static SkWidget *cpu = NULL;
static SkWidget *internet = NULL;

#define NET_POPUP_W 300
#define NET_POPUP_H 100
#define NET_ROW_H 26
#define NET_PAD 16
#define NET_SAVE_EVERY_SEC 5
#define NET_STATE_FILE "net_usage.txt"

typedef struct {
  unsigned long long today_bytes;
  unsigned long long month_bytes;
  int today_day;
  int month_index;
  unsigned long long last_total_rx;
  double rate_bps;
  int have_baseline;
} NetUsage;

static NetUsage net;
static SkTimer net_last_save;
static SkTimer net_last_sample;
static bool net_persist_loaded;

static void net_localtime(time_t t, struct tm *out) {
#if defined(_WIN32)
  localtime_s(out, &t);
#elif defined(__linux__)
  localtime_r(&t, out);
#endif
}

static void net_usage_reset_day(NetUsage *u, int day, int month_index) {
  if (u->today_day == day && u->month_index == month_index)
    return;
  if (u->month_index != month_index) {
    u->month_bytes = 0;
    u->month_index = month_index;
  }
  u->today_bytes = 0;
  u->today_day = day;
}

static void net_usage_load(void) {
  if (net_persist_loaded)
    return;
  net_persist_loaded = true;

  time_t now = time(NULL);
  struct tm tm_now;
  net_localtime(now, &tm_now);
  net.today_day = tm_now.tm_mday;
  net.month_index = tm_now.tm_mon;

  char *body = sk_load_file(NET_STATE_FILE);
  if (!body)
    return;

  char *cursor = body;
  while (cursor && *cursor) {
    char *nl = strchr(cursor, '\n');
    if (nl)
      *nl = '\0';
    unsigned long long today = 0, month = 0;
    int day = 0, month_index = 0;
    if (sscanf(cursor, "%d %d %llu %llu", &day, &month_index, &today, &month) ==
        4) {
      if (month_index == net.month_index) {
        net.month_bytes = month;
        if (day == net.today_day)
          net.today_bytes = today;
      }
    }
    cursor = nl ? nl + 1 : NULL;
  }
  free(body);
}

static void net_usage_save(void) {
  char line[128];
  snprintf(line, sizeof(line), "%d %d %llu %llu", net.today_day,
           net.month_index, net.today_bytes, net.month_bytes);
  sk_write_string_to_file(NET_STATE_FILE, line);
}

static void net_usage_sample(void) {
  unsigned long long total_rx = sk_net_rx_bytes();
  double elapsed = sk_end_timer(net_last_sample);
  if (net.have_baseline && total_rx >= net.last_total_rx && elapsed > 0.05) {
    double delta = (double)(total_rx - net.last_total_rx);
    double inst = delta / elapsed;
    if (net.rate_bps <= 0.0)
      net.rate_bps = inst;
    else
      net.rate_bps = net.rate_bps * 0.7 + inst * 0.3;
  }
  if (net.have_baseline && total_rx > net.last_total_rx) {
    unsigned long long delta = total_rx - net.last_total_rx;
    net.today_bytes += delta;
    net.month_bytes += delta;
  }
  net.last_total_rx = total_rx;
  net.have_baseline = 1;
  net_last_sample = sk_start_timer();
}



static SkTimer net_last_tick;

static void net_usage_tick(void) {
  if (sk_end_timer(net_last_tick) < s_net_sample_interval_ms)
    return;
  net_last_tick = sk_start_timer();
  net_usage_load();

  time_t now = time(NULL);
  struct tm tm_now;
  net_localtime(now, &tm_now);
  net_usage_reset_day(&net, tm_now.tm_mday, tm_now.tm_mon);
  net_usage_sample();

  if (sk_end_timer(net_last_save) >= (double)NET_SAVE_EVERY_SEC * 1000.0) {
    net_usage_save();
    net_last_save = sk_start_timer();
  }
}

static void net_row(int y, const char *label, const char *value,
                    unsigned long color) {
  sk_draw_text(.text = label, .x = NET_PAD, .y = y, .font_size = s_widget_font_size,
               .color = color, .align = "LEFT");
  sk_draw_text(.text = value, .x = NET_POPUP_W - NET_PAD, .y = y,
               .font_size = s_widget_font_size, .color = RGB(235, 235, 245),
               .align = "RIGHT");
}

static void internet_hover_render(void *ud) {
  char *today = sk_format_bytes(net.today_bytes);
  char *month = sk_format_bytes(net.month_bytes);
  char *rate = sk_format_bytes(net.rate_bps * 1024);

  int y = NET_PAD;
  net_row(y, "rate now:", rate, RGB(160, 220, 255));
  y += NET_ROW_H;

  net_row(y, "today:", today, RGB(200, 200, 220));
  y += NET_ROW_H;

  net_row(y, "this month:", month, RGB(200, 200, 220));
  y += NET_ROW_H + 4;
  free(today);
  free(month);
  free(rate);
}

static void on_internet_hover(void) {
  Pos pos = sk_get_widget_pos("internet");
  sk_message(.x = pos.x, .y = pos.y - 70, .w = NET_POPUP_W, .h = NET_POPUP_H,
             .render = internet_hover_render, .hover_key = "internet");
}

static void widgets_fps_change(const char *field, const char *value) {
  (void)field;
  int n = widgets_parse_int(value, s_widget_fps);
  if (n < 1)
    n = 1;
  s_widget_fps = n;
  snprintf(s_fps_buf, sizeof(s_fps_buf), "%d", n);
  sk_config_set_setting("Widgets.FPS", s_fps_buf);
}

static void widgets_font_change(const char *field, const char *value) {
  (void)field;
  int n = widgets_parse_int(value, s_widget_font_size);
  if (n < 6)
    n = 6;
  s_widget_font_size = n;
  snprintf(s_font_buf, sizeof(s_font_buf), "%d", n);
  sk_config_set_setting("Widgets.FontSize", s_font_buf);
}

static void widgets_net_change(const char *field, const char *value) {
  (void)field;
  int n = widgets_parse_int(value, (int)s_net_sample_interval_ms);
  if (n < 50)
    n = 50;
  s_net_sample_interval_ms = (double)n;
  snprintf(s_net_buf, sizeof(s_net_buf), "%d", n);
  sk_config_set_setting("Widgets.NetSampleMs", s_net_buf);
}

SKALD_FEATURE_INIT {
  s_widget_fps = widgets_setting_int("Widgets.FPS", 1);
  if (s_widget_fps < 1)
    s_widget_fps = 1;
  snprintf(s_fps_buf, sizeof(s_fps_buf), "%d", s_widget_fps);

  s_widget_font_size = widgets_setting_int("Widgets.FontSize", 14);
  if (s_widget_font_size < 6)
    s_widget_font_size = 6;
  snprintf(s_font_buf, sizeof(s_font_buf), "%d", s_widget_font_size);

  s_net_sample_interval_ms =
      widgets_setting_double("Widgets.NetSampleMs", 500.0);
  if (s_net_sample_interval_ms < 50.0)
    s_net_sample_interval_ms = 50.0;
  snprintf(s_net_buf, sizeof(s_net_buf), "%.0f", s_net_sample_interval_ms);

  disk = sk_widget(.name = "disk_usage", .fps = s_widget_fps, .no_bg = NO_BG,
                   .slot = WIDGET_SLOT_BOTTOM_LEFT,
                   .render = widgets_disk_usage_render);

  mem = sk_widget(.name = "memory_usage", .fps = s_widget_fps, .no_bg = NO_BG,
                  .slot = WIDGET_SLOT_BOTTOM_LEFT,
                  .render = widgets_memory_usage_render);

  cpu = sk_widget(.name = "cpu_usage", .fps = s_widget_fps, .no_bg = NO_BG,
                  .slot = WIDGET_SLOT_BOTTOM_LEFT,
                  .render = widgets_cpu_usage_render);

  sk_widget(.name = "date_and_time", .fps = s_widget_fps, .no_bg = NO_BG,
            .slot = WIDGET_SLOT_BOTTOM_RIGHT,
            .render = widgets_date_and_time_render);

  internet =
      sk_widget(.name = "internet", .fps = s_widget_fps, .no_bg = NO_BG,
                .on_hover = on_internet_hover, .slot = WIDGET_SLOT_BOTTOM_RIGHT,
                .render = widgets_internet_render);

  SkSettingsEntry *entry = sk_add_settings_entry(
      .name = "Widgets", .icon = "/widgets.png",
      .desc = "Desktop widget refresh and appearance.");
  SkSettingsEditboxField fps_field = {.name = "fps",
                                       .label = "FPS:",
                                       .buffer = s_fps_buf,
                                       .buffer_size = sizeof(s_fps_buf)};
  sk_settings_entry_add_editbox(entry, .title = "Refresh FPS",
                                .sub = "Widget refresh rate; restarts widgets.",
                                .fields = &fps_field, .field_count = 1,

                                .on_change = widgets_fps_change);
  SkSettingsEditboxField font_field = {.name = "font_size",
                                        .label = "Size:",
                                        .buffer = s_font_buf,
                                        .buffer_size = sizeof(s_font_buf)};
  sk_settings_entry_add_editbox(entry, .title = "Font Size",
                                .sub = "Font size used by widget text.",
                                .fields = &font_field, .field_count = 1,

                                .on_change = widgets_font_change);
  SkSettingsEditboxField net_field = {.name = "net_ms",
                                       .label = "ms:",
                                       .buffer = s_net_buf,
                                       .buffer_size = sizeof(s_net_buf)};
  sk_settings_entry_add_editbox(entry, .title = "Network Sample Interval",
                                .sub = "How often network usage is sampled.",
                                .fields = &net_field, .field_count = 1,

                                .on_change = widgets_net_change);
}
