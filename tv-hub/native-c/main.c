#define _GNU_SOURCE
#include "tizen_ffi.h"
#include "renderer.h"
#include "../media-c/media_os.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  LIVING ROOM HUB — TIER 4 BARE-METAL ARMv7 C WAYLAND + MALI GLES2 + ASTC
 *  + DYNAMIC BUTTON MAPPING SERVICE (tizen_keyrouter OverrideExclusive)
 *  + AUTO SMART MEMORY GUARD + EGL_KHR_partial_update
 * ═══════════════════════════════════════════════════════════════════════════ */

#define WIN_W 1920
#define WIN_H 1080
#define HTTP_PORT 18080

#define PLEX_APP_ID       "kIciSQlYEM.plex"
#define YOUTUBE_APP_ID    "9Ur5IzDKqV.TizenYouTube"
#define COBALT_YT_APP_ID  "com.samsung.tv.cobalt-yt"
#define SETTINGS_APP_ID   "org.tizen.menu"
#define SAMSUNG_HOME_ID   "com.samsung.tv.csfs"
#define PLEX_HOST         ""
#define PLEX_PORT         32400
#define SWITCH2_SRC_TYPE  13
#define LOW_RAM_MIB       340

#define COMPANION_PID_FILE "/run/user/5001/codex-frozen-companions.pids"
#define MAX_KEYBINDS       32

/* Evdev keycodes delivered by wl_keyboard::key (XKB = evdev + 8) */
#define EV_ESC        1
#define EV_1          2
#define EV_2          3
#define EV_3          4
#define EV_0          11
#define EV_BACKSPACE  14
#define EV_ENTER      28
#define EV_SPACE      57
#define EV_XF86HOME   63   /* XKB 71  */
#define EV_KPENTER    96
#define EV_HOME       102
#define EV_UP         103
#define EV_LEFT       105
#define EV_RIGHT      106
#define EV_DOWN       108
#define EV_MUTE       113
#define EV_VOLDOWN    114
#define EV_VOLUP      115
#define EV_POWER      116
#define EV_MENU       139
#define EV_BACK       158
#define EV_HOMEPAGE   172  /* XKB 180 */
#define EV_OK         352
#define EV_SELECT     353
#define EV_RED        398
#define EV_GREEN      399
#define EV_YELLOW     400
#define EV_BLUE       401

typedef enum { ZONE_HERO = 0, ZONE_DOCK = 1, ZONE_MODAL = 2, ZONE_VPN_MODAL = 3 } NavZone;

typedef struct {
    bool available;
    char status[24];          /* disconnected|connecting|connected|disconnecting|error|unavailable */
    bool tunnel_established;  /* ONLY true when verified active TUN + exitIp + current bootId */
    bool tun_present;
    bool diverged;            /* selectedLocation != activeLocation */
    char selected_id[16];
    char selected_name[48];
    char auth_id[16];
    char auth_name[48];
    char active_id[16];
    char active_name[48];
    char exit_ip[48];
    char colo[16];
    int  rss_kb;
    int  hwm_kb;
    char err_code[32];
    char err_msg[96];
} VpnStatus;

typedef struct {
    int  source_type;
    char port_label[24];
    char name[64];
    char unique_key[64];
    bool connected;
    int  power_state;
} SourceInfo;

typedef struct {
    char ip[40];
    int  mem_avail_mib;
    int  mem_total_mib;
    int  volume;
    bool muted;
    int  active_source;
    int  signal_status;
    bool plex_running;
    bool plex_online;
    int  plex_latency_ms;
    char plex_version[40];
    bool yt_running;
    bool sw_connected;
    int  sw_power;
    int  sw_cec_power;
    bool sw_active;
    char sw_key[64];
    bool airplay_running;
    bool dmr_running;
    bool allshare_running;
    SourceInfo sources[4];
    VpnStatus vpn;
} HubSnapshot;

typedef struct {
    uint32_t xkb;          /* XKB keycode grabbed via tizen_keyrouter (= evdev + 8) */
    uint32_t evdev;        /* Evdev keycode received in wl_keyboard::key */
    char action[48];       /* e.g. launch_plex, app, source, script, service_toggle */
    char arg[128];         /* app_id, sourceType, script name, unit name */
    char label[64];        /* human-friendly description */
    bool grabbed;          /* currently grabbed in tizen_keyrouter */
} KeyBind;

typedef struct {
    int x, y, w, h;
} RectI;

typedef struct {
    char room_title[48];
    char plex_host[64];
    int  plex_port;
    char gateway_url[128];
    bool media_os_enabled;
    char hdmi1_name[48];
    char hdmi1_port[24];
    int  hdmi1_source_type;
    bool hdmi1_is_switch2;
} HubConfig;

/* ── Global application state ── */
static char g_base_dir[256] = "/opt/usr/etc/codex-ui";
static char g_asset_dir[300];
static char g_state_file[300];
static char g_keybinds_file[300];
static char g_actions_dir[300];
static char g_config_file[300];
static HubConfig g_cfg = {
    .room_title = "LIVING ROOM",
    .plex_host = PLEX_HOST,
    .plex_port = PLEX_PORT,
    .gateway_url = "",
    .media_os_enabled = false,
    .hdmi1_name = "HDMI 1",
    .hdmi1_port = "HDMI 1",
    .hdmi1_source_type = SWITCH2_SRC_TYPE,
    .hdmi1_is_switch2 = false,
};

static volatile sig_atomic_t g_running = 1;
static int g_wake_efd = -1;
static int g_work_efd = -1;

/* Wayland & EGL handles (main thread only) */
static struct wl_display       *g_wl_dpy = NULL;
static struct wl_registry      *g_wl_reg = NULL;
static struct wl_compositor    *g_wl_comp = NULL;
static struct wl_shell         *g_wl_shell = NULL;
static struct wl_seat          *g_wl_seat = NULL;
static struct wl_keyboard      *g_wl_kbd = NULL;
static struct wl_surface       *g_wl_surf = NULL;
static struct wl_shell_surface *g_wl_ssurf = NULL;
static struct wl_proxy         *g_tz_policy = NULL;
static struct wl_proxy         *g_tz_keyrouter = NULL;
static struct wl_proxy         *g_tz_surface = NULL;
static struct wl_proxy         *g_tz_resource = NULL;
static uint32_t                 g_wl_surface_id = 0;
static struct wl_egl_window    *g_egl_win = NULL;

static EGLDisplay g_egl_dpy = EGL_NO_DISPLAY;
static EGLContext g_egl_ctx = EGL_NO_CONTEXT;
static EGLSurface g_egl_surf = EGL_NO_SURFACE;
static Renderer   g_ren;
static char       g_gl_renderer[128] = "unknown";
static bool       g_has_partial_update = false;
static bool       g_home_grab_active = false;

/* UI state (main thread owns rendering; mutex protects cross-thread telemetry/cmds) */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static HubSnapshot     g_snap;

static NavZone g_zone = ZONE_HERO;
static int     g_hero_idx = 0;
static int     g_dock_idx = 0;
static int     g_modal_idx = 0;
static int     g_vpn_modal_idx = 0;
static char    g_last_selected[16] = "plex";
static char    g_last_action[96] = "initialized";
static char    g_last_exec[128] = "";
static char    g_last_key[48] = "";

static bool    g_visible = true;
static bool    g_hot_sleeping = false;
static bool    g_needs_render = true;
static int     g_full_redraws = 3;
static RectI   g_dirty = {0, 0, WIN_W, WIN_H};
static bool    g_dirty_valid = false;
static int     g_draw_calls_last = 0;

/* Animations (smooth 95 ms cubic-bezier ease-out for Hero cards) */
static float   g_card_scale[3] = {1.032f, 0.985f, 0.985f};
static float   g_card_yoff[3]  = {-14.0f, 0.0f, 0.0f};
static float   g_card_s0[3], g_card_s1[3];
static float   g_card_y0[3], g_card_y1[3];
static int64_t g_anim_start_us = 0;
static bool    g_animating = false;

/* Toast & timed triggers (all monotonic microseconds on main thread) */
static char    g_toast_msg[128] = "";
static int64_t g_toast_until_us = 0;
static int64_t g_hide_at_us = 0;
static bool    g_hide_force_cold = false;
static int64_t g_wake_refresh_at_us = 0;
static int64_t g_hdmi_guard_at_us = 0;
static int64_t g_next_poll_us = 0;
static int64_t g_vol_poll_at_us = 0;
static int64_t g_suppress_home_until_us = 0;
static int64_t g_last_home_ev_us = 0;

/* Auto Smart Memory Guard telemetry */
static int     g_auto_guard_trims = 0;
static char    g_last_guard_log[96] = "armed";

/* Performance telemetry */
static double  g_last_key_handler_us = 0.0;
static double  g_last_key_swap_ms = 0.0;
static double  g_sum_swap_ms = 0.0;
static int     g_swap_samples = 0;
static int64_t g_pending_key_ts_us = 0;

/* Dynamic Button Mapping Service state */
static KeyBind  g_binds[MAX_KEYBINDS];
static int      g_bind_count = 0;
static bool     g_binds_dirty = false;
static uint32_t g_ungrab_queue[MAX_KEYBINDS];
static int      g_ungrab_count = 0;
static int64_t  g_learn_until_us = 0;
static uint32_t g_last_raw_evdev = 0;
static uint32_t g_last_raw_xkb = 0;
static uint32_t g_last_learned_evdev = 0;
static uint32_t g_last_learned_xkb = 0;

/* Pending command queue from HTTP thread -> main thread */
typedef enum {
    CMD_NONE = 0,
    CMD_KEY_NAME,
    CMD_ACTION,
    CMD_SYNC_BINDS,
    CMD_SNAP_UPDATED,
    CMD_TOAST,
    CMD_MEDIA_WAKE
} PendingCmdType;

typedef struct {
    PendingCmdType type;
    char a[64];
    char b[128];
} PendingCmd;

#define CMD_Q_CAP 32
static PendingCmd g_cmd_q[CMD_Q_CAP];
static int        g_cmd_head = 0, g_cmd_tail = 0;

/* Pending async work queue from main thread -> worker thread */
typedef struct {
    char action[64];
    char arg[128];
} WorkItem;

#define WORK_Q_CAP 32
static WorkItem g_work_q[WORK_Q_CAP];
static int      g_work_head = 0, g_work_tail = 0;
static bool     g_refresh_requested = false;

#define DOCK_COUNT 7
static const char *HERO_IDS[3] = { "plex", "youtube", "switch2" };
static const char *DOCK_ACTIONS[DOCK_COUNT] = {
    "open_inputs", "open_vpn", "volume_down", "toggle_mute", "volume_up",
    "clean_background", "open_settings"
};

/* ── Time & logging helpers ── */
static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000LL);
}

static void hub_log(const char *fmt, ...) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(stderr, "[%02d:%02d:%02d.%03ld] ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void signal_main(void) {
    uint64_t v = 1;
    if (g_wake_efd >= 0) (void)write(g_wake_efd, &v, sizeof(v));
}

static void signal_worker(void) {
    uint64_t v = 1;
    if (g_work_efd >= 0) (void)write(g_work_efd, &v, sizeof(v));
}

static void push_cmd_locked(PendingCmdType t, const char *a, const char *b) {
    int next = (g_cmd_tail + 1) % CMD_Q_CAP;
    if (next == g_cmd_head) return;
    g_cmd_q[g_cmd_tail].type = t;
    snprintf(g_cmd_q[g_cmd_tail].a, sizeof(g_cmd_q[g_cmd_tail].a), "%s", a ? a : "");
    snprintf(g_cmd_q[g_cmd_tail].b, sizeof(g_cmd_q[g_cmd_tail].b), "%s", b ? b : "");
    g_cmd_tail = next;
    signal_main();
}

static void push_work(const char *action, const char *arg) {
    pthread_mutex_lock(&g_lock);
    int next = (g_work_tail + 1) % WORK_Q_CAP;
    if (next != g_work_head) {
        snprintf(g_work_q[g_work_tail].action, sizeof(g_work_q[g_work_tail].action), "%s", action ? action : "");
        snprintf(g_work_q[g_work_tail].arg, sizeof(g_work_q[g_work_tail].arg), "%s", arg ? arg : "");
        g_work_tail = next;
    }
    pthread_mutex_unlock(&g_lock);
    signal_worker();
}

static void request_refresh(void) {
    pthread_mutex_lock(&g_lock);
    g_refresh_requested = true;
    pthread_mutex_unlock(&g_lock);
    signal_worker();
}

/* ── Dirty-rect helpers for Mali TBDR partial update ── */
static void mark_full_redraw(int frames) {
    if (frames > g_full_redraws) g_full_redraws = frames;
    g_dirty.x = 0; g_dirty.y = 0; g_dirty.w = WIN_W; g_dirty.h = WIN_H;
    g_dirty_valid = true;
    g_needs_render = true;
}

static void mark_dirty_rect(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > WIN_W) w = WIN_W - x;
    if (y + h > WIN_H) h = WIN_H - y;
    if (w <= 0 || h <= 0) return;
    /* Align outward to 16x16 Mali tile boundaries */
    int x0 = (x & ~15);
    int y0 = (y & ~15);
    int x1 = ((x + w + 15) & ~15);
    int y1 = ((y + h + 15) & ~15);
    if (x1 > WIN_W) x1 = WIN_W;
    if (y1 > WIN_H) y1 = WIN_H;
    if (!g_dirty_valid) {
        g_dirty.x = x0; g_dirty.y = y0; g_dirty.w = x1 - x0; g_dirty.h = y1 - y0;
        g_dirty_valid = true;
    } else {
        int ux0 = g_dirty.x < x0 ? g_dirty.x : x0;
        int uy0 = g_dirty.y < y0 ? g_dirty.y : y0;
        int ux1 = (g_dirty.x + g_dirty.w) > x1 ? (g_dirty.x + g_dirty.w) : x1;
        int uy1 = (g_dirty.y + g_dirty.h) > y1 ? (g_dirty.y + g_dirty.h) : y1;
        g_dirty.x = ux0; g_dirty.y = uy0; g_dirty.w = ux1 - ux0; g_dirty.h = uy1 - uy0;
    }
    g_needs_render = true;
}

static void show_toast(const char *msg, int dur_ms) {
    snprintf(g_toast_msg, sizeof(g_toast_msg), "%s", msg ? msg : "");
    g_toast_until_us = now_us() + (int64_t)dur_ms * 1000LL;
    mark_full_redraw(3);
}

/* ── Persistence: last selected card & dynamic keybinds ── */
static void load_last_selected(void) {
    FILE *f = fopen(g_state_file, "r");
    if (!f) return;
    char buf[32] = {0};
    if (fgets(buf, sizeof(buf), f)) {
        size_t n = strcspn(buf, "\r\n \t");
        buf[n] = 0;
        for (int i = 0; i < 3; i++) {
            if (strcmp(buf, HERO_IDS[i]) == 0) {
                g_hero_idx = i;
                snprintf(g_last_selected, sizeof(g_last_selected), "%s", HERO_IDS[i]);
                break;
            }
        }
    }
    fclose(f);
}

static void save_last_selected(const char *id) {
    snprintf(g_last_selected, sizeof(g_last_selected), "%s", id);
    FILE *f = fopen(g_state_file, "w");
    if (f) { fputs(id, f); fclose(f); }
}

static bool is_protected_evdev(uint32_t ev, bool allow_soft_override) {
    if (ev == EV_POWER || ev == EV_XF86HOME || ev == EV_HOMEPAGE || ev == EV_HOME) return true;
    if (!allow_soft_override) {
        if (ev == EV_UP || ev == EV_DOWN || ev == EV_LEFT || ev == EV_RIGHT ||
            ev == EV_ENTER || ev == EV_OK || ev == EV_SELECT || ev == EV_ESC || ev == EV_BACK ||
            ev == EV_VOLUP || ev == EV_VOLDOWN || ev == EV_MUTE || ev == 67 || ev == 68)
            return true;
    }
    return false;
}

static void keybinds_load_locked(void) {
    for (int i = 0; i < g_bind_count; i++) {
        if (g_binds[i].grabbed && g_ungrab_count < MAX_KEYBINDS)
            g_ungrab_queue[g_ungrab_count++] = g_binds[i].xkb;
    }
    g_bind_count = 0;
    FILE *f = fopen(g_keybinds_file, "r");
    if (!f) { g_binds_dirty = true; return; }
    char line[320];
    while (fgets(line, sizeof(line), f) && g_bind_count < MAX_KEYBINDS) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\r' || *s == 0) continue;
        size_t len = strcspn(s, "\r\n");
        s[len] = 0;
        /* Format: xkb|action|arg|label */
        char *p1 = strchr(s, '|');
        if (!p1) continue;
        *p1++ = 0;
        char *p2 = strchr(p1, '|');
        char *p3 = NULL;
        if (p2) { *p2++ = 0; p3 = strchr(p2, '|'); if (p3) *p3++ = 0; }
        uint32_t xkb = (uint32_t)strtoul(s, NULL, 10);
        if (xkb < 9 || xkb > 1000) continue;
        uint32_t ev = xkb - 8;
        if (is_protected_evdev(ev, true)) continue;
        KeyBind *kb = &g_binds[g_bind_count++];
        memset(kb, 0, sizeof(*kb));
        kb->xkb = xkb;
        kb->evdev = ev;
        snprintf(kb->action, sizeof(kb->action), "%s", p1);
        snprintf(kb->arg, sizeof(kb->arg), "%s", p2 ? p2 : "");
        snprintf(kb->label, sizeof(kb->label), "%s", p3 ? p3 : "");
        kb->grabbed = false;
    }
    fclose(f);
    g_binds_dirty = true;
}

static bool keybinds_save_locked(void) {
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_keybinds_file);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f, "# Living Room Hub dynamic button map: xkb_keycode|action|arg|label\n");
    fprintf(f, "# Actions: show_hub, launch_plex, launch_youtube, switch_switch2, open_inputs,\n");
    fprintf(f, "#          open_settings, open_samsung_home, clean_background, volume_up, volume_down,\n");
    fprintf(f, "#          toggle_mute, app|<app_id>, source|<sourceType>, script|<name>, service_toggle|<unit>\n");
    for (int i = 0; i < g_bind_count; i++) {
        fprintf(f, "%u|%s|%s|%s\n", g_binds[i].xkb, g_binds[i].action, g_binds[i].arg, g_binds[i].label);
    }
    fclose(f);
    return rename(tmp, g_keybinds_file) == 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TIZEN SYSTEM APIS & BACKGROUND WORKER (Plex, HDMI, CEC, Audio, Auto-Guard)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int map_ep_source(int ep) {
    switch (ep) {
        case 0:  return 0;
        case 2:  return 13;
        case 3:  return 14;
        case 11: return 15;
        case 12: return 16;
        default: return ep;
    }
}

static const char *source_label(int st) {
    switch (st) {
        case 0:  return "Live TV";
        case 13: return "HDMI 1";
        case 14: return "HDMI 2";
        case 15: return "HDMI 3";
        case 16: return "HDMI 4";
        default: return "Input";
    }
}

static bool safe_is_running(const char *app_id) {
    if (!G.app_manager_is_running) return false;
    bool r = false;
    return (G.app_manager_is_running(app_id, &r) == 0) ? r : false;
}

static bool safe_launch_app(const char *app_id) {
    if (!G.app_control_create || !G.app_control_set_app_id || !G.app_control_send_launch_request) return false;
    void *h = NULL;
    if (G.app_control_create(&h) != 0 || !h) return false;
    G.app_control_set_app_id(h, app_id);
    int rc = G.app_control_send_launch_request(h, NULL, NULL);
    if (G.app_control_destroy) G.app_control_destroy(h);
    return rc == 0;
}

