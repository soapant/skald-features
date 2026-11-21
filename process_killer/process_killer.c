#include "core/skapi.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#if defined(__linux__)
#include <strings.h>
#endif

#define PK_MAX 2048
#define PK_NAME_LEN 64
#define PK_ROW_LEN 160
#define PK_SORT_MODES 3

typedef struct {
  char name[PK_NAME_LEN];
  unsigned long long rss_kb;
  double cpu;
} pk_proc;

static pk_proc pk[PK_MAX];
static int pk_count = 0;

static int pk_sort_mode(void) {
  int mode = sk_config_get_setting_int("ProcessKiller.DefaultSort", 0);
  if (mode < 0 || mode >= PK_SORT_MODES)
    mode = 0;
  return mode;
}

static int pk_refresh_ms(void) {
  int ms = sk_config_get_setting_int("ProcessKiller.RefreshMs", 1000);
  return ms < 100 ? 100 : ms;
}
static SkTimer pk_last_collect;
static bool pk_collect_started = false;
static char pk_rows[PK_MAX][PK_ROW_LEN];
static const char *pk_items[PK_MAX];

static const char *pk_sort_label(int mode) {
  if (mode == 1)
    return "cpu";
  if (mode == 2)
    return "name";
  return "memory";
}

static int pk_cmp_mem(const void *a, const void *b) {
  const pk_proc *x = a;
  const pk_proc *y = b;
  if (x->rss_kb != y->rss_kb)
    return x->rss_kb < y->rss_kb ? 1 : -1;
  return strcmp(x->name, y->name);
}

static int pk_cmp_cpu(const void *a, const void *b) {
  const pk_proc *x = a;
  const pk_proc *y = b;
  if (x->cpu != y->cpu)
    return x->cpu < y->cpu ? 1 : -1;
  if (x->rss_kb != y->rss_kb)
    return x->rss_kb < y->rss_kb ? 1 : -1;
  return strcmp(x->name, y->name);
}

static int pk_cmp_alpha(const void *a, const void *b) {
  return strcasecmp(((const pk_proc *)a)->name, ((const pk_proc *)b)->name);
}

#if defined(__linux__)
static void pk_rtrim(char *s) {
  size_t n = strlen(s);
  while (n > 0 && isspace(s[n - 1]))
    s[--n] = '\0';
}

static void pk_parse_line(char *line) {
  pk_rtrim(line);
  size_t len = strlen(line);
  if (len == 0)
    return;

  size_t e = len;
  while (e > 0 && isspace(line[e - 1]))
    e--;
  if (e == 0)
    return;

  size_t f3 = e;
  while (f3 > 0 && !isspace(line[f3 - 1]))
    f3--;
  if (f3 == 0)
    return;
  line[f3] = '\0';
  const char *cpu_s = line + f3 + 1;

  size_t f2 = f3;
  while (f2 > 0 && isspace(line[f2 - 1]))
    f2--;
  if (f2 == 0)
    return;
  size_t f2e = f2;
  while (f2 > 0 && !isspace(line[f2 - 1]))
    f2--;

  char rss_buf[32];
  size_t rlen = f2e - f2;
  if (rlen >= sizeof(rss_buf))
    rlen = sizeof(rss_buf) - 1;
  memcpy(rss_buf, line + f2, rlen);
  rss_buf[rlen] = '\0';
  line[f2] = '\0';
  const char *rss_s = rss_buf;

  char *name = line;
  while (isspace(*name))
    name++;
  if (*name == '\0')
    return;
  if (pk_count >= PK_MAX)
    return;

  pk_proc *p = &pk[pk_count++];
  snprintf(p->name, sizeof(p->name), "%s", name);
  p->rss_kb = strtoull(rss_s, NULL, 10);
  p->cpu = strtod(cpu_s, NULL);
}

static void pk_collect_running_processes(void) {
  pk_count = 0;
  char *out = sk_run_terminal_command("ps -eo comm=,rss=,%cpu= --no-headers");
  if (!out)
    return;
  char *p = out;
  while (*p && pk_count < PK_MAX) {
    char *nl = strchr(p, '\n');
    if (nl)
      *nl = '\0';
    pk_parse_line(p);
    p = nl ? nl + 1 : p + strlen(p);
  }
  free(out);
}
#elif defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

