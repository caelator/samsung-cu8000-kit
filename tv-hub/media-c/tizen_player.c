#include "tizen_player.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *player_h;

typedef struct {
    void *lib;
    int (*player_create)(player_h *);
    int (*player_destroy)(player_h);
    int (*player_set_uri)(player_h, const char *);
    /* Verified from libcapi-media-player.so.0 disassembly (0x1b1c8):
     * (player, display_type=0 OVERLAY, wl_surface_id, height, width, x, y) */
    int (*player_set_display_webapp)(player_h, int, int, int, int, int, int);
    int (*player_set_display_visible)(player_h, bool);
    int (*player_set_display_mode)(player_h, int);
    int (*player_set_streaming_buffering_time)(player_h, int, int);
    int (*player_set_UHD_resolution)(player_h, bool);
    int (*player_set_hd_audio_mode)(player_h, int);
    int (*player_prepare)(player_h);
    int (*player_unprepare)(player_h);
    int (*player_start)(player_h);
    int (*player_pause)(player_h);
    int (*player_stop)(player_h);
    int (*player_get_state)(player_h, int *);
    int (*player_get_play_position)(player_h, int *);
    int (*player_set_play_position)(player_h, int, bool, void (*)(void *), void *);
    int (*player_get_duration)(player_h, int *);
    int (*player_get_video_size)(player_h, int *, int *);
    int (*player_get_track_count)(player_h, int, int *);
    int (*player_get_current_track)(player_h, int, int *);
    int (*player_select_track)(player_h, int, int);
} PlayerFFI;

#define MAX_MARKERS 32

struct TizenPlayer {
    PlayerFFI f;
    bool loaded;
    player_h handle;
    pthread_mutex_t mu;
    bool active;
    bool preparing;
    bool cancel_requested;
    int last_rc;
    char last_err[128];

    /* Subtitle cues (sorted by start_ms) */
    SubCue *cues;
    int cue_count;

    /* Skip markers (intro, credits, SponsorBlock) */
    SkipMarker markers[MAX_MARKERS];
    int marker_count;
};

#define PSYM(lib, name) do { tp->f.name = dlsym(lib, #name); } while (0)

TizenPlayer *tizen_player_create(void) {
    TizenPlayer *tp = calloc(1, sizeof(TizenPlayer));
    if (!tp) return NULL;
    pthread_mutex_init(&tp->mu, NULL);

    void *lib = dlopen("libcapi-media-player.so.0", RTLD_LAZY);
    if (!lib) lib = dlopen("libcapi-media-player.so", RTLD_LAZY);
    if (!lib) {
        snprintf(tp->last_err, sizeof(tp->last_err), "dlopen libcapi-media-player failed: %s", dlerror());
        return tp;
    }
    tp->f.lib = lib;
    PSYM(lib, player_create);
    PSYM(lib, player_destroy);
    PSYM(lib, player_set_uri);
    PSYM(lib, player_set_display_webapp);
    PSYM(lib, player_set_display_visible);
    PSYM(lib, player_set_display_mode);
    PSYM(lib, player_set_streaming_buffering_time);
    PSYM(lib, player_set_UHD_resolution);
    PSYM(lib, player_set_hd_audio_mode);
    PSYM(lib, player_prepare);
    PSYM(lib, player_unprepare);
    PSYM(lib, player_start);
    PSYM(lib, player_pause);
    PSYM(lib, player_stop);
    PSYM(lib, player_get_state);
    PSYM(lib, player_get_play_position);
    PSYM(lib, player_set_play_position);
    PSYM(lib, player_get_duration);
    PSYM(lib, player_get_video_size);
    PSYM(lib, player_get_track_count);
    PSYM(lib, player_get_current_track);
    PSYM(lib, player_select_track);

    tp->loaded = (tp->f.player_create && tp->f.player_destroy && tp->f.player_set_uri &&
                  tp->f.player_set_display_webapp && tp->f.player_prepare && tp->f.player_start);
    if (!tp->loaded) {
        snprintf(tp->last_err, sizeof(tp->last_err), "missing core player_* symbols");
    }
    return tp;
}

bool tizen_player_is_available(TizenPlayer *tp) {
    return tp && tp->loaded;
}

static void destroy_handle_locked(TizenPlayer *tp) {
    if (!tp->handle) return;
    int st = TP_STATE_NONE;
    if (tp->f.player_get_state) tp->f.player_get_state(tp->handle, &st);
    if (st == TP_STATE_PLAYING || st == TP_STATE_PAUSED) {
        if (tp->f.player_stop) tp->f.player_stop(tp->handle);
    }
    if (st >= TP_STATE_READY) {
        if (tp->f.player_unprepare) tp->f.player_unprepare(tp->handle);
    }
    tp->f.player_destroy(tp->handle);
    tp->handle = NULL;
    tp->active = false;
}

