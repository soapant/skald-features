#include "core/skapi.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>

static char db_path[4096] = {0};
static char exec_name[64] = "librewolf";

static SkLibrary g_sqlite_lib = NULL;
static int (*p_sqlite3_open_v2)(const char *, sqlite3 **, int,
                                const char *) = NULL;
static int (*p_sqlite3_close)(sqlite3 *) = NULL;
static int (*p_sqlite3_prepare_v2)(sqlite3 *, const char *, int,
                                   sqlite3_stmt **, const char **) = NULL;
static int (*p_sqlite3_step)(sqlite3_stmt *) = NULL;
static const unsigned char *(*p_sqlite3_column_text)(sqlite3_stmt *,
                                                     int) = NULL;
static int (*p_sqlite3_finalize)(sqlite3_stmt *) = NULL;

static int sqlite_init(void) {
  if (g_sqlite_lib)
    return 1;
  g_sqlite_lib = sk_library_get("sqlite");
  if (!g_sqlite_lib) {
    char buf[1024] = {0};
    char *dir = sk_resolve_feature_path("librewolf");
    if (dir) {
#if defined(_WIN32)
      snprintf(buf, sizeof(buf), "%s/sqlite3.dll", dir);
#elif defined(__linux__)
      snprintf(buf, sizeof(buf), "%s/libsqlite3.so", dir);
#endif
      g_sqlite_lib = sk_library_open(buf);
      if (g_sqlite_lib)
        sk_library_register("sqlite", g_sqlite_lib);
    }
  }
  if (!g_sqlite_lib)
    return 0;
  p_sqlite3_open_v2 =
      (int (*)(const char *, sqlite3 **, int, const char *))sk_library_symbol(
          g_sqlite_lib, "sqlite3_open_v2");
  p_sqlite3_close =
      (int (*)(sqlite3 *))sk_library_symbol(g_sqlite_lib, "sqlite3_close");
  p_sqlite3_prepare_v2 =
      (int (*)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **))
          sk_library_symbol(g_sqlite_lib, "sqlite3_prepare_v2");
  p_sqlite3_step =
      (int (*)(sqlite3_stmt *))sk_library_symbol(g_sqlite_lib, "sqlite3_step");
  p_sqlite3_column_text =
      (const unsigned char *(*)(sqlite3_stmt *, int))sk_library_symbol(
          g_sqlite_lib, "sqlite3_column_text");
  p_sqlite3_finalize = (int (*)(sqlite3_stmt *))sk_library_symbol(
      g_sqlite_lib, "sqlite3_finalize");
  return p_sqlite3_open_v2 && p_sqlite3_prepare_v2 && p_sqlite3_step;
}

static void browser_scan(void) {
  char path[1024];
#if defined(_WIN32)
  const char *appdata = getenv("APPDATA");
  if (appdata)
    snprintf(path, sizeof(path), "%s\\LibreWolf", appdata);
  else
    return;
#elif defined(__linux__)
  const char *home = getenv("HOME");
  if (!home)
    return;
  snprintf(path, sizeof(path), "%s/.config/librewolf", home);
#endif
  if (!sk_file_exists(path)) {
    return;
  }
  DIR *d = opendir(path);
  if (!d) {
    return;
  }
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.')
      continue;
    char subdir[2048];
    snprintf(subdir, sizeof(subdir), "%s/%s", path, e->d_name);
    DIR *d2 = opendir(subdir);
    if (!d2)
      continue;
    struct dirent *e2;
    while ((e2 = readdir(d2)) != NULL) {
      if (e2->d_name[0] == '.')
        continue;
      char full[4096];
      snprintf(full, sizeof(full), "%s/%s/places.sqlite", subdir, e2->d_name);
      if (sk_file_exists(full)) {
        snprintf(db_path, sizeof(db_path), "%s", full);
        closedir(d2);
        closedir(d);
        return;
      }
      // also direct
      snprintf(full, sizeof(full), "%s/places.sqlite", subdir);
      if (sk_file_exists(full)) {
        snprintf(db_path, sizeof(db_path), "%s", full);
        closedir(d2);
        closedir(d);
        return;
      }
    }
    closedir(d2);
  }
  closedir(d);
}

static void open_browser(void) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "%s", exec_name);
  sk_launch_detached(cmd);
}

static void open_private(void) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "%s --private-window", exec_name);
  sk_launch_detached(cmd);
}

static void show_bookmarks(void) {
  if (!db_path[0]) {
    sk_message(.message = "LibreWolf places.sqlite not found.");
    return;
  }
  if (!sqlite_init()) {
    sk_message(.message = "SQLite unavailable.");
    return;
  }
  char buf[4096] = {0};
  size_t pos = 0;
  sqlite3 *db = NULL;
  if (p_sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) !=
      SQLITE_OK) {
    if (db)
      p_sqlite3_close(db);
    sk_message(.message = "Could not open DB.");
    return;
  }
  sqlite3_stmt *stmt = NULL;
  if (p_sqlite3_prepare_v2(db,
                           "SELECT p.title, p.url FROM moz_bookmarks b JOIN "
                           "moz_places p ON b.fk=p.id WHERE b.type=1 LIMIT 20;",
                           -1, &stmt, NULL) == SQLITE_OK) {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "LibreWolf bookmarks:\\n");
    while (p_sqlite3_step(stmt) == SQLITE_ROW && pos < sizeof(buf) - 256) {
      const unsigned char *title = p_sqlite3_column_text(stmt, 0);
      const unsigned char *url = p_sqlite3_column_text(stmt, 1);
      pos += snprintf(buf + pos, sizeof(buf) - pos, " - %s (%s)\\n",
                      title ? (const char *)title : "(no title)",
                      url ? (const char *)url : "");
    }
    p_sqlite3_finalize(stmt);
  }
  p_sqlite3_close(db);
  if (pos == 0)
    snprintf(buf, sizeof(buf), "No bookmarks found.");
  sk_message(.message = buf);
  return;
}

SKALD_FEATURE_INIT {
  sqlite_init();
  browser_scan();
  sk_add_to_cc(.display_name = "Open LibreWolf Window",
               .on_enter = (bool (*)(void))open_browser, .type = "Browser",
               .icon = "/librewolf.png");
  sk_add_to_cc(.display_name = "Open LibreWolf Private Window",
               .on_enter = (bool (*)(void))open_private, .type = "Browser",
               .icon = "/librewolf.png");
  sk_add_to_cc(.display_name = "Show LibreWolf Bookmarks",
               .on_enter = (bool (*)(void))show_bookmarks, .type = "Browser",
               .icon = "/librewolf.png");
}