static int find_or_add_pk_proc(const char *name) {
  for (int i = 0; i < pk_count; i++) {
    if (strcasecmp(pk[i].name, name) == 0) {
      return i;
    }
  }
  if (pk_count >= PK_MAX)
    return -1;

  int index = pk_count++;
  snprintf(pk[index].name, sizeof(pk[index].name), "%s", name);
  pk[index].rss_kb = 0;
  pk[index].cpu = 0.0;
  return index;
}

static void pk_collect_running_processes(void) {
  pk_count = 0;

  typedef struct {
    DWORD pid;
    ULONGLONG cpu_start;
  } CpuSample;
  static CpuSample samples[PK_MAX];
  int sample_count = 0;

  SYSTEM_INFO sysInfo;
  GetSystemInfo(&sysInfo);
  DWORD num_cores = sysInfo.dwNumberOfProcessors;

  HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnapshot == INVALID_HANDLE_VALUE)
    return;

  PROCESSENTRY32 pe32;
  pe32.dwSize = sizeof(PROCESSENTRY32);

  if (!Process32First(hSnapshot, &pe32)) {
    CloseHandle(hSnapshot);
    return;
  }

  do {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                  pe32.th32ProcessID);
    if (hProcess) {
      FILETIME ftCreation, ftExit, ftKernel, ftUser;
      if (GetProcessTimes(hProcess, &ftCreation, &ftExit, &ftKernel, &ftUser) &&
          sample_count < PK_MAX) {
        ULONGLONG total_ticks =
            (((ULONGLONG)ftKernel.dwHighDateTime << 32) |
             ftKernel.dwLowDateTime) +
            (((ULONGLONG)ftUser.dwHighDateTime << 32) | ftUser.dwLowDateTime);
        samples[sample_count].pid = pe32.th32ProcessID;
        samples[sample_count].cpu_start = total_ticks;
        sample_count++;
      }
      CloseHandle(hProcess);
    }
  } while (Process32Next(hSnapshot, &pe32));

  if (!Process32First(hSnapshot, &pe32)) {
    CloseHandle(hSnapshot);
    return;
  }

  do {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                  pe32.th32ProcessID);
    if (hProcess) {
      PROCESS_MEMORY_COUNTERS_EX pmc;
      if (GetProcessMemoryInfo(hProcess, (PROCESS_MEMORY_COUNTERS *)&pmc,
                               sizeof(pmc))) {

        int index = find_or_add_pk_proc(pe32.szExeFile);
        if (index != -1) {

          pk[index].rss_kb += (unsigned long long)(pmc.PrivateUsage / 1024);

          FILETIME ftCreation, ftExit, ftKernel, ftUser;
          if (GetProcessTimes(hProcess, &ftCreation, &ftExit, &ftKernel,
                              &ftUser)) {
            ULONGLONG cpu_end = (((ULONGLONG)ftKernel.dwHighDateTime << 32) |
                                 ftKernel.dwLowDateTime) +
                                (((ULONGLONG)ftUser.dwHighDateTime << 32) |
                                 ftUser.dwLowDateTime);

            for (int s = 0; s < sample_count; s++) {
              if (samples[s].pid == pe32.th32ProcessID) {
                ULONGLONG delta_ticks = (cpu_end > samples[s].cpu_start)
                                            ? (cpu_end - samples[s].cpu_start)
                                            : 0;

                double process_pct = ((double)delta_ticks / 1000000.0) * 100.0;
                pk[index].cpu += (process_pct / num_cores);
                break;
              }
            }
          }
        }
      }
      CloseHandle(hProcess);
    }
  } while (Process32Next(hSnapshot, &pe32));

  CloseHandle(hSnapshot);

  for (int i = 0; i < pk_count; i++) {
    if (pk[i].cpu > 100.0)
      pk[i].cpu = 100.0;
  }
}
#endif

static void pk_sort(void) {
  if (pk_count <= 1)
    return;
  int mode = pk_sort_mode();
  qsort(pk, (size_t)pk_count, sizeof(pk_proc),
        mode == 0   ? pk_cmp_mem
        : mode == 1 ? pk_cmp_cpu
                    : pk_cmp_alpha);
}

