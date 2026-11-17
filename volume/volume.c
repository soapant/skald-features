#include "core/skapi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

const GUID skald_IID_IAudioEndpointVolume = {
    0x5cdf2c82,
    0x841e,
    0x4546,
    {0x97, 0x22, 0x0c, 0xf7, 0x40, 0x78, 0x22, 0x9a}};
#elif defined(__linux__)
#include <unistd.h>

#define VOLUME_GIO_PATH "libgio-2.0.so.0"
#define VOLUME_G_BUS_TYPE_SESSION 2
#define VOLUME_G_DBUS_FLAGS_NONE 0

typedef struct {
  int code;
  char *message;
} GlError;

typedef struct _GDBusConnection GlDbConnection;
typedef struct _GDBusProxy GlDbProxy;
typedef struct _GVariant GlVariant;
typedef struct _GVariantIter GlVariantIter;

typedef GlDbConnection *(*pfn_g_bus_get_sync)(int, void *, GlError **);
typedef GlVariant *(*pfn_g_dbus_connection_call_sync)(
    GlDbConnection *, const char *, const char *, const char *, const char *,
    GlVariant *, void *, int, int, void *, GlError **);
typedef GlDbProxy *(*pfn_g_dbus_proxy_new_sync)(GlDbConnection *, int, void *,
                                                const char *, const char *,
                                                const char *, void *,
                                                GlError **);
typedef GlVariant *(*pfn_g_dbus_proxy_call_sync)(GlDbProxy *, const char *,
                                                 GlVariant *, int, int, void *,
                                                 GlError **);
typedef void (*pfn_g_variant_get)(GlVariant *, const char *, ...);
typedef int (*pfn_g_variant_iter_next)(GlVariantIter *, const char *, ...);
typedef void (*pfn_g_variant_iter_free)(GlVariantIter *);
typedef void (*pfn_g_variant_unref)(GlVariant *);
typedef void (*pfn_g_object_unref)(void *);
typedef void (*pfn_g_error_free)(GlError *);
typedef char *(*pfn_g_strdup)(const char *);
typedef int (*pfn_g_str_has_prefix)(const char *, const char *);
typedef void (*pfn_g_free)(void *);

struct {
  bool ready;
  SkLibrary handle;
  pfn_g_bus_get_sync bus_get_sync;
  pfn_g_dbus_connection_call_sync dbus_connection_call_sync;
  pfn_g_dbus_proxy_new_sync dbus_proxy_new_sync;
  pfn_g_dbus_proxy_call_sync dbus_proxy_call_sync;
  pfn_g_variant_get variant_get;
  pfn_g_variant_iter_next variant_iter_next;
  pfn_g_variant_iter_free variant_iter_free;
  pfn_g_variant_unref variant_unref;
  pfn_g_object_unref object_unref;
  pfn_g_error_free error_free;
  pfn_g_strdup strdup;
  pfn_g_str_has_prefix str_has_prefix;
  pfn_g_free free;
} gio;

#define GIO_RESOLVE(handle, field, symbol)                                     \
  gio.field = (pfn_##symbol)sk_library_symbol((handle), #symbol)

static bool gio_load(void) {
  if (gio.ready)
    return true;
  SkLibrary handle = sk_library_open(VOLUME_GIO_PATH);
  if (!handle) {
    printf("volume: cannot open %s: %s\n", VOLUME_GIO_PATH,
           sk_library_error() ? sk_library_error() : "not found");
    return false;
  }
  GIO_RESOLVE(handle, bus_get_sync, g_bus_get_sync);
  GIO_RESOLVE(handle, dbus_connection_call_sync, g_dbus_connection_call_sync);
  GIO_RESOLVE(handle, dbus_proxy_new_sync, g_dbus_proxy_new_sync);
  GIO_RESOLVE(handle, dbus_proxy_call_sync, g_dbus_proxy_call_sync);
  GIO_RESOLVE(handle, variant_get, g_variant_get);
  GIO_RESOLVE(handle, variant_iter_next, g_variant_iter_next);
  GIO_RESOLVE(handle, variant_iter_free, g_variant_iter_free);
  GIO_RESOLVE(handle, variant_unref, g_variant_unref);
  GIO_RESOLVE(handle, object_unref, g_object_unref);
  GIO_RESOLVE(handle, error_free, g_error_free);
  GIO_RESOLVE(handle, strdup, g_strdup);
  GIO_RESOLVE(handle, str_has_prefix, g_str_has_prefix);
  GIO_RESOLVE(handle, free, g_free);
  if (!gio.bus_get_sync || !gio.dbus_connection_call_sync ||
      !gio.dbus_proxy_new_sync || !gio.dbus_proxy_call_sync ||
      !gio.variant_get || !gio.variant_iter_next || !gio.variant_iter_free ||
      !gio.variant_unref || !gio.object_unref || !gio.error_free ||
      !gio.strdup || !gio.str_has_prefix || !gio.free) {
    printf("volume: gio symbols missing: %s\n",
           sk_library_error() ? sk_library_error() : "null");
    sk_library_close(handle);
    memset(&gio, 0, sizeof(gio));
    return false;
  }
  gio.handle = handle;
  gio.ready = true;
  return true;
}

static char *find_player_bus_name(GlDbConnection *conn) {
  GlVariant *reply = gio.dbus_connection_call_sync(
      conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "ListNames", NULL, NULL, VOLUME_G_DBUS_FLAGS_NONE,
      -1, NULL, NULL);
  char *player_name = NULL;
  if (reply) {
    GlVariantIter *iter = NULL;
    const char *name = NULL;
    gio.variant_get(reply, "(as)", &iter);
    while (gio.variant_iter_next(iter, "&s", &name)) {
      if (gio.str_has_prefix(name, "org.mpris.MediaPlayer2.")) {
        player_name = gio.strdup(name);
        break;
      }
    }
    gio.variant_iter_free(iter);
    gio.variant_unref(reply);
  }
  return player_name;
}

static void pulse_sink(void) {
  if (getenv("XDG_RUNTIME_DIR"))
    return;
  char runtime_dir[64];
  snprintf(runtime_dir, sizeof(runtime_dir), "/run/user/%d", (int)getuid());
  setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
}

static int parse_percent(const char *text) {
  if (!text || !text[0])
    return -1;
  const char *p = text;
  while (*p) {
    if (*p >= '0' && *p <= '9') {
      int value = 0;
      while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        p++;
      }
      if (*p == '%')
        return value;
    } else {
      p++;
    }
  }
  return -1;
}