void tizen_player_stop(TizenPlayer *tp) {
    if (!tp) return;
    pthread_mutex_lock(&tp->mu);
    if (tp->preparing) {
        /* Never destroy handle out from under blocking player_prepare(); let play_sync abort safely. */
        tp->cancel_requested = true;
        tp->active = false;
    } else {
        destroy_handle_locked(tp);
    }
    pthread_mutex_unlock(&tp->mu);
}

void tizen_player_destroy(TizenPlayer *tp) {
    if (!tp) return;
    tizen_player_stop(tp);
    tizen_player_clear_cues(tp);
    /* Keep libcapi-media-player.so.0 resident (its muse_core thread may outlive destroy). */
    pthread_mutex_destroy(&tp->mu);
    free(tp);
}

int tizen_player_play_sync(TizenPlayer *tp, uint32_t wl_surface_id,
                           const char *uri, int start_ms, int x, int y, int w, int h) {
    if (!tp || !tp->loaded || !uri || !uri[0]) return -1;

    pthread_mutex_lock(&tp->mu);
    destroy_handle_locked(tp);
    tp->preparing = true;
    tp->cancel_requested = false;
    tp->last_err[0] = '\0';

    player_h p = NULL;
    int rc = tp->f.player_create(&p);
    if (rc != 0 || !p) {
        tp->last_rc = rc;
        snprintf(tp->last_err, sizeof(tp->last_err), "player_create=%d", rc);
        tp->preparing = false;
        pthread_mutex_unlock(&tp->mu);
        return rc ? rc : -2;
    }

    /* Bind hardware video overlay directly to our bare-metal Wayland surface_id! */
    rc = tp->f.player_set_display_webapp(p, 0 /* OVERLAY */, (int)wl_surface_id, h, w, x, y);
    if (rc != 0) {
        tp->last_rc = rc;
        snprintf(tp->last_err, sizeof(tp->last_err), "set_display_webapp(sid=%u)=%d", wl_surface_id, rc);
        tp->f.player_destroy(p);
        tp->preparing = false;
        pthread_mutex_unlock(&tp->mu);
        return rc;
    }
    if (tp->f.player_set_display_mode) tp->f.player_set_display_mode(p, 0 /* LETTER_BOX */);
    if (tp->f.player_set_display_visible) tp->f.player_set_display_visible(p, true);

    rc = tp->f.player_set_uri(p, uri);
    if (rc != 0) {
        tp->last_rc = rc;
        snprintf(tp->last_err, sizeof(tp->last_err), "set_uri=%d", rc);
        tp->f.player_destroy(p);
        tp->preparing = false;
        pthread_mutex_unlock(&tp->mu);
        return rc;
    }
    if (tp->f.player_set_streaming_buffering_time) {
        tp->f.player_set_streaming_buffering_time(p, 1000, 1500);
    }

    tp->handle = p;
    pthread_mutex_unlock(&tp->mu);

    /* Blocking prepare outside mutex so UI polling never stalls */
    rc = tp->f.player_prepare(p);

    pthread_mutex_lock(&tp->mu);
    if (tp->cancel_requested || tp->handle != p) {
        /* Stopped/replaced while preparing */
        if (rc == 0 && tp->f.player_unprepare) tp->f.player_unprepare(p);
        tp->f.player_destroy(p);
        if (tp->handle == p) tp->handle = NULL;
        tp->preparing = false;
        tp->cancel_requested = false;
        tp->active = false;
        pthread_mutex_unlock(&tp->mu);
        return -99;
    }
    if (rc != 0) {
        tp->last_rc = rc;
        snprintf(tp->last_err, sizeof(tp->last_err), "player_prepare=%d", rc);
        destroy_handle_locked(tp);
        tp->preparing = false;
        pthread_mutex_unlock(&tp->mu);
        return rc;
    }

    if (start_ms > 2000 && tp->f.player_set_play_position) {
        tp->f.player_set_play_position(p, start_ms, true, NULL, NULL);
    }

    rc = tp->f.player_start(p);
    tp->last_rc = rc;
    tp->preparing = false;
    if (rc == 0) {
        tp->active = true;
    } else {
        snprintf(tp->last_err, sizeof(tp->last_err), "player_start=%d", rc);
        destroy_handle_locked(tp);
    }
    pthread_mutex_unlock(&tp->mu);
    return rc;
}