static void pk_split_row(const char *row, char *name, size_t name_len,
                         char *mem, size_t mem_len, char *cpu, size_t cpu_len) {
  if (name_len)
    name[0] = '\0';
  if (mem_len)
    mem[0] = '\0';
  if (cpu_len)
    cpu[0] = '\0';
  if (!row || !*row)
    return;

  const char *bar1 = strchr(row, '|');
  if (!bar1) {
    snprintf(name, name_len, "%s", row);
    return;
  }

  size_t n = (size_t)(bar1 - row);
  while (n > 0 && (row[n - 1] == ' ' || row[n - 1] == '\t'))
    n--;
  snprintf(name, name_len, "%.*s", (int)n, row);

  const char *rest = bar1 + 1;
  while (*rest == ' ' || *rest == '\t')
    rest++;
  const char *bar2 = strchr(rest, '|');
  if (!bar2) {
    snprintf(mem, mem_len, "%s", rest);
    return;
  }

  size_t m = (size_t)(bar2 - rest);
  while (m > 0 && (rest[m - 1] == ' ' || rest[m - 1] == '\t'))
    m--;
  snprintf(mem, mem_len, "%.*s", (int)m, rest);

  const char *c = bar2 + 1;
  while (*c == ' ' || *c == '\t')
    c++;
  snprintf(cpu, cpu_len, "%s%%", c);
}

static void pk_build_rows(void) {
  for (int i = 0; i < pk_count; i++) {
    char *mem = sk_format_bytes((unsigned long long)pk[i].rss_kb * 1024ULL);
    snprintf(pk_rows[i], PK_ROW_LEN, "%.48s | %.20s | %.1f", pk[i].name,
             mem ? mem : "?", pk[i].cpu);
    free(mem);
    pk_items[i] = pk_rows[i];
  }
}

static void pk_collect_and_sort(void) {
  pk_collect_running_processes();
  pk_sort();
  pk_build_rows();
}

static void pk_cycle_sort(void) {
  int mode = (pk_sort_mode() + 1) % PK_SORT_MODES;
  sk_config_set_setting_int("ProcessKiller.DefaultSort", mode);
  pk_collect_and_sort();
}

static bool pk_enter_callback(PromptState *state) {
  if (!state->lb->selected_text)
    return false;
  char name[PK_NAME_LEN];
  char mem[32];
  char cpu[24];
  pk_split_row(state->lb->selected_text, name, sizeof(name), mem, sizeof(mem),
               cpu, sizeof(cpu));
  if (name[0] == '\0')
    return false;
  sk_terminate_process_by_name(name);
  return true;
}

static void pk_refresh_callback(PromptState *state) {
  pk_last_collect = sk_start_timer();
  pk_collect_started = true;
  pk_collect_and_sort();
  sk_populate_listbox(.lb = state->lb, .filter = state->eb->filter,
                      .items = pk_items, .count = pk_count);
}

#define PK_COL_MEM 430
#define PK_COL_CPU 600