static int parse_fraction(const char *text) {
  if (!text || !text[0])
    return -1;
  const char *slash = strchr(text, '/');
  if (!slash)
    return -1;
  const char *p = slash + 1;
  while (*p == ' ')
    p++;
  if (*p >= '0' && *p <= '9') {
    int value = 0;
    while (*p >= '0' && *p <= '9') {
      value = value * 10 + (*p - '0');
      p++;
    }
    while (*p == ' ')
      p++;
    if (*p == '%')
      return value;
  }
  return -1;
}

static int run_parse(const char *cmd) {
  char *output = sk_run_terminal_command_blocking((char *)cmd);
  if (!output)
    return -1;
  int volume = parse_fraction(output);
  if (volume < 0)
    volume = parse_percent(output);
  free(output);
  return volume;
}
#endif

static int get_current_volume(void) {
#if defined(_WIN32)
  HRESULT hr;
  IMMDeviceEnumerator *pEnumerator = NULL;
  IMMDevice *pDevice = NULL;
  IAudioEndpointVolume *pVolume = NULL;
  float currentVolume = 0.0f;

  hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  if (hr == RPC_E_CHANGED_MODE) {
  } else if (FAILED(hr)) {
    return 0;
  }

  hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                        &IID_IMMDeviceEnumerator, (void **)&pEnumerator);
  if (FAILED(hr))
    goto cleanup;

  hr = pEnumerator->lpVtbl->GetDefaultAudioEndpoint(pEnumerator, eRender,
                                                    eConsole, &pDevice);
  if (FAILED(hr))
    goto cleanup;

  hr = pDevice->lpVtbl->Activate(pDevice, &skald_IID_IAudioEndpointVolume,
                                 CLSCTX_ALL, NULL, (void **)&pVolume);
  if (FAILED(hr))
    goto cleanup;

  pVolume->lpVtbl->GetMasterVolumeLevelScalar(pVolume, &currentVolume);

cleanup:
  if (pVolume)
    pVolume->lpVtbl->Release(pVolume);
  if (pDevice)
    pDevice->lpVtbl->Release(pDevice);
  if (pEnumerator)
    pEnumerator->lpVtbl->Release(pEnumerator);
  CoUninitialize();
  return (int)(currentVolume * 100.0f + 0.5f);
#elif defined(__linux__)
  pulse_sink();
  int volume = run_parse("pactl get-sink-volume @DEFAULT_SINK@");
  if (volume < 0)
    volume = run_parse("wpctl get-volume @DEFAULT_AUDIO_SINK@");
  return volume < 0 ? 0 : volume;
#endif
}

static void set_volume(int volume) {
#if defined(_WIN32)
  HRESULT hr;
  IMMDeviceEnumerator *pEnumerator = NULL;
  IMMDevice *pDevice = NULL;
  IAudioEndpointVolume *pVolume = NULL;

  hr = CoInitialize(NULL);
  if (FAILED(hr))
    return;

  hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                        &IID_IMMDeviceEnumerator, (void **)&pEnumerator);
  if (FAILED(hr))
    goto cleanup;

  hr = pEnumerator->lpVtbl->GetDefaultAudioEndpoint(pEnumerator, eRender,
                                                    eConsole, &pDevice);
  if (FAILED(hr))
    goto cleanup;

  hr = pDevice->lpVtbl->Activate(pDevice, &skald_IID_IAudioEndpointVolume,
                                 CLSCTX_ALL, NULL, (void **)&pVolume);
  if (FAILED(hr))
    goto cleanup;

  float volume_float = (float)volume / 100.0f;

  pVolume->lpVtbl->SetMasterVolumeLevelScalar(pVolume, volume_float, NULL);