typedef struct {
    TizenPlayer *tp;
    uint32_t sid;
    char uri[1024];
    int start_ms, x, y, w, h;
} AsyncPlayArgs;

static void *async_play_thread(void *arg) {
    AsyncPlayArgs *a = (AsyncPlayArgs *)arg;
    tizen_player_play_sync(a->tp, a->sid, a->uri, a->start_ms, a->x, a->y, a->w, a->h);
    free(a);
    return NULL;
}

bool tizen_player_play_async(TizenPlayer *tp, uint32_t wl_surface_id,
                             const char *uri, int start_ms, int x, int y, int w, int h) {
    if (!tp || !tp->loaded || !uri || !uri[0]) return false;
    AsyncPlayArgs *a = calloc(1, sizeof(AsyncPlayArgs));
    if (!a) return false;
    a->tp = tp;
    a->sid = wl_surface_id;
    strncpy(a->uri, uri, sizeof(a->uri) - 1);
    a->start_ms = start_ms;
    a->x = x; a->y = y; a->w = w; a->h = h;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &attr, async_play_thread, a);
    pthread_attr_destroy(&attr);
    if (rc != 0) { free(a); return false; }
    return true;
}

int tizen_player_pause(TizenPlayer *tp) {
    if (!tp) return -1;
    pthread_mutex_lock(&tp->mu);
    int rc = (tp->handle && tp->f.player_pause) ? tp->f.player_pause(tp->handle) : -1;
    pthread_mutex_unlock(&tp->mu);
    return rc;
}

int tizen_player_resume(TizenPlayer *tp) {
    if (!tp) return -1;
    pthread_mutex_lock(&tp->mu);
    int rc = (tp->handle && tp->f.player_start) ? tp->f.player_start(tp->handle) : -1;
    pthread_mutex_unlock(&tp->mu);
    return rc;
}

int tizen_player_toggle_pause(TizenPlayer *tp) {
    if (!tp) return -1;
    pthread_mutex_lock(&tp->mu);
    int rc = -1;
    if (tp->handle && tp->f.player_get_state) {
        int st = TP_STATE_NONE;
        tp->f.player_get_state(tp->handle, &st);
        if (st == TP_STATE_PLAYING && tp->f.player_pause) rc = tp->f.player_pause(tp->handle);
        else if (st == TP_STATE_PAUSED && tp->f.player_start) rc = tp->f.player_start(tp->handle);
        tp->last_rc = rc;
    }
    pthread_mutex_unlock(&tp->mu);
    return rc;
}

int tizen_player_seek_ms(TizenPlayer *tp, int target_ms) {
    if (!tp) return -1;
    if (target_ms < 0) target_ms = 0;
    pthread_mutex_lock(&tp->mu);
    int rc = (tp->handle && tp->f.player_set_play_position)
             ? tp->f.player_set_play_position(tp->handle, target_ms, true, NULL, NULL)
             : -1;
    pthread_mutex_unlock(&tp->mu);
    return rc;
}

int tizen_player_seek_rel_ms(TizenPlayer *tp, int delta_ms) {
    if (!tp) return -1;
    pthread_mutex_lock(&tp->mu);
    int rc = -1;
    if (tp->handle && tp->f.player_get_play_position && tp->f.player_set_play_position) {
        int pos = 0, dur = 0;
        tp->f.player_get_play_position(tp->handle, &pos);
        if (tp->f.player_get_duration) tp->f.player_get_duration(tp->handle, &dur);
        int target = pos + delta_ms;
        if (target < 0) target = 0;
        if (dur > 0 && target > dur - 1000) target = dur - 1000;
        rc = tp->f.player_set_play_position(tp->handle, target, true, NULL, NULL);
    }
    pthread_mutex_unlock(&tp->mu);
    return rc;
}

int tizen_player_select_audio(TizenPlayer *tp, int track_idx) {
    if (!tp) return -1;
    pthread_mutex_lock(&tp->mu);
    int rc = (tp->handle && tp->f.player_select_track)
             ? tp->f.player_select_track(tp->handle, TP_STREAM_AUDIO, track_idx)
             : -1;
    pthread_mutex_unlock(&tp->mu);
    return rc;
}