static void pk_render_callback(PromptState *state) {
  if (!state)
    return;
  double since = pk_collect_started ? sk_end_timer(pk_last_collect) : 1e9;
  if (since >= pk_refresh_ms()) {
    pk_last_collect = sk_start_timer();
    pk_collect_started = true;
    int keep = state->lb->selected_index;
    pk_collect_and_sort();
    sk_populate_listbox(.lb = state->lb, .filter = state->eb->filter,
                        .items = pk_items, .count = pk_count);
    if (keep >= 0 && keep < state->lb->item_count) {
      state->lb->selected_index = keep;
    }
  }

  if (sk_hotkey_down(sk_hotkey_from_string("Ctrl+L"))) {
    pk_cycle_sort();
    sk_populate_listbox(.lb = state->lb, .filter = NULL, .items = pk_items,
                        .count = pk_count);
  }

  sk_draw_text(.text = "memory", .x = PK_COL_MEM, .y = 8, .font_size = 13,
               .align = "LEFT", .color = RGB(130, 130, 160));
  sk_draw_text(.text = "cpu", .x = PK_COL_CPU, .y = 8, .font_size = 13,
               .align = "LEFT", .color = RGB(130, 130, 160));

  char sort_label[64];
  snprintf(sort_label, sizeof(sort_label), "current sort: %s",
           pk_sort_label(pk_sort_mode()));

  sk_draw_text(.text = sort_label, .x = 880, .y = 8, .font_size = 13,
               .align = "RIGHT", .color = RGB(160, 160, 190));

  sk_place_editbox();
  if (!sk_listbox_begin(.lb = state->lb, .item_height = FontSize + 20))
    return;

  SkListItemCtx ctx;
  while (sk_listbox_next(&ctx)) {
    char name[PK_NAME_LEN];
    char mem[32];
    char cpu[24];
    pk_split_row(ctx.text ? ctx.text : "", name, sizeof(name), mem, sizeof(mem),
                 cpu, sizeof(cpu));
    if (name[0] == '\0')
      continue;

    SkRect r_text =
        sk_text_bounds(.text = name, .x = ctx.x + 14, .y = ctx.y + 4,
                       .align = "LEFT", .pad = ctx.x + 14);
    r_text.color = sk_color_lerp(COLOR_BG, SK_BLACK, 0.32f);
    r_text.radius = 5;
    r_text.filled = true;
    ski_draw_rect(&r_text);
    sk_draw_text(.text = name, .x = ctx.x + 14, .y = ctx.y + 4, .align = "LEFT",
                 .pad = ctx.x + 14);

    SkRect r_mem = sk_text_bounds(.text = mem, .x = PK_COL_MEM, .y = ctx.y + 4,
                                  .align = "LEFT");
    r_mem.color = sk_color_lerp(COLOR_BG, SK_BLACK, 0.32f);
    r_mem.radius = 5;
    r_mem.filled = true;
    ski_draw_rect(&r_mem);
    sk_draw_text(.text = mem, .x = PK_COL_MEM, .y = ctx.y + 4, .align = "LEFT");

    SkRect r_cpu = sk_text_bounds(.text = cpu, .x = PK_COL_CPU, .y = ctx.y + 4,
                                  .align = "LEFT");
    r_cpu.color = sk_color_lerp(COLOR_BG, SK_BLACK, 0.32f);
    r_cpu.radius = 5;
    r_cpu.filled = true;
    ski_draw_rect(&r_cpu);
    sk_draw_text(.text = cpu, .x = PK_COL_CPU, .y = ctx.y + 4, .align = "LEFT");
  }
  sk_listbox_end();
}

static void trigger_process_killer(void) {
  pk_collect_and_sort();
  sk_window(.title = "ProcessKiller", .refresh = pk_refresh_callback,
            .enter = pk_enter_callback, .render = pk_render_callback);
}

static const char *s_sort_options[] = {"memory", "cpu", "name"};

static int pk_sort_options(const char ***out_options) {
  *out_options = s_sort_options;
  return PK_SORT_MODES;
}

static void pk_sort_select(const char *option, int index) {
  (void)option;
  if (index < 0 || index >= PK_SORT_MODES)
    return;
  sk_config_set_setting_int("ProcessKiller.DefaultSort", index);
}

SKALD_FEATURE_INIT {
  sk_register_hotkey(.hotkey = "Alt+U", .cb = trigger_process_killer);
  sk_add_feature(.display_name = "Process Killer",
                 .on_action = trigger_process_killer, .icon = "/skull.png");

  SkSettingsEntry *entry =
      sk_add_settings_entry(.name = "Process Killer", .icon = "/skull.png",
                            .desc = "Process list refresh and sorting.");
  SkSettingsEditboxField field = {.name = "refresh_ms",
                                  .label = "ms:",
                                  .config_key = "ProcessKiller.RefreshMs",
                                  .default_value = "1000",
                                  .type = SK_SETTINGS_EDITBOX_INT,
                                  .min_value = 10.0f};
  sk_settings_entry_add_editbox(entry, .title = "Refresh Interval",
                                .sub = "How often the process list updates.",
                                .fields = &field, .field_count = 1);

  sk_settings_entry_add_dropdown(
      entry, .title = "Default Sort", .sub = "Sort order when the list opens.",
      .refresh_options = pk_sort_options, .initial = pk_sort_mode(),
      .on_select = pk_sort_select);
}