cleanup:
  if (pVolume)
    pVolume->lpVtbl->Release(pVolume);
  if (pDevice)
    pDevice->lpVtbl->Release(pDevice);
  if (pEnumerator)
    pEnumerator->lpVtbl->Release(pEnumerator);
  CoUninitialize();
#elif defined(__linux__)
  pulse_sink();
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "pactl set-sink-volume @DEFAULT_SINK@ %d%%",
           volume);
  char *out = sk_run_terminal_command_blocking(cmd);
  bool ok = out && !out[0];
  if (out)
    free(out);
  if (!ok) {
    snprintf(cmd, sizeof(cmd), "wpctl set-volume @DEFAULT_AUDIO_SINK@ %d%%",
             volume);
    out = sk_run_terminal_command_blocking(cmd);
    if (out)
      free(out);
  }
  if (volume > 0) {
    out = sk_run_terminal_command_blocking(
        "pactl set-sink-mute @DEFAULT_SINK@ 0");
    if (out)
      free(out);
  }
#endif
  return;
}

#define VK_MEDIA_PLAY_PAUSE 0xB3

void toggle_play_pause(void) {
#if defined(_WIN32)
  INPUT input[2] = {0};

  input[0].type = INPUT_KEYBOARD;
  input[0].ki.wVk = VK_MEDIA_PLAY_PAUSE;

  input[1].type = INPUT_KEYBOARD;
  input[1].ki.wVk = VK_MEDIA_PLAY_PAUSE;
  input[1].ki.dwFlags = KEYEVENTF_KEYUP;

  SendInput(2, input, sizeof(INPUT));
#elif defined(__linux__)
  if (!gio_load())
    return;
  GlError *error = NULL;
  GlDbConnection *conn =
      gio.bus_get_sync(VOLUME_G_BUS_TYPE_SESSION, NULL, &error);
  if (!conn) {
    if (error)
      gio.error_free(error);
    return;
  }

  char *bus_name = find_player_bus_name(conn);
  if (!bus_name) {
    gio.object_unref(conn);
    return;
  }

  GlDbProxy *proxy = gio.dbus_proxy_new_sync(
      conn, VOLUME_G_DBUS_FLAGS_NONE, NULL, bus_name, "/org/mpris/MediaPlayer2",
      "org.mpris.MediaPlayer2.Player", NULL, &error);

  if (proxy) {
    GlVariant *result = gio.dbus_proxy_call_sync(
        proxy, "PlayPause", NULL, VOLUME_G_DBUS_FLAGS_NONE, -1, NULL, &error);
    if (result)
      gio.variant_unref(result);
    gio.object_unref(proxy);
  }

  if (error)
    gio.error_free(error);
  gio.free(bus_name);
  gio.object_unref(conn);
#endif
}

static void increment_volume() { set_volume(get_current_volume() + 10); }

static void decrement_volume() { set_volume(get_current_volume() - 10); }
static void set_volume_to_25() { set_volume(25); }
static void set_volume_to_50() { set_volume(50); }
static void set_volume_to_75() { set_volume(75); }
static void set_volume_to_100() { set_volume(100); }
static void set_volume_to_0() { set_volume(0); }

SKALD_FEATURE_INIT {
  sk_add_to_cc(.type = "Volume Control",
               .display_name = "Increment Volume By 10",
               .on_action = increment_volume, .icon = "/volume.png");
  sk_add_to_cc(.type = "Volume Control",
               .display_name = "Decrement Volume By 10",
               .on_action = decrement_volume, .icon = "/volume.png");
  sk_add_to_cc(.type = "Volume Control", .display_name = "Set Volume To 0%",
               .on_action = set_volume_to_0, .icon = "/volume.png");
  sk_add_to_cc(.type = "Volume Control", .display_name = "Set Volume To 25%",
               .on_action = set_volume_to_25, .icon = "/volume.png");
  sk_add_to_cc(.type = "Volume Control", .display_name = "Set Volume To 50%",
               .on_action = set_volume_to_50, .icon = "/volume.png");
  sk_add_to_cc(.type = "Volume Control", .display_name = "Set Volume To 75%",
               .on_action = set_volume_to_75, .icon = "/volume.png");
  sk_add_to_cc(.type = "Volume Control", .display_name = "Set Volume To 100%",
               .on_action = set_volume_to_100, .icon = "/volume.png");
  sk_add_to_cc(.type = "Volume Control", .display_name = "Toggle Play/Pause",
               .on_action = toggle_play_pause, .icon = "/volume.png");
  sk_cap_register("toggle.playpause", toggle_play_pause);
}
