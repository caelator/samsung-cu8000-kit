#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../native-c/renderer.h"

/* Host callbacks supplied by main.c so media_os stays cleanly decoupled */
typedef struct {
    void (*show_toast)(const char *msg, int dur_ms);
    void (*mark_full_redraw)(int frames);
    void (*signal_main)(void);
    void (*switch_switch2)(void);
    void (*launch_stock_plex)(void);
    void (*launch_stock_youtube)(void);
    void (*open_inputs)(void);
    void (*open_settings)(void);
    void (*get_top_status)(int *vol, bool *muted, int *mem_avail_mib, bool *sw_connected);
    uint32_t (*get_wl_surface_id)(void);
    void (*set_opaque_state)(bool opaque);
    void (*flush_opaque_cover)(bool pure_black);
    void (*gl_delete_textures)(GLsizei n, const GLuint *ids);
} MediaHostCallbacks;

typedef enum {
    MEDIA_VIEW_NONE = 0,     /* Classic 3-card Living Room Hub launcher */
    MEDIA_VIEW_HOME,         /* Unified Continue Watching + YouTube + Recent Movies/TV */
    MEDIA_VIEW_MOVIES,       /* Plex Movies virtualized poster grid */
    MEDIA_VIEW_TV,           /* Plex TV Shows virtualized poster grid */
    MEDIA_VIEW_YOUTUBE,      /* YouTube Subscriptions / Home / Watch Later / History */
    MEDIA_VIEW_KIDS,         /* Dedicated Kids Hub (Movies + TV + ytdl-sub, no PIN) */
    MEDIA_VIEW_SEARCH,       /* Unified Search (Plex + YouTube + Jellyseerr 1-click) */
    MEDIA_VIEW_DETAIL,       /* Movie / Show / Season / Episode detail sheet */
    MEDIA_VIEW_YT_CHANNEL,   /* Dedicated YouTube Channel page (Adults or Kids) */
    MEDIA_VIEW_PLAYER        /* Fullscreen hardware video plane + transparent GLES2 HUD */
} MediaViewMode;

void media_os_init(const MediaHostCallbacks *cb, const char *gateway_base_url);
void media_os_shutdown(void);

/* Returns wake fd (pipe) to register in main epoll loop so async HTTP/ASTC wakes rendering cleanly. */
int  media_os_wake_fd(void);

/* True whenever Media OS owns the screen (any view != MEDIA_VIEW_NONE). */
bool media_os_is_active(void);
bool media_os_is_playing(void);
MediaViewMode media_os_view(void);

/* Enter a Media OS tab directly from Living Room Hub or HTTP API. */
void media_os_open_view(MediaViewMode mode);
void media_os_play_plex(const char *rating_key);
void media_os_play_youtube(const char *video_id);
void media_os_exit_to_hub(void);

/* Called on HDMI / Hide / Cold-Sleep: pauses/stops playback, syncs timeline, flushes ASTC VRAM. */
void media_os_on_hub_hidden(bool flush_vram);

/* Called on GL thread from main loop: pumps ASTC uploads, player polling, skip markers, HUD auto-hide.
 * Returns true if a redraw is needed this tick. */
bool media_os_tick(int64_t now_us);

/* Recommended epoll timeout in ms (e.g. 250ms during active playback for clock/subtitles, -1 when idle). */
int  media_os_next_timeout_ms(int64_t now_us);

/* Handle Linux evdev keycode when Media OS is active. Returns true if consumed. */
bool media_os_handle_key(uint32_t ev);

/* Render active Media OS view (1920x1080) onto Mali GLES2 surface. */
void media_os_render(Renderer *r, int64_t now_us);

/* Append compact JSON telemetry snippet into buf for /api/status. */
void media_os_status_json(char *buf, size_t cap);