static bool safe_terminate_bg(const char *app_id) {
    if (!safe_is_running(app_id)) return false;
    if (!G.app_manager_get_app_context || !G.app_context_destroy) return false;
    void *ctx = NULL;
    if (G.app_manager_get_app_context(app_id, &ctx) != 0 || !ctx) return false;
    bool ok = false;
    if (G.app_manager_request_terminate_bg_app && G.app_manager_request_terminate_bg_app(ctx) == 0) ok = true;
    else if (G.app_manager_terminate_app_without_restarting && G.app_manager_terminate_app_without_restarting(ctx) == 0) ok = true;
    G.app_context_destroy(ctx);
    return ok;
}

static void resume_frozen_companions(void) {
    FILE *f = fopen(COMPANION_PID_FILE, "r");
    if (!f) return;
    char line[64];
    int woke = 0;
    while (fgets(line, sizeof(line), f)) {
        pid_t pid = (pid_t)atoi(line);
        if (pid > 1 && kill(pid, SIGCONT) == 0) woke++;
    }
    fclose(f);
    if (woke > 0) hub_log("Resumed %d frozen Samsung Home companions (SIGCONT)", woke);
}

static void read_mem_mib(int *avail, int *total) {
    *avail = 0; *total = 1583;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        unsigned long kb = 0;
        if (strncmp(line, "MemAvailable:", 13) == 0 && sscanf(line + 13, "%lu", &kb) == 1) *avail = (int)(kb / 1024UL);
        else if (strncmp(line, "MemTotal:", 9) == 0 && sscanf(line + 9, "%lu", &kb) == 1) *total = (int)(kb / 1024UL);
    }
    fclose(f);
}

static void get_local_ip(char *out, size_t cap) {
    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) != 0) return;
    for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;
        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        char buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
            snprintf(out, cap, "%s", buf);
            break;
        }
    }
    freeifaddrs(ifaddr);
}

static void trim_ws(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

/* Optional per-property runtime config (<base_dir>/hub.conf).
 * Absent file preserves exact original defaults (zero regression). */
static void load_hub_config(void) {
    FILE *f = fopen(g_config_file, "r");
    if (!f) return;
    bool media_os_explicit = false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim_ws(k);
        trim_ws(v);
        if (strcmp(k, "room_title") == 0 && v[0]) {
            snprintf(g_cfg.room_title, sizeof(g_cfg.room_title), "%s", v);
        } else if (strcmp(k, "plex_host") == 0) {
            snprintf(g_cfg.plex_host, sizeof(g_cfg.plex_host), "%s", v);
        } else if (strcmp(k, "plex_port") == 0 && v[0]) {
            int p = atoi(v);
            if (p > 0 && p <= 65535) g_cfg.plex_port = p;
        } else if (strcmp(k, "gateway_url") == 0) {
            snprintf(g_cfg.gateway_url, sizeof(g_cfg.gateway_url), "%s", v);
            if (!media_os_explicit) g_cfg.media_os_enabled = (v[0] != 0);
        } else if (strcmp(k, "media_os_enabled") == 0 && v[0]) {
            media_os_explicit = true;
            g_cfg.media_os_enabled = (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y');
        } else if (strcmp(k, "hdmi1_name") == 0 && v[0]) {
            snprintf(g_cfg.hdmi1_name, sizeof(g_cfg.hdmi1_name), "%s", v);
        } else if (strcmp(k, "hdmi1_port") == 0 && v[0]) {
            snprintf(g_cfg.hdmi1_port, sizeof(g_cfg.hdmi1_port), "%s", v);
        } else if (strcmp(k, "hdmi1_source_type") == 0 && v[0]) {
            int st = atoi(v);
            if (st >= 0 && st <= 32) g_cfg.hdmi1_source_type = st;
        }
    }
    fclose(f);
    if (!g_cfg.gateway_url[0]) g_cfg.media_os_enabled = false;
    g_cfg.hdmi1_is_switch2 = (strstr(g_cfg.hdmi1_name, "Switch") != NULL || strstr(g_cfg.hdmi1_name, "switch") != NULL);
    hub_log("Loaded hub.conf: room='%s' plex='%s:%d' gateway='%s' media_os=%d hdmi1='%s'(%s,st=%d)",
            g_cfg.room_title, g_cfg.plex_host, g_cfg.plex_port, g_cfg.gateway_url,
            g_cfg.media_os_enabled ? 1 : 0, g_cfg.hdmi1_name, g_cfg.hdmi1_port, g_cfg.hdmi1_source_type);
}

/* Raw non-blocking POSIX TCP HTTP probe to configured Plex server (ZERO curl/libhttp!) */
static void probe_plex_server(bool *online, int *latency_ms, char *ver_out, size_t ver_cap) {
    *online = false;
    *latency_ms = -1;
    ver_out[0] = 0;
    if (!g_cfg.plex_host[0]) return;

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)g_cfg.plex_port);
    if (inet_pton(AF_INET, g_cfg.plex_host, &sa.sin_addr) != 1) { close(fd); return; }

    int64_t t0 = now_us();
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return; }

    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    if (poll(&pfd, 1, 900) <= 0) { close(fd); return; }
    int err = 0; socklen_t el = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err != 0) { close(fd); return; }

    char req[192];
    snprintf(req, sizeof(req), "GET /identity HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
             g_cfg.plex_host, g_cfg.plex_port);
    (void)send(fd, req, strlen(req), MSG_NOSIGNAL);

    pfd.events = POLLIN;
    char buf[1024];
    int total = 0;
    while (total < (int)sizeof(buf) - 1 && poll(&pfd, 1, 800) > 0) {
        ssize_t n = recv(fd, buf + total, sizeof(buf) - 1 - (size_t)total, 0);
        if (n <= 0) break;
        total += (int)n;
        if (strstr(buf, "</MediaContainer>")) break;
    }
    close(fd);
    buf[total] = 0;

    if (strstr(buf, "200 OK") || strstr(buf, "MediaContainer")) {
        *online = true;
        *latency_ms = (int)((now_us() - t0) / 1000LL);
        const char *mc = strstr(buf, "<MediaContainer");
        const char *vp = mc ? strstr(mc, "version=\"") : NULL;
        if (vp) {
            vp += 9;
            size_t i = 0;
            while (vp[i] && vp[i] != '"' && vp[i] != '-' && i + 1 < ver_cap) {
                ver_out[i] = vp[i];
                i++;
            }
            ver_out[i] = 0;
        }
    }
}

/* Lightweight JSON extractor for get_source_list() objects */
static bool json_get_int(const char *obj, const char *end, const char *key, int *out) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p || p >= end) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p || p >= end) return false;
    p++;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    *out = atoi(p);
    return true;
}

static bool json_get_bool(const char *obj, const char *end, const char *key, bool *out) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p || p >= end) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p || p >= end) return false;
    p++;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '"')) p++;
    if (strncmp(p, "true", 4) == 0 || *p == '1') { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0 || *p == '0') { *out = false; return true; }
    return false;
}

static bool json_get_str(const char *obj, const char *end, const char *key, char *out, size_t cap) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p || p >= end) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p || p >= end) return false;
    p = strchr(p, '"');
    if (!p || p >= end) return false;
    p++;
    size_t i = 0;
    while (p < end && *p && *p != '"' && i + 1 < cap) out[i++] = *p++;
    out[i] = 0;
    return true;
}

/* Read public read-only /run/codex-vpn/status.json (ZERO credentials, verifies current bootId!) */
static bool json_get_subobj(const char *buf, const char *end, const char *key, const char **os, const char **oe) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p || p >= end) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p || p >= end) return false;
    p++;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (p >= end || *p != '{') return false;
    const char *close = strchr(p, '}');
    if (!close || close >= end) return false;
    *os = p; *oe = close + 1;
    return true;
}

static void read_vpn_status(VpnStatus *v) {
    static char cur_boot_id[64] = "";
    if (!cur_boot_id[0]) {
        FILE *bf = fopen("/proc/sys/kernel/random/boot_id", "r");
        if (bf) {
            if (fgets(cur_boot_id, sizeof(cur_boot_id), bf))
                cur_boot_id[strcspn(cur_boot_id, "\r\n \t")] = 0;
            fclose(bf);
        }
    }

    memset(v, 0, sizeof(*v));
    snprintf(v->status, sizeof(v->status), "unavailable");
    v->diverged = true;

    FILE *f = fopen("/run/codex-vpn/status.json", "r");
    if (!f) return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *end = buf + n;

    char file_boot[64] = {0};
    json_get_str(buf, end, "bootId", file_boot, sizeof(file_boot));
    if (cur_boot_id[0] && file_boot[0] && strcmp(cur_boot_id, file_boot) != 0) {
        /* Stale status from an earlier boot: never claim active tunnel */
        snprintf(v->status, sizeof(v->status), "disconnected");
        return;
    }

    v->available = true;
    if (!json_get_str(buf, end, "status", v->status, sizeof(v->status)))
        snprintf(v->status, sizeof(v->status), "disconnected");
    (void)json_get_bool(buf, end, "tunnelEstablished", &v->tunnel_established);
    (void)json_get_bool(buf, end, "tunPresent", &v->tun_present);
    (void)json_get_bool(buf, end, "selectedActiveDiverged", &v->diverged);
    (void)json_get_str(buf, end, "exitIp", v->exit_ip, sizeof(v->exit_ip));
    (void)json_get_str(buf, end, "colo", v->colo, sizeof(v->colo));
    (void)json_get_int(buf, end, "rssKiB", &v->rss_kb);
    (void)json_get_int(buf, end, "hwmKiB", &v->hwm_kb);

    const char *os = NULL, *oe = NULL;
    if (json_get_subobj(buf, end, "selectedLocation", &os, &oe)) {
        json_get_str(os, oe, "id", v->selected_id, sizeof(v->selected_id));
        json_get_str(os, oe, "name", v->selected_name, sizeof(v->selected_name));
    }
    if (json_get_subobj(buf, end, "authenticatedLocation", &os, &oe)) {
        json_get_str(os, oe, "id", v->auth_id, sizeof(v->auth_id));
        json_get_str(os, oe, "name", v->auth_name, sizeof(v->auth_name));
    }
    if (json_get_subobj(buf, end, "activeLocation", &os, &oe)) {
        json_get_str(os, oe, "id", v->active_id, sizeof(v->active_id));
        json_get_str(os, oe, "name", v->active_name, sizeof(v->active_name));
    }
    if (json_get_subobj(buf, end, "lastError", &os, &oe)) {
        json_get_str(os, oe, "code", v->err_code, sizeof(v->err_code));
        json_get_str(os, oe, "message", v->err_msg, sizeof(v->err_msg));
    }

    /* Strict honesty invariant: never infer connected without live TUN + activeLocation + exitIp */
    if (!(v->tunnel_established && v->tun_present && v->active_id[0] && v->exit_ip[0])) {
        v->tunnel_established = false;
        if (strcmp(v->status, "connected") == 0) snprintf(v->status, sizeof(v->status), "disconnected");
    }
}

static bool vpn_status_equal(const VpnStatus *a, const VpnStatus *b) {
    return a->available == b->available &&
           a->tunnel_established == b->tunnel_established &&
           a->diverged == b->diverged &&
           strcmp(a->status, b->status) == 0 &&
           strcmp(a->selected_id, b->selected_id) == 0 &&
           strcmp(a->active_id, b->active_id) == 0 &&
           strcmp(a->exit_ip, b->exit_ip) == 0 &&
           strcmp(a->colo, b->colo) == 0 &&
           strcmp(a->err_code, b->err_code) == 0;
}

static void read_tv_volume_mute(int *vol_out, bool *muted_out) {
    int v = -1;
    bool m = false;
    if (G.sound_manager_get_current_speaker_volume && G.sound_manager_get_current_speaker_volume(&v) == 0 && v >= 0) {
        *vol_out = v;
    } else if (G.sound_manager_get_tv_master_volume && G.sound_manager_get_tv_master_volume(&v) == 0 && v >= 0) {
        *vol_out = v;
    } else if (G.sound_manager_get_volume && G.sound_manager_get_volume(4, &v) == 0) {
        *vol_out = v;
    }

    if (G.sound_manager_get_current_speaker_mute && G.sound_manager_get_current_speaker_mute(&m) == 0) {
        *muted_out = m;
    } else if (G.sound_manager_get_tv_master_mute && G.sound_manager_get_tv_master_mute(&m) == 0) {
        *muted_out = m;
    } else if (G.sound_manager_get_mute && G.sound_manager_get_mute(4, &m) == 0) {
        *muted_out = m;
    }
}

static void collect_snapshot(HubSnapshot *s) {
    static char cached_ip[40] = "";
    static int64_t last_ip_us = 0;

    memset(s, 0, sizeof(*s));
    s->volume = -1; s->active_source = -1; s->signal_status = -1;
    s->plex_latency_ms = -1; s->sw_connected = true; s->sw_power = 2; s->sw_cec_power = -1;

    int def_types[4] = { 13, 14, 15, 0 };
    const char *def_ports[4] = { "HDMI 1", "HDMI 2", "HDMI 3", "TV" };
    const char *def_names[4] = { g_cfg.hdmi1_name, "HDMI 2", "HDMI 3", "Live TV" };
    for (int i = 0; i < 4; i++) {
        s->sources[i].source_type = def_types[i];
        snprintf(s->sources[i].port_label, sizeof(s->sources[i].port_label), "%s", def_ports[i]);
        snprintf(s->sources[i].name, sizeof(s->sources[i].name), "%s", def_names[i]);
    }
    s->sources[0].connected = true;
    s->sources[0].power_state = 2;

    if (G.ep_source_get_source) {
        int ep = -1;
        if (G.ep_source_get_source(&ep, 0, 0) == 0) s->active_source = map_ep_source(ep);
    }
    if (G.ep_source_get_signal_status) {
        int sig = -1;
        if (G.ep_source_get_signal_status(&sig, 0) == 0) s->signal_status = sig;
    }
    if (G.cec_get_device_power_status_by_hdmi_port) {
        int pwr = -1;
        int cec_port = (g_cfg.hdmi1_source_type >= 13 && g_cfg.hdmi1_source_type <= 16)
                       ? (g_cfg.hdmi1_source_type - 12) : 1;
        if (G.cec_get_device_power_status_by_hdmi_port(cec_port, &pwr) == 0) s->sw_cec_power = pwr;
    }

    if (G.get_source_list) {
        char *raw = G.get_source_list();
        if (raw) {
            const char *cur = raw;
            while ((cur = strchr(cur, '{')) != NULL) {
                const char *end = strchr(cur, '}');
                if (!end) break;
                int st = -1, ps = 0;
                bool cs = false;
                if (json_get_int(cur, end, "sourceType", &st)) {
                    if (!json_get_bool(cur, end, "connectionState", &cs))
                        (void)json_get_bool(cur, end, "connectState", &cs);
                    (void)json_get_int(cur, end, "powerState", &ps);
                    for (int i = 0; i < 4; i++) {
                        if (s->sources[i].source_type == st) {
                            s->sources[i].connected = cs;
                            s->sources[i].power_state = ps;
                            char dname[64] = {0}, ukey[64] = {0};
                            json_get_str(cur, end, "deviceName", dname, sizeof(dname));
                            json_get_str(cur, end, "uniqueKey", ukey, sizeof(ukey));
                            if (ukey[0]) snprintf(s->sources[i].unique_key, sizeof(s->sources[i].unique_key), "%s", ukey);
                            if (st != g_cfg.hdmi1_source_type && dname[0]) snprintf(s->sources[i].name, sizeof(s->sources[i].name), "%s", dname);
                        }
                    }
                    if (st == g_cfg.hdmi1_source_type) {
                        s->sw_connected = cs;
                        s->sw_power = ps;
                        json_get_str(cur, end, "uniqueKey", s->sw_key, sizeof(s->sw_key));
                    }
                }
                cur = end + 1;
            }
            free(raw);
        }
    }
    s->sw_active = (s->active_source == g_cfg.hdmi1_source_type);

    probe_plex_server(&s->plex_online, &s->plex_latency_ms, s->plex_version, sizeof(s->plex_version));

    s->plex_running = safe_is_running(PLEX_APP_ID);
    s->yt_running = safe_is_running(YOUTUBE_APP_ID) || safe_is_running(COBALT_YT_APP_ID);
    s->airplay_running = safe_is_running("com.samsung.tv.airplay-app-2") || safe_is_running("com.samsung.tv.airplay-service");
    s->dmr_running = safe_is_running("dmr_service_app");
    s->allshare_running = s->dmr_running || safe_is_running("com.samsung.tv.allshare");

    read_tv_volume_mute(&s->volume, &s->muted);
    read_vpn_status(&s->vpn);

    read_mem_mib(&s->mem_avail_mib, &s->mem_total_mib);

    int64_t t = now_us();
    if (t - last_ip_us > 60000000LL) {
        get_local_ip(cached_ip, sizeof(cached_ip));
        last_ip_us = t;
    }
    snprintf(s->ip, sizeof(s->ip), "%s", cached_ip);
}

static bool is_safe_token(const char *s) {
    if (!s || !*s || strlen(s) > 96) return false;
    if (strstr(s, "..") || strchr(s, '/')) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!(isalnum(*p) || *p == '.' || *p == '_' || *p == '-' || *p == '@' || *p == ':'))
            return false;
    }
    return true;
}

