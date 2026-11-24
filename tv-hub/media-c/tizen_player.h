#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Exact Tizen CAPI player_state_e */
typedef enum {
    TP_STATE_NONE = 0,
    TP_STATE_IDLE = 1,
    TP_STATE_READY = 2,
    TP_STATE_PLAYING = 3,
    TP_STATE_PAUSED = 4
} TizenPlayerState;

/* Exact Tizen CAPI player_stream_type_e */
typedef enum {
    TP_STREAM_AUDIO = 0,
    TP_STREAM_TEXT  = 1
} TizenStreamType;

typedef struct {
    int start_ms;
    int end_ms;
    char text[256];
} SubCue;

typedef struct {
    int start_ms;
    int end_ms;
    char type[24];   /* "intro", "credits", "sponsor", "selfpromo", etc. */
    bool auto_skip;
    bool triggered;
} SkipMarker;

typedef struct {
    bool available;
    bool active;
    bool preparing;
    int  state;
    int  pos_ms;
    int  dur_ms;
    int  video_w;
    int  video_h;
    int  audio_track_count;
    int  current_audio_track;
    int  last_rc;
    char last_err[128];
} TizenPlayerSnapshot;

typedef struct TizenPlayer TizenPlayer;

TizenPlayer *tizen_player_create(void);
void         tizen_player_destroy(TizenPlayer *tp);
bool         tizen_player_is_available(TizenPlayer *tp);

/* Start hardware playback on compositor surface_id (from tizen_surface.get_tizen_resource).
 * Runs prepare+start on background thread so GLES2 HUD stays locked at 60 FPS! */
bool         tizen_player_play_async(TizenPlayer *tp, uint32_t wl_surface_id,
                                     const char *uri, int start_ms, int x, int y, int w, int h);

/* Blocking version useful for Phase 0 spike / diagnostics. */
int          tizen_player_play_sync(TizenPlayer *tp, uint32_t wl_surface_id,
                                    const char *uri, int start_ms, int x, int y, int w, int h);

int          tizen_player_toggle_pause(TizenPlayer *tp);
int          tizen_player_pause(TizenPlayer *tp);
int          tizen_player_resume(TizenPlayer *tp);
int          tizen_player_seek_ms(TizenPlayer *tp, int target_ms);
int          tizen_player_seek_rel_ms(TizenPlayer *tp, int delta_ms);
int          tizen_player_select_audio(TizenPlayer *tp, int track_idx);
void         tizen_player_stop(TizenPlayer *tp);

/* Poll fresh position/duration/state into out snapshot. */
void         tizen_player_poll(TizenPlayer *tp, TizenPlayerSnapshot *out);

/* Native Subtitle Cues & Skip Markers (loaded from gateway JSON) */
void         tizen_player_clear_cues(TizenPlayer *tp);
void         tizen_player_set_cues(TizenPlayer *tp, SubCue *cues, int count); /* takes ownership */
const char  *tizen_player_active_cue(TizenPlayer *tp, int pos_ms);
bool         tizen_player_copy_active_cue(TizenPlayer *tp, int pos_ms, char *out, size_t cap);

void         tizen_player_clear_markers(TizenPlayer *tp);
void         tizen_player_add_marker(TizenPlayer *tp, int start_ms, int end_ms, const char *type, bool auto_skip);
SkipMarker  *tizen_player_active_marker(TizenPlayer *tp, int pos_ms);
bool         tizen_player_check_marker(TizenPlayer *tp, int pos_ms, SkipMarker *out_copy, bool trigger_auto);