void tizen_player_poll(TizenPlayer *tp, TizenPlayerSnapshot *out) {
    memset(out, 0, sizeof(*out));
    if (!tp) return;
    pthread_mutex_lock(&tp->mu);
    out->available = tp->loaded;
    out->active = tp->active;
    out->preparing = tp->preparing;
    out->last_rc = tp->last_rc;
    strncpy(out->last_err, tp->last_err, sizeof(out->last_err) - 1);
    if (tp->handle && !tp->preparing) {
        if (tp->f.player_get_state) tp->f.player_get_state(tp->handle, &out->state);
        if (tp->f.player_get_play_position) tp->f.player_get_play_position(tp->handle, &out->pos_ms);
        if (tp->f.player_get_duration) tp->f.player_get_duration(tp->handle, &out->dur_ms);
        if (tp->f.player_get_video_size) tp->f.player_get_video_size(tp->handle, &out->video_w, &out->video_h);
        if (tp->f.player_get_track_count) tp->f.player_get_track_count(tp->handle, TP_STREAM_AUDIO, &out->audio_track_count);
        if (tp->f.player_get_current_track) tp->f.player_get_current_track(tp->handle, TP_STREAM_AUDIO, &out->current_audio_track);
    }
    pthread_mutex_unlock(&tp->mu);
}

/* ── Native Subtitle Cues & Skip Markers ── */

void tizen_player_clear_cues(TizenPlayer *tp) {
    if (!tp) return;
    pthread_mutex_lock(&tp->mu);
    free(tp->cues);
    tp->cues = NULL;
    tp->cue_count = 0;
    pthread_mutex_unlock(&tp->mu);
}

void tizen_player_set_cues(TizenPlayer *tp, SubCue *cues, int count) {
    if (!tp) { free(cues); return; }
    pthread_mutex_lock(&tp->mu);
    free(tp->cues);
    tp->cues = cues;
    tp->cue_count = count;
    pthread_mutex_unlock(&tp->mu);
}

const char *tizen_player_active_cue(TizenPlayer *tp, int pos_ms) {
    if (!tp || !tp->cues || tp->cue_count <= 0) return NULL;
    /* Binary search for last cue with start_ms <= pos_ms */
    int lo = 0, hi = tp->cue_count - 1, ans = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (tp->cues[mid].start_ms <= pos_ms) { ans = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (ans >= 0 && pos_ms <= tp->cues[ans].end_ms) return tp->cues[ans].text;
    /* Check up to 2 preceding overlapping cues */
    for (int j = ans - 1; j >= 0 && j >= ans - 2; j--) {
        if (tp->cues[j].start_ms <= pos_ms && pos_ms <= tp->cues[j].end_ms) return tp->cues[j].text;
    }
    return NULL;
}

bool tizen_player_copy_active_cue(TizenPlayer *tp, int pos_ms, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!tp) return false;
    pthread_mutex_lock(&tp->mu);
    const char *c = tizen_player_active_cue(tp, pos_ms);
    bool found = false;
    if (c && c[0]) {
        strncpy(out, c, cap - 1);
        out[cap - 1] = '\0';
        found = true;
    }
    pthread_mutex_unlock(&tp->mu);
    return found;
}

void tizen_player_clear_markers(TizenPlayer *tp) {
    if (!tp) return;
    pthread_mutex_lock(&tp->mu);
    tp->marker_count = 0;
    pthread_mutex_unlock(&tp->mu);
}

void tizen_player_add_marker(TizenPlayer *tp, int start_ms, int end_ms, const char *type, bool auto_skip) {
    if (!tp || tp->marker_count >= MAX_MARKERS || end_ms <= start_ms) return;
    pthread_mutex_lock(&tp->mu);
    SkipMarker *m = &tp->markers[tp->marker_count++];
    m->start_ms = start_ms;
    m->end_ms = end_ms;
    strncpy(m->type, type ? type : "skip", sizeof(m->type) - 1);
    m->type[sizeof(m->type) - 1] = '\0';
    m->auto_skip = auto_skip;
    m->triggered = false;
    pthread_mutex_unlock(&tp->mu);
}

SkipMarker *tizen_player_active_marker(TizenPlayer *tp, int pos_ms) {
    if (!tp) return NULL;
    for (int i = 0; i < tp->marker_count; i++) {
        if (pos_ms >= tp->markers[i].start_ms && pos_ms < tp->markers[i].end_ms - 500) {
            return &tp->markers[i];
        }
    }
    return NULL;
}

bool tizen_player_check_marker(TizenPlayer *tp, int pos_ms, SkipMarker *out_copy, bool trigger_auto) {
    if (!tp || !out_copy) return false;
    bool found = false;
    pthread_mutex_lock(&tp->mu);
    SkipMarker *m = tizen_player_active_marker(tp, pos_ms);
    if (m) {
        *out_copy = *m;
        if (trigger_auto && m->auto_skip && !m->triggered) {
            m->triggered = true;
        }
        found = true;
    }
    pthread_mutex_unlock(&tp->mu);
    return found;
}