static void run_safe_script(const char *name) {
    if (!is_safe_token(name)) return;
    char full[384];
    snprintf(full, sizeof(full), "%s/%s", g_actions_dir, name);
    if (access(full, X_OK) != 0) {
        hub_log("Custom action script missing or not executable: %s", full);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        execl(full, full, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int st = 0;
        waitpid(pid, &st, WNOHANG);
    }
}

static void run_auto_guard_check(const char *launching_target) {
    int avail = 0, total = 0;
    read_mem_mib(&avail, &total);
    if (avail <= 0 || avail >= LOW_RAM_MIB) return;

    int trimmed = 0;
    if (strcmp(launching_target, "plex") == 0) {
        if (safe_terminate_bg(YOUTUBE_APP_ID)) trimmed++;
        if (safe_terminate_bg(COBALT_YT_APP_ID)) trimmed++;
    } else if (strcmp(launching_target, "youtube") == 0) {
        if (safe_terminate_bg(PLEX_APP_ID)) trimmed++;
    }
    if (trimmed > 0) {
        pthread_mutex_lock(&g_lock);
        g_auto_guard_trims += trimmed;
        snprintf(g_last_guard_log, sizeof(g_last_guard_log), "low-ram(%dMB)->trimmed %d before %s", avail, trimmed, launching_target);
        pthread_mutex_unlock(&g_lock);
        hub_log("Auto Memory Guard: %s", g_last_guard_log);
    }
}

static void execute_work_item(const char *act, const char *arg) {
    char exec_log[128] = "";
    if (strcmp(act, "launch_plex") == 0) {
        run_auto_guard_check("plex");
        bool ok = safe_launch_app(PLEX_APP_ID);
        snprintf(exec_log, sizeof(exec_log), "launch_plex:%s", ok ? "ok" : "err");
    } else if (strcmp(act, "launch_youtube") == 0) {
        run_auto_guard_check("youtube");
        bool ok = safe_launch_app(YOUTUBE_APP_ID);
        snprintf(exec_log, sizeof(exec_log), "launch_youtube:%s", ok ? "ok" : "err");
    } else if (strcmp(act, "switch_switch2") == 0) {
        bool ok = false;
        if (arg && arg[0] && G.connect_source_with_uniquekey) ok = G.connect_source_with_uniquekey(g_cfg.hdmi1_source_type, arg);
        if (!ok && G.connect_source) ok = G.connect_source(g_cfg.hdmi1_source_type);
        snprintf(exec_log, sizeof(exec_log), "switch_switch2:%s", ok ? "ok" : "err");
    } else if (strcmp(act, "source") == 0) {
        int st = atoi(arg ? arg : "13");
        bool ok = G.connect_source ? G.connect_source(st) : false;
        snprintf(exec_log, sizeof(exec_log), "source(%d):%s", st, ok ? "ok" : "err");
    } else if (strcmp(act, "app") == 0 && arg && arg[0]) {
        bool ok = safe_launch_app(arg);
        snprintf(exec_log, sizeof(exec_log), "app(%s):%s", arg, ok ? "ok" : "err");
    } else if (strcmp(act, "open_settings") == 0) {
        bool ok = safe_launch_app(SETTINGS_APP_ID);
        snprintf(exec_log, sizeof(exec_log), "open_settings:%s", ok ? "ok" : "err");
    } else if (strcmp(act, "open_samsung_home") == 0) {
        resume_frozen_companions();
        bool ok = safe_launch_app(SAMSUNG_HOME_ID);
        snprintf(exec_log, sizeof(exec_log), "open_samsung_home:%s", ok ? "ok" : "err");
    } else if (strcmp(act, "clean_background") == 0) {
        int n = 0;
        if (safe_terminate_bg(PLEX_APP_ID)) n++;
        if (safe_terminate_bg(YOUTUBE_APP_ID)) n++;
        if (safe_terminate_bg(COBALT_YT_APP_ID)) n++;
        snprintf(exec_log, sizeof(exec_log), "clean_bg:stopped=%d", n);
    } else if (strcmp(act, "hdmi_guard") == 0) {
        int ep = -1;
        if (G.ep_source_get_source && G.ep_source_get_source(&ep, 0, 0) == 0 && map_ep_source(ep) == g_cfg.hdmi1_source_type) {
            int n = 0;
            if (safe_terminate_bg(PLEX_APP_ID)) n++;
            if (safe_terminate_bg(YOUTUBE_APP_ID)) n++;
            if (safe_terminate_bg(COBALT_YT_APP_ID)) n++;
            if (n > 0) {
                pthread_mutex_lock(&g_lock);
                g_auto_guard_trims += n;
                snprintf(g_last_guard_log, sizeof(g_last_guard_log), "hdmi1-45s->trimmed %d", n);
                pthread_mutex_unlock(&g_lock);
                hub_log("Auto Memory Guard: %s", g_last_guard_log);
            }
        }
    } else if (strcmp(act, "volume_up") == 0 || strcmp(act, "volume_down") == 0) {
        int delta = (strcmp(act, "volume_up") == 0) ? 2 : -2;
        int cur = -1; bool m = false;
        read_tv_volume_mute(&cur, &m);
        if (cur >= 0) {
            int next = cur + delta;
            if (next < 0) next = 0;
            if (next > 100) next = 100;
            bool ok = false;
            if (G.sound_manager_set_current_speaker_volume && G.sound_manager_set_current_speaker_volume(next) == 0) ok = true;
            else if (G.sound_manager_set_tv_master_volume && G.sound_manager_set_tv_master_volume(next) == 0) ok = true;
            else if (G.sound_manager_set_volume && G.sound_manager_set_volume(4, next) == 0) ok = true;
            if (ok) {
                pthread_mutex_lock(&g_lock);
                g_snap.volume = next;
                push_cmd_locked(CMD_SNAP_UPDATED, "", "");
                pthread_mutex_unlock(&g_lock);
            }
            snprintf(exec_log, sizeof(exec_log), "%s:%s(%d->%d)", act, ok ? "ok" : "err", cur, next);
        } else {
            snprintf(exec_log, sizeof(exec_log), "%s:no_cur", act);
        }
    } else if (strcmp(act, "toggle_mute") == 0) {
        int cur = -1; bool m = false;
        read_tv_volume_mute(&cur, &m);
        bool next_m = !m;
        bool ok = false;
        if (G.sound_manager_set_current_speaker_mute && G.sound_manager_set_current_speaker_mute(next_m) == 0) ok = true;
        else if (G.sound_manager_set_tv_master_mute && G.sound_manager_set_tv_master_mute(next_m) == 0) ok = true;
        else if (G.sound_manager_set_mute && G.sound_manager_set_mute(4, next_m) == 0) ok = true;
        if (ok) {
            pthread_mutex_lock(&g_lock);
            g_snap.muted = next_m;
            push_cmd_locked(CMD_SNAP_UPDATED, "", "");
            pthread_mutex_unlock(&g_lock);
        }
        snprintf(exec_log, sizeof(exec_log), "toggle_mute:%s(%d)", ok ? "ok" : "err", next_m ? 1 : 0);
    } else if (strcmp(act, "poll_volume") == 0) {
        int v = -1; bool m = false;
        read_tv_volume_mute(&v, &m);
        VpnStatus vpn_now;
        read_vpn_status(&vpn_now);
        pthread_mutex_lock(&g_lock);
        bool changed = (v >= 0 && v != g_snap.volume) || (m != g_snap.muted) || !vpn_status_equal(&g_snap.vpn, &vpn_now);
        if (v >= 0) g_snap.volume = v;
        g_snap.muted = m;
        g_snap.vpn = vpn_now;
        if (changed) push_cmd_locked(CMD_SNAP_UPDATED, "", "");
        pthread_mutex_unlock(&g_lock);
    } else if (strcmp(act, "script") == 0) {
        run_safe_script(arg);
        snprintf(exec_log, sizeof(exec_log), "script(%s)", arg ? arg : "");
    } else if (strcmp(act, "service_toggle") == 0 && is_safe_token(arg)) {
        run_safe_script("service-toggle.sh");
        snprintf(exec_log, sizeof(exec_log), "service_toggle(%s)", arg);
    }

    if (exec_log[0]) {
        pthread_mutex_lock(&g_lock);
        snprintf(g_last_exec, sizeof(g_last_exec), "%s", exec_log);
        pthread_mutex_unlock(&g_lock);
        hub_log("Work: %s", exec_log);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  WAYLAND POLICY, KEYROUTER & SURFACE SHOW/HIDE
 * ═══════════════════════════════════════════════════════════════════════════ */

static void apply_input_opaque_regions(bool active) {
    if (!g_wl_comp || !g_wl_surf) return;
    struct wl_proxy *reg = G.wl_proxy_marshal_constructor(
        (struct wl_proxy *)g_wl_comp, WL_COMPOSITOR_CREATE_REGION, G.wl_region_iface, NULL);
    if (!reg) return;
    if (active) {
        G.wl_proxy_marshal(reg, WL_REGION_ADD, 0, 0, WIN_W, WIN_H);
    }
    G.wl_proxy_marshal((struct wl_proxy *)g_wl_surf, WL_SURFACE_SET_INPUT_REGION, reg);
    bool opaque = active && !media_os_is_playing();
    G.wl_proxy_marshal((struct wl_proxy *)g_wl_surf, WL_SURFACE_SET_OPAQUE_REGION, opaque ? reg : NULL);
    G.wl_proxy_marshal(reg, WL_REGION_DESTROY);
    G.wl_proxy_destroy(reg);
}

static void set_surface_opaque_only(bool opaque) {
    if (!g_wl_comp || !g_wl_surf) return;
    if (opaque) {
        struct wl_proxy *reg = G.wl_proxy_marshal_constructor(
            (struct wl_proxy *)g_wl_comp, WL_COMPOSITOR_CREATE_REGION, G.wl_region_iface, NULL);
        if (reg) {
            G.wl_proxy_marshal(reg, WL_REGION_ADD, 0, 0, WIN_W, WIN_H);
            G.wl_proxy_marshal((struct wl_proxy *)g_wl_surf, WL_SURFACE_SET_OPAQUE_REGION, reg);
            G.wl_proxy_marshal(reg, WL_REGION_DESTROY);
            G.wl_proxy_destroy(reg);
        }
    } else {
        G.wl_proxy_marshal((struct wl_proxy *)g_wl_surf, WL_SURFACE_SET_OPAQUE_REGION, NULL);
    }
    if (g_tz_policy) {
        G.wl_proxy_marshal(g_tz_policy, TIZEN_POLICY_SET_OPAQUE_STATE, g_wl_surf, opaque ? 1 : 0);
    }
    G.wl_proxy_marshal((struct wl_proxy *)g_wl_surf, WL_SURFACE_COMMIT);
    G.wl_display_flush(g_wl_dpy);
}

static void keyrouter_grab(uint32_t xkb_code, bool grab) {
    if (!g_tz_keyrouter || !g_wl_surf) return;
    if (grab) {
        G.wl_proxy_marshal(g_tz_keyrouter, TIZEN_KEYROUTER_SET_KEYGRAB, g_wl_surf, xkb_code, (uint32_t)TIZEN_KEYGRAB_OVERRIDE_EXCL);
    } else {
        G.wl_proxy_marshal(g_tz_keyrouter, TIZEN_KEYROUTER_UNSET_KEYGRAB, g_wl_surf, xkb_code);
    }
}

static void sync_all_keygrabs(bool force_reassert) {
    if (!g_tz_keyrouter || !g_wl_surf) return;
    if (!g_home_grab_active || force_reassert) {
        keyrouter_grab(71, true);   /* XF86Home */
        keyrouter_grab(180, true);  /* XF86HomePage */
        g_home_grab_active = true;
    }

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_ungrab_count; i++) {
        uint32_t xkb = g_ungrab_queue[i];
        if (xkb != 71 && xkb != 180) keyrouter_grab(xkb, false);
    }
    g_ungrab_count = 0;

    for (int i = 0; i < g_bind_count; i++) {
        if (!g_binds[i].grabbed || force_reassert) {
            keyrouter_grab(g_binds[i].xkb, true);
            g_binds[i].grabbed = true;
        }
    }
    g_binds_dirty = false;
    pthread_mutex_unlock(&g_lock);
    G.wl_display_flush(g_wl_dpy);
}

static void start_focus_animation(bool include_dock) {
    int64_t t = now_us();
    for (int i = 0; i < 3; i++) {
        bool foc = (g_zone == ZONE_HERO && i == g_hero_idx);
        g_card_s0[i] = g_card_scale[i];
        g_card_y0[i] = g_card_yoff[i];
        g_card_s1[i] = foc ? 1.032f : (g_zone == ZONE_DOCK ? 0.972f : 0.985f);
        g_card_y1[i] = foc ? -14.0f : 0.0f;
    }
    g_anim_start_us = t;
    g_animating = true;
    /* Only dirty hero band (104..884) during Left/Right so Top Bar & Dock are skipped by Mali TBDR! */
    mark_dirty_rect(0, 104, WIN_W, include_dock ? 892 : 780);
}

static void show_hub(void) {
    g_hide_at_us = 0;
    g_hdmi_guard_at_us = 0;
    bool was_hidden = !g_visible;
    g_visible = true;
    g_hot_sleeping = false;
    if (g_zone == ZONE_MODAL || g_zone == ZONE_VPN_MODAL) g_zone = ZONE_HERO;

    /* Re-assert Home + custom keybinds in case TV just woke from Suspend-to-RAM */
    sync_all_keygrabs(was_hidden);
    apply_input_opaque_regions(true);
    if (g_tz_policy) {
        G.wl_proxy_marshal(g_tz_policy, TIZEN_POLICY_SET_OPAQUE_STATE, g_wl_surf, media_os_is_playing() ? 0 : 1);
        G.wl_proxy_marshal(g_tz_policy, TIZEN_POLICY_UNSET_FOCUS_SKIP, g_wl_surf);
        G.wl_proxy_marshal(g_tz_policy, TIZEN_POLICY_SHOW, g_wl_surf);
        G.wl_proxy_marshal(g_tz_policy, TIZEN_POLICY_RAISE, g_wl_surf);
        G.wl_proxy_marshal(g_tz_policy, TIZEN_POLICY_ACTIVATE, g_wl_surf);
    }
    G.wl_proxy_marshal((struct wl_proxy *)g_wl_surf, WL_SURFACE_COMMIT);
    G.wl_display_flush(g_wl_dpy);

    mark_full_redraw(3);
    g_wake_refresh_at_us = now_us() + 260000LL;
    g_vol_poll_at_us = now_us() + 1800000LL;
    g_next_poll_us = now_us() + 20000000LL;
    if (was_hidden) hub_log("Hub shown (INSTANT BAREMETAL WAKE)");
}

static void hide_hub(bool force_cold) {
    /* Stop active playback, sync timeline back to Plex/YouTube, and flush dynamic ASTC VRAM to 0! */
    media_os_on_hub_hidden(true);

    g_hide_at_us = 0;
    g_wake_refresh_at_us = 0;
    g_vol_poll_at_us = 0;
    g_visible = false;
    g_hot_sleeping = !force_cold;
    g_animating = false;
    g_needs_render = false;

    /* Release input & opaque regions FIRST so underlying app/HDMI gets 100% of remote keys */
    apply_input_opaque_regions(false);
    if (g_tz_policy) {
        G.wl_proxy_marshal(g_tz_policy, TIZEN_POLICY_SET_OPAQUE_STATE, g_wl_surf, 0);
        G.wl_proxy_marshal(g_tz_policy, TIZEN_POLICY_SET_FOCUS_SKIP, g_wl_surf);
        G.wl_proxy_marshal(g_tz_policy, TIZEN_POLICY_LOWER, g_wl_surf);
        G.wl_proxy_marshal(g_tz_policy, TIZEN_POLICY_HIDE, g_wl_surf);
    }
    /* Clear backbuffer to transparent once so compositor never flashes stale pixels */
    renderer_begin_frame(&g_ren, WIN_W, WIN_H, true);
    G.eglSwapBuffers(g_egl_dpy, g_egl_surf);
    G.wl_proxy_marshal((struct wl_proxy *)g_wl_surf, WL_SURFACE_COMMIT);
    G.wl_display_flush(g_wl_dpy);
    hub_log("Hub hidden (zero-steal policy hide, EGL resident)");
}

static void schedule_hide(int delay_ms, bool force_cold) {
    g_hide_force_cold = force_cold;
    g_hide_at_us = now_us() + (int64_t)delay_ms * 1000LL;
}

static const char *dispatch_action(const char *act, const char *arg);

/* ── Host callbacks for embedded Unified Media OS (Plex + YouTube + Kids + Search + Player) ── */
static void host_show_toast(const char *msg, int dur_ms) {
    char dbuf[24];
    snprintf(dbuf, sizeof(dbuf), "%d", dur_ms);
    pthread_mutex_lock(&g_lock);
    push_cmd_locked(CMD_TOAST, dbuf, msg ? msg : "");
    pthread_mutex_unlock(&g_lock);
}
static void host_mark_full_redraw(int frames) { mark_full_redraw(frames); }
static void host_signal_main(void) {
    pthread_mutex_lock(&g_lock);
    push_cmd_locked(CMD_MEDIA_WAKE, "", "");
    pthread_mutex_unlock(&g_lock);
}
static void host_switch_switch2(void)      { dispatch_action("switch_switch2", ""); }
static void host_launch_stock_plex(void)   { dispatch_action("launch_stock_plex", ""); }
static void host_launch_stock_youtube(void){ dispatch_action("launch_stock_youtube", ""); }
static void host_open_inputs(void)         { media_os_exit_to_hub(); dispatch_action("open_inputs", ""); }
static void host_open_settings(void)       { media_os_exit_to_hub(); dispatch_action("open_settings", ""); }
static void host_get_top_status(int *vol, bool *muted, int *mem_avail_mib, bool *sw_connected) {
    pthread_mutex_lock(&g_lock);
    if (vol) *vol = g_snap.volume;
    if (muted) *muted = g_snap.muted;
    if (mem_avail_mib) *mem_avail_mib = g_snap.mem_avail_mib;
    if (sw_connected) *sw_connected = g_snap.sw_connected;
    pthread_mutex_unlock(&g_lock);
}
static uint32_t host_get_wl_surface_id(void) { return g_wl_surface_id; }
static void host_set_opaque_state(bool opaque) { set_surface_opaque_only(opaque); }
static void host_flush_opaque_cover(bool pure_black) {
    if (!g_visible || !g_egl_dpy || !g_egl_surf) return;
    renderer_begin_frame(&g_ren, WIN_W, WIN_H, false);
    if (!pure_black && g_ren.bg_base.id) {
        r_quad_tex(&g_ren, g_ren.bg_base.id, 0, 0, WIN_W, WIN_H, 0, 0, 1, 1, 1, 1, 1, 1);
    } else {
        r_round_rect(&g_ren, 0, 0, WIN_W, WIN_H, 0, 0.0f, 0.0f, 0.0f, 1.0f, 0, 0, 0, 0, 0);
    }
    r_flush(&g_ren);
    G.eglSwapBuffers(g_egl_dpy, g_egl_surf);
    if (!pure_black) set_surface_opaque_only(true);
    else G.wl_display_flush(g_wl_dpy);
}
static void host_gl_delete_textures(GLsizei n, const GLuint *ids) {
    if (G.glDeleteTextures && n > 0 && ids) G.glDeleteTextures(n, ids);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ACTION DISPATCHER (UI + Dynamic Button Mapping)
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *dispatch_action(const char *act, const char *arg) {
    if (!act || !*act) return "bad_action";
    pthread_mutex_lock(&g_lock);
    snprintf(g_last_action, sizeof(g_last_action), "%s", act);
    pthread_mutex_unlock(&g_lock);

    if (strcmp(act, "show_hub") == 0) {
        show_hub();
        return "ok";
    }
    if (strcmp(act, "hide_hub") == 0) {
        hide_hub(false);
        return "ok";
    }
    if (strcmp(act, "launch_plex") == 0 || strcmp(act, "open_media_home") == 0) {
        save_last_selected("plex");
        g_hero_idx = 0; g_zone = ZONE_HERO;
        if (!g_cfg.media_os_enabled && strcmp(act, "launch_plex") == 0) {
            show_toast("Opening Plex\xE2\x80\xA6", 1400);
            push_work("launch_plex", "");
            schedule_hide(480, false);
            return "ok";
        }
        if (!g_visible) show_hub();
        media_os_open_view(MEDIA_VIEW_HOME);
        return "ok";
    }
    if (strcmp(act, "launch_youtube") == 0 || strcmp(act, "open_media_youtube") == 0) {
        save_last_selected("youtube");
        g_hero_idx = 1; g_zone = ZONE_HERO;
        if (!g_cfg.media_os_enabled && strcmp(act, "launch_youtube") == 0) {
            show_toast("Opening YouTube\xE2\x80\xA6", 1400);
            push_work("launch_youtube", "");
            schedule_hide(480, false);
            return "ok";
        }
        if (!g_visible) show_hub();
        media_os_open_view(MEDIA_VIEW_YOUTUBE);
        return "ok";
    }
    if (strcmp(act, "open_media_movies") == 0) {
        if (!g_visible) show_hub();
        media_os_open_view(MEDIA_VIEW_MOVIES);
        return "ok";
    }
    if (strcmp(act, "open_media_tv") == 0) {
        if (!g_visible) show_hub();
        media_os_open_view(MEDIA_VIEW_TV);
        return "ok";
    }
    if (strcmp(act, "open_media_kids") == 0) {
        if (!g_visible) show_hub();
        media_os_open_view(MEDIA_VIEW_KIDS);
        return "ok";
    }
    if (strcmp(act, "open_media_search") == 0) {
        if (!g_visible) show_hub();
        media_os_open_view(MEDIA_VIEW_SEARCH);
        return "ok";
    }
    if (strcmp(act, "exit_media") == 0) {
        media_os_exit_to_hub();
        g_zone = ZONE_HERO;
        mark_full_redraw(3);
        return "ok";
    }
    if (strcmp(act, "play_plex") == 0 && arg && arg[0]) {
        if (!g_visible) show_hub();
        media_os_play_plex(arg);
        return "ok";
    }
    if (strcmp(act, "play_youtube") == 0 && arg && arg[0]) {
        if (!g_visible) show_hub();
        media_os_play_youtube(arg);
        return "ok";
    }
    if (strcmp(act, "launch_stock_plex") == 0) {
        save_last_selected("plex");
        g_hero_idx = 0; g_zone = ZONE_HERO;
        media_os_on_hub_hidden(true);
        media_os_exit_to_hub();
        show_toast("Opening Stock Plex\xE2\x80\xA6", 1400);
        push_work("launch_plex", "");
        schedule_hide(1100, false);
        return "ok";
    }
    if (strcmp(act, "launch_stock_youtube") == 0) {
        save_last_selected("youtube");
        g_hero_idx = 1; g_zone = ZONE_HERO;
        media_os_on_hub_hidden(true);
        media_os_exit_to_hub();
        show_toast("Opening Stock YouTube\xE2\x80\xA6", 1400);
        push_work("launch_youtube", "");
        schedule_hide(1100, false);
        return "ok";
    }
    if (strcmp(act, "switch_switch2") == 0) {
        save_last_selected("switch2");
        g_hero_idx = 2; g_zone = ZONE_HERO;
        char ukey[64] = {0};
        pthread_mutex_lock(&g_lock);
        snprintf(ukey, sizeof(ukey), "%s", g_snap.sw_key);
        pthread_mutex_unlock(&g_lock);
        char tmsg[96];
        snprintf(tmsg, sizeof(tmsg), "Switching to %s (%s)\xE2\x80\xA6", g_cfg.hdmi1_name, g_cfg.hdmi1_port);
        show_toast(tmsg, 1400);
        push_work("switch_switch2", ukey);
        schedule_hide(650, false);
        g_hdmi_guard_at_us = now_us() + 45000000LL; /* Auto Smart Memory Guard: 45s on HDMI 1 */
        return "ok";
    }
    if (strcmp(act, "source") == 0) {
        int st = atoi(arg ? arg : "13");
        char msg[80];
        snprintf(msg, sizeof(msg), "Switching to %s\xE2\x80\xA6", source_label(st));
        show_toast(msg, 1400);
        push_work("source", arg ? arg : "13");
        schedule_hide(650, false);
        if (st == g_cfg.hdmi1_source_type) g_hdmi_guard_at_us = now_us() + 45000000LL;
        return "ok";
    }
    if (strcmp(act, "app") == 0 && arg && arg[0]) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Opening %s\xE2\x80\xA6", arg);
        show_toast(msg, 1400);
        push_work("app", arg);
        schedule_hide(1100, false);
        return "ok";
    }
    if (strcmp(act, "open_inputs") == 0) {
        if (!g_visible) show_hub();
        g_zone = ZONE_MODAL;
        g_modal_idx = 0;
        mark_full_redraw(3);
        request_refresh();
        return "ok";
    }
    if (strcmp(act, "open_vpn") == 0) {
        if (!g_visible) show_hub();
        g_zone = ZONE_VPN_MODAL;
        g_vpn_modal_idx = 0;
        mark_full_redraw(3);
        push_work("poll_volume", "");
        g_vol_poll_at_us = now_us() + 1000000LL;
        return "ok";
    }
    if (strcmp(act, "vpn_toggle") == 0) {
        show_toast("Updating ExpressVPN Lightway\xE2\x80\xA6", 2400);
        push_work("script", "vpn-toggle.sh");
        g_vol_poll_at_us = now_us() + 1000000LL;
        return "ok";
    }
    if (strcmp(act, "vpn_miami_54") == 0) {
        show_toast("Connecting ExpressVPN \xC2\xB7 USA - Miami (#54)\xE2\x80\xA6", 2600);
        push_work("script", "vpn-miami-primary.sh");
        g_vol_poll_at_us = now_us() + 1000000LL;
        return "ok";
    }
    if (strcmp(act, "vpn_miami_202") == 0) {
        show_toast("Connecting ExpressVPN \xC2\xB7 USA - Miami 2 (#202)\xE2\x80\xA6", 2600);
        push_work("script", "vpn-miami-2.sh");
        g_vol_poll_at_us = now_us() + 1000000LL;
        return "ok";
    }
    if (strcmp(act, "vpn_disconnect") == 0) {
        show_toast("Disconnecting ExpressVPN Lightway\xE2\x80\xA6", 2200);
        push_work("script", "vpn-disconnect.sh");
        g_vol_poll_at_us = now_us() + 800000LL;
        return "ok";
    }
    if (strcmp(act, "close_modal") == 0) {
        g_zone = ZONE_HERO;
        mark_full_redraw(3);
        return "ok";
    }
    if (strcmp(act, "volume_up") == 0 || strcmp(act, "volume_down") == 0 || strcmp(act, "toggle_mute") == 0) {
        push_work(act, "");
        g_vol_poll_at_us = now_us() + 180000LL;
        return "ok";
    }
    if (strcmp(act, "clean_background") == 0) {
        show_toast("Freeing background streaming RAM…", 1800);
        push_work("clean_background", "");
        g_wake_refresh_at_us = now_us() + 800000LL;
        return "ok";
    }
    if (strcmp(act, "open_settings") == 0) {
        show_toast("Opening TV Settings\xE2\x80\xA6", 1200);
        push_work("open_settings", "");
        schedule_hide(650, false);
        return "ok";
    }
    if (strcmp(act, "open_samsung_home") == 0) {
        g_suppress_home_until_us = now_us() + 2500000LL;
        show_toast("Opening Samsung Home…", 1200);
        push_work("open_samsung_home", "");
        schedule_hide(120, true);
        return "ok";
    }
    if (strcmp(act, "script") == 0 || strcmp(act, "service_toggle") == 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Running %s…", arg && arg[0] ? arg : act);
        show_toast(msg, 1400);
        push_work(act, arg ? arg : "");
        return "ok";
    }
    return "unknown_action";
}

/* ── Keyboard handling (Wayland evdev & HTTP /api/key) ── */
static void handle_evdev_key(uint32_t ev) {
    int64_t t0 = now_us();
    uint32_t xkb = ev + 8;

    /* Fast-path: physical remote Volume/Mute keys -> update live volume indicator in ~80ms, don't skew nav latency */
    if (ev == EV_VOLUP || ev == EV_VOLDOWN || ev == EV_MUTE || ev == 67 || ev == 68 || ev == 66) {
        pthread_mutex_lock(&g_lock);
        g_last_raw_evdev = ev;
        g_last_raw_xkb = xkb;
        snprintf(g_last_key, sizeof(g_last_key), "ev:%u(xkb:%u)", ev, xkb);
        pthread_mutex_unlock(&g_lock);
        if (g_visible) {
            push_work("poll_volume", "");
            g_vol_poll_at_us = t0 + 180000LL;
        }
        return;
    }

    pthread_mutex_lock(&g_lock);
    g_last_raw_evdev = ev;
    g_last_raw_xkb = xkb;
    snprintf(g_last_key, sizeof(g_last_key), "ev:%u(xkb:%u)", ev, xkb);
    bool learning = (g_learn_until_us > t0);
    if (learning && !is_protected_evdev(ev, true)) {
        g_last_learned_evdev = ev;
        g_last_learned_xkb = xkb;
        g_learn_until_us = 0;
        pthread_mutex_unlock(&g_lock);
        char msg[96];
        snprintf(msg, sizeof(msg), "Captured Button: evdev=%u · xkb=%u", ev, xkb);
        show_toast(msg, 3000);
        hub_log("KeyLearn captured evdev=%u xkb=%u", ev, xkb);
        return;
    }

    /* 1. Check dynamic custom button bindings FIRST (works globally even while Hub is hidden!) */
    char bound_act[48] = {0}, bound_arg[128] = {0}, bound_lbl[64] = {0};
    for (int i = 0; i < g_bind_count; i++) {
        if (g_binds[i].evdev == ev || g_binds[i].xkb == xkb) {
            snprintf(bound_act, sizeof(bound_act), "%s", g_binds[i].action);
            snprintf(bound_arg, sizeof(bound_arg), "%s", g_binds[i].arg);
            snprintf(bound_lbl, sizeof(bound_lbl), "%s", g_binds[i].label);
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);

    if (bound_act[0]) {
        g_pending_key_ts_us = t0;
        hub_log("Dynamic KeyBind trigger ev=%u(xkb=%u) -> %s(%s) [%s]", ev, xkb, bound_act, bound_arg, bound_lbl);
        dispatch_action(bound_act, bound_arg);
        g_last_key_handler_us = (double)(now_us() - t0);
        return;
    }

    /* 2. Global HOME key summon */
    if (ev == EV_XF86HOME || ev == EV_HOMEPAGE || ev == EV_HOME) {
        if (t0 < g_suppress_home_until_us) return;
        if (t0 - g_last_home_ev_us < 260000LL) return;
        g_last_home_ev_us = t0;
        g_pending_key_ts_us = t0;
        if (!g_visible) {
            show_hub();
        } else if (media_os_is_active()) {
            media_os_exit_to_hub();
            g_zone = ZONE_HERO;
            start_focus_animation(true);
        } else {
            g_zone = ZONE_HERO;
            start_focus_animation(true);
        }
        g_last_key_handler_us = (double)(now_us() - t0);
        return;
    }

    if (!g_visible) return;

    /* 2b. Delegate to embedded Unified Media OS when active */
    if (media_os_is_active()) {
        if (media_os_handle_key(ev)) {
            g_pending_key_ts_us = t0;
            g_last_key_handler_us = (double)(now_us() - t0);
            return;
        }
    }

    /* 3. Number / color quick-launch shortcuts */
    if (ev == EV_1 || ev == EV_RED)    { g_pending_key_ts_us = t0; dispatch_action("launch_plex", "");    return; }
    if (ev == EV_2 || ev == EV_GREEN)  { g_pending_key_ts_us = t0; dispatch_action("launch_youtube", ""); return; }
    if (ev == EV_3 || ev == EV_YELLOW) { g_pending_key_ts_us = t0; dispatch_action("switch_switch2", ""); return; }
    if (ev == EV_0 || ev == EV_BLUE)   { g_pending_key_ts_us = t0; dispatch_action("open_inputs", "");    return; }
    if (ev == EV_MENU)                 { g_pending_key_ts_us = t0; dispatch_action("open_settings", "");  return; }

    /* 4. Spatial D-pad / OK / BACK navigation */
    bool handled = true;
    if (g_zone == ZONE_MODAL) {
        if (ev == EV_UP)   { g_modal_idx = (g_modal_idx + 3) % 4; mark_dirty_rect(540, 240, 840, 580); }
        else if (ev == EV_DOWN) { g_modal_idx = (g_modal_idx + 1) % 4; mark_dirty_rect(540, 240, 840, 580); }
        else if (ev == EV_ESC || ev == EV_BACK || ev == EV_BACKSPACE) { dispatch_action("close_modal", ""); }
        else if (ev == EV_ENTER || ev == EV_KPENTER || ev == EV_OK || ev == EV_SELECT || ev == EV_SPACE) {
            int st = 13;
            pthread_mutex_lock(&g_lock);
            st = g_snap.sources[g_modal_idx].source_type;
            pthread_mutex_unlock(&g_lock);
            g_zone = ZONE_HERO;
            char stbuf[16]; snprintf(stbuf, sizeof(stbuf), "%d", st);
            dispatch_action(st == g_cfg.hdmi1_source_type ? "switch_switch2" : "source", stbuf);
        } else { handled = false; }
    } else if (g_zone == ZONE_VPN_MODAL) {
        if (ev == EV_UP)   { g_vpn_modal_idx = (g_vpn_modal_idx + 3) % 4; mark_dirty_rect(510, 190, 900, 680); }
        else if (ev == EV_DOWN) { g_vpn_modal_idx = (g_vpn_modal_idx + 1) % 4; mark_dirty_rect(510, 190, 900, 680); }
        else if (ev == EV_ESC || ev == EV_BACK || ev == EV_BACKSPACE) { dispatch_action("close_modal", ""); }
        else if (ev == EV_ENTER || ev == EV_KPENTER || ev == EV_OK || ev == EV_SELECT || ev == EV_SPACE) {
            const char *vacts[4] = { "vpn_toggle", "vpn_miami_54", "vpn_miami_202", "vpn_disconnect" };
            dispatch_action(vacts[g_vpn_modal_idx], "");
        } else { handled = false; }
    } else if (g_zone == ZONE_HERO) {
        if (ev == EV_LEFT)       { g_hero_idx = (g_hero_idx + 2) % 3; start_focus_animation(false); }
        else if (ev == EV_RIGHT) { g_hero_idx = (g_hero_idx + 1) % 3; start_focus_animation(false); }
        else if (ev == EV_UP)    { dispatch_action("open_media_home", ""); }
        else if (ev == EV_DOWN)  { g_zone = ZONE_DOCK; start_focus_animation(true); }
        else if (ev == EV_ESC || ev == EV_BACK || ev == EV_BACKSPACE) { hide_hub(false); }
        else if (ev == EV_ENTER || ev == EV_KPENTER || ev == EV_OK || ev == EV_SELECT || ev == EV_SPACE) {
            const char *acts[3] = { "launch_plex", "launch_youtube", "switch_switch2" };
            dispatch_action(acts[g_hero_idx], "");
        } else { handled = false; }
    } else if (g_zone == ZONE_DOCK) {
        if (ev == EV_LEFT)       { g_dock_idx = (g_dock_idx + DOCK_COUNT - 1) % DOCK_COUNT; mark_dirty_rect(400, 896, 1120, 104); }
        else if (ev == EV_RIGHT) { g_dock_idx = (g_dock_idx + 1) % DOCK_COUNT; mark_dirty_rect(400, 896, 1120, 104); }
        else if (ev == EV_UP)    { g_zone = ZONE_HERO; start_focus_animation(true); }
        else if (ev == EV_ESC || ev == EV_BACK || ev == EV_BACKSPACE) { g_zone = ZONE_HERO; start_focus_animation(true); }
        else if (ev == EV_ENTER || ev == EV_KPENTER || ev == EV_OK || ev == EV_SELECT || ev == EV_SPACE) {
            dispatch_action(DOCK_ACTIONS[g_dock_idx], "");
        } else { handled = false; }
    } else {
        handled = false;
    }

    if (handled) {
        g_pending_key_ts_us = t0;
        g_last_key_handler_us = (double)(now_us() - t0);
    }
}

static uint32_t name_to_evdev(const char *k) {
    if (strcmp(k, "LEFT") == 0) return EV_LEFT;
    if (strcmp(k, "RIGHT") == 0) return EV_RIGHT;
    if (strcmp(k, "UP") == 0) return EV_UP;
    if (strcmp(k, "DOWN") == 0) return EV_DOWN;
    if (strcmp(k, "ENTER") == 0 || strcmp(k, "OK") == 0 || strcmp(k, "RETURN") == 0) return EV_ENTER;
    if (strcmp(k, "BACK") == 0 || strcmp(k, "ESCAPE") == 0 || strcmp(k, "ESC") == 0) return EV_BACK;
    if (strcmp(k, "HOME") == 0 || strcmp(k, "XF86HOME") == 0) return EV_XF86HOME;
    if (strcmp(k, "1") == 0 || strcmp(k, "RED") == 0) return EV_1;
    if (strcmp(k, "2") == 0 || strcmp(k, "GREEN") == 0) return EV_2;
    if (strcmp(k, "3") == 0 || strcmp(k, "YELLOW") == 0) return EV_3;
    if (strcmp(k, "0") == 0 || strcmp(k, "BLUE") == 0) return EV_0;
    if (strncmp(k, "EV_", 3) == 0) return (uint32_t)atoi(k + 3);
    if (strncmp(k, "XKB_", 4) == 0) { int x = atoi(k + 4); return x > 8 ? (uint32_t)(x - 8) : 0; }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  OBSIDIAN ATELIER 1920x1080 MALI GLES2 + ASTC FRAME RENDERER
 * ═══════════════════════════════════════════════════════════════════════════ */

static float ease_out_cubic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

static void render_frame(void) {
    int64_t t_frame0 = now_us();

    /* Fast path: embedded Unified Media OS (Plex + YouTube + Kids + Search + Transparent Player HUD) */
    if (media_os_is_active()) {
        media_os_render(&g_ren, t_frame0);
        if (g_toast_until_us > t_frame0 && g_toast_msg[0]) {
            r_round_rect(&g_ren, 680, 34, 560, 52, 26, 0.953f, 0.965f, 0.984f, 0.97f, 1, 1, 1, 1.0f, 1.0f);
            r_text_fit(&g_ren, 3, g_toast_msg, 704, 34, 512, 52, 1, 0.035f, 0.051f, 0.086f, 1.0f);
            r_flush(&g_ren);
        }
        g_draw_calls_last = r_get_draw_calls();
        G.eglSwapBuffers(g_egl_dpy, g_egl_surf);
        G.wl_display_flush(g_wl_dpy);
        g_dirty_valid = false;
        g_needs_render = false;
        if (g_pending_key_ts_us > 0) {
            double ms = (double)(now_us() - g_pending_key_ts_us) / 1000.0;
            g_last_key_swap_ms = ms;
            g_sum_swap_ms += ms;
            g_swap_samples++;
            g_pending_key_ts_us = 0;
        }
        return;
    }

    /* Step animations (95 ms smooth lift) */
    if (g_animating) {
        float t = (float)(t_frame0 - g_anim_start_us) / 95000.0f;
        float e = ease_out_cubic(t);
        for (int i = 0; i < 3; i++) {
            g_card_scale[i] = g_card_s0[i] + (g_card_s1[i] - g_card_s0[i]) * e;
            g_card_yoff[i]  = g_card_y0[i] + (g_card_y1[i] - g_card_y0[i]) * e;
        }
        if (t >= 1.0f) g_animating = false;
        mark_dirty_rect(0, 104, WIN_W, (g_zone == ZONE_DOCK) ? 892 : 780);
    }

    /* Check EGL_KHR_partial_update + EGL_BUFFER_AGE_KHR */
    bool use_partial = false;
    RectI dmg = {0, 0, WIN_W, WIN_H};
    if (g_has_partial_update && g_full_redraws <= 0 && g_dirty_valid) {
        EGLint age = 0;
        if (G.eglQuerySurface && G.eglQuerySurface(g_egl_dpy, g_egl_surf, EGL_BUFFER_AGE_KHR, &age) && age > 0 && age <= 3) {
            use_partial = true;
            dmg = g_dirty;
            EGLint egl_rect[4] = { dmg.x, WIN_H - (dmg.y + dmg.h), dmg.w, dmg.h };
            G.eglSetDamageRegionKHR(g_egl_dpy, g_egl_surf, egl_rect, 1);
        }
    }
    if (g_full_redraws > 0) g_full_redraws--;

    /* Copy snapshot quickly under lock */
    HubSnapshot s;
    pthread_mutex_lock(&g_lock);
    s = g_snap;
    pthread_mutex_unlock(&g_lock);

    /* Current clock & date */
    time_t now_t = time(NULL);
    struct tm tm;
    localtime_r(&now_t, &tm);
    static const char *DAYS[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    static const char *MONS[12] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
    char date_str[32], time_str[24];
    int h12 = tm.tm_hour % 12; if (h12 == 0) h12 = 12;
    snprintf(date_str, sizeof(date_str), "%s \xC2\xB7 %s %d", DAYS[tm.tm_wday], MONS[tm.tm_mon], tm.tm_mday);
    snprintf(time_str, sizeof(time_str), "%d:%02d %s", h12, tm.tm_min, tm.tm_hour >= 12 ? "PM" : "AM");

    renderer_begin_frame(&g_ren, WIN_W, WIN_H, false);
    if (use_partial) renderer_set_scissor(dmg.x, dmg.y, dmg.w, dmg.h);

    /* 1. Opaque ASTC 6x6 Obsidian background (covers 100% — no glClear needed!) */
    r_quad_tex(&g_ren, g_ren.bg_base.id, 0, 0, WIN_W, WIN_H, 0, 0, 1, 1, 1, 1, 1, 1);

    /* 2. Top Chrome Bar (batched: SDF pills -> icons -> text = 2 draw calls total!) */
    if (!use_partial || dmg.y < 108) {
        r_round_rect(&g_ren, 88, 52, 42, 42, 12, 1, 1, 1, 0.08f, 1, 1, 1, 0.16f, 1.2f);
        r_round_rect(&g_ren, 390, 55, 224, 36, 18, 1, 1, 1, 0.052f, 1, 1, 1, 0.10f, 1.0f);
        r_round_rect(&g_ren, 404, 69, 8, 8, 4, 0.204f, 0.827f, 0.600f, 1.0f, 0, 0, 0, 0, 0);
        r_round_rect(&g_ren, 626, 55, 212, 36, 18, 1, 1, 1, 0.052f, 1, 1, 1, 0.10f, 1.0f);
        r_round_rect(&g_ren, 640, 69, 8, 8, 4, 0.0f, 0.784f, 0.918f, 1.0f, 0, 0, 0, 0, 0);
        r_round_rect(&g_ren, 850, 55, 142, 36, 18, 1, 1, 1, 0.052f, 1, 1, 1, 0.10f, 1.0f);

        /* Live ExpressVPN Lightway pill (strictly truthful: active tunnel vs off/connecting/error) */
        const bool vpn_on = s.vpn.tunnel_established;
        const bool vpn_busy = (strcmp(s.vpn.status, "connecting") == 0 || strcmp(s.vpn.status, "disconnecting") == 0);
        const bool vpn_err = (strcmp(s.vpn.status, "error") == 0);
        if (vpn_on) {
            r_round_rect(&g_ren, 1004, 55, 248, 36, 18, 0.204f, 0.827f, 0.600f, 0.16f, 0.204f, 0.827f, 0.600f, 0.40f, 1.0f);
            r_round_rect(&g_ren, 1018, 69, 8, 8, 4, 0.204f, 0.827f, 0.600f, 1.0f, 0, 0, 0, 0, 0);
        } else if (vpn_busy) {
            r_round_rect(&g_ren, 1004, 55, 248, 36, 18, 0.898f, 0.627f, 0.051f, 0.18f, 1.0f, 0.761f, 0.231f, 0.42f, 1.0f);
            r_round_rect(&g_ren, 1018, 69, 8, 8, 4, 1.0f, 0.761f, 0.231f, 1.0f, 0, 0, 0, 0, 0);
        } else if (vpn_err) {
            r_round_rect(&g_ren, 1004, 55, 248, 36, 18, 1.0f, 0.169f, 0.239f, 0.16f, 1.0f, 0.349f, 0.404f, 0.45f, 1.0f);
            r_round_rect(&g_ren, 1018, 69, 8, 8, 4, 1.0f, 0.349f, 0.404f, 1.0f, 0, 0, 0, 0, 0);
        } else {
            r_round_rect(&g_ren, 1004, 55, 248, 36, 18, 1, 1, 1, 0.052f, 1, 1, 1, 0.10f, 1.0f);
            r_round_rect(&g_ren, 1018, 69, 8, 8, 4, 0.392f, 0.455f, 0.545f, 1.0f, 0, 0, 0, 0, 0);
        }

        if (s.muted) {
            r_round_rect(&g_ren, 1334, 53, 128, 40, 20, 1.0f, 0.169f, 0.239f, 0.16f, 1.0f, 0.349f, 0.404f, 0.45f, 1.0f);
        } else {
            r_round_rect(&g_ren, 1334, 53, 128, 40, 20, 1, 1, 1, 0.052f, 1, 1, 1, 0.10f, 1.0f);
        }
        r_round_rect(&g_ren, 1484, 58, 1.5f, 30, 0.5f, 1, 1, 1, 0.14f, 0, 0, 0, 0, 0);

        /* Top bar icons in one batch */
        r_icon(&g_ren, 0 /*tv*/, 98, 62, 22, 22, 0.953f, 0.965f, 0.984f, 1.0f);
        if (s.muted) r_icon(&g_ren, 3 /*mute*/, 1350, 63, 20, 20, 0.992f, 0.643f, 0.686f, 1.0f);
        else         r_icon(&g_ren, 8 /*vol*/, 1350, 63, 20, 20, 0.953f, 0.965f, 0.984f, 1.0f);

        /* Top bar texts in one batch */
        char lan_txt[64], ram_txt[32], vol_txt[24], vpn_txt[56];
        snprintf(lan_txt, sizeof(lan_txt), "LAN \xC2\xB7 %s", s.ip[0] ? s.ip : "Auto");
        const char *cast_txt = s.airplay_running ? "AirPlay \xC2\xB7 Cast Ready" :
                               (s.dmr_running ? "DLNA \xC2\xB7 Cast Ready" : "Cast Standby");
        if (s.mem_avail_mib > 0) snprintf(ram_txt, sizeof(ram_txt), "%d MB Free", s.mem_avail_mib);
        else snprintf(ram_txt, sizeof(ram_txt), "-- MB Free");
        if (vpn_on) {
            const char *short_loc = strstr(s.vpn.active_name, " - ");
            short_loc = short_loc ? (short_loc + 3) : (s.vpn.active_name[0] ? s.vpn.active_name : "Active");
            if (s.vpn.colo[0]) snprintf(vpn_txt, sizeof(vpn_txt), "VPN \xC2\xB7 %s (%s)", short_loc, s.vpn.colo);
            else snprintf(vpn_txt, sizeof(vpn_txt), "VPN \xC2\xB7 %s", short_loc);
        } else if (vpn_busy) {
            snprintf(vpn_txt, sizeof(vpn_txt), "VPN \xC2\xB7 %s\xE2\x80\xA6", strcmp(s.vpn.status, "disconnecting") == 0 ? "Stopping" : "Connecting");
        } else if (vpn_err) {
            snprintf(vpn_txt, sizeof(vpn_txt), "VPN \xC2\xB7 Error");
        } else if (s.vpn.available) {
            snprintf(vpn_txt, sizeof(vpn_txt), "VPN \xC2\xB7 Off");
        } else {
            snprintf(vpn_txt, sizeof(vpn_txt), "VPN \xC2\xB7 Standby");
        }
        if (s.muted) snprintf(vol_txt, sizeof(vol_txt), "Muted");
        else if (s.volume >= 0) snprintf(vol_txt, sizeof(vol_txt), "Vol %d", s.volume);
        else snprintf(vol_txt, sizeof(vol_txt), "Audio");

        r_text_spaced(&g_ren, 3 /*21px*/, g_cfg.room_title, 148, 52, 240, 42, 0, 3.8f, 0.95f, 0.96f, 0.98f, 0.88f);
        r_text(&g_ren, 1 /*16px*/, lan_txt, 420, 55, 186, 36, 0, 0.635f, 0.682f, 0.769f, 1.0f);
        r_text(&g_ren, 1 /*16px*/, cast_txt, 656, 55, 174, 36, 0, 0.635f, 0.682f, 0.769f, 1.0f);
        r_text(&g_ren, 1 /*16px*/, ram_txt, 850, 55, 142, 36, 1, 0.635f, 0.682f, 0.769f, 1.0f);
        if (vpn_on)        r_text(&g_ren, 1 /*16px*/, vpn_txt, 1034, 55, 208, 36, 0, 0.525f, 0.937f, 0.675f, 1.0f);
        else if (vpn_busy) r_text(&g_ren, 1 /*16px*/, vpn_txt, 1034, 55, 208, 36, 0, 0.992f, 0.902f, 0.541f, 1.0f);
        else if (vpn_err)  r_text(&g_ren, 1 /*16px*/, vpn_txt, 1034, 55, 208, 36, 0, 0.992f, 0.643f, 0.686f, 1.0f);
        else               r_text(&g_ren, 1 /*16px*/, vpn_txt, 1034, 55, 208, 36, 0, 0.635f, 0.682f, 0.769f, 1.0f);
        if (s.muted) r_text(&g_ren, 2 /*19px*/, vol_txt, 1378, 53, 76, 40, 0, 0.992f, 0.643f, 0.686f, 1.0f);
        else         r_text(&g_ren, 2 /*19px*/, vol_txt, 1378, 53, 76, 40, 0, 0.953f, 0.965f, 0.984f, 1.0f);
        r_text(&g_ren, 3 /*21px*/, date_str, 1502, 54, 158, 38, 2, 0.635f, 0.682f, 0.769f, 1.0f);
        r_text(&g_ren, 5 /*36px*/, time_str, 1666, 48, 166, 48, 2, 0.953f, 0.965f, 0.984f, 1.0f);
    }

    /* 3. Monumental ASTC Hero Triptych (batched: halo -> 3 plates -> all SDF badges + texts = 5 draw calls!) */
    static const float CARD_XS[3] = { 88.0f, 682.0f, 1276.0f };
    const float CW = 556.0f, CH = 610.0f, TOP_Y = 186.0f;

    if (!use_partial || (dmg.y < 880 && dmg.y + dmg.h > 100)) {
        if (g_zone == ZONE_HERO) {
            int hi = g_hero_idx;
            r_quad_tex(&g_ren, g_ren.halo[hi].id, CARD_XS[hi] - 64.0f, TOP_Y - 76.0f, CW + 128.0f, CH + 128.0f, 0, 0, 1, 1, 1, 1, 1, 1);
        }

        float crx[3], cry[3], csc[3];
        for (int i = 0; i < 3; i++) {
            bool foc = (g_zone == ZONE_HERO && i == g_hero_idx);
            float sc = g_card_scale[i];
            float cx = CARD_XS[i] + CW * 0.5f;
            float cy = TOP_Y + CH * 0.5f + g_card_yoff[i];
            float rw = CW * sc, rh = CH * sc;
            crx[i] = cx - rw * 0.5f; cry[i] = cy - rh * 0.5f; csc[i] = sc;
            GLuint plate = foc ? g_ren.card_focus[i].id : g_ren.card_idle[i].id;
            r_quad_tex(&g_ren, plate, crx[i], cry[i], rw, rh, 0, 0, 1, 1, 1, 1, 1, 1);
        }

        /* Pass B: ALL dynamic card SDF badges + texts in ONE single glyph+SDF batch! */
        for (int i = 0; i < 3; i++) {
            bool foc = (g_zone == ZONE_HERO && i == g_hero_idx);
            float rx = crx[i], ry = cry[i], sc = csc[i];
            #define LX(x) (rx + (x) * sc)
            #define LY(y) (ry + (y) * sc)
            #define LS(v) ((v) * sc)

            if (i == 0) {
                if (s.plex_running) {
                    r_round_rect(&g_ren, LX(178), LY(34), LS(112), LS(34), LS(17),
                                 0.898f, 0.627f, 0.051f, 0.20f, 1.0f, 0.761f, 0.231f, 0.46f, 1.0f);
                    r_round_rect(&g_ren, LX(191), LY(47), LS(8), LS(8), LS(4), 1.0f, 0.761f, 0.231f, 1.0f, 0, 0, 0, 0, 0);
                    r_text(&g_ren, 0, "RUNNING", LX(205), LY(34), LS(78), LS(34), 0, 0.992f, 0.902f, 0.541f, 1.0f);
                }
                char nas_badge[40], sub[128];
                if (!g_cfg.plex_host[0]) {
                    r_round_rect(&g_ren, LX(346), LY(34), LS(174), LS(34), LS(17),
                                 0.204f, 0.827f, 0.600f, 0.16f, 0.204f, 0.827f, 0.600f, 0.38f, 1.0f);
                    r_round_rect(&g_ren, LX(360), LY(47), LS(8), LS(8), LS(4), 0.204f, 0.827f, 0.600f, 1.0f, 0, 0, 0, 0, 0);
                    r_text(&g_ren, 0, "PLEX READY", LX(374), LY(34), LS(136), LS(34), 0, 0.525f, 0.937f, 0.675f, 1.0f);
                    snprintf(sub, sizeof(sub), "%s", g_cfg.media_os_enabled ? "Unified Media Hub" : "Movies \xC2\xB7 TV Shows \xC2\xB7 Streaming");
                } else if (s.plex_online) {
                    snprintf(nas_badge, sizeof(nas_badge), "NAS ONLINE \xC2\xB7 %dms", s.plex_latency_ms >= 0 ? s.plex_latency_ms : 1);
                    if (s.plex_version[0]) snprintf(sub, sizeof(sub), "Local Server \xC2\xB7 %s \xC2\xB7 v%s", g_cfg.plex_host, s.plex_version);
                    else snprintf(sub, sizeof(sub), "Local Server \xC2\xB7 %s", g_cfg.plex_host);
                    r_round_rect(&g_ren, LX(300), LY(34), LS(220), LS(34), LS(17),
                                 0.204f, 0.827f, 0.600f, 0.16f, 0.204f, 0.827f, 0.600f, 0.38f, 1.0f);
                    r_round_rect(&g_ren, LX(314), LY(47), LS(8), LS(8), LS(4), 0.204f, 0.827f, 0.600f, 1.0f, 0, 0, 0, 0, 0);
                    r_text(&g_ren, 0, nas_badge, LX(328), LY(34), LS(182), LS(34), 0, 0.525f, 0.937f, 0.675f, 1.0f);
                } else {
                    r_round_rect(&g_ren, LX(300), LY(34), LS(220), LS(34), LS(17), 1, 1, 1, 0.052f, 1, 1, 1, 0.10f, 1.0f);
                    r_round_rect(&g_ren, LX(314), LY(47), LS(8), LS(8), LS(4), 1.0f, 0.761f, 0.231f, 1.0f, 0, 0, 0, 0, 0);
                    r_text(&g_ren, 0, "NAS UNREACHABLE", LX(328), LY(34), LS(182), LS(34), 0, 0.635f, 0.682f, 0.769f, 1.0f);
                    snprintf(sub, sizeof(sub), "Check Plex Server \xC2\xB7 %s", g_cfg.plex_host);
                }
                r_text(&g_ren, 3, sub, LX(36), LY(444), LS(484), LS(34), 0, 0.635f, 0.682f, 0.769f, 1.0f);
                const char *cta = g_cfg.media_os_enabled ? "Open Media Hub" : "Open Plex";
                if (foc) r_text(&g_ren, 4, cta, LX(60), LY(508), LS(390), LS(62), 0, 0.086f, 0.063f, 0.016f, 1.0f);
                else     r_text(&g_ren, 4, cta, LX(60), LY(508), LS(390), LS(62), 0, 0.635f, 0.682f, 0.769f, 1.0f);
            } else if (i == 1) {
                if (s.yt_running) {
                    r_round_rect(&g_ren, LX(302), LY(34), LS(112), LS(34), LS(17),
                                 1.0f, 0.169f, 0.239f, 0.20f, 1.0f, 0.349f, 0.404f, 0.46f, 1.0f);
                    r_round_rect(&g_ren, LX(315), LY(47), LS(8), LS(8), LS(4), 1.0f, 0.349f, 0.404f, 1.0f, 0, 0, 0, 0, 0);
                    r_text(&g_ren, 0, "RUNNING", LX(329), LY(34), LS(78), LS(34), 0, 0.996f, 0.804f, 0.827f, 1.0f);
                }
                const char *yt_sub = g_cfg.media_os_enabled
                    ? "Ad-Free \xC2\xB7 Subscriptions \xC2\xB7 SponsorBlock"
                    : "Subscriptions \xC2\xB7 Search \xC2\xB7 Phone Cast";
                r_text(&g_ren, 3, yt_sub, LX(36), LY(444), LS(484), LS(34), 0, 0.635f, 0.682f, 0.769f, 1.0f);
                const char *cta = "Open YouTube";
                if (foc) r_text(&g_ren, 4, cta, LX(60), LY(508), LS(390), LS(62), 0, 1.0f, 1.0f, 1.0f, 1.0f);
                else     r_text(&g_ren, 4, cta, LX(60), LY(508), LS(390), LS(62), 0, 0.635f, 0.682f, 0.769f, 1.0f);
            } else {
                bool awake = (s.sw_power == 1 || s.sw_cec_power == 0);
                const char *badge = "STANDBY";
                char sub[128], cta[64];
                snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", g_cfg.hdmi1_name, g_cfg.hdmi1_port);
                snprintf(cta, sizeof(cta), "Switch to %s", g_cfg.hdmi1_port);
                float bg[4] = {1, 1, 1, 0.052f}, bdr[4] = {1, 1, 1, 0.10f};
                float dot[3] = {0.392f, 0.455f, 0.545f}, ink[3] = {0.635f, 0.682f, 0.769f};
                if (s.sw_active && awake) {
                    badge = "ACTIVE \xC2\xB7 AWAKE";
                    snprintf(sub, sizeof(sub), "%s \xC2\xB7 Signal Live", g_cfg.hdmi1_name);
                    snprintf(cta, sizeof(cta), "Return to %s", g_cfg.hdmi1_port);
                    bg[0]=0.204f; bg[1]=0.827f; bg[2]=0.600f; bg[3]=0.16f;
                    bdr[0]=0.204f; bdr[1]=0.827f; bdr[2]=0.600f; bdr[3]=0.38f;
                    dot[0]=0.204f; dot[1]=0.827f; dot[2]=0.600f; ink[0]=0.525f; ink[1]=0.937f; ink[2]=0.675f;
                } else if (awake) {
                    badge = "AWAKE \xC2\xB7 READY";
                    snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", g_cfg.hdmi1_name, g_cfg.hdmi1_is_switch2 ? "Console Awake" : "Signal Ready");
                    snprintf(cta, sizeof(cta), "Open %s", g_cfg.hdmi1_port);
                    bg[0]=0.204f; bg[1]=0.827f; bg[2]=0.600f; bg[3]=0.16f;
                    bdr[0]=0.204f; bdr[1]=0.827f; bdr[2]=0.600f; bdr[3]=0.38f;
                    dot[0]=0.204f; dot[1]=0.827f; dot[2]=0.600f; ink[0]=0.525f; ink[1]=0.937f; ink[2]=0.675f;
                } else if (s.sw_active) {
                    badge = "ACTIVE INPUT";
                    snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", g_cfg.hdmi1_name, g_cfg.hdmi1_is_switch2 ? "Dock Standby" : "Input Active");
                    snprintf(cta, sizeof(cta), "Return to %s", g_cfg.hdmi1_port);
                    bg[0]=0.0f; bg[1]=0.784f; bg[2]=0.918f; bg[3]=0.16f;
                    bdr[0]=0.0f; bdr[1]=0.784f; bdr[2]=0.918f; bdr[3]=0.42f;
                    dot[0]=1.0f; dot[1]=0.761f; dot[2]=0.231f; ink[0]=0.647f; ink[1]=0.953f; ink[2]=0.988f;
                } else if (s.sw_connected) {
                    badge = g_cfg.hdmi1_is_switch2 ? "DOCKED \xC2\xB7 STANDBY" : "CONNECTED";
                    snprintf(sub, sizeof(sub), "%s \xC2\xB7 HDMI-CEC Ready", g_cfg.hdmi1_name);
                    dot[0]=1.0f; dot[1]=0.761f; dot[2]=0.231f;
                }
                r_round_rect(&g_ren, LX(326), LY(34), LS(194), LS(34), LS(17),
                             bg[0], bg[1], bg[2], bg[3], bdr[0], bdr[1], bdr[2], bdr[3], 1.0f);
                r_round_rect(&g_ren, LX(340), LY(47), LS(8), LS(8), LS(4), dot[0], dot[1], dot[2], 1.0f, 0, 0, 0, 0, 0);
                r_text(&g_ren, 0, badge, LX(354), LY(34), LS(158), LS(34), 0, ink[0], ink[1], ink[2], 1.0f);
                r_round_rect(&g_ren, LX(CW * 0.5f - 5.0f), LY(256.0f), LS(10), LS(10), LS(5), dot[0], dot[1], dot[2], 1.0f, 0, 0, 0, 0, 0);
                r_text(&g_ren, 3, sub, LX(36), LY(444), LS(484), LS(34), 0, 0.635f, 0.682f, 0.769f, 1.0f);
                if (foc) r_text(&g_ren, 4, cta, LX(60), LY(508), LS(390), LS(62), 0, 0.035f, 0.051f, 0.086f, 1.0f);
                else     r_text(&g_ren, 4, cta, LX(60), LY(508), LS(390), LS(62), 0, 0.635f, 0.682f, 0.769f, 1.0f);
            }
            #undef LX
            #undef LY
            #undef LS
        }
    }

    /* 4. Bottom Utility Dock (7 balanced buttons: Inputs | VPN | Vol- Mute Vol+ | Free RAM Settings — NO Samsung Home!) */
    if (!use_partial || dmg.y + dmg.h > 895) {
        const float RAIL_W = 1082.0f, RAIL_X = (WIN_W - 1082.0f) * 0.5f;
        const float RAIL_Y = (g_zone == ZONE_DOCK) ? 906.0f : 910.0f;
        const float RAIL_A = (g_zone == ZONE_DOCK) ? 1.0f : 0.82f;
        r_quad_tex(&g_ren, g_ren.dock_rail.id, RAIL_X, RAIL_Y, RAIL_W, 76.0f, 0, 0, 1, 1, 1, 1, 1, RAIL_A);

        static const float DOCK_W[DOCK_COUNT] = { 146, 128, 112, 134, 112, 164, 156 };
        static const int   DOCK_ICO[DOCK_COUNT] = { 1, 9, 2, 3, 4, 5, 6 };
        static const bool  DOCK_SEP[DOCK_COUNT] = { true, true, false, false, true, false, false };
        const char *dock_lbl[DOCK_COUNT] = {
            "Inputs", s.vpn.tunnel_established ? "VPN On" : "VPN",
            "Vol", s.muted ? "Unmute" : "Mute", "Vol", "Free RAM", "Settings"
        };

        float bx_arr[DOCK_COUNT];
        float cur_x = RAIL_X + 14.0f;
        for (int i = 0; i < DOCK_COUNT; i++) {
            bx_arr[i] = cur_x;
            bool foc = (g_zone == ZONE_DOCK && i == g_dock_idx);
            float bw = DOCK_W[i], bh = 56.0f, by = RAIL_Y + 10.0f;
            if (foc) {
                float dw = bw * 0.045f, dh = bh * 0.045f;
                r_round_rect(&g_ren, cur_x - dw * 0.5f, by - dh * 0.5f, bw + dw, bh + dh, 16.0f,
                             0.953f, 0.965f, 0.984f, 1.0f, 0, 0, 0, 0, 0);
            } else if (i == 1 && s.vpn.tunnel_established) {
                r_round_rect(&g_ren, cur_x, by, bw, bh, 16.0f, 0.204f, 0.827f, 0.600f, 0.14f * RAIL_A,
                             0.204f, 0.827f, 0.600f, 0.36f * RAIL_A, 1.0f);
            } else {
                r_round_rect(&g_ren, cur_x, by, bw, bh, 16.0f, 1, 1, 1, 0.045f * RAIL_A, 0, 0, 0, 0, 0);
            }
            cur_x += bw + (DOCK_SEP[i] ? 24.0f : 10.0f);
        }
        /* All 7 dock icons in 1 draw call */
        for (int i = 0; i < DOCK_COUNT; i++) {
            bool foc = (g_zone == ZONE_DOCK && i == g_dock_idx);
            float by = RAIL_Y + 10.0f;
            if (foc) r_icon(&g_ren, DOCK_ICO[i], bx_arr[i] + 18, by + 17, 22, 22, 0.035f, 0.051f, 0.086f, 1.0f);
            else if (i == 1 && s.vpn.tunnel_established) r_icon(&g_ren, DOCK_ICO[i], bx_arr[i] + 18, by + 17, 22, 22, 0.525f, 0.937f, 0.675f, RAIL_A);
            else     r_icon(&g_ren, DOCK_ICO[i], bx_arr[i] + 18, by + 17, 22, 22, 0.635f, 0.682f, 0.769f, RAIL_A);
        }
        /* All 7 dock labels + hint in 1 draw call */
        for (int i = 0; i < DOCK_COUNT; i++) {
            bool foc = (g_zone == ZONE_DOCK && i == g_dock_idx);
            float bw = DOCK_W[i], bh = 56.0f, by = RAIL_Y + 10.0f;
            if (foc) r_text(&g_ren, 2, dock_lbl[i], bx_arr[i] + 48, by, bw - 58, bh, 0, 0.035f, 0.051f, 0.086f, 1.0f);
            else if (i == 1 && s.vpn.tunnel_established) r_text(&g_ren, 2, dock_lbl[i], bx_arr[i] + 48, by, bw - 58, bh, 0, 0.525f, 0.937f, 0.675f, RAIL_A);
            else     r_text(&g_ren, 2, dock_lbl[i], bx_arr[i] + 48, by, bw - 58, bh, 0, 0.635f, 0.682f, 0.769f, RAIL_A);
        }
        r_text(&g_ren, 1,
               "\xE2\x97\x80 \xE2\x96\xB6  Select      \xC2\xB7      OK  Launch      \xC2\xB7      \xE2\x96\xBC  Controls      \xC2\xB7      BACK  Resume Current      \xC2\xB7      HOME  Living Room Hub",
               88, 1004, 1744, 34, 1, 0.635f, 0.682f, 0.769f, 0.52f);
    }

    /* 5. Inputs & Casting Modal */
    if (g_zone == ZONE_MODAL) {
        r_round_rect(&g_ren, 0, 0, WIN_W, WIN_H, 0, 0.016f, 0.024f, 0.043f, 0.84f, 0, 0, 0, 0, 0);
        r_round_rect(&g_ren, 550, 250, 820, 560, 30, 0.075f, 0.098f, 0.145f, 0.99f, 1, 1, 1, 0.18f, 1.5f);
        r_text(&g_ren, 4, "Inputs & Casting", 590, 284, 420, 44, 0, 0.953f, 0.965f, 0.984f, 1.0f);
        r_text(&g_ren, 2, "Select a port \xC2\xB7 BACK to close", 1010, 292, 320, 34, 2, 0.635f, 0.682f, 0.769f, 1.0f);

        for (int i = 0; i < 4; i++) {
            float ry = 354.0f + i * 88.0f;
            bool foc = (i == g_modal_idx);
            const SourceInfo *si = &s.sources[i];
            bool is_act = (s.active_source == si->source_type);
            if (foc) {
                r_round_rect(&g_ren, 590, ry, 740, 74, 18, 0.953f, 0.965f, 0.984f, 1.0f, 1, 1, 1, 1.0f, 1.5f);
                r_round_rect(&g_ren, 610, ry + 19, 98, 36, 10, 0.035f, 0.051f, 0.086f, 0.13f, 0, 0, 0, 0, 0);
                r_text(&g_ren, 1, si->port_label, 610, ry + 19, 98, 36, 1, 0.035f, 0.051f, 0.086f, 1.0f);
                r_text(&g_ren, 4, si->name, 726, ry, 340, 74, 0, 0.035f, 0.051f, 0.086f, 1.0f);
            } else {
                r_round_rect(&g_ren, 590, ry, 740, 74, 18, 1, 1, 1, 0.055f, 1, 1, 1, 0.09f, 1.5f);
                r_round_rect(&g_ren, 610, ry + 19, 98, 36, 10, 1, 1, 1, 0.10f, 0, 0, 0, 0, 0);
                r_text(&g_ren, 1, si->port_label, 610, ry + 19, 98, 36, 1, 0.953f, 0.965f, 0.984f, 1.0f);
                r_text(&g_ren, 4, si->name, 726, ry, 340, 74, 0, 0.953f, 0.965f, 0.984f, 1.0f);
            }
            float dr = 0.392f, dg = 0.455f, db = 0.545f;
            const char *meta = "Available";
            if (is_act) { dr = 0.204f; dg = 0.827f; db = 0.600f; meta = "Active Now"; }
            else if (si->power_state == 1) { dr = 0.0f; dg = 0.784f; db = 0.918f; meta = "Awake \xC2\xB7 Ready"; }
            else if (si->connected) { dr = 1.0f; dg = 0.761f; db = 0.231f; meta = "Connected \xC2\xB7 Standby"; }
            r_round_rect(&g_ren, 1090, ry + 33, 8, 8, 4, dr, dg, db, 1.0f, 0, 0, 0, 0, 0);
            if (foc) r_text(&g_ren, 1, meta, 1108, ry, 200, 74, 0, 0.20f, 0.25f, 0.33f, 1.0f);
            else     r_text(&g_ren, 1, meta, 1108, ry, 200, 74, 0, 0.635f, 0.682f, 0.769f, 1.0f);
        }

        r_round_rect(&g_ren, 590, 724, 740, 54, 16, 1, 1, 1, 0.04f, 1, 1, 1, 0.09f, 1.0f);
        r_text(&g_ren, 1, "Wireless Casting", 612, 724, 220, 54, 0, 0.635f, 0.682f, 0.769f, 1.0f);
        r_text(&g_ren, 1, s.dmr_running ? "AirPlay \xC2\xB7 DLNA \xC2\xB7 AllShare Active" : "Services Standby",
               850, 724, 458, 54, 2, 0.953f, 0.965f, 0.984f, 1.0f);
    }

    /* 5b. ExpressVPN Lightway Modal (strict separation of Selected vs Authenticated Active tunnel) */
    if (g_zone == ZONE_VPN_MODAL) {
        r_round_rect(&g_ren, 0, 0, WIN_W, WIN_H, 0, 0.016f, 0.024f, 0.043f, 0.84f, 0, 0, 0, 0, 0);
        r_round_rect(&g_ren, 520, 200, 880, 660, 30, 0.075f, 0.098f, 0.145f, 0.99f, 1, 1, 1, 0.18f, 1.5f);
        r_text(&g_ren, 4, "ExpressVPN Lightway", 560, 232, 440, 44, 0, 0.953f, 0.965f, 0.984f, 1.0f);
        r_text(&g_ren, 2, "OK to apply \xC2\xB7 BACK to close", 1020, 240, 340, 34, 2, 0.635f, 0.682f, 0.769f, 1.0f);

        /* Truthful status summary banner (Selected != Active) */
        const bool von = s.vpn.tunnel_established;
        const bool verr = (strcmp(s.vpn.status, "error") == 0);
        if (von) {
            r_round_rect(&g_ren, 560, 290, 800, 82, 16, 0.204f, 0.827f, 0.600f, 0.12f, 0.204f, 0.827f, 0.600f, 0.36f, 1.2f);
            r_round_rect(&g_ren, 582, 314, 10, 10, 5, 0.204f, 0.827f, 0.600f, 1.0f, 0, 0, 0, 0, 0);
        } else if (verr) {
            r_round_rect(&g_ren, 560, 290, 800, 82, 16, 1.0f, 0.169f, 0.239f, 0.14f, 1.0f, 0.349f, 0.404f, 0.42f, 1.2f);
            r_round_rect(&g_ren, 582, 314, 10, 10, 5, 1.0f, 0.349f, 0.404f, 1.0f, 0, 0, 0, 0, 0);
        } else {
            r_round_rect(&g_ren, 560, 290, 800, 82, 16, 1, 1, 1, 0.05f, 1, 1, 1, 0.10f, 1.0f);
            r_round_rect(&g_ren, 582, 314, 10, 10, 5, 0.392f, 0.455f, 0.545f, 1.0f, 0, 0, 0, 0, 0);
        }
        char line1[128], line2[128];
        if (von) {
            snprintf(line1, sizeof(line1), "Active Tunnel: %s (%s)",
                     s.vpn.active_name[0] ? s.vpn.active_name : "Connected",
                     s.vpn.colo[0] ? s.vpn.colo : "LIVE");
            snprintf(line2, sizeof(line2), "Exit IP: %s   \xC2\xB7   Selected: %s",
                     s.vpn.exit_ip, s.vpn.selected_name[0] ? s.vpn.selected_name : "USA - Miami");
        } else if (verr) {
            snprintf(line1, sizeof(line1), "Tunnel Error: %s", s.vpn.err_code[0] ? s.vpn.err_code : "handshake_failed");
            snprintf(line2, sizeof(line2), "%s", s.vpn.err_msg[0] ? s.vpn.err_msg : "Rollback verified; LAN unaffected");
        } else {
            snprintf(line1, sizeof(line1), "Active Tunnel: %s",
                     strcmp(s.vpn.status, "connecting") == 0 ? "Connecting\xE2\x80\xA6" : "Disconnected");
            snprintf(line2, sizeof(line2), "Selected Location: %s   \xC2\xB7   Tunnel not active",
                     s.vpn.selected_name[0] ? s.vpn.selected_name : "USA - Miami");
        }
        r_text(&g_ren, 2, line1, 604, 298, 738, 34, 0, von ? 0.525f : 0.953f, von ? 0.937f : 0.965f, von ? 0.675f : 0.984f, 1.0f);
        r_text(&g_ren, 1, line2, 604, 334, 738, 30, 0, 0.635f, 0.682f, 0.769f, 1.0f);

        const char *tags[4]  = { "TOGGLE", "MIAMI 1", "MIAMI 2", "OFF" };
        const char *names[4] = {
            von ? "Disconnect ExpressVPN" : "Connect ExpressVPN",
            "USA - Miami (Primary #54)",
            "USA - Miami - 2 (Backup #202)",
            "Disconnect & Teardown Tunnel"
        };
        const char *metas[4] = {
            von ? "Tunnel Live" : "Ready",
            (von && strcmp(s.vpn.active_id, "54") == 0) ? "Active Now" : (strcmp(s.vpn.selected_id, "54") == 0 ? "Selected" : "Available"),
            (von && strcmp(s.vpn.active_id, "202") == 0) ? "Active Now" : (strcmp(s.vpn.selected_id, "202") == 0 ? "Selected" : "Available"),
            von ? "Instant Rollback" : "Already Off"
        };

        for (int i = 0; i < 4; i++) {
            float ry = 392.0f + i * 86.0f;
            bool foc = (i == g_vpn_modal_idx);
            bool active_row = (i == 1 && von && strcmp(s.vpn.active_id, "54") == 0) ||
                              (i == 2 && von && strcmp(s.vpn.active_id, "202") == 0) ||
                              (i == 0 && von);
            if (foc) {
                r_round_rect(&g_ren, 560, ry, 800, 72, 18, 0.953f, 0.965f, 0.984f, 1.0f, 1, 1, 1, 1.0f, 1.5f);
                r_round_rect(&g_ren, 580, ry + 18, 108, 36, 10, 0.035f, 0.051f, 0.086f, 0.13f, 0, 0, 0, 0, 0);
                r_text(&g_ren, 1, tags[i], 580, ry + 18, 108, 36, 1, 0.035f, 0.051f, 0.086f, 1.0f);
                r_text(&g_ren, 4, names[i], 706, ry, 420, 72, 0, 0.035f, 0.051f, 0.086f, 1.0f);
            } else {
                r_round_rect(&g_ren, 560, ry, 800, 72, 18, 1, 1, 1, 0.055f, 1, 1, 1, 0.09f, 1.5f);
                r_round_rect(&g_ren, 580, ry + 18, 108, 36, 10, 1, 1, 1, 0.10f, 0, 0, 0, 0, 0);
                r_text(&g_ren, 1, tags[i], 580, ry + 18, 108, 36, 1, 0.953f, 0.965f, 0.984f, 1.0f);
                r_text(&g_ren, 4, names[i], 706, ry, 420, 72, 0, 0.953f, 0.965f, 0.984f, 1.0f);
            }
            float dr = active_row ? 0.204f : 0.392f, dg = active_row ? 0.827f : 0.455f, db = active_row ? 0.600f : 0.545f;
            r_round_rect(&g_ren, 1142, ry + 32, 8, 8, 4, dr, dg, db, 1.0f, 0, 0, 0, 0, 0);
            if (foc) r_text(&g_ren, 1, metas[i], 1160, ry, 184, 72, 0, 0.20f, 0.25f, 0.33f, 1.0f);
            else     r_text(&g_ren, 1, metas[i], 1160, ry, 184, 72, 0, 0.635f, 0.682f, 0.769f, 1.0f);
        }

        r_round_rect(&g_ren, 560, 760, 800, 56, 16, 1, 1, 1, 0.04f, 1, 1, 1, 0.09f, 1.0f);
        r_text(&g_ren, 1, "Split-Tunnel Direct", 582, 760, 220, 56, 0, 0.635f, 0.682f, 0.769f, 1.0f);
        r_text(&g_ren, 1, "LAN \xC2\xB7 Plex :32400 \xC2\xB7 Gateway :8765 \xC2\xB7 Casting", 820, 760, 518, 56, 2, 0.953f, 0.965f, 0.984f, 1.0f);
    }

    /* 6. Floating Toast Pill */
    if (g_toast_until_us > t_frame0 && g_toast_msg[0]) {
        r_round_rect(&g_ren, 680, 34, 560, 52, 26, 0.953f, 0.965f, 0.984f, 0.97f, 1, 1, 1, 1.0f, 1.0f);
        r_text(&g_ren, 3, g_toast_msg, 704, 34, 512, 52, 1, 0.035f, 0.051f, 0.086f, 1.0f);
    }

    r_flush(&g_ren);
    if (use_partial) renderer_clear_scissor();
    g_draw_calls_last = r_get_draw_calls();

    if (use_partial && G.eglSwapBuffersWithDamageKHR) {
        EGLint egl_rect[4] = { dmg.x, WIN_H - (dmg.y + dmg.h), dmg.w, dmg.h };
        G.eglSwapBuffersWithDamageKHR(g_egl_dpy, g_egl_surf, egl_rect, 1);
    } else {
        G.eglSwapBuffers(g_egl_dpy, g_egl_surf);
    }
    G.wl_display_flush(g_wl_dpy);

    g_dirty_valid = false;
    g_needs_render = false;

    if (g_pending_key_ts_us > 0) {
        double ms = (double)(now_us() - g_pending_key_ts_us) / 1000.0;
        g_last_key_swap_ms = ms;
        g_sum_swap_ms += ms;
        g_swap_samples++;
        g_pending_key_ts_us = 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  WAYLAND LISTENERS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void kbd_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t sz) { (void)d;(void)k;(void)f;(void)sz; if (fd >= 0) close(fd); }
static void kbd_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, void *ks) { (void)d;(void)k;(void)s;(void)sf;(void)ks; }
static void kbd_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) { (void)d;(void)k;(void)s;(void)sf; }
static void kbd_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t g) { (void)d;(void)k;(void)s;(void)a;(void)b;(void)c;(void)g; }
static void kbd_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t dl) { (void)d;(void)k;(void)r;(void)dl; }

static void kbd_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time_ms, uint32_t key, uint32_t state) {
    (void)d; (void)k; (void)serial; (void)time_ms;
    if (state == 1 /* WL_KEYBOARD_KEY_STATE_PRESSED */) {
        handle_evdev_key(key);
    }
}

static const struct wl_keyboard_listener KBD_LISTENER = {
    .keymap = kbd_keymap, .enter = kbd_enter, .leave = kbd_leave,
    .key = kbd_key, .modifiers = kbd_mods, .repeat_info = kbd_repeat
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps) {
    (void)d;
    if ((caps & 2 /* WL_SEAT_CAPABILITY_KEYBOARD */) && !g_wl_kbd) {
        g_wl_kbd = (struct wl_keyboard *)G.wl_proxy_marshal_constructor(
            (struct wl_proxy *)seat, WL_SEAT_GET_KEYBOARD, G.wl_keyboard_iface, NULL);
        if (g_wl_kbd) G.wl_proxy_add_listener((struct wl_proxy *)g_wl_kbd, (void (**)(void))&KBD_LISTENER, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *seat, const char *name) { (void)d;(void)seat;(void)name; }
static const struct wl_seat_listener SEAT_LISTENER = { .capabilities = seat_caps, .name = seat_name };

static void ssurf_ping(void *d, struct wl_shell_surface *ss, uint32_t serial) {
    (void)d;
    G.wl_proxy_marshal((struct wl_proxy *)ss, WL_SHELL_SURFACE_PONG, serial);
}
static void ssurf_cfg(void *d, struct wl_shell_surface *ss, uint32_t e, int32_t w, int32_t h) { (void)d;(void)ss;(void)e;(void)w;(void)h; }
static void ssurf_pop(void *d, struct wl_shell_surface *ss) { (void)d;(void)ss; }
static const struct wl_shell_surface_listener SSURF_LISTENER = { .ping = ssurf_ping, .configure = ssurf_cfg, .popup_done = ssurf_pop };

static void tz_res_id(void *d, struct wl_proxy *res, uint32_t id) {
    (void)d; (void)res;
    g_wl_surface_id = id;
}
static const struct tizen_resource_listener TZ_RES_LISTENER = { .resource_id = tz_res_id };

static void reg_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (strcmp(iface, "wl_compositor") == 0) {
        uint32_t v = ver < 4 ? ver : 4;
        g_wl_comp = (struct wl_compositor *)G.wl_proxy_marshal_constructor_versioned(
            (struct wl_proxy *)reg, WL_REGISTRY_BIND, G.wl_compositor_iface, v, name, iface, v, NULL);
    } else if (strcmp(iface, "wl_shell") == 0) {
        g_wl_shell = (struct wl_shell *)G.wl_proxy_marshal_constructor_versioned(
            (struct wl_proxy *)reg, WL_REGISTRY_BIND, G.wl_shell_iface, 1, name, iface, 1, NULL);
    } else if (strcmp(iface, "wl_seat") == 0) {
        uint32_t v = ver < 4 ? ver : 4;
        g_wl_seat = (struct wl_seat *)G.wl_proxy_marshal_constructor_versioned(
            (struct wl_proxy *)reg, WL_REGISTRY_BIND, G.wl_seat_iface, v, name, iface, v, NULL);
        if (g_wl_seat) G.wl_proxy_add_listener((struct wl_proxy *)g_wl_seat, (void (**)(void))&SEAT_LISTENER, NULL);
    } else if (strcmp(iface, "tizen_policy") == 0 && G.tizen_policy_iface) {
        uint32_t v = ver < 13 ? ver : 13;
        g_tz_policy = G.wl_proxy_marshal_constructor_versioned(
            (struct wl_proxy *)reg, WL_REGISTRY_BIND, G.tizen_policy_iface, v, name, iface, v, NULL);
    } else if (strcmp(iface, "tizen_keyrouter") == 0 && G.tizen_keyrouter_iface) {
        uint32_t v = ver < 2 ? ver : 2;
        g_tz_keyrouter = G.wl_proxy_marshal_constructor_versioned(
            (struct wl_proxy *)reg, WL_REGISTRY_BIND, G.tizen_keyrouter_iface, v, name, iface, v, NULL);
    } else if (strcmp(iface, "tizen_surface") == 0 && G.tizen_surface_iface) {
        g_tz_surface = G.wl_proxy_marshal_constructor_versioned(
            (struct wl_proxy *)reg, WL_REGISTRY_BIND, G.tizen_surface_iface, 1, name, iface, 1, NULL);
    }
}
static void reg_remove(void *d, struct wl_registry *reg, uint32_t name) { (void)d;(void)reg;(void)name; }
static const struct wl_registry_listener REG_LISTENER = { .global = reg_global, .global_remove = reg_remove };

/* ═══════════════════════════════════════════════════════════════════════════
 *  LOOPBACK HTTP API + DYNAMIC KEYBIND SERVICE + BACKGROUND WORKER THREAD
 *  (Thread 2 of 2: ZERO thread pool bloat!)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void json_escape(char *dst, size_t cap, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 2 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = (char)c; }
        else if (c < 32) { dst[j++] = ' '; }
        else dst[j++] = (char)c;
    }
    dst[j] = 0;
}

static bool body_get_str(const char *body, const char *key, char *out, size_t cap) {
    return json_get_str(body, body + strlen(body), key, out, cap);
}

static bool body_get_int(const char *body, const char *key, int *out) {
    return json_get_int(body, body + strlen(body), key, out);
}

static void send_http(int fd, int code, const char *json) {
    char hdr[256];
    size_t blen = strlen(json);
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        code, code == 200 ? "OK" : "Bad Request", blen);
    (void)send(fd, hdr, (size_t)hlen, MSG_NOSIGNAL);
    (void)send(fd, json, blen, MSG_NOSIGNAL);
}

static void handle_http_client(int cfd) {
    char req[4096];
    struct pollfd p = { .fd = cfd, .events = POLLIN };
    if (poll(&p, 1, 400) <= 0) { close(cfd); return; }
    ssize_t n = recv(cfd, req, sizeof(req) - 1, 0);
    if (n <= 0) { close(cfd); return; }
    req[n] = 0;

    char method[12] = {0}, url[256] = {0};
    sscanf(req, "%11s %255s", method, url);
    char *body = strstr(req, "\r\n\r\n");
    body = body ? (body + 4) : "";

    /* If Content-Length indicates more body bytes, read them */
    const char *cl = strcasestr(req, "Content-Length:");
    if (cl) {
        int want = atoi(cl + 15);
        int have = (int)(req + n - body);
        while (have < want && n < (ssize_t)sizeof(req) - 1 && poll(&p, 1, 300) > 0) {
            ssize_t r = recv(cfd, req + n, sizeof(req) - 1 - (size_t)n, 0);
            if (r <= 0) break;
            n += r; req[n] = 0; have += (int)r;
        }
        req[n] = 0;
    }

    char *q = strchr(url, '?');
    char *query = "";
    if (q) { *q++ = 0; query = q; }

    static char resp[8192];

    if (strcmp(url, "/api/status") == 0) {
        if (strstr(query, "fresh=1")) {
            HubSnapshot fresh;
            collect_snapshot(&fresh);
            pthread_mutex_lock(&g_lock);
            g_snap = fresh;
            push_cmd_locked(CMD_SNAP_UPDATED, "", "");
            pthread_mutex_unlock(&g_lock);
        }

        HubSnapshot s;
        char focus[48], last_sel[32], last_act[96], last_ex[128], last_k[48], gl_r[128], guard_l[96];
        int binds_n, trims;
        bool vis, hot, grab;
        double kh_us, ks_ms, avg_ms;
        int samples, dcalls;
        pthread_mutex_lock(&g_lock);
        s = g_snap;
        if (g_zone == ZONE_HERO) snprintf(focus, sizeof(focus), "hero:%s", HERO_IDS[g_hero_idx]);
        else if (g_zone == ZONE_DOCK) snprintf(focus, sizeof(focus), "dock:%s", DOCK_ACTIONS[g_dock_idx]);
        else if (g_zone == ZONE_VPN_MODAL) snprintf(focus, sizeof(focus), "vpn_modal:%d", g_vpn_modal_idx);
        else snprintf(focus, sizeof(focus), "modal:%d", g_modal_idx);
        json_escape(last_sel, sizeof(last_sel), g_last_selected);
        json_escape(last_act, sizeof(last_act), g_last_action);
        json_escape(last_ex, sizeof(last_ex), g_last_exec);
        json_escape(last_k, sizeof(last_k), g_last_key);
        json_escape(gl_r, sizeof(gl_r), g_gl_renderer);
        json_escape(guard_l, sizeof(guard_l), g_last_guard_log);
        binds_n = g_bind_count; trims = g_auto_guard_trims;
        vis = g_visible; hot = g_hot_sleeping; grab = g_home_grab_active;
        kh_us = g_last_key_handler_us; ks_ms = g_last_key_swap_ms;
        samples = g_swap_samples; avg_ms = samples > 0 ? (g_sum_swap_ms / samples) : 0.0;
        dcalls = g_draw_calls_last;
        pthread_mutex_unlock(&g_lock);

        char media_json[768];
        media_os_status_json(media_json, sizeof(media_json));
        char v_sel[64], v_auth[64], v_act[64], v_err[128];
        json_escape(v_sel, sizeof(v_sel), s.vpn.selected_name);
        json_escape(v_auth, sizeof(v_auth), s.vpn.auth_name);
        json_escape(v_act, sizeof(v_act), s.vpn.active_name);
        json_escape(v_err, sizeof(v_err), s.vpn.err_msg);

        char c_room[64], c_plex[80], c_gw[160], c_hdmi[64], c_hport[32];
        json_escape(c_room, sizeof(c_room), g_cfg.room_title);
        json_escape(c_plex, sizeof(c_plex), g_cfg.plex_host);
        json_escape(c_gw, sizeof(c_gw), g_cfg.gateway_url);
        json_escape(c_hdmi, sizeof(c_hdmi), g_cfg.hdmi1_name);
        json_escape(c_hport, sizeof(c_hport), g_cfg.hdmi1_port);

        snprintf(resp, sizeof(resp),
            "{\"engine\":\"bare-metal-c-gles2-astc\",\"glRenderer\":\"%s\",\"wlSurfaceId\":%u,\"visible\":%s,\"hotSleeping\":%s,"
            "\"partialUpdate\":%s,\"homeGrabActive\":%s,\"customKeybinds\":%d,\"winSize\":\"1920x1080\","
            "\"config\":{\"roomTitle\":\"%s\",\"plexHost\":\"%s\",\"plexPort\":%d,\"gatewayUrl\":\"%s\",\"mediaOsEnabled\":%s,\"hdmi1Name\":\"%s\",\"hdmi1Port\":\"%s\"},"
            "\"uiFocus\":\"%s\",\"lastSelected\":\"%s\",\"lastAction\":\"%s\",\"lastExec\":\"%s\",\"lastKey\":\"%s\","
            "\"autoGuard\":{\"trims\":%d,\"last\":\"%s\"},"
            "\"perf\":{\"lastKeyHandlerUs\":%.1f,\"lastKeySwapMs\":%.2f,\"avgKeySwapMs\":%.2f,\"swapSamples\":%d,\"drawCallsLastFrame\":%d,\"gc\":0},"
            "\"ip\":\"%s\",\"memAvailableMiB\":%d,\"memTotalMiB\":%d,\"volume\":%d,\"muted\":%s,"
            "\"activeSource\":%d,\"signalStatus\":%d,"
            "\"plex\":{\"appId\":\"%s\",\"running\":%s,\"serverOnline\":%s,\"latencyMs\":%d,\"serverVersion\":\"%s\"},"
            "\"youtube\":{\"appId\":\"%s\",\"running\":%s},"
            "\"switch2\":{\"sourceType\":%d,\"port\":\"%s\",\"name\":\"%s\",\"connected\":%s,\"powerState\":%d,\"cecPower\":%d,\"active\":%s},"
            "\"casting\":{\"airplay\":%s,\"dlna\":%s,\"allshare\":%s},"
            "\"vpn\":{\"available\":%s,\"status\":\"%s\",\"tunnelEstablished\":%s,\"selectedActiveDiverged\":%s,"
            "\"selectedId\":\"%s\",\"selectedName\":\"%s\",\"authenticatedId\":\"%s\",\"authenticatedName\":\"%s\","
            "\"activeId\":\"%s\",\"activeName\":\"%s\",\"exitIp\":\"%s\",\"colo\":\"%s\",\"lastError\":\"%s\"},%s}",
            gl_r, g_wl_surface_id, vis?"true":"false", hot?"true":"false", g_has_partial_update?"true":"false",
            grab?"true":"false", binds_n,
            c_room, c_plex, g_cfg.plex_port, c_gw, g_cfg.media_os_enabled?"true":"false", c_hdmi, c_hport,
            focus, last_sel, last_act, last_ex, last_k,
            trims, guard_l, kh_us, ks_ms, avg_ms, samples, dcalls,
            s.ip, s.mem_avail_mib, s.mem_total_mib, s.volume, s.muted?"true":"false",
            s.active_source, s.signal_status,
            PLEX_APP_ID, s.plex_running?"true":"false", s.plex_online?"true":"false", s.plex_latency_ms, s.plex_version,
            YOUTUBE_APP_ID, s.yt_running?"true":"false",
            g_cfg.hdmi1_source_type, c_hport, c_hdmi, s.sw_connected?"true":"false", s.sw_power, s.sw_cec_power, s.sw_active?"true":"false",
            s.airplay_running?"true":"false", s.dmr_running?"true":"false", s.allshare_running?"true":"false",
            s.vpn.available?"true":"false", s.vpn.status, s.vpn.tunnel_established?"true":"false", s.vpn.diverged?"true":"false",
            s.vpn.selected_id, v_sel, s.vpn.auth_id, v_auth, s.vpn.active_id, v_act, s.vpn.exit_ip, s.vpn.colo, v_err,
            media_json);
        send_http(cfd, 200, resp);
    } else if (strcmp(url, "/api/key") == 0) {
        char k[32] = {0};
        const char *kp = strstr(query, "k=");
        if (kp) {
            kp += 2;
            size_t i = 0;
            while (kp[i] && kp[i] != '&' && i + 1 < sizeof(k)) { k[i] = (char)toupper((unsigned char)kp[i]); i++; }
            k[i] = 0;
        }
        if (k[0]) {
            pthread_mutex_lock(&g_lock);
            push_cmd_locked(CMD_KEY_NAME, k, "");
            pthread_mutex_unlock(&g_lock);
            snprintf(resp, sizeof(resp), "{\"ok\":true,\"key\":\"%s\"}", k);
            send_http(cfd, 200, resp);
        } else {
            send_http(cfd, 400, "{\"ok\":false,\"error\":\"missing k\"}");
        }
    } else if (strcmp(url, "/api/action") == 0) {
        char act[64] = {0}, arg[128] = {0};
        if (body[0]) {
            body_get_str(body, "action", act, sizeof(act));
            body_get_str(body, "arg", arg, sizeof(arg));
            if (!arg[0]) {
                int st = -1;
                if (body_get_int(body, "sourceType", &st)) snprintf(arg, sizeof(arg), "%d", st);
            }
        }
        if (!act[0]) {
            const char *np = strstr(query, "name=");
            if (np) {
                np += 5;
                size_t i = 0;
                while (np[i] && np[i] != '&' && i + 1 < sizeof(act)) { act[i] = np[i]; i++; }
                act[i] = 0;
            }
        }
        if (!arg[0]) {
            const char *ap = strstr(query, "arg=");
            if (ap) {
                ap += 4;
                size_t i = 0;
                while (ap[i] && ap[i] != '&' && i + 1 < sizeof(arg)) { arg[i] = ap[i]; i++; }
                arg[i] = 0;
            }
        }
        if (act[0]) {
            pthread_mutex_lock(&g_lock);
            push_cmd_locked(CMD_ACTION, act, arg);
            pthread_mutex_unlock(&g_lock);
            snprintf(resp, sizeof(resp), "{\"ok\":true,\"action\":\"%s\"}", act);
            send_http(cfd, 200, resp);
        } else {
            send_http(cfd, 400, "{\"ok\":false,\"error\":\"missing action\"}");
        }
    } else if (strcmp(url, "/api/keybinds") == 0) {
        if (strcmp(method, "POST") == 0) {
            char op[24] = {0};
            body_get_str(body, "op", op, sizeof(op));
            if (strcmp(op, "set") == 0) {
                int xkb = 0, ev = 0, force = 0;
                char act[48] = {0}, arg[128] = {0}, lbl[64] = {0};
                body_get_int(body, "xkb", &xkb);
                body_get_int(body, "evdev", &ev);
                body_get_int(body, "force", &force);
                body_get_str(body, "action", act, sizeof(act));
                body_get_str(body, "arg", arg, sizeof(arg));
                body_get_str(body, "label", lbl, sizeof(lbl));
                if (xkb <= 0 && ev > 0) xkb = ev + 8;
                if (ev <= 0 && xkb > 8) ev = xkb - 8;
                if (xkb < 9 || !act[0] || is_protected_evdev((uint32_t)ev, force != 0)) {
                    send_http(cfd, 400, "{\"ok\":false,\"error\":\"invalid_or_protected_key\"}");
                } else {
                    pthread_mutex_lock(&g_lock);
                    int slot = -1;
                    for (int i = 0; i < g_bind_count; i++) if (g_binds[i].xkb == (uint32_t)xkb) { slot = i; break; }
                    if (slot < 0 && g_bind_count < MAX_KEYBINDS) slot = g_bind_count++;
                    if (slot >= 0) {
                        g_binds[slot].xkb = (uint32_t)xkb;
                        g_binds[slot].evdev = (uint32_t)ev;
                        snprintf(g_binds[slot].action, sizeof(g_binds[slot].action), "%s", act);
                        snprintf(g_binds[slot].arg, sizeof(g_binds[slot].arg), "%s", arg);
                        snprintf(g_binds[slot].label, sizeof(g_binds[slot].label), "%s", lbl);
                        g_binds[slot].grabbed = false;
                        g_binds_dirty = true;
                        keybinds_save_locked();
                        push_cmd_locked(CMD_SYNC_BINDS, "", "");
                    }
                    pthread_mutex_unlock(&g_lock);
                    snprintf(resp, sizeof(resp), "{\"ok\":true,\"xkb\":%d,\"evdev\":%d,\"action\":\"%s\"}", xkb, ev, act);
                    send_http(cfd, 200, resp);
                }
            } else if (strcmp(op, "del") == 0) {
                int xkb = 0, ev = 0;
                body_get_int(body, "xkb", &xkb);
                body_get_int(body, "evdev", &ev);
                if (xkb <= 0 && ev > 0) xkb = ev + 8;
                bool removed = false;
                pthread_mutex_lock(&g_lock);
                for (int i = 0; i < g_bind_count; i++) {
                    if (g_binds[i].xkb == (uint32_t)xkb) {
                        if (g_binds[i].grabbed && g_ungrab_count < MAX_KEYBINDS)
                            g_ungrab_queue[g_ungrab_count++] = g_binds[i].xkb;
                        for (int j = i; j + 1 < g_bind_count; j++) g_binds[j] = g_binds[j + 1];
                        g_bind_count--;
                        removed = true;
                        g_binds_dirty = true;
                        keybinds_save_locked();
                        push_cmd_locked(CMD_SYNC_BINDS, "", "");
                        break;
                    }
                }
                pthread_mutex_unlock(&g_lock);
                snprintf(resp, sizeof(resp), "{\"ok\":%s,\"removed\":%s}", removed?"true":"false", removed?"true":"false");
                send_http(cfd, 200, resp);
            } else if (strcmp(op, "clear") == 0) {
                pthread_mutex_lock(&g_lock);
                for (int i = 0; i < g_bind_count; i++) {
                    if (g_binds[i].grabbed && g_ungrab_count < MAX_KEYBINDS)
                        g_ungrab_queue[g_ungrab_count++] = g_binds[i].xkb;
                }
                g_bind_count = 0;
                g_binds_dirty = true;
                keybinds_save_locked();
                push_cmd_locked(CMD_SYNC_BINDS, "", "");
                pthread_mutex_unlock(&g_lock);
                send_http(cfd, 200, "{\"ok\":true,\"cleared\":true}");
            } else if (strcmp(op, "reload") == 0) {
                pthread_mutex_lock(&g_lock);
                keybinds_load_locked();
                push_cmd_locked(CMD_SYNC_BINDS, "force", "");
                pthread_mutex_unlock(&g_lock);
                send_http(cfd, 200, "{\"ok\":true,\"reloaded\":true}");
            } else if (strcmp(op, "learn") == 0) {
                int secs = 15;
                body_get_int(body, "seconds", &secs);
                if (secs < 1) secs = 1;
                if (secs > 60) secs = 60;
                pthread_mutex_lock(&g_lock);
                g_last_learned_evdev = 0;
                g_last_learned_xkb = 0;
                g_learn_until_us = now_us() + (int64_t)secs * 1000000LL;
                pthread_mutex_unlock(&g_lock);
                snprintf(resp, sizeof(resp), "{\"ok\":true,\"learning\":true,\"seconds\":%d}", secs);
                send_http(cfd, 200, resp);
            } else {
                send_http(cfd, 400, "{\"ok\":false,\"error\":\"unknown_op\"}");
            }
        } else {
            /* GET /api/keybinds */
            pthread_mutex_lock(&g_lock);
            int off = snprintf(resp, sizeof(resp),
                "{\"configFile\":\"%s\",\"count\":%d,\"learning\":%s,\"lastRawEvdev\":%u,\"lastRawXkb\":%u,"
                "\"lastLearnedEvdev\":%u,\"lastLearnedXkb\":%u,\"binds\":[",
                g_keybinds_file, g_bind_count, (g_learn_until_us > now_us())?"true":"false",
                g_last_raw_evdev, g_last_raw_xkb, g_last_learned_evdev, g_last_learned_xkb);
            for (int i = 0; i < g_bind_count && off < (int)sizeof(resp) - 256; i++) {
                char ea[64], er[160], el[80];
                json_escape(ea, sizeof(ea), g_binds[i].action);
                json_escape(er, sizeof(er), g_binds[i].arg);
                json_escape(el, sizeof(el), g_binds[i].label);
                off += snprintf(resp + off, sizeof(resp) - (size_t)off,
                    "%s{\"xkb\":%u,\"evdev\":%u,\"action\":\"%s\",\"arg\":\"%s\",\"label\":\"%s\",\"grabbed\":%s}",
                    i ? "," : "", g_binds[i].xkb, g_binds[i].evdev, ea, er, el, g_binds[i].grabbed?"true":"false");
            }
            snprintf(resp + off, sizeof(resp) - (size_t)off, "]}");
            pthread_mutex_unlock(&g_lock);
            send_http(cfd, 200, resp);
        }
    } else {
        send_http(cfd, 200, "{\"service\":\"living-room-hub-native\",\"endpoints\":[\"/api/status\",\"/api/action\",\"/api/key\",\"/api/keybinds\"]}");
    }
    close(cfd);
}

static void *worker_thread_fn(void *arg) {
    (void)arg;
    /* Load Tizen TV service libraries asynchronously off main thread (keeps cold first paint ~340 ms!) */
    ffi_load_services();
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(HTTP_PORT);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);

    if (bind(sfd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(sfd, 16) == 0) {
        hub_log("Loopback HTTP & Dynamic KeyBind Service listening on 127.0.0.1:%d", HTTP_PORT);
    } else {
        hub_log("WARNING: loopback bind 127.0.0.1:%d failed: %s", HTTP_PORT, strerror(errno));
        close(sfd); sfd = -1;
    }

    while (g_running) {
        struct pollfd pfds[2];
        int nfds = 0;
        pfds[nfds].fd = g_work_efd; pfds[nfds].events = POLLIN; nfds++;
        if (sfd >= 0) { pfds[nfds].fd = sfd; pfds[nfds].events = POLLIN; nfds++; }

        int pr = poll(pfds, (nfds_t)nfds, 2000);
        if (pr <= 0) continue;

        if (pfds[0].revents & POLLIN) {
            uint64_t v = 0; (void)read(g_work_efd, &v, sizeof(v));
            for (;;) {
                WorkItem wi;
                bool has_item = false, do_ref = false;
                pthread_mutex_lock(&g_lock);
                if (g_work_head != g_work_tail) {
                    wi = g_work_q[g_work_head];
                    g_work_head = (g_work_head + 1) % WORK_Q_CAP;
                    has_item = true;
                } else if (g_refresh_requested) {
                    g_refresh_requested = false;
                    do_ref = true;
                }
                pthread_mutex_unlock(&g_lock);

                if (has_item) {
                    execute_work_item(wi.action, wi.arg);
                } else if (do_ref) {
                    HubSnapshot fresh;
                    collect_snapshot(&fresh);
                    pthread_mutex_lock(&g_lock);
                    g_snap = fresh;
                    push_cmd_locked(CMD_SNAP_UPDATED, "", "");
                    pthread_mutex_unlock(&g_lock);
                } else {
                    break;
                }
            }
        }

        if (sfd >= 0 && nfds > 1 && (pfds[1].revents & POLLIN)) {
            int cfd = accept(sfd, NULL, NULL);
            if (cfd >= 0) handle_http_client(cfd);
        }
    }
    if (sfd >= 0) close(sfd);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  MAIN ENTRYPOINT — ZERO-OVERHEAD EPOLL EVENT LOOP
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_sig(int sig) { (void)sig; g_running = 0; signal_main(); signal_worker(); }

static int compute_next_timeout_ms(void) {
    if (g_needs_render) return 0;
    if (g_animating) return 14; /* ~60 Hz pacing while animating */
    int64_t t = now_us();
    int64_t soonest = INT64_MAX;
    if (g_hide_at_us > 0 && g_hide_at_us < soonest) soonest = g_hide_at_us;
    if (g_toast_until_us > 0 && g_toast_until_us < soonest) soonest = g_toast_until_us;
    if (g_wake_refresh_at_us > 0 && g_wake_refresh_at_us < soonest) soonest = g_wake_refresh_at_us;
    if (g_hdmi_guard_at_us > 0 && g_hdmi_guard_at_us < soonest) soonest = g_hdmi_guard_at_us;
    if (g_vol_poll_at_us > 0 && g_vol_poll_at_us < soonest) soonest = g_vol_poll_at_us;
    if (g_visible && g_next_poll_us > 0 && g_next_poll_us < soonest) soonest = g_next_poll_us;
    int m_ms = g_visible ? media_os_next_timeout_ms(t) : -1;
    if (m_ms >= 0) {
        int64_t m_at = t + (int64_t)m_ms * 1000LL;
        if (m_at < soonest) soonest = m_at;
    }
    if (soonest == INT64_MAX) return -1; /* Infinite sleep: 0.00% true idle CPU! */
    int64_t diff_ms = (soonest - t + 999LL) / 1000LL;
    return diff_ms < 0 ? 0 : (int)diff_ms;
}

int main(int argc, char **argv) {
    int64_t boot_t0 = now_us();
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    if (argc > 1 && argv[1][0]) snprintf(g_base_dir, sizeof(g_base_dir), "%s", argv[1]);
    snprintf(g_asset_dir, sizeof(g_asset_dir), "%s/assets", g_base_dir);
    snprintf(g_state_file, sizeof(g_state_file), "%s/last-selected.txt", g_base_dir);
    snprintf(g_keybinds_file, sizeof(g_keybinds_file), "%s/keybinds.conf", g_base_dir);
    snprintf(g_actions_dir, sizeof(g_actions_dir), "%s/actions", g_base_dir);
    snprintf(g_config_file, sizeof(g_config_file), "%s/hub.conf", g_base_dir);

    hub_log("Living Room Hub Tier-4 Bare-Metal ARMv7 C engine starting (dir=%s)", g_base_dir);
    load_hub_config();

    if (!ffi_load_graphics()) { hub_log("FATAL: graphics FFI load failed"); return 1; }

    /* Init defaults & load state */
    memset(&g_snap, 0, sizeof(g_snap));
    get_local_ip(g_snap.ip, sizeof(g_snap.ip));
    g_snap.mem_total_mib = 1583;
    g_snap.sw_connected = true; g_snap.sw_power = 2; g_snap.sw_cec_power = -1;
    g_snap.dmr_running = true; g_snap.allshare_running = true;
    load_last_selected();
    for (int i = 0; i < 3; i++) {
        g_card_scale[i] = (i == g_hero_idx) ? 1.032f : 0.985f;
        g_card_yoff[i]  = (i == g_hero_idx) ? -14.0f : 0.0f;
    }

    pthread_mutex_lock(&g_lock);
    keybinds_load_locked();
    pthread_mutex_unlock(&g_lock);

    /* 1. Connect to Enlightenment Wayland compositor */
    g_wl_dpy = G.wl_display_connect(NULL);
    if (!g_wl_dpy) { hub_log("FATAL: wl_display_connect failed"); return 2; }

    g_wl_reg = (struct wl_registry *)G.wl_proxy_marshal_constructor(
        (struct wl_proxy *)g_wl_dpy, WL_DISPLAY_GET_REGISTRY, G.wl_registry_iface, NULL);
    G.wl_proxy_add_listener((struct wl_proxy *)g_wl_reg, (void (**)(void))&REG_LISTENER, NULL);
    G.wl_display_roundtrip(g_wl_dpy);
    G.wl_display_roundtrip(g_wl_dpy);

    if (!g_wl_comp || !g_wl_shell) {
        hub_log("FATAL: missing wl_compositor=%p wl_shell=%p", (void *)g_wl_comp, (void *)g_wl_shell);
        return 3;
    }
    hub_log("Wayland bound: comp=%p shell=%p seat=%p kbd=%p policy=%p keyrouter=%p",
            (void *)g_wl_comp, (void *)g_wl_shell, (void *)g_wl_seat, (void *)g_wl_kbd,
            (void *)g_tz_policy, (void *)g_tz_keyrouter);

    /* 2. Create 1920x1080 fullscreen surface */
    g_wl_surf = (struct wl_surface *)G.wl_proxy_marshal_constructor(
        (struct wl_proxy *)g_wl_comp, WL_COMPOSITOR_CREATE_SURFACE, G.wl_surface_iface, NULL);
    g_wl_ssurf = (struct wl_shell_surface *)G.wl_proxy_marshal_constructor(
        (struct wl_proxy *)g_wl_shell, WL_SHELL_GET_SHELL_SURFACE, G.wl_shell_surface_iface, NULL, g_wl_surf);
    G.wl_proxy_add_listener((struct wl_proxy *)g_wl_ssurf, (void (**)(void))&SSURF_LISTENER, NULL);
    G.wl_proxy_marshal((struct wl_proxy *)g_wl_ssurf, WL_SHELL_SURFACE_SET_TITLE, "Living Room Hub");
    G.wl_proxy_marshal((struct wl_proxy *)g_wl_ssurf, WL_SHELL_SURFACE_SET_TOPLEVEL);
    G.wl_proxy_marshal((struct wl_proxy *)g_wl_ssurf, WL_SHELL_SURFACE_SET_FULLSCREEN, 0, 0, NULL);

    /* 3. Create Mali EGL + GLES2 context (ZERO depth, ZERO stencil, ZERO MSAA!) */
    g_egl_dpy = G.eglGetDisplay(g_wl_dpy);
    EGLint maj = 0, min = 0;
    if (g_egl_dpy == EGL_NO_DISPLAY || !G.eglInitialize(g_egl_dpy, &maj, &min)) {
        hub_log("FATAL: eglInitialize failed"); return 4;
    }
    G.eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };
    EGLConfig cfg = NULL; EGLint ncfg = 0;
    if (!G.eglChooseConfig(g_egl_dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
        hub_log("FATAL: eglChooseConfig failed"); return 5;
    }
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_egl_ctx = G.eglCreateContext(g_egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    g_egl_win = G.wl_egl_window_create(g_wl_surf, WIN_W, WIN_H);
    g_egl_surf = G.eglCreateWindowSurface(g_egl_dpy, cfg, g_egl_win, NULL);
    if (!G.eglMakeCurrent(g_egl_dpy, g_egl_surf, g_egl_surf, g_egl_ctx)) {
        hub_log("FATAL: eglMakeCurrent failed"); return 6;
    }
    G.eglSwapInterval(g_egl_dpy, 1);

    const char *glr = (const char *)G.glGetString(GL_RENDERER);
    snprintf(g_gl_renderer, sizeof(g_gl_renderer), "%s", glr ? glr : "Mali");
    g_has_partial_update = (G.eglSetDamageRegionKHR != NULL && G.eglSwapBuffersWithDamageKHR != NULL);
    hub_log("EGL %d.%d ready · GPU=%s · PartialUpdate=%s", maj, min, g_gl_renderer, g_has_partial_update ? "ON" : "OFF");

    /* 3b. Query Enlightenment compositor wl_surface_id for zero-Ecore hardware video overlay! */
    if (g_tz_surface && G.tizen_resource_iface && g_wl_surf) {
        g_tz_resource = G.wl_proxy_marshal_constructor(
            g_tz_surface, TIZEN_SURFACE_GET_TIZEN_RESOURCE,
            G.tizen_resource_iface, NULL, g_wl_surf);
        if (g_tz_resource) {
            G.wl_proxy_add_listener(g_tz_resource, (void (**)(void))&TZ_RES_LISTENER, NULL);
            G.wl_display_roundtrip(g_wl_dpy);
        }
    }
    hub_log("Wayland compositor wl_surface_id=%u (hardware video plane ready)", g_wl_surface_id);

    /* 4. Upload all hardware ASTC textures + build FreeType SamsungOneUI glyph atlas */
    if (!renderer_init(&g_ren, g_asset_dir)) {
        hub_log("FATAL: renderer_init failed (asset_dir=%s)", g_asset_dir); return 7;
    }

    /* 4b. Initialize embedded Unified Media OS (Plex + YouTube + Kids + Search + Hardware Player) */
    g_wake_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    g_work_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    MediaHostCallbacks mcb = {
        .show_toast = host_show_toast,
        .mark_full_redraw = host_mark_full_redraw,
        .signal_main = host_signal_main,
        .switch_switch2 = host_switch_switch2,
        .launch_stock_plex = host_launch_stock_plex,
        .launch_stock_youtube = host_launch_stock_youtube,
        .open_inputs = host_open_inputs,
        .open_settings = host_open_settings,
        .get_top_status = host_get_top_status,
        .get_wl_surface_id = host_get_wl_surface_id,
        .set_opaque_state = host_set_opaque_state,
        .flush_opaque_cover = host_flush_opaque_cover,
        .gl_delete_textures = host_gl_delete_textures,
    };
    media_os_init(&mcb, g_cfg.gateway_url[0] ? g_cfg.gateway_url : "http://127.0.0.1:1");

    /* 5. Grab HOME + custom keybinds & launch single background worker thread */
    sync_all_keygrabs(true);

    pthread_t worker_tid;
    pthread_create(&worker_tid, NULL, worker_thread_fn, NULL);

    /* 6. First paint & show */
    show_hub();
    render_frame();
    hub_log("READY in %.1f ms (customKeybinds=%d, wlSurfaceId=%u)", (double)(now_us() - boot_t0) / 1000.0, g_bind_count, g_wl_surface_id);

    /* 7. Main epoll loop (TRUE 0.00% CPU when idle!) */
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    int wl_fd = G.wl_display_get_fd(g_wl_dpy);
    int media_fd = media_os_wake_fd();
    struct epoll_event ev1 = { .events = EPOLLIN, .data.fd = wl_fd };
    struct epoll_event ev2 = { .events = EPOLLIN, .data.fd = g_wake_efd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, wl_fd, &ev1);
    epoll_ctl(epfd, EPOLL_CTL_ADD, g_wake_efd, &ev2);
    if (media_fd >= 0) {
        struct epoll_event ev3 = { .events = EPOLLIN, .data.fd = media_fd };
        epoll_ctl(epfd, EPOLL_CTL_ADD, media_fd, &ev3);
    }

    while (g_running) {
        /* Drain pending commands from HTTP/worker thread */
        for (;;) {
            PendingCmd cmd;
            bool has = false;
            pthread_mutex_lock(&g_lock);
            if (g_cmd_head != g_cmd_tail) {
                cmd = g_cmd_q[g_cmd_head];
                g_cmd_head = (g_cmd_head + 1) % CMD_Q_CAP;
                has = true;
            }
            pthread_mutex_unlock(&g_lock);
            if (!has) break;

            if (cmd.type == CMD_KEY_NAME) {
                uint32_t evc = name_to_evdev(cmd.a);
                if (evc > 0) handle_evdev_key(evc);
            } else if (cmd.type == CMD_ACTION) {
                dispatch_action(cmd.a, cmd.b);
            } else if (cmd.type == CMD_SYNC_BINDS) {
                sync_all_keygrabs(strcmp(cmd.a, "force") == 0);
            } else if (cmd.type == CMD_SNAP_UPDATED) {
                if (g_visible) mark_full_redraw(2);
            } else if (cmd.type == CMD_TOAST) {
                int dms = atoi(cmd.a);
                show_toast(cmd.b, dms > 0 ? dms : 2000);
            } else if (cmd.type == CMD_MEDIA_WAKE) {
                if (g_visible && media_os_is_active()) mark_full_redraw(2);
            }
        }

        /* Service monotonic timers & embedded Media OS tick (always drains ASTC wake pipe cleanly!) */
        int64_t t = now_us();
        if (media_os_tick(t) && g_visible) {
            mark_full_redraw(2);
        }
        if (g_hide_at_us > 0 && t >= g_hide_at_us) {
            hide_hub(g_hide_force_cold);
        }
        if (g_toast_until_us > 0 && t >= g_toast_until_us) {
            g_toast_until_us = 0;
            g_toast_msg[0] = 0;
            if (g_visible) mark_full_redraw(2);
        }
        if (g_vol_poll_at_us > 0 && t >= g_vol_poll_at_us) {
            if (g_visible) {
                push_work("poll_volume", "");
                g_vol_poll_at_us = t + 2000000LL;
            } else {
                g_vol_poll_at_us = 0;
            }
        }
        if (g_wake_refresh_at_us > 0 && t >= g_wake_refresh_at_us) {
            g_wake_refresh_at_us = 0;
            if (g_visible) request_refresh();
        }
        if (g_hdmi_guard_at_us > 0 && t >= g_hdmi_guard_at_us) {
            g_hdmi_guard_at_us = 0;
            if (!g_visible) push_work("hdmi_guard", "");
        }
        if (g_visible && g_next_poll_us > 0 && t >= g_next_poll_us) {
            g_next_poll_us = t + 20000000LL;
            request_refresh();
        }

        /* Render frame if visible & needed */
        if (g_visible && (g_needs_render || g_animating)) {
            render_frame();
        }

        /* Wayland prepare_read + epoll_wait */
        while (G.wl_display_prepare_read(g_wl_dpy) != 0) {
            G.wl_display_dispatch_pending(g_wl_dpy);
        }
        G.wl_display_flush(g_wl_dpy);

        int timeout_ms = compute_next_timeout_ms();
        struct epoll_event evs[8];
        int nr = epoll_wait(epfd, evs, 8, timeout_ms);
        bool wl_ready = false;
        for (int i = 0; i < nr; i++) {
            if (evs[i].data.fd == wl_fd) wl_ready = true;
            else if (evs[i].data.fd == g_wake_efd) {
                uint64_t v = 0; (void)read(g_wake_efd, &v, sizeof(v));
            }
        }
        if (wl_ready) {
            if (G.wl_display_read_events(g_wl_dpy) < 0) break;
            if (G.wl_display_dispatch_pending(g_wl_dpy) < 0) break;
        } else {
            G.wl_display_cancel_read(g_wl_dpy);
        }
    }

    hub_log("Shutting down cleanly");
    media_os_shutdown();
    if (g_home_grab_active) { keyrouter_grab(71, false); keyrouter_grab(180, false); }
    for (int i = 0; i < g_bind_count; i++) if (g_binds[i].grabbed) keyrouter_grab(g_binds[i].xkb, false);
    G.wl_display_flush(g_wl_dpy);
    pthread_join(worker_tid, NULL);
    close(epfd); close(g_wake_efd); close(g_work_efd);
    return 0;
}
