#include "media_os.h"
#include "astc_pool.h"
#include "http_client.h"
#include "json_mini.h"
#include "tizen_player.h"
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Linux evdev keycodes (Samsung Tizen BT remote + USB/API) */
#define K_ESC        1
#define K_BACKSPACE  14
#define K_ENTER      28
#define K_SPACE      57
#define K_KPENTER    96
#define K_UP         103
#define K_LEFT       105
#define K_RIGHT      106
#define K_DOWN       108
#define K_PAUSE      119
#define K_STOP       128
#define K_BACK       158
#define K_NEXT       163
#define K_PLAYPAUSE  164
#define K_PREV       165
#define K_RW         168
#define K_PLAY       207
#define K_FF         208
#define K_OK         352
#define K_SELECT     353
#define K_INFO       358
#define K_SUBTITLE   370
#define K_RED        398
#define K_GREEN      399
#define K_YELLOW     400
#define K_BLUE       401

static bool is_ok(uint32_t ev) {
    return ev == K_ENTER || ev == K_KPENTER || ev == K_OK || ev == K_SELECT || ev == K_SPACE;
}
static bool is_back(uint32_t ev) {
    return ev == K_BACK || ev == K_ESC || ev == K_BACKSPACE;
}

#define MAX_ROW_CARDS  42
#define MAX_GRID_CARDS 280
#define MAX_CHILDREN   64
#define MAX_TRACKS     16
#define MAX_CH_VIDEOS  36

typedef struct {
    char source[16];     /* "plex", "youtube", "yt_channel", "jellyseerr" */
    char id[64];         /* rating_key, video_id, channel_id, or tmdb_id */
    char media_type[16]; /* "movie", "show", "season", "episode", "video", "channel", "tv" */
    char title[128];
    char subtitle[128];
    char art_url[384];
    char backdrop_url[384];
    char status[32];     /* Jellyseerr: not_requested, pending, processing, available */
    int  duration_ms;
    int  offset_ms;
    float progress;
    int  tmdb_id;
    bool can_request;
    bool is_wide;
    bool is_kids;
    bool is_favorite;
} MediaCard;

typedef struct {
    int  id;
    int  index;
    char lang[24];
    char label[64];
    char codec[16];
    bool native_overlay;
} MediaTrack;

typedef struct {
    bool loaded;
    char source[16];
    char rating_key[64];
    char type[16];
    char title[128];
    char subtitle[128];
    char summary[512];
    char poster_url[384];
    char backdrop_url[384];
    char play_mode[32];
    char media_badge[128];
    int  duration_ms;
    int  offset_ms;
    int  marker_count;
    MediaTrack audio[MAX_TRACKS];
    int  audio_count;
    int  sel_audio_idx;
    MediaTrack subs[MAX_TRACKS];
    int  sub_count;
    int  sel_sub_idx; /* -1 = Off */
    MediaCard children[MAX_CHILDREN];
    int  child_count;
} MediaDetail;

typedef struct {
    bool active;
    char source[16];
    char item_id[64];
    char title[128];
    char subtitle[128];
    char mode[32];
    char badge[128];
    char base_stream_url[1024];
    bool is_remux_stream;
    int  base_offset_ms;
    int  known_dur_ms;
    int  last_pos_ms;
    int  player_state;
    int  video_w;
    int  video_h;
    bool video_revealed;
    int64_t play_started_us;
    int  play_start_pos_ms;
    bool paused;
    bool fallback_stock_yt;
    char err_msg[128];
    /* Up next episode */
    bool has_up_next;
    char up_next_rk[64];
    char up_next_title[128];
    char up_next_sub[128];
    /* Tracks available for live switching */
    MediaTrack subs[MAX_TRACKS];
    int  sub_count;
    int  cur_sub_idx; /* -1 = Off */
    MediaTrack audio[MAX_TRACKS];
    int  audio_count;
    int  cur_audio_idx;
    /* Timing & 10-foot Interactive HUD Controls */
    int64_t hud_until_us;
    bool    hud_focused;
    int     hud_btn_idx; /* 0=-15s, 1=Play/Pause, 2=+30s, 3=Subtitles, 4=Exit */
    char    feedback_txt[48];
    int64_t feedback_until_us;
    int64_t last_sync_us;
    int64_t last_poll_us;
    char active_sub_text[256];
    char skip_pill_text[64];
    int  skip_pill_target_ms;
    bool skip_pill_is_up_next;
} ActivePlayback;

/* Top nav tabs */
#define NAV_COUNT 8
static const char *NAV_LABELS[NAV_COUNT] = {
    "Hub", "Home", "Movies", "TV Shows", "YouTube", "Kids", "Search", "HDMI 1"
};

/* Background job queue */
typedef enum {
    JOB_NONE = 0,
    JOB_FETCH_HOME,
    JOB_FETCH_KIDS,
    JOB_FETCH_GRID,
    JOB_FETCH_YT_FEED,
    JOB_FETCH_YT_CHANNELS,
    JOB_FETCH_YT_CHANNEL,
    JOB_CLASSIFY_YT_CHANNEL,
    JOB_FETCH_DETAIL,
    JOB_FETCH_SEARCH,
    JOB_SEERR_REQUEST,
    JOB_START_PLAY_PLEX,
    JOB_START_PLAY_YT,
    JOB_FETCH_SUBS,
    JOB_SYNC_TIMELINE
} MediaJobType;

typedef struct {
    MediaJobType type;
    char a[128];
    char b[128];
    int  i1;
    int  i2;
    int  i3;
} MediaJob;

#define JOB_Q_CAP 24

/* Singleton Media OS state */
static struct {
    bool initialized;
    MediaHostCallbacks host;
    char gateway[128];
    AstcPool *pool;
    TizenPlayer *player;

    pthread_mutex_t mu;
    pthread_cond_t  cv;
    pthread_t       worker;
    bool            stop;
    MediaJob        jobs[JOB_Q_CAP];
    int             j_head, j_tail;

    /* View & navigation */
    MediaViewMode view;
    MediaViewMode prev_view;
    MediaViewMode pre_player_view;
    bool focus_nav;
    int  nav_idx;
    bool loading;
    char status_note[128];

    /* Home rows (4 rows: CW, YT Subs, Recent Movies, Recent TV) */
    MediaCard home_rows[4][MAX_ROW_CARDS];
    int       home_counts[4];
    bool      home_loaded;
    bool      yt_signed_in;
    int       home_row;
    int       home_col[4];

    /* Kids rows (4 rows: Kids CW, Kids YouTube Channels, Kids Movies, Kids TV) */
    MediaCard kids_rows[4][MAX_ROW_CARDS];
    int       kids_counts[4];
    bool      kids_loaded;
    int       kids_row;
    int       kids_col[4];

    /* Grid view (Movies, TV, YouTube) */
    MediaCard grid_items[MAX_GRID_CARDS];
    int       grid_count;
    int       grid_total;
    int       grid_idx;
    int       grid_section_id; /* 1=Movies, 2=TV, 0=YouTube */
    int       grid_sort_idx;   /* 0=Recent, 1=A-Z, 2=Year */
    bool      grid_unwatched;
    int       yt_tab_idx;      /* 0=Adults Channels, 1=Kids Channels, 2=Latest Adults, 3=Kids Videos, 4=Home, 5=Stock YT */
    int       yt_cat_idx;      /* 0=active,1=favorites,2=woodworking,3=home,4=tech,5=cars,6=podcasts,7=all */
    bool      focus_subbar;
    bool      focus_catbar;
    int       subbar_idx;

    /* Dedicated YouTube Channel Page */
    struct {
        bool      loaded;
        char      channel_id[64];
        char      title[128];
        char      category_label[48];
        char      avatar_url[384];
        bool      is_kids;
        bool      is_favorite;
        MediaCard videos[MAX_CH_VIDEOS];
        int       video_count;
        bool      focus_header;
        int       hdr_btn;     /* 0=Back, 1=Favorite, 2=Move Audience */
        int       vid_idx;
    } yt_ch;

    /* Detail view */
    MediaDetail detail;
    int         detail_zone;   /* 0=CTAs, 1=Children strip */
    int         detail_cta_idx;
    int         detail_child_idx;

    /* Search view */
    char      search_query[64];
    bool      search_in_kb;
    int       kb_row;
    int       kb_col;
    MediaCard search_rows[3][12]; /* 0=Plex, 1=YouTube, 2=Jellyseerr */
    int       search_counts[3];
    int       search_res_row;
    int       search_res_col[3];

    /* Active player */
    ActivePlayback pb;
} M;

static int64_t m_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000);
}

static void m_strlcpy(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void push_job(MediaJobType type, const char *a, const char *b, int i1, int i2, int i3) {
    pthread_mutex_lock(&M.mu);
    int next = (M.j_tail + 1) % JOB_Q_CAP;
    if (next != M.j_head) {
        MediaJob *j = &M.jobs[M.j_tail];
        memset(j, 0, sizeof(*j));
        j->type = type;
        if (a) m_strlcpy(j->a, a, sizeof(j->a));
        if (b) m_strlcpy(j->b, b, sizeof(j->b));
        j->i1 = i1; j->i2 = i2; j->i3 = i3;
        M.j_tail = next;
        pthread_cond_signal(&M.cv);
    }
    pthread_mutex_unlock(&M.mu);
}

static void parse_card(const JVal *v, MediaCard *c, bool prefer_wide) {
    memset(c, 0, sizeof(*c));
    m_strlcpy(c->source, json_str(json_get(v, "source"), "plex"), sizeof(c->source));
    c->is_kids = json_bool(json_get(v, "is_kids"), false);
    c->is_favorite = json_bool(json_get(v, "is_favorite"), false);
    if (strcmp(c->source, "yt_channel") == 0) {
        m_strlcpy(c->id, json_str(json_get(v, "channel_id"), json_str(json_get(v, "id"), "")), sizeof(c->id));
        m_strlcpy(c->media_type, "channel", sizeof(c->media_type));
        c->is_wide = false;
    } else if (strcmp(c->source, "youtube") == 0) {
        m_strlcpy(c->id, json_str(json_get(v, "video_id"), ""), sizeof(c->id));
        m_strlcpy(c->media_type, "video", sizeof(c->media_type));
        c->is_wide = true;
    } else if (strcmp(c->source, "jellyseerr") == 0) {
        c->tmdb_id = (int)json_i64(json_get(v, "tmdb_id"), 0);
        snprintf(c->id, sizeof(c->id), "%d", c->tmdb_id);
        m_strlcpy(c->media_type, json_str(json_get(v, "media_type"), "movie"), sizeof(c->media_type));
        m_strlcpy(c->status, json_str(json_get(v, "status"), ""), sizeof(c->status));
        c->can_request = json_bool(json_get(v, "can_request"), false);
        c->is_wide = false;
    } else {
        m_strlcpy(c->id, json_str(json_get(v, "rating_key"), ""), sizeof(c->id));
        m_strlcpy(c->media_type, json_str(json_get(v, "type"), "movie"), sizeof(c->media_type));
        c->is_wide = prefer_wide;
    }
    m_strlcpy(c->title, json_str(json_get(v, "title"), ""), sizeof(c->title));
    m_strlcpy(c->subtitle, json_str(json_get(v, "subtitle"), ""), sizeof(c->subtitle));
    const char *art = prefer_wide
        ? json_str(json_get(v, "thumb_astc"), json_str(json_get(v, "art_astc"), ""))
        : json_str(json_get(v, "poster_astc"), json_str(json_get(v, "art_astc"), ""));
    if (!art[0]) art = json_str(json_get(v, "art_astc"), "");
    m_strlcpy(c->art_url, art, sizeof(c->art_url));
    m_strlcpy(c->backdrop_url, json_str(json_get(v, "backdrop_astc"), ""), sizeof(c->backdrop_url));
    c->duration_ms = (int)json_i64(json_get(v, "duration_ms"), 0);
    c->offset_ms = (int)json_i64(json_get(v, "offset_ms"), 0);
    c->progress = (float)json_f64(json_get(v, "progress"), 0.0);
}

static int parse_card_array(const JVal *arr, MediaCard *dst, int cap, bool prefer_wide) {
    if (!arr || arr->type != J_ARR) return 0;
    int n = (int)arr->count;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) parse_card(json_at(arr, (size_t)i), &dst[i], prefer_wide);
    return n;
}

static void fmt_mmss(int ms, char *out, size_t cap) {
    if (ms < 0) ms = 0;
    int s = ms / 1000;
    int h = s / 3600;
    int m = (s % 3600) / 60;
    int sec = s % 60;
    if (h > 0) snprintf(out, cap, "%d:%02d:%02d", h, m, sec);
    else snprintf(out, cap, "%d:%02d", m, sec);
}

/* ── Worker thread: executes all LAN HTTP requests + subtitle loading + player starts ── */

static void exec_job(const MediaJob *j) {
    char url[1024];
    HttpResponse resp;

    if (j->type == JOB_FETCH_HOME) {
        snprintf(url, sizeof(url), "%s/v1/home", M.gateway);
        if (http_get(url, 10000, 1024 * 1024, &resp) && resp.status == 200) {
            JVal *root = json_parse((const char *)resp.body);
            if (root) {
                pthread_mutex_lock(&M.mu);
                M.home_counts[0] = parse_card_array(json_get(root, "continue_watching"), M.home_rows[0], MAX_ROW_CARDS, true);
                M.home_counts[1] = parse_card_array(json_get(root, "youtube_row"),       M.home_rows[1], MAX_ROW_CARDS, true);
                M.home_counts[2] = parse_card_array(json_get(root, "recent_movies"),     M.home_rows[2], MAX_ROW_CARDS, false);
                M.home_counts[3] = parse_card_array(json_get(root, "recent_tv"),         M.home_rows[3], MAX_ROW_CARDS, false);
                M.yt_signed_in = json_bool(json_get(root, "youtube_signed_in"), false);
                M.home_loaded = true;
                M.loading = false;
                pthread_mutex_unlock(&M.mu);
                json_free(root);
            }
        } else {
            pthread_mutex_lock(&M.mu); M.loading = false;
            m_strlcpy(M.status_note, "Media gateway unreachable", sizeof(M.status_note));
            pthread_mutex_unlock(&M.mu);
        }
        http_response_free(&resp);
        if (M.host.signal_main) M.host.signal_main();
        return;
    }

    if (j->type == JOB_FETCH_KIDS) {
        snprintf(url, sizeof(url), "%s/v1/kids", M.gateway);
        if (http_get(url, 12000, 1024 * 1024, &resp) && resp.status == 200) {
            JVal *root = json_parse((const char *)resp.body);
            if (root) {
                pthread_mutex_lock(&M.mu);
                M.kids_counts[0] = parse_card_array(json_get(root, "continue_watching"), M.kids_rows[0], MAX_ROW_CARDS, true);
                if (M.kids_counts[0] == 0) {
                    M.kids_counts[0] = parse_card_array(json_get(root, "recent_kids_episodes"), M.kids_rows[0], MAX_ROW_CARDS, true);
                }
                M.kids_counts[1] = parse_card_array(json_get(root, "kids_youtube_channels"), M.kids_rows[1], MAX_ROW_CARDS, false);
                M.kids_counts[2] = parse_card_array(json_get(root, "kids_movies"), M.kids_rows[2], MAX_ROW_CARDS, false);
                M.kids_counts[3] = parse_card_array(json_get(root, "kids_tv"),     M.kids_rows[3], MAX_ROW_CARDS, false);
                M.kids_loaded = true;
                M.loading = false;
                pthread_mutex_unlock(&M.mu);
                json_free(root);
            }
        } else {
            pthread_mutex_lock(&M.mu); M.loading = false; pthread_mutex_unlock(&M.mu);
        }
        http_response_free(&resp);
        if (M.host.signal_main) M.host.signal_main();
        return;
    }

    if (j->type == JOB_FETCH_GRID) {
        const char *sorts[3] = { "addedAt:desc", "titleSort:asc", "year:desc" };
        int sid = j->i1, start = j->i2, sort_i = j->i3 & 3;
        bool unw = (j->i3 & 8) != 0;
        snprintf(url, sizeof(url), "%s/v1/plex/section/%d?start=%d&size=42&sort=%s&unwatched=%d",
                 M.gateway, sid, start, sorts[sort_i < 3 ? sort_i : 0], unw ? 1 : 0);
        if (http_get(url, 10000, 1024 * 1024, &resp) && resp.status == 200) {
            JVal *root = json_parse((const char *)resp.body);
            if (root) {
                pthread_mutex_lock(&M.mu);
                if (M.grid_section_id == sid) {
                    int base = (start == 0) ? 0 : M.grid_count;
                    if (base < MAX_GRID_CARDS) {
                        int got = parse_card_array(json_get(root, "items"),
                                                   &M.grid_items[base], MAX_GRID_CARDS - base, false);
                        M.grid_count = base + got;
                    }
                    M.grid_total = (int)json_i64(json_get(root, "total"), M.grid_count);
                    if (start == 0) M.grid_idx = 0;
                }
                M.loading = false;
                pthread_mutex_unlock(&M.mu);
                json_free(root);
            }
        } else {
            pthread_mutex_lock(&M.mu); M.loading = false; pthread_mutex_unlock(&M.mu);
        }
        http_response_free(&resp);
        if (M.host.signal_main) M.host.signal_main();
        return;
    }

    if (j->type == JOB_FETCH_YT_FEED) {
        /* tab_idx 2=subscriptions (Adults), 3=kids (Kids Videos), 4=home */
        const char *feed_name = "subscriptions";
        if (j->i1 == 3) feed_name = "kids";
        else if (j->i1 == 4) feed_name = "home";
        snprintf(url, sizeof(url), "%s/v1/youtube/feed/%s?limit=36", M.gateway, feed_name);
        if (http_get(url, 15000, 1024 * 1024, &resp) && resp.status == 200) {
            JVal *root = json_parse((const char *)resp.body);
            if (root) {
                pthread_mutex_lock(&M.mu);
                if (M.view == MEDIA_VIEW_YOUTUBE && M.yt_tab_idx == j->i1) {
                    M.grid_count = parse_card_array(json_get(root, "items"), M.grid_items, MAX_GRID_CARDS, true);
                    M.grid_total = M.grid_count;
                    M.grid_idx = 0;
                    M.yt_signed_in = json_bool(json_get(root, "signed_in"), M.yt_signed_in);
                }
                M.loading = false;
                pthread_mutex_unlock(&M.mu);
                json_free(root);
            }
        } else {
            pthread_mutex_lock(&M.mu); M.loading = false; pthread_mutex_unlock(&M.mu);
        }
        http_response_free(&resp);
        if (M.host.signal_main) M.host.signal_main();
        return;
    }

    if (j->type == JOB_FETCH_YT_CHANNELS) {
        static const char *CAT_SLUGS[8] = {
            "active", "favorites", "woodworking", "home", "tech", "cars", "podcasts", "all"
        };
        bool kids = (j->i1 == 1);
        int ci = (j->i2 >= 0 && j->i2 < 8) ? j->i2 : 0;
        snprintf(url, sizeof(url), "%s/v1/youtube/channels?audience=%s&category=%s",
                 M.gateway, kids ? "kids" : "adults", kids ? "all" : CAT_SLUGS[ci]);
        if (http_get(url, 15000, 1024 * 1024, &resp) && resp.status == 200) {
            JVal *root = json_parse((const char *)resp.body);
            if (root) {
                pthread_mutex_lock(&M.mu);
                if (M.view == MEDIA_VIEW_YOUTUBE && M.yt_tab_idx == j->i1) {
                    M.grid_count = parse_card_array(json_get(root, "items"), M.grid_items, MAX_GRID_CARDS, false);
                    M.grid_total = (int)json_i64(json_get(root, "total"), M.grid_count);
                    M.grid_idx = 0;
                    M.yt_signed_in = json_bool(json_get(root, "signed_in"), M.yt_signed_in);
                }
                M.loading = false;
                pthread_mutex_unlock(&M.mu);
                json_free(root);
            }
        } else {
            pthread_mutex_lock(&M.mu); M.loading = false; pthread_mutex_unlock(&M.mu);
        }
        http_response_free(&resp);
        if (M.host.signal_main) M.host.signal_main();
        return;
    }

    if (j->type == JOB_FETCH_YT_CHANNEL) {
        snprintf(url, sizeof(url), "%s/v1/youtube/channel/%s?limit=32", M.gateway, j->a);
        if (http_get(url, 15000, 1024 * 1024, &resp) && resp.status == 200) {
            JVal *root = json_parse((const char *)resp.body);
            if (root) {
                pthread_mutex_lock(&M.mu);
                if (strcmp(M.yt_ch.channel_id, j->a) == 0) {
                    m_strlcpy(M.yt_ch.title, json_str(json_get(root, "title"), j->a), sizeof(M.yt_ch.title));
                    m_strlcpy(M.yt_ch.category_label, json_str(json_get(root, "category_label"), "YouTube"), sizeof(M.yt_ch.category_label));
                    m_strlcpy(M.yt_ch.avatar_url, json_str(json_get(root, "avatar_astc"), ""), sizeof(M.yt_ch.avatar_url));
                    M.yt_ch.is_kids = json_bool(json_get(root, "is_kids"), false);
                    M.yt_ch.is_favorite = json_bool(json_get(root, "is_favorite"), false);
                    M.yt_ch.video_count = parse_card_array(json_get(root, "items"), M.yt_ch.videos, MAX_CH_VIDEOS, true);
                    M.yt_ch.vid_idx = 0;
                    M.yt_ch.focus_header = (M.yt_ch.video_count == 0);
                    M.yt_ch.loaded = true;
                }
                M.loading = false;
                pthread_mutex_unlock(&M.mu);
                json_free(root);
            }
        } else {
            pthread_mutex_lock(&M.mu); M.loading = false; pthread_mutex_unlock(&M.mu);
        }
        http_response_free(&resp);
        if (M.host.signal_main) M.host.signal_main();
        return;
    }

    if (j->type == JOB_CLASSIFY_YT_CHANNEL) {
        /* i1: 1=set audience (j->b="kids"/"adults"), 2=set favorite (i2=0/1) */
        char body[192];
        if (j->i1 == 1) {
            snprintf(body, sizeof(body), "{\"channel_id\":\"%s\",\"audience\":\"%s\"}", j->a, j->b);
        } else {
            snprintf(body, sizeof(body), "{\"channel_id\":\"%s\",\"favorite\":%s}", j->a, j->i2 ? "true" : "false");
        }
        snprintf(url, sizeof(url), "%s/v1/youtube/channels/classify", M.gateway);
        if (http_post_json(url, body, 8000, &resp) && resp.status == 200) {
            pthread_mutex_lock(&M.mu);
            if (strcmp(M.yt_ch.channel_id, j->a) == 0) {
                if (j->i1 == 1) M.yt_ch.is_kids = (strcmp(j->b, "kids") == 0);
                else M.yt_ch.is_favorite = (j->i2 != 0);
            }
            M.kids_loaded = false;
            pthread_mutex_unlock(&M.mu);
            if (M.host.show_toast) {
                if (j->i1 == 1) M.host.show_toast(strcmp(j->b, "kids") == 0 ? "Moved to Kids YouTube Channels" : "Moved to Adults YouTube Channels", 2000);
                else M.host.show_toast(j->i2 ? "Pinned to Favorites" : "Removed from Favorites", 1800);
            }
        } else if (M.host.show_toast) {
            M.host.show_toast("Channel update failed", 1800);
        }
        http_response_free(&resp);
        if (M.host.signal_main) M.host.signal_main();
        return;
    }

    if (j->type == JOB_FETCH_DETAIL) {
        snprintf(url, sizeof(url), "%s/v1/plex/item/%s", M.gateway, j->a);
        if (http_get(url, 10000, 1024 * 1024, &resp) && resp.status == 200) {
            JVal *root = json_parse((const char *)resp.body);
            if (root) {
                MediaDetail d;
                memset(&d, 0, sizeof(d));
                d.loaded = true;
                d.sel_sub_idx = -1;
                m_strlcpy(d.source, "plex", sizeof(d.source));
                m_strlcpy(d.rating_key, json_str(json_get(root, "rating_key"), j->a), sizeof(d.rating_key));
                m_strlcpy(d.type, json_str(json_get(root, "type"), "movie"), sizeof(d.type));
                m_strlcpy(d.title, json_str(json_get(root, "title"), ""), sizeof(d.title));
                m_strlcpy(d.subtitle, json_str(json_get(root, "subtitle"), ""), sizeof(d.subtitle));
                m_strlcpy(d.summary, json_str(json_get(root, "summary"), ""), sizeof(d.summary));
                m_strlcpy(d.poster_url, json_str(json_get(root, "poster_astc"), ""), sizeof(d.poster_url));
                m_strlcpy(d.backdrop_url, json_str(json_get(root, "backdrop_astc"), ""), sizeof(d.backdrop_url));
                d.duration_ms = (int)json_i64(json_get(root, "duration_ms"), 0);
                d.offset_ms = (int)json_i64(json_get(root, "offset_ms"), 0);

                const JVal *med = json_get(root, "media");
                if (med) {
                    const char *mode = json_str(json_get(med, "mode"), "");
                    m_strlcpy(d.play_mode, mode, sizeof(d.play_mode));
                    const char *mlbl = strcmp(mode, "direct_play") == 0 ? "Direct Play" :
                                       (strcmp(mode, "direct_stream_audio") == 0 ? "EAC3 5.1 Remux" : "QuickSync");
                    snprintf(d.media_badge, sizeof(d.media_badge), "%s  ·  %dp %s  ·  %s %dch",
                             mlbl, (int)json_i64(json_get(med, "height"), 0),
                             json_str(json_get(med, "video_codec"), ""),
                             json_str(json_get(med, "audio_codec"), ""),
                             (int)json_i64(json_get(med, "audio_channels"), 2));
                }
                const JVal *mk = json_get(root, "markers");
                d.marker_count = (mk && mk->type == J_ARR) ? (int)mk->count : 0;

                const JVal *au = json_get(root, "audio_tracks");
                if (au && au->type == J_ARR) {
                    for (size_t i = 0; i < au->count && d.audio_count < MAX_TRACKS; i++) {
                        const JVal *t = json_at(au, i);
                        MediaTrack *mt = &d.audio[d.audio_count];
                        mt->id = (int)json_i64(json_get(t, "id"), 0);
                        mt->index = (int)json_i64(json_get(t, "index"), 0);
                        m_strlcpy(mt->lang, json_str(json_get(t, "lang"), ""), sizeof(mt->lang));
                        m_strlcpy(mt->label, json_str(json_get(t, "label"), "Audio"), sizeof(mt->label));
                        m_strlcpy(mt->codec, json_str(json_get(t, "codec"), ""), sizeof(mt->codec));
                        if (json_bool(json_get(t, "selected"), false)) d.sel_audio_idx = d.audio_count;
                        d.audio_count++;
                    }
                }
                const JVal *su = json_get(root, "subtitle_tracks");
                if (su && su->type == J_ARR) {
                    for (size_t i = 0; i < su->count && d.sub_count < MAX_TRACKS; i++) {
                        const JVal *t = json_at(su, i);
                        MediaTrack *mt = &d.subs[d.sub_count];
                        mt->id = (int)json_i64(json_get(t, "id"), 0);
                        mt->index = (int)json_i64(json_get(t, "index"), 0);
                        m_strlcpy(mt->lang, json_str(json_get(t, "lang"), ""), sizeof(mt->lang));
                        m_strlcpy(mt->label, json_str(json_get(t, "label"), "Sub"), sizeof(mt->label));
                        m_strlcpy(mt->codec, json_str(json_get(t, "codec"), ""), sizeof(mt->codec));
                        mt->native_overlay = json_bool(json_get(t, "native_overlay"), false);
                        if (json_bool(json_get(t, "selected"), false)) d.sel_sub_idx = d.sub_count;
                        d.sub_count++;
                    }
                }
                bool kids_wide = (strcmp(d.type, "season") == 0);
                d.child_count = parse_card_array(json_get(root, "children"), d.children, MAX_CHILDREN, kids_wide);

                pthread_mutex_lock(&M.mu);
                M.detail = d;
                M.detail_zone = (d.child_count > 0 && strcmp(d.type, "movie") != 0 && strcmp(d.type, "episode") != 0) ? 1 : 0;
                M.detail_cta_idx = 0;
                M.detail_child_idx = 0;
                M.loading = false;
                pthread_mutex_unlock(&M.mu);
                json_free(root);
            }
        } else {
            pthread_mutex_lock(&M.mu); M.loading = false; pthread_mutex_unlock(&M.mu);
        }
        http_response_free(&resp);
        if (M.host.signal_main) M.host.signal_main();
        return;
    }

    if (j->type == JOB_FETCH_SEARCH) {
        char enc[256];
        http_url_encode(j->a, enc, sizeof(enc));
        snprintf(url, sizeof(url), "%s/v1/search?q=%s&limit=10", M.gateway, enc);
        if (http_get(url, 12000, 1024 * 1024, &resp) && resp.status == 200) {
            JVal *root = json_parse((const char *)resp.body);
            if (root) {
                pthread_mutex_lock(&M.mu);
                M.search_counts[0] = parse_card_array(json_get(root, "plex"),       M.search_rows[0], 12, false);
                M.search_counts[1] = parse_card_array(json_get(root, "youtube"),    M.search_rows[1], 12, true);
                M.search_counts[2] = parse_card_array(json_get(root, "jellyseerr"), M.search_rows[2], 12, false);
                M.search_res_row = (M.search_counts[0] > 0) ? 0 : (M.search_counts[1] > 0 ? 1 : 2);
                M.search_res_col[0] = M.search_res_col[1] = M.search_res_col[2] = 0;
                M.loading = false;
                pthread_mutex_unlock(&M.mu);
                json_free(root);
            }
        } else {
            pthread_mutex_lock(&M.mu); M.loading = false; pthread_mutex_unlock(&M.mu);
        }
        http_response_free(&resp);
        if (M.host.signal_main) M.host.signal_main();
        return;
    }

    if (j->type == JOB_SEERR_REQUEST) {
        char body[160];
        snprintf(body, sizeof(body), "{\"media_type\":\"%s\",\"tmdb_id\":%d,\"seasons\":\"all\"}", j->a, j->i1);
        snprintf(url, sizeof(url), "%s/v1/jellyseerr/request", M.gateway);
        if (http_post_json(url, body, 10000, &resp) && resp.status == 200) {
            pthread_mutex_lock(&M.mu);
            for (int i = 0; i < M.search_counts[2]; i++) {
                if (M.search_rows[2][i].tmdb_id == j->i1) {
                    M.search_rows[2][i].can_request = false;
                    m_strlcpy(M.search_rows[2][i].status, "requested", sizeof(M.search_rows[2][i].status));
                }
            }
            pthread_mutex_unlock(&M.mu);
            if (M.host.show_toast) M.host.show_toast("Requested via Jellyseerr (720p First)", 2400);
        } else {
            if (M.host.show_toast) M.host.show_toast("Jellyseerr request failed", 2000);
        }
        http_response_free(&resp);
        if (M.host.signal_main) M.host.signal_main();
        return;
    }

    if (j->type == JOB_START_PLAY_PLEX || j->type == JOB_START_PLAY_YT) {
        bool is_yt = (j->type == JOB_START_PLAY_YT);
        int start_ms = j->i1;
        if (is_yt) {
            snprintf(url, sizeof(url), "%s/v1/playback/youtube/%s?max_height=1080&container=ts&start_ms=%d",
                     M.gateway, j->a, start_ms);
        } else {
            snprintf(url, sizeof(url), "%s/v1/playback/plex/%s?start_ms=%d%s%s",
                     M.gateway, j->a, start_ms,
                     j->i2 > 0 ? "" : "",
                     "");
            if (j->i2 > 0) {
                char extra[64];
                snprintf(extra, sizeof(extra), "&audio_id=%d", j->i2);
                strncat(url, extra, sizeof(url) - strlen(url) - 1);
            }
            if (j->i3 >= 0) {
                char extra[64];
                snprintf(extra, sizeof(extra), "&sub_id=%d", j->i3);
                strncat(url, extra, sizeof(url) - strlen(url) - 1);
            }
        }

        if (!http_get(url, 20000, 512 * 1024, &resp) || resp.status != 200) {
            http_response_free(&resp);
            pthread_mutex_lock(&M.mu);
            m_strlcpy(M.pb.err_msg, "Could not resolve stream from gateway", sizeof(M.pb.err_msg));
            M.loading = false;
            pthread_mutex_unlock(&M.mu);
            if (M.host.show_toast) M.host.show_toast("Playback resolve failed", 2400);
            if (M.host.signal_main) M.host.signal_main();
            return;
        }
        JVal *root = json_parse((const char *)resp.body);
        http_response_free(&resp);
        if (!root) return;

        const char *stream_url = json_str(json_get(root, "stream_url"), "");
        const char *sub_url = json_str(json_get(root, "subtitle_url"), "");
        const JVal *dec = json_get(root, "decision");
        const char *mode = json_str(json_get(dec, "mode"), "");
        bool fb_yt = json_bool(json_get(dec, "fallback_stock_youtube"), false);
        int dur_ms = (int)json_i64(json_get(root, "duration_ms"), 0);
        int resume_ms = (start_ms > 0) ? start_ms : (int)json_i64(json_get(root, "resume_ms"), 0);

        tizen_player_clear_markers(M.player);
        tizen_player_clear_cues(M.player);
        const JVal *mks = json_get(root, "markers");
        if (mks && mks->type == J_ARR) {
            for (size_t i = 0; i < mks->count; i++) {
                const JVal *mk = json_at(mks, i);
                const char *act = json_str(json_get(mk, "action"), "prompt_skip");
                tizen_player_add_marker(M.player,
                                        (int)json_i64(json_get(mk, "start_ms"), 0),
                                        (int)json_i64(json_get(mk, "end_ms"), 0),
                                        json_str(json_get(mk, "type"), "skip"),
                                        strcmp(act, "auto_skip") == 0);
            }
        }

        bool is_remux = (strcmp(mode, "direct_play") != 0 && strcmp(mode, "local_ytdl_sub_direct") != 0);
        pthread_mutex_lock(&M.mu);
        M.pb.active = true;
        m_strlcpy(M.pb.source, is_yt ? "youtube" : "plex", sizeof(M.pb.source));
        m_strlcpy(M.pb.item_id, j->a, sizeof(M.pb.item_id));
        m_strlcpy(M.pb.title, json_str(json_get(root, "title"), j->a), sizeof(M.pb.title));
        m_strlcpy(M.pb.subtitle, json_str(json_get(root, "subtitle"), ""), sizeof(M.pb.subtitle));
        m_strlcpy(M.pb.mode, mode, sizeof(M.pb.mode));
        m_strlcpy(M.pb.base_stream_url, stream_url, sizeof(M.pb.base_stream_url));
        M.pb.is_remux_stream = is_remux;
        M.pb.base_offset_ms = is_remux ? resume_ms : 0;
        M.pb.known_dur_ms = dur_ms;
        M.pb.last_pos_ms = resume_ms;
        M.pb.paused = false;
        M.pb.hud_focused = false;
        M.pb.hud_btn_idx = 1; /* Default focus on Play/Pause */
        M.pb.feedback_txt[0] = '\0';
        M.pb.feedback_until_us = 0;
        int dec_w = (int)json_i64(json_get(dec, "width"), 0);
        int dec_h = (int)json_i64(json_get(dec, "height"), 0);
        if (dec_w > 0 && dec_h > 0) { M.pb.video_w = dec_w; M.pb.video_h = dec_h; }
        M.pb.fallback_stock_yt = fb_yt;
        M.pb.err_msg[0] = '\0';
        M.pb.hud_until_us = m_now_us() + 5500000LL;
        M.pb.last_sync_us = m_now_us();

        if (is_yt) {
            snprintf(M.pb.badge, sizeof(M.pb.badge), "YouTube Ad-Free · %dp · SponsorBlock",
                     (int)json_i64(json_get(dec, "height"), 1080));
            /* Populate caption tracks */
            M.pb.sub_count = 0; M.pb.cur_sub_idx = -1;
            const JVal *caps = json_get(root, "caption_tracks");
            if (caps && caps->type == J_ARR) {
                for (size_t i = 0; i < caps->count && M.pb.sub_count < MAX_TRACKS; i++) {
                    const JVal *c = json_at(caps, i);
                    MediaTrack *mt = &M.pb.subs[M.pb.sub_count++];
                    mt->id = (int)i + 1;
                    m_strlcpy(mt->lang, json_str(json_get(c, "lang"), "en"), sizeof(mt->lang));
                    m_strlcpy(mt->label, json_str(json_get(c, "label"), "CC"), sizeof(mt->label));
                    mt->native_overlay = true;
                }
            }
        } else {
            const char *ml = strcmp(mode, "direct_play") == 0 ? "Direct Play" :
                             (strcmp(mode, "direct_stream_audio") == 0 ? "EAC3 5.1" : "Transcode");
            snprintf(M.pb.badge, sizeof(M.pb.badge), "%s · %dp %s · %s %dch",
                     ml, (int)json_i64(json_get(dec, "height"), 0),
                     json_str(json_get(dec, "video_codec"), ""),
                     json_str(json_get(dec, "target_audio_codec"), json_str(json_get(dec, "audio_codec"), "")),
                     (int)json_i64(json_get(dec, "audio_channels"), 2));
            /* Copy tracks from detail if matching item */
            if (M.detail.loaded && strcmp(M.detail.rating_key, j->a) == 0) {
                M.pb.sub_count = M.detail.sub_count;
                memcpy(M.pb.subs, M.detail.subs, sizeof(M.detail.subs));
                M.pb.cur_sub_idx = M.detail.sel_sub_idx;
                M.pb.audio_count = M.detail.audio_count;
                memcpy(M.pb.audio, M.detail.audio, sizeof(M.detail.audio));
                M.pb.cur_audio_idx = M.detail.sel_audio_idx;
            }
        }
        /* Up Next episode */
        const JVal *un = json_get(root, "up_next");
        if (un && un->type == J_OBJ) {
            M.pb.has_up_next = true;
            m_strlcpy(M.pb.up_next_rk, json_str(json_get(un, "rating_key"), ""), sizeof(M.pb.up_next_rk));
            m_strlcpy(M.pb.up_next_title, json_str(json_get(un, "title"), ""), sizeof(M.pb.up_next_title));
            m_strlcpy(M.pb.up_next_sub, json_str(json_get(un, "subtitle"), ""), sizeof(M.pb.up_next_sub));
        } else {
            M.pb.has_up_next = false;
        }
        pthread_mutex_unlock(&M.mu);

        if (sub_url[0] && !is_yt) {
            push_job(JOB_FETCH_SUBS, sub_url, "", 0, 0, 0);
        }

        if (!fb_yt && stream_url[0] && M.host.get_wl_surface_id) {
            uint32_t sid = M.host.get_wl_surface_id();
            int player_start_ms = is_remux ? 0 : resume_ms;
            int prc = tizen_player_play_sync(M.player, sid, stream_url, player_start_ms, 0, 0, 1920, 1080);
            if (prc != 0) {
                pthread_mutex_lock(&M.mu);
                snprintf(M.pb.err_msg, sizeof(M.pb.err_msg), "Hardware player start failed (rc=%d)", prc);
                pthread_mutex_unlock(&M.mu);
            }
        }
        json_free(root);
        pthread_mutex_lock(&M.mu); M.loading = false; pthread_mutex_unlock(&M.mu);
        if (M.host.signal_main) M.host.signal_main();
        return;
    }

    if (j->type == JOB_FETCH_SUBS) {
        if (http_get(j->a, 15000, 2 * 1024 * 1024, &resp) && resp.status == 200) {
            JVal *root = json_parse((const char *)resp.body);
            if (root) {
                const JVal *arr = json_get(root, "cues");
                if (arr && arr->type == J_ARR && arr->count > 0) {
                    int cnt = (int)arr->count;
                    if (cnt > 12000) cnt = 12000;
                    SubCue *cues = calloc((size_t)cnt, sizeof(SubCue));
                    if (cues) {
                        for (int i = 0; i < cnt; i++) {
                            const JVal *row = json_at(arr, (size_t)i);
                            cues[i].start_ms = (int)json_i64(json_at(row, 0), 0);
                            cues[i].end_ms = (int)json_i64(json_at(row, 1), 0);
                            m_strlcpy(cues[i].text, json_str(json_at(row, 2), ""), sizeof(cues[i].text));
                        }
                        tizen_player_set_cues(M.player, cues, cnt);
                    }
                }
                json_free(root);
            }
        }
        http_response_free(&resp);
        if (M.host.signal_main) M.host.signal_main();
        return;
    }

    if (j->type == JOB_SYNC_TIMELINE) {
        char body[512];
        if (strcmp(j->a, "youtube") == 0) {
            snprintf(url, sizeof(url), "%s/v1/playback/youtube/progress", M.gateway);
            snprintf(body, sizeof(body),
                     "{\"video_id\":\"%s\",\"time_ms\":%d,\"duration_ms\":%d}",
                     j->b, j->i1, j->i2);
        } else {
            const char *st = (j->i3 == 0) ? "stopped" : (j->i3 == 1 ? "paused" : "playing");
            snprintf(url, sizeof(url), "%s/v1/playback/plex/timeline", M.gateway);
            snprintf(body, sizeof(body),
                     "{\"rating_key\":\"%s\",\"state\":\"%s\",\"time_ms\":%d,\"duration_ms\":%d}",
                     j->b, st, j->i1, j->i2);
        }
        http_post_json(url, body, 4000, &resp);
        http_response_free(&resp);
        return;
    }
}

static void *media_worker_fn(void *arg) {
    (void)arg;
    for (;;) {
        MediaJob job;
        pthread_mutex_lock(&M.mu);
        while (!M.stop && M.j_head == M.j_tail) {
            pthread_cond_wait(&M.cv, &M.mu);
        }
        if (M.stop) { pthread_mutex_unlock(&M.mu); break; }
        job = M.jobs[M.j_head];
        M.j_head = (M.j_head + 1) % JOB_Q_CAP;
        pthread_mutex_unlock(&M.mu);
        exec_job(&job);
    }
    return NULL;
}

/* ── Lifecycle API ── */

void media_os_init(const MediaHostCallbacks *cb, const char *gateway_base_url) {
    memset(&M, 0, sizeof(M));
    M.host = *cb;
    m_strlcpy(M.gateway, gateway_base_url ? gateway_base_url : "http://127.0.0.1:8765", sizeof(M.gateway));
    pthread_mutex_init(&M.mu, NULL);
    pthread_cond_init(&M.cv, NULL);
    M.pool = astc_pool_create(cb->gl_delete_textures);
    M.player = tizen_player_create();
    M.initialized = true;
    pthread_create(&M.worker, NULL, media_worker_fn, NULL);
    /* Warm unified Home data silently right after startup so pressing Plex/YouTube is instant! */
    push_job(JOB_FETCH_HOME, "", "", 0, 0, 0);
}

void media_os_shutdown(void) {
    if (!M.initialized) return;
    tizen_player_stop(M.player);
    pthread_mutex_lock(&M.mu);
    M.stop = true;
    pthread_cond_broadcast(&M.cv);
    pthread_mutex_unlock(&M.mu);
    pthread_join(M.worker, NULL);
    tizen_player_destroy(M.player);
    astc_pool_destroy(M.pool);
    pthread_cond_destroy(&M.cv);
    pthread_mutex_destroy(&M.mu);
    M.initialized = false;
}

int media_os_wake_fd(void) {
    return M.pool ? astc_pool_wake_fd(M.pool) : -1;
}

bool media_os_is_active(void) {
    return M.initialized && M.view != MEDIA_VIEW_NONE;
}

bool media_os_is_playing(void) {
    return M.initialized && M.view == MEDIA_VIEW_PLAYER;
}

MediaViewMode media_os_view(void) {
    return M.view;
}

static void stop_active_playback_and_sync(void) {
    if (!M.pb.active) return;
    /* Paint opaque cover & restore opaque_state BEFORE destroying hardware video plane -> ZERO Samsung Home flash! */
    if (M.host.flush_opaque_cover) M.host.flush_opaque_cover(false);
    TizenPlayerSnapshot sn;
    tizen_player_poll(M.player, &sn);
    int cur_pos = M.pb.is_remux_stream ? (M.pb.base_offset_ms + sn.pos_ms) : (sn.pos_ms > 0 ? sn.pos_ms : M.pb.last_pos_ms);
    int dur = (sn.dur_ms > 0) ? sn.dur_ms : M.pb.known_dur_ms;
    push_job(JOB_SYNC_TIMELINE, M.pb.source, M.pb.item_id, cur_pos, dur, 0 /* stopped */);
    tizen_player_stop(M.player);
    tizen_player_clear_cues(M.player);
    tizen_player_clear_markers(M.player);
    pthread_mutex_lock(&M.mu);
    M.pb.active = false;
    M.pb.player_state = 0;
    M.pb.video_w = 0;
    M.pb.video_h = 0;
    M.pb.video_revealed = false;
    M.pb.play_started_us = 0;
    pthread_mutex_unlock(&M.mu);
    if (M.host.set_opaque_state) M.host.set_opaque_state(true);
}

static void enter_player_view(const char *source, const char *id, int start_ms, int audio_id, int sub_id) {
    if (M.view != MEDIA_VIEW_PLAYER) M.pre_player_view = M.view;
    /* Cover screen with 100% solid pure black BEFORE releasing opaque_state or stopping any previous video! */
    if (M.host.flush_opaque_cover) M.host.flush_opaque_cover(true);
    stop_active_playback_and_sync();
    memset(&M.pb, 0, sizeof(M.pb));
    M.pb.cur_sub_idx = -1;
    M.pb.hud_until_us = m_now_us() + 6500000LL;
    m_strlcpy(M.pb.source, source, sizeof(M.pb.source));
    m_strlcpy(M.pb.item_id, id, sizeof(M.pb.item_id));
    m_strlcpy(M.pb.title, "Loading stream…", sizeof(M.pb.title));
    M.view = MEDIA_VIEW_PLAYER;
    M.loading = true;
    if (M.host.flush_opaque_cover) M.host.flush_opaque_cover(true);
    if (M.host.set_opaque_state) M.host.set_opaque_state(false); /* Ready for hardware overlay once video_revealed flips true */
    push_job(strcmp(source, "youtube") == 0 ? JOB_START_PLAY_YT : JOB_START_PLAY_PLEX,
             id, "", start_ms, audio_id, sub_id);
    if (M.host.mark_full_redraw) M.host.mark_full_redraw(3);
}

static void yt_fetch_current_tab(void) {
    M.grid_count = 0;
    M.grid_idx = 0;
    M.loading = true;
    if (M.yt_tab_idx == 0 || M.yt_tab_idx == 1) {
        push_job(JOB_FETCH_YT_CHANNELS, "", "", M.yt_tab_idx, M.yt_cat_idx, 0);
    } else {
        push_job(JOB_FETCH_YT_FEED, "", "", M.yt_tab_idx, 0, 0);
    }
}

void media_os_open_view(MediaViewMode mode) {
    if (!M.initialized) return;
    if (M.view == MEDIA_VIEW_PLAYER && mode != MEDIA_VIEW_PLAYER) {
        stop_active_playback_and_sync();
    }
    M.prev_view = M.view;
    M.view = mode;
    M.focus_nav = false;
    M.focus_subbar = false;
    M.focus_catbar = false;

    if (mode == MEDIA_VIEW_HOME) {
        M.nav_idx = 1;
        M.loading = !M.home_loaded;
        push_job(JOB_FETCH_HOME, "", "", 0, 0, 0);
    } else if (mode == MEDIA_VIEW_MOVIES) {
        M.nav_idx = 2;
        M.grid_section_id = 1; M.grid_count = 0; M.grid_idx = 0; M.loading = true;
        push_job(JOB_FETCH_GRID, "", "", 1, 0, M.grid_sort_idx | (M.grid_unwatched ? 8 : 0));
    } else if (mode == MEDIA_VIEW_TV) {
        M.nav_idx = 3;
        M.grid_section_id = 2; M.grid_count = 0; M.grid_idx = 0; M.loading = true;
        push_job(JOB_FETCH_GRID, "", "", 2, 0, M.grid_sort_idx | (M.grid_unwatched ? 8 : 0));
    } else if (mode == MEDIA_VIEW_YOUTUBE) {
        M.nav_idx = 4;
        M.grid_section_id = 0;
        if (M.yt_tab_idx < 0 || M.yt_tab_idx > 4) M.yt_tab_idx = 0;
        yt_fetch_current_tab();
    } else if (mode == MEDIA_VIEW_KIDS) {
        M.nav_idx = 5;
        M.loading = !M.kids_loaded;
        push_job(JOB_FETCH_KIDS, "", "", 0, 0, 0);
    } else if (mode == MEDIA_VIEW_SEARCH) {
        M.nav_idx = 6;
        M.search_in_kb = true;
    }
    if (M.host.mark_full_redraw) M.host.mark_full_redraw(3);
}

void media_os_play_plex(const char *rating_key) {
    if (!M.initialized || !rating_key || !rating_key[0]) return;
    enter_player_view("plex", rating_key, 0, -1, -1);
}

void media_os_play_youtube(const char *video_id) {
    if (!M.initialized || !video_id || !video_id[0]) return;
    enter_player_view("youtube", video_id, 0, -1, -1);
}

void media_os_exit_to_hub(void) {
    if (!M.initialized) return;
    if (M.view == MEDIA_VIEW_PLAYER) stop_active_playback_and_sync();
    M.view = MEDIA_VIEW_NONE;
    if (M.host.mark_full_redraw) M.host.mark_full_redraw(3);
}

void media_os_on_hub_hidden(bool flush_vram) {
    if (!M.initialized) return;
    if (M.pb.active) stop_active_playback_and_sync();
    if (flush_vram) {
        astc_pool_flush_all(M.pool);
        if (M.view == MEDIA_VIEW_PLAYER) M.view = MEDIA_VIEW_HOME;
    }
}

/* ── On-screen QWERTY Keyboard for Unified Search ── */
static const char *KB_ROWS[4] = {
    "QWERTYUIOP",
    "ASDFGHJKL<",
    "ZXCVBNM012",
    "3456789 _>"  /* ' '=SPACE, '_'=CLEAR, '>'=GO */
};

static void open_card(const MediaCard *c) {
    if (!c || !c->id[0]) return;
    if (strcmp(c->source, "yt_channel") == 0) {
        M.prev_view = M.view;
        M.view = MEDIA_VIEW_YT_CHANNEL;
        memset(&M.yt_ch, 0, sizeof(M.yt_ch));
        m_strlcpy(M.yt_ch.channel_id, c->id, sizeof(M.yt_ch.channel_id));
        m_strlcpy(M.yt_ch.title, c->title, sizeof(M.yt_ch.title));
        m_strlcpy(M.yt_ch.avatar_url, c->art_url, sizeof(M.yt_ch.avatar_url));
        M.yt_ch.is_kids = c->is_kids;
        M.yt_ch.is_favorite = c->is_favorite;
        M.yt_ch.hdr_btn = 1;
        M.loading = true;
        push_job(JOB_FETCH_YT_CHANNEL, c->id, "", 0, 0, 0);
        if (M.host.mark_full_redraw) M.host.mark_full_redraw(3);
        return;
    }
    if (strcmp(c->source, "youtube") == 0) {
        enter_player_view("youtube", c->id, c->offset_ms, -1, -1);
    } else if (strcmp(c->source, "jellyseerr") == 0) {
        if (c->can_request) {
            if (M.host.show_toast) M.host.show_toast("Sending request to Jellyseerr…", 1500);
            push_job(JOB_SEERR_REQUEST, c->media_type, "", c->tmdb_id, 0, 0);
        } else {
            char msg[96];
            snprintf(msg, sizeof(msg), "%s is already %s", c->title, c->status);
            if (M.host.show_toast) M.host.show_toast(msg, 1800);
        }
    } else {
        /* Plex: Continue Watching episode -> instant play; Movies/Shows -> Detail sheet */
        if (strcmp(c->media_type, "episode") == 0 && M.view != MEDIA_VIEW_DETAIL) {
            enter_player_view("plex", c->id, c->offset_ms, -1, -1);
        } else {
            M.prev_view = M.view;
            M.view = MEDIA_VIEW_DETAIL;
            M.detail.loaded = false;
            M.loading = true;
            push_job(JOB_FETCH_DETAIL, c->id, "", 0, 0, 0);
            if (M.host.mark_full_redraw) M.host.mark_full_redraw(3);
        }
    }
}

static void player_seek_smart(int target_ms) {
    if (target_ms < 0) target_ms = 0;
    if (M.pb.known_dur_ms > 0 && target_ms > M.pb.known_dur_ms - 1500) target_ms = M.pb.known_dur_ms - 1500;
    M.pb.hud_until_us = m_now_us() + 4500000LL;
    if (M.pb.is_remux_stream) {
        /* Seamless server-side seek for YouTube DASH & DTS->EAC3 remux streams! */
        char saved_src[16], saved_id[64];
        m_strlcpy(saved_src, M.pb.source, sizeof(saved_src));
        m_strlcpy(saved_id, M.pb.item_id, sizeof(saved_id));
        int a_id = (M.pb.cur_audio_idx >= 0 && M.pb.cur_audio_idx < M.pb.audio_count) ? M.pb.audio[M.pb.cur_audio_idx].id : -1;
        int s_id = (M.pb.cur_sub_idx >= 0 && M.pb.cur_sub_idx < M.pb.sub_count) ? M.pb.subs[M.pb.cur_sub_idx].id : -1;
        M.pb.base_offset_ms = target_ms;
        M.pb.last_pos_ms = target_ms;
        M.pb.video_revealed = false;
        M.pb.play_started_us = 0;
        M.loading = true;
        if (M.host.flush_opaque_cover) M.host.flush_opaque_cover(true);
        push_job(strcmp(saved_src, "youtube") == 0 ? JOB_START_PLAY_YT : JOB_START_PLAY_PLEX,
                 saved_id, "", target_ms, a_id, s_id);
    } else {
        tizen_player_seek_ms(M.player, target_ms);
        M.pb.last_pos_ms = target_ms;
    }
    if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
}

static void cycle_player_subtitles(void) {
    M.pb.hud_until_us = m_now_us() + 4500000LL;
    if (M.pb.sub_count <= 0) {
        if (M.host.show_toast) M.host.show_toast("No subtitles available", 1600);
        return;
    }
    M.pb.cur_sub_idx++;
    if (M.pb.cur_sub_idx >= M.pb.sub_count) M.pb.cur_sub_idx = -1;
    if (M.pb.cur_sub_idx < 0) {
        tizen_player_clear_cues(M.player);
        M.pb.active_sub_text[0] = '\0';
        if (M.host.show_toast) M.host.show_toast("Subtitles: Off", 1600);
    } else {
        MediaTrack *st = &M.pb.subs[M.pb.cur_sub_idx];
        char msg[96], surl[512];
        snprintf(msg, sizeof(msg), "Subtitles: %s", st->label);
        if (M.host.show_toast) M.host.show_toast(msg, 1800);
        if (strcmp(M.pb.source, "youtube") == 0) {
            snprintf(surl, sizeof(surl), "%s/v1/subtitles/youtube/%s?lang=%s", M.gateway, M.pb.item_id, st->lang);
        } else {
            snprintf(surl, sizeof(surl), "%s/v1/subtitles/plex/%s/%d", M.gateway, M.pb.item_id, st->id);
        }
        push_job(JOB_FETCH_SUBS, surl, "", 0, 0, 0);
    }
    if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
}

/* ── Main-loop tick (GL thread): ASTC uploads, playback polling, subtitles, skip markers ── */

bool media_os_tick(int64_t now_us) {
    if (!M.initialized) return false;
    bool dirty = false;

    if (astc_pool_pump_gl(M.pool) > 0 && M.view != MEDIA_VIEW_NONE && M.view != MEDIA_VIEW_PLAYER) {
        dirty = true;
    }

    if (M.view == MEDIA_VIEW_PLAYER && M.pb.active) {
        int64_t poll_interval = M.pb.video_revealed ? 250000LL : 100000LL;
        if (now_us - M.pb.last_poll_us >= poll_interval) {
            M.pb.last_poll_us = now_us;
            TizenPlayerSnapshot sn;
            tizen_player_poll(M.player, &sn);
            pthread_mutex_lock(&M.mu);
            int prev_st = M.pb.player_state, prev_vw = M.pb.video_w, prev_vh = M.pb.video_h;
            M.pb.player_state = sn.state;
            if (sn.video_w > 0 && sn.video_h > 0) { M.pb.video_w = sn.video_w; M.pb.video_h = sn.video_h; }
            if (M.pb.player_state != prev_st || M.pb.video_w != prev_vw || M.pb.video_h != prev_vh) dirty = true;
            if (!M.loading && !M.pb.video_revealed && sn.state >= TP_STATE_PLAYING && M.pb.video_w > 0 && M.pb.video_h > 0) {
                if (M.pb.play_started_us == 0) {
                    M.pb.play_started_us = now_us;
                    M.pb.play_start_pos_ms = sn.pos_ms;
                }
                if (sn.state == TP_STATE_PAUSED || sn.pos_ms != M.pb.play_start_pos_ms || (now_us - M.pb.play_started_us) >= 350000LL) {
                    M.pb.video_revealed = true;
                    M.pb.hud_until_us = now_us + 5500000LL;
                    dirty = true;
                }
            }
            int pos = M.pb.last_pos_ms;
            if (sn.active && !sn.preparing) {
                pos = M.pb.is_remux_stream ? (M.pb.base_offset_ms + sn.pos_ms) : sn.pos_ms;
                if (sn.dur_ms > 0) M.pb.known_dur_ms = sn.dur_ms;
                M.pb.paused = (sn.state == TP_STATE_PAUSED);
                if (pos != M.pb.last_pos_ms) {
                    M.pb.last_pos_ms = pos;
                    if (now_us < M.pb.hud_until_us) dirty = true;
                }
            }

            /* Native FreeType subtitle cue update (dirty ONLY when subtitle line actually changes!) */
            char want_cue[256];
            tizen_player_copy_active_cue(M.player, pos, want_cue, sizeof(want_cue));
            if (strcmp(M.pb.active_sub_text, want_cue) != 0) {
                m_strlcpy(M.pb.active_sub_text, want_cue, sizeof(M.pb.active_sub_text));
                dirty = true;
            }

            /* Skip Intro / Credits / SponsorBlock Auto-Skip */
            SkipMarker mk;
            bool has_mk = tizen_player_check_marker(M.player, pos, &mk, sn.state == TP_STATE_PLAYING);
            char want_pill[64] = "";
            int want_target = 0;
            bool want_up = false;
            int auto_seek_target = -1;
            char auto_toast[64] = "";
            if (has_mk) {
                if (mk.auto_skip && !mk.triggered && sn.state == TP_STATE_PLAYING) {
                    snprintf(auto_toast, sizeof(auto_toast), "Skipped %s", mk.type);
                    auto_seek_target = mk.end_ms;
                    dirty = true;
                } else if (!mk.auto_skip) {
                    if (strcmp(mk.type, "credits") == 0 && M.pb.has_up_next) {
                        snprintf(want_pill, sizeof(want_pill), "OK \xC2\xB7 Next Episode");
                        want_up = true;
                    } else {
                        snprintf(want_pill, sizeof(want_pill), "OK \xC2\xB7 Skip %s",
                                 strcmp(mk.type, "intro") == 0 ? "Intro" : "Credits");
                        want_target = mk.end_ms;
                    }
                }
            } else if (M.pb.has_up_next && M.pb.known_dur_ms > 15000 && pos >= M.pb.known_dur_ms - 12000) {
                snprintf(want_pill, sizeof(want_pill), "OK \xC2\xB7 Next Episode");
                want_up = true;
            }
            if (strcmp(M.pb.skip_pill_text, want_pill) != 0) {
                m_strlcpy(M.pb.skip_pill_text, want_pill, sizeof(M.pb.skip_pill_text));
                M.pb.skip_pill_target_ms = want_target;
                M.pb.skip_pill_is_up_next = want_up;
                dirty = true;
            }

            bool do_sync = false;
            char sync_src[16] = "", sync_id[64] = "";
            int sync_dur = M.pb.known_dur_ms;
            if (sn.state == TP_STATE_PLAYING && now_us - M.pb.last_sync_us >= 15000000LL) {
                M.pb.last_sync_us = now_us;
                do_sync = true;
                m_strlcpy(sync_src, M.pb.source, sizeof(sync_src));
                m_strlcpy(sync_id, M.pb.item_id, sizeof(sync_id));
            }
            pthread_mutex_unlock(&M.mu);

            if (auto_toast[0] && M.host.show_toast) M.host.show_toast(auto_toast, 2200);
            if (auto_seek_target >= 0) player_seek_smart(auto_seek_target);
            if (do_sync) push_job(JOB_SYNC_TIMELINE, sync_src, sync_id, pos, sync_dur, 2 /* playing */);
        }
        /* Clear HUD right when auto-hide deadline expires -> minimal GPU/CPU during movie! */
        if (M.pb.hud_until_us > 0 && now_us >= M.pb.hud_until_us) {
            M.pb.hud_until_us = 0;
            M.pb.hud_focused = false;
            dirty = true;
        }
        if (M.pb.feedback_until_us > 0 && now_us >= M.pb.feedback_until_us) {
            M.pb.feedback_until_us = 0;
            M.pb.feedback_txt[0] = '\0';
            dirty = true;
        }
    }

    if (dirty && M.host.mark_full_redraw) M.host.mark_full_redraw(2);
    return dirty;
}

int media_os_next_timeout_ms(int64_t now_us) {
    if (!M.initialized || M.view != MEDIA_VIEW_PLAYER) return -1;
    if (M.pb.active || M.loading) {
        (void)now_us;
        return M.pb.video_revealed ? 250 : 100;
    }
    return -1;
}

/* ── Remote Control Key Handling ── */

static void activate_nav_tab(int idx) {
    switch (idx) {
        case 0: media_os_exit_to_hub(); break;
        case 1: media_os_open_view(MEDIA_VIEW_HOME); break;
        case 2: media_os_open_view(MEDIA_VIEW_MOVIES); break;
        case 3: media_os_open_view(MEDIA_VIEW_TV); break;
        case 4: media_os_open_view(MEDIA_VIEW_YOUTUBE); break;
        case 5: media_os_open_view(MEDIA_VIEW_KIDS); break;
        case 6: media_os_open_view(MEDIA_VIEW_SEARCH); break;
        case 7:
            media_os_on_hub_hidden(true);
            M.view = MEDIA_VIEW_NONE;
            if (M.host.switch_switch2) M.host.switch_switch2();
            break;
    }
}

bool media_os_handle_key(uint32_t ev) {
    if (!M.initialized || M.view == MEDIA_VIEW_NONE) return false;

    /* ── Player Controls (Dual: 1-tap Remote Shortcuts + Big Focusable 10-Foot Transport Bar!) ── */
    if (M.view == MEDIA_VIEW_PLAYER) {
        int64_t now = m_now_us();
        if (is_back(ev) || ev == K_STOP) {
            stop_active_playback_and_sync();
            M.view = (M.pre_player_view != MEDIA_VIEW_NONE && M.pre_player_view != MEDIA_VIEW_PLAYER)
                     ? M.pre_player_view : MEDIA_VIEW_HOME;
            if (M.host.mark_full_redraw) M.host.mark_full_redraw(3);
            return true;
        }
        if (M.pb.fallback_stock_yt && is_ok(ev)) {
            stop_active_playback_and_sync();
            M.view = MEDIA_VIEW_YOUTUBE;
            if (M.host.launch_stock_youtube) M.host.launch_stock_youtube();
            return true;
        }
        if (is_ok(ev) && !M.pb.hud_focused && M.pb.skip_pill_text[0]) {
            if (M.pb.skip_pill_is_up_next && M.pb.up_next_rk[0]) {
                char nxt[64]; m_strlcpy(nxt, M.pb.up_next_rk, sizeof(nxt));
                enter_player_view("plex", nxt, 0, -1, -1);
            } else if (M.pb.skip_pill_target_ms > 0) {
                player_seek_smart(M.pb.skip_pill_target_ms);
            }
            return true;
        }

        /* Dedicated hardware Play/Pause/RW/FF/Subtitle keys ALWAYS work immediately */
        if (ev == K_PLAYPAUSE || ev == K_PLAY || ev == K_PAUSE) {
            M.pb.hud_until_us = now + 5000000LL;
            if (ev == K_PLAY) { tizen_player_resume(M.player); M.pb.paused = false; }
            else if (ev == K_PAUSE) { tizen_player_pause(M.player); M.pb.paused = true; }
            else { tizen_player_toggle_pause(M.player); M.pb.paused = !M.pb.paused; }
            snprintf(M.pb.feedback_txt, sizeof(M.pb.feedback_txt), "%s", M.pb.paused ? "PAUSED" : "PLAYING");
            M.pb.feedback_until_us = now + 1100000LL;
            push_job(JOB_SYNC_TIMELINE, M.pb.source, M.pb.item_id, M.pb.last_pos_ms, M.pb.known_dur_ms, M.pb.paused ? 1 : 2);
            if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
            return true;
        }
        if (ev == K_RW) {
            snprintf(M.pb.feedback_txt, sizeof(M.pb.feedback_txt), "-15s");
            M.pb.feedback_until_us = now + 1000000LL;
            player_seek_smart(M.pb.last_pos_ms - 15000);
            return true;
        }
        if (ev == K_FF) {
            snprintf(M.pb.feedback_txt, sizeof(M.pb.feedback_txt), "+30s");
            M.pb.feedback_until_us = now + 1000000LL;
            player_seek_smart(M.pb.last_pos_ms + 30000);
            return true;
        }
        if (ev == K_SUBTITLE) {
            cycle_player_subtitles();
            const char *sn = (M.pb.cur_sub_idx >= 0 && M.pb.cur_sub_idx < M.pb.sub_count) ? M.pb.subs[M.pb.cur_sub_idx].label : "Off";
            snprintf(M.pb.feedback_txt, sizeof(M.pb.feedback_txt), "CC: %.36s", sn);
            M.pb.feedback_until_us = now + 1400000LL;
            return true;
        }

        /* Interactive 5-button Transport Bar navigation when focused (DOWN/INFO wakes it!) */
        if (M.pb.hud_focused) {
            if (ev == K_LEFT) {
                M.pb.hud_btn_idx = (M.pb.hud_btn_idx + 4) % 5;
                M.pb.hud_until_us = now + 5500000LL;
                if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
                return true;
            }
            if (ev == K_RIGHT) {
                M.pb.hud_btn_idx = (M.pb.hud_btn_idx + 1) % 5;
                M.pb.hud_until_us = now + 5500000LL;
                if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
                return true;
            }
            if (ev == K_UP) {
                /* Hide controls cleanly back to unobstructed full cinema */
                M.pb.hud_focused = false;
                M.pb.hud_until_us = 0;
                if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
                return true;
            }
            if (ev == K_DOWN || ev == K_INFO) {
                M.pb.hud_until_us = now + 5500000LL;
                if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
                return true;
            }
            if (is_ok(ev)) {
                M.pb.hud_until_us = now + 5500000LL;
                switch (M.pb.hud_btn_idx) {
                    case 0: /* -15s */
                        snprintf(M.pb.feedback_txt, sizeof(M.pb.feedback_txt), "-15s");
                        M.pb.feedback_until_us = now + 1000000LL;
                        player_seek_smart(M.pb.last_pos_ms - 15000);
                        break;
                    case 1: /* Play / Pause */
                        tizen_player_toggle_pause(M.player);
                        M.pb.paused = !M.pb.paused;
                        snprintf(M.pb.feedback_txt, sizeof(M.pb.feedback_txt), "%s", M.pb.paused ? "PAUSED" : "PLAYING");
                        M.pb.feedback_until_us = now + 1100000LL;
                        push_job(JOB_SYNC_TIMELINE, M.pb.source, M.pb.item_id, M.pb.last_pos_ms, M.pb.known_dur_ms, M.pb.paused ? 1 : 2);
                        if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
                        break;
                    case 2: /* +30s */
                        snprintf(M.pb.feedback_txt, sizeof(M.pb.feedback_txt), "+30s");
                        M.pb.feedback_until_us = now + 1000000LL;
                        player_seek_smart(M.pb.last_pos_ms + 30000);
                        break;
                    case 3: { /* Subtitles */
                        cycle_player_subtitles();
                        const char *sn = (M.pb.cur_sub_idx >= 0 && M.pb.cur_sub_idx < M.pb.sub_count) ? M.pb.subs[M.pb.cur_sub_idx].label : "Off";
                        snprintf(M.pb.feedback_txt, sizeof(M.pb.feedback_txt), "CC: %.36s", sn);
                        M.pb.feedback_until_us = now + 1400000LL;
                        break;
                    }
                    case 4: /* Exit Movie */
                        stop_active_playback_and_sync();
                        M.view = (M.pre_player_view != MEDIA_VIEW_NONE && M.pre_player_view != MEDIA_VIEW_PLAYER)
                                 ? M.pre_player_view : MEDIA_VIEW_HOME;
                        if (M.host.mark_full_redraw) M.host.mark_full_redraw(3);
                        break;
                }
                return true;
            }
        }

        /* Unfocused cinema shortcuts: OK=Play/Pause + focus bar, LEFT=-15s, RIGHT=+30s, DOWN/INFO=show big controls! */
        if (is_ok(ev)) {
            M.pb.hud_until_us = now + 5500000LL;
            M.pb.hud_focused = true;
            M.pb.hud_btn_idx = 1;
            tizen_player_toggle_pause(M.player);
            M.pb.paused = !M.pb.paused;
            snprintf(M.pb.feedback_txt, sizeof(M.pb.feedback_txt), "%s", M.pb.paused ? "PAUSED" : "PLAYING");
            M.pb.feedback_until_us = now + 1100000LL;
            push_job(JOB_SYNC_TIMELINE, M.pb.source, M.pb.item_id, M.pb.last_pos_ms, M.pb.known_dur_ms, M.pb.paused ? 1 : 2);
            if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
            return true;
        }
        if (ev == K_LEFT) {
            snprintf(M.pb.feedback_txt, sizeof(M.pb.feedback_txt), "-15s");
            M.pb.feedback_until_us = now + 1000000LL;
            player_seek_smart(M.pb.last_pos_ms - 15000);
            return true;
        }
        if (ev == K_RIGHT) {
            snprintf(M.pb.feedback_txt, sizeof(M.pb.feedback_txt), "+30s");
            M.pb.feedback_until_us = now + 1000000LL;
            player_seek_smart(M.pb.last_pos_ms + 30000);
            return true;
        }
        if (ev == K_DOWN || ev == K_INFO) {
            M.pb.hud_focused = true;
            M.pb.hud_btn_idx = 1;
            M.pb.hud_until_us = now + 5500000LL;
            if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
            return true;
        }
        if (ev == K_UP) {
            if (M.pb.hud_until_us > now) {
                M.pb.hud_until_us = 0;
                M.pb.hud_focused = false;
                if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
            } else {
                M.pb.hud_focused = true;
                M.pb.hud_btn_idx = 3; /* Focus CC Subtitles directly */
                M.pb.hud_until_us = now + 5500000LL;
                if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
            }
            return true;
        }
        if (ev == K_BLUE) {
            /* 1-click escape to Stock YouTube / Plex */
            bool is_yt = strcmp(M.pb.source, "youtube") == 0;
            stop_active_playback_and_sync();
            M.view = MEDIA_VIEW_NONE;
            if (is_yt && M.host.launch_stock_youtube) M.host.launch_stock_youtube();
            else if (!is_yt && M.host.launch_stock_plex) M.host.launch_stock_plex();
            return true;
        }
        return true;
    }

    /* ── Top Nav Bar (shared across Home/Movies/TV/YouTube/Kids/Search) ── */
    if (M.focus_nav && M.view != MEDIA_VIEW_DETAIL && M.view != MEDIA_VIEW_YT_CHANNEL) {
        if (ev == K_LEFT)       { M.nav_idx = (M.nav_idx + NAV_COUNT - 1) % NAV_COUNT; }
        else if (ev == K_RIGHT) { M.nav_idx = (M.nav_idx + 1) % NAV_COUNT; }
        else if (ev == K_DOWN)  { M.focus_nav = false; }
        else if (is_ok(ev))     { activate_nav_tab(M.nav_idx); return true; }
        else if (is_back(ev))   { media_os_exit_to_hub(); return true; }
        else return false;
        if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
        return true;
    }

    /* ── Home View (4 horizontal rows) ── */
    if (M.view == MEDIA_VIEW_HOME) {
        if (is_back(ev)) { media_os_exit_to_hub(); return true; }
        int cnt = M.home_counts[M.home_row];
        if (ev == K_LEFT && cnt > 0) { if (M.home_col[M.home_row] > 0) M.home_col[M.home_row]--; }
        else if (ev == K_RIGHT && cnt > 0) { if (M.home_col[M.home_row] + 1 < cnt) M.home_col[M.home_row]++; }
        else if (ev == K_UP) {
            int nr = M.home_row - 1;
            while (nr >= 0 && M.home_counts[nr] == 0) nr--;
            if (nr < 0) M.focus_nav = true;
            else M.home_row = nr;
        } else if (ev == K_DOWN) {
            int nr = M.home_row + 1;
            while (nr < 4 && M.home_counts[nr] == 0) nr++;
            if (nr < 4) M.home_row = nr;
        } else if (is_ok(ev) && cnt > 0) {
            open_card(&M.home_rows[M.home_row][M.home_col[M.home_row]]);
            return true;
        } else return false;
        if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
        return true;
    }

    /* ── Kids View (4 horizontal rows: CW, Dedicated Kids YouTube Channels, Kids Movies, Kids TV) ── */
    if (M.view == MEDIA_VIEW_KIDS) {
        if (is_back(ev)) { media_os_open_view(MEDIA_VIEW_HOME); return true; }
        int cnt = M.kids_counts[M.kids_row];
        if (ev == K_LEFT && cnt > 0) { if (M.kids_col[M.kids_row] > 0) M.kids_col[M.kids_row]--; }
        else if (ev == K_RIGHT && cnt > 0) { if (M.kids_col[M.kids_row] + 1 < cnt) M.kids_col[M.kids_row]++; }
        else if (ev == K_UP) {
            int nr = M.kids_row - 1;
            while (nr >= 0 && M.kids_counts[nr] == 0) nr--;
            if (nr < 0) M.focus_nav = true;
            else M.kids_row = nr;
        } else if (ev == K_DOWN) {
            int nr = M.kids_row + 1;
            while (nr < 4 && M.kids_counts[nr] == 0) nr++;
            if (nr < 4) M.kids_row = nr;
        } else if (is_ok(ev) && cnt > 0) {
            open_card(&M.kids_rows[M.kids_row][M.kids_col[M.kids_row]]);
            return true;
        } else return false;
        if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
        return true;
    }

    /* ── Grid Views (Movies, TV Shows, YouTube Adults/Kids Channels & Feeds) ── */
    if (M.view == MEDIA_VIEW_MOVIES || M.view == MEDIA_VIEW_TV || M.view == MEDIA_VIEW_YOUTUBE) {
        if (is_back(ev)) { media_os_open_view(MEDIA_VIEW_HOME); return true; }
        bool is_yt = (M.view == MEDIA_VIEW_YOUTUBE);
        bool yt_chan_mode = is_yt && (M.yt_tab_idx == 0 || M.yt_tab_idx == 1);
        bool has_catbar = is_yt && (M.yt_tab_idx == 0);
        int cols = is_yt ? (yt_chan_mode ? 6 : 4) : 7;
        int sub_count = is_yt ? 6 : 5;

        if (M.focus_subbar) {
            if (ev == K_LEFT)       { M.subbar_idx = (M.subbar_idx + sub_count - 1) % sub_count; }
            else if (ev == K_RIGHT) { M.subbar_idx = (M.subbar_idx + 1) % sub_count; }
            else if (ev == K_UP)    { M.focus_subbar = false; M.focus_nav = true; }
            else if (ev == K_DOWN)  { M.focus_subbar = false; if (has_catbar) M.focus_catbar = true; }
            else if (is_ok(ev)) {
                if (is_yt) {
                    if (M.subbar_idx == 5) {
                        media_os_exit_to_hub();
                        if (M.host.launch_stock_youtube) M.host.launch_stock_youtube();
                        return true;
                    }
                    M.yt_tab_idx = M.subbar_idx;
                    M.focus_catbar = false;
                    yt_fetch_current_tab();
                } else {
                    if (M.subbar_idx == 4) {
                        media_os_exit_to_hub();
                        if (M.host.launch_stock_plex) M.host.launch_stock_plex();
                        return true;
                    }
                    if (M.subbar_idx < 3) M.grid_sort_idx = M.subbar_idx;
                    else M.grid_unwatched = !M.grid_unwatched;
                    M.grid_count = 0; M.loading = true;
                    push_job(JOB_FETCH_GRID, "", "", M.grid_section_id, 0, M.grid_sort_idx | (M.grid_unwatched ? 8 : 0));
                }
            } else return false;
            if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
            return true;
        }

        if (has_catbar && M.focus_catbar) {
            if (ev == K_LEFT)       { M.yt_cat_idx = (M.yt_cat_idx + 7) % 8; yt_fetch_current_tab(); }
            else if (ev == K_RIGHT) { M.yt_cat_idx = (M.yt_cat_idx + 1) % 8; yt_fetch_current_tab(); }
            else if (ev == K_UP)    { M.focus_catbar = false; M.focus_subbar = true; }
            else if (ev == K_DOWN)  { M.focus_catbar = false; }
            else if (is_ok(ev))     { M.focus_catbar = false; yt_fetch_current_tab(); }
            else return false;
            if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
            return true;
        }

        if (M.grid_count == 0) {
            if (ev == K_UP) {
                if (has_catbar) M.focus_catbar = true;
                else M.focus_subbar = true;
                if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
                return true;
            }
            return false;
        }
        if (ev == K_LEFT)       { if (M.grid_idx > 0) M.grid_idx--; }
        else if (ev == K_RIGHT) { if (M.grid_idx + 1 < M.grid_count) M.grid_idx++; }
        else if (ev == K_UP) {
            if (M.grid_idx < cols) {
                if (has_catbar) M.focus_catbar = true;
                else M.focus_subbar = true;
            } else M.grid_idx -= cols;
        } else if (ev == K_DOWN) {
            if (M.grid_idx + cols < M.grid_count) M.grid_idx += cols;
            else if (M.grid_idx < M.grid_count - 1) M.grid_idx = M.grid_count - 1;
            /* Auto-paginate Plex grid when approaching bottom */
            if (!is_yt && !M.loading && M.grid_count < M.grid_total && M.grid_count < MAX_GRID_CARDS &&
                M.grid_idx >= M.grid_count - cols * 2) {
                M.loading = true;
                push_job(JOB_FETCH_GRID, "", "", M.grid_section_id, M.grid_count,
                         M.grid_sort_idx | (M.grid_unwatched ? 8 : 0));
            }
        } else if (is_ok(ev)) {
            open_card(&M.grid_items[M.grid_idx]);
            return true;
        } else return false;
        if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
        return true;
    }

    /* ── Dedicated YouTube Channel Page (Adults or Kids) ── */
    if (M.view == MEDIA_VIEW_YT_CHANNEL) {
        if (is_back(ev)) {
            MediaViewMode dst = (M.prev_view != MEDIA_VIEW_NONE && M.prev_view != MEDIA_VIEW_YT_CHANNEL)
                                ? M.prev_view : MEDIA_VIEW_YOUTUBE;
            media_os_open_view(dst);
            return true;
        }
        if (M.yt_ch.focus_header) {
            if (ev == K_LEFT)       { M.yt_ch.hdr_btn = (M.yt_ch.hdr_btn + 2) % 3; }
            else if (ev == K_RIGHT) { M.yt_ch.hdr_btn = (M.yt_ch.hdr_btn + 1) % 3; }
            else if (ev == K_DOWN && M.yt_ch.video_count > 0) { M.yt_ch.focus_header = false; }
            else if (is_ok(ev)) {
                if (M.yt_ch.hdr_btn == 0) {
                    MediaViewMode dst = (M.prev_view != MEDIA_VIEW_NONE && M.prev_view != MEDIA_VIEW_YT_CHANNEL)
                                        ? M.prev_view : MEDIA_VIEW_YOUTUBE;
                    media_os_open_view(dst);
                    return true;
                } else if (M.yt_ch.hdr_btn == 1) {
                    push_job(JOB_CLASSIFY_YT_CHANNEL, M.yt_ch.channel_id, "", 2, M.yt_ch.is_favorite ? 0 : 1, 0);
                } else if (M.yt_ch.hdr_btn == 2) {
                    push_job(JOB_CLASSIFY_YT_CHANNEL, M.yt_ch.channel_id, M.yt_ch.is_kids ? "adults" : "kids", 1, 0, 0);
                }
            } else return false;
            if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
            return true;
        }
        int cols = 4;
        if (M.yt_ch.video_count == 0) {
            if (ev == K_UP) { M.yt_ch.focus_header = true; if (M.host.mark_full_redraw) M.host.mark_full_redraw(2); return true; }
            return false;
        }
        if (ev == K_LEFT)       { if (M.yt_ch.vid_idx > 0) M.yt_ch.vid_idx--; }
        else if (ev == K_RIGHT) { if (M.yt_ch.vid_idx + 1 < M.yt_ch.video_count) M.yt_ch.vid_idx++; }
        else if (ev == K_UP) {
            if (M.yt_ch.vid_idx < cols) M.yt_ch.focus_header = true;
            else M.yt_ch.vid_idx -= cols;
        } else if (ev == K_DOWN) {
            if (M.yt_ch.vid_idx + cols < M.yt_ch.video_count) M.yt_ch.vid_idx += cols;
            else if (M.yt_ch.vid_idx < M.yt_ch.video_count - 1) M.yt_ch.vid_idx = M.yt_ch.video_count - 1;
        } else if (is_ok(ev)) {
            open_card(&M.yt_ch.videos[M.yt_ch.vid_idx]);
            return true;
        } else return false;
        if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
        return true;
    }

    /* ── Detail Sheet ── */
    if (M.view == MEDIA_VIEW_DETAIL) {
        if (is_back(ev)) {
            M.view = (M.prev_view != MEDIA_VIEW_NONE && M.prev_view != MEDIA_VIEW_DETAIL) ? M.prev_view : MEDIA_VIEW_HOME;
            if (M.host.mark_full_redraw) M.host.mark_full_redraw(3);
            return true;
        }
        bool playable = (strcmp(M.detail.type, "movie") == 0 || strcmp(M.detail.type, "episode") == 0);
        int cta_cnt = playable ? 4 : 1; /* Play/Resume, Start Over, Audio, Subtitles */
        if (M.detail_zone == 0 && playable) {
            if (ev == K_LEFT)       { if (M.detail_cta_idx > 0) M.detail_cta_idx--; }
            else if (ev == K_RIGHT) { if (M.detail_cta_idx + 1 < cta_cnt) M.detail_cta_idx++; }
            else if (ev == K_DOWN && M.detail.child_count > 0) { M.detail_zone = 1; }
            else if (is_ok(ev)) {
                int a_id = (M.detail.sel_audio_idx >= 0 && M.detail.sel_audio_idx < M.detail.audio_count) ? M.detail.audio[M.detail.sel_audio_idx].id : -1;
                int s_id = (M.detail.sel_sub_idx >= 0 && M.detail.sel_sub_idx < M.detail.sub_count) ? M.detail.subs[M.detail.sel_sub_idx].id : 0;
                if (M.detail_cta_idx == 0) {
                    enter_player_view("plex", M.detail.rating_key, M.detail.offset_ms, a_id, s_id);
                    return true;
                } else if (M.detail_cta_idx == 1) {
                    enter_player_view("plex", M.detail.rating_key, 0, a_id, s_id);
                    return true;
                } else if (M.detail_cta_idx == 2 && M.detail.audio_count > 0) {
                    M.detail.sel_audio_idx = (M.detail.sel_audio_idx + 1) % M.detail.audio_count;
                } else if (M.detail_cta_idx == 3 && M.detail.sub_count > 0) {
                    M.detail.sel_sub_idx++;
                    if (M.detail.sel_sub_idx >= M.detail.sub_count) M.detail.sel_sub_idx = -1;
                }
            } else return false;
        } else if (M.detail.child_count > 0) {
            if (ev == K_LEFT)       { if (M.detail_child_idx > 0) M.detail_child_idx--; }
            else if (ev == K_RIGHT) { if (M.detail_child_idx + 1 < M.detail.child_count) M.detail_child_idx++; }
            else if (ev == K_UP && playable) { M.detail_zone = 0; }
            else if (is_ok(ev)) {
                MediaCard child = M.detail.children[M.detail_child_idx];
                if (strcmp(child.media_type, "episode") == 0) {
                    enter_player_view("plex", child.id, child.offset_ms, -1, -1);
                } else {
                    M.detail.loaded = false; M.loading = true;
                    push_job(JOB_FETCH_DETAIL, child.id, "", 0, 0, 0);
                }
                return true;
            } else return false;
        } else return false;
        if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
        return true;
    }

    /* ── Unified Search (On-Screen Keyboard + Plex / YouTube / Jellyseerr Results) ── */
    if (M.view == MEDIA_VIEW_SEARCH) {
        if (is_back(ev)) {
            if (!M.search_in_kb) { M.search_in_kb = true; if (M.host.mark_full_redraw) M.host.mark_full_redraw(2); return true; }
            media_os_open_view(MEDIA_VIEW_HOME);
            return true;
        }
        if (M.search_in_kb) {
            if (ev == K_LEFT)       { M.kb_col = (M.kb_col + 9) % 10; }
            else if (ev == K_RIGHT) { M.kb_col = (M.kb_col + 1) % 10; }
            else if (ev == K_UP) {
                if (M.kb_row == 0) M.focus_nav = true;
                else M.kb_row--;
            } else if (ev == K_DOWN) {
                if (M.kb_row < 3) M.kb_row++;
                else if (M.search_counts[0] + M.search_counts[1] + M.search_counts[2] > 0) M.search_in_kb = false;
            } else if (is_ok(ev)) {
                char ch = KB_ROWS[M.kb_row][M.kb_col];
                size_t len = strlen(M.search_query);
                if (ch == '<') {
                    if (len > 0) M.search_query[len - 1] = '\0';
                } else if (ch == '_') {
                    M.search_query[0] = '\0';
                } else if (ch == '>') {
                    if (len > 0) {
                        M.loading = true;
                        push_job(JOB_FETCH_SEARCH, M.search_query, "", 0, 0, 0);
                    }
                } else if (len + 1 < sizeof(M.search_query)) {
                    M.search_query[len] = (char)tolower((unsigned char)ch);
                    M.search_query[len + 1] = '\0';
                    if (strlen(M.search_query) >= 2) {
                        M.loading = true;
                        push_job(JOB_FETCH_SEARCH, M.search_query, "", 0, 0, 0);
                    }
                }
            } else return false;
        } else {
            int r = M.search_res_row;
            int cnt = M.search_counts[r];
            if (ev == K_LEFT && cnt > 0) { if (M.search_res_col[r] > 0) M.search_res_col[r]--; }
            else if (ev == K_RIGHT && cnt > 0) { if (M.search_res_col[r] + 1 < cnt) M.search_res_col[r]++; }
            else if (ev == K_UP) {
                int nr = r - 1;
                while (nr >= 0 && M.search_counts[nr] == 0) nr--;
                if (nr < 0) M.search_in_kb = true;
                else M.search_res_row = nr;
            } else if (ev == K_DOWN) {
                int nr = r + 1;
                while (nr < 3 && M.search_counts[nr] == 0) nr++;
                if (nr < 3) M.search_res_row = nr;
            } else if (is_ok(ev) && cnt > 0) {
                open_card(&M.search_rows[r][M.search_res_col[r]]);
                return true;
            } else return false;
        }
        if (M.host.mark_full_redraw) M.host.mark_full_redraw(2);
        return true;
    }

    return false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  1920x1080 OBSIDIAN GLASS MALI GLES2 + ASTC RENDERER
 * ═══════════════════════════════════════════════════════════════════════════ */

static void draw_top_nav(Renderer *r) {
    /* Subtle obsidian glass top header bar */
    r_round_rect(r, 0, 0, 1920, 92, 0, 0.03f, 0.04f, 0.065f, 0.88f, 0, 0, 0, 0, 0);
    r_round_rect(r, 0, 91, 1920, 1, 0, 1.0f, 1.0f, 1.0f, 0.08f, 0, 0, 0, 0, 0);

    /* Nav pills */
    float x = 72.0f;
    for (int i = 0; i < NAV_COUNT; i++) {
        float w = (i == 3 || i == 4 || i == 7) ? 136.0f : 112.0f;
        bool active_view = ((i == 1 && M.view == MEDIA_VIEW_HOME) ||
                            (i == 2 && M.view == MEDIA_VIEW_MOVIES) ||
                            (i == 3 && M.view == MEDIA_VIEW_TV) ||
                            (i == 4 && M.view == MEDIA_VIEW_YOUTUBE) ||
                            (i == 5 && M.view == MEDIA_VIEW_KIDS) ||
                            (i == 6 && M.view == MEDIA_VIEW_SEARCH));
        bool foc = (M.focus_nav && M.nav_idx == i);
        if (foc) {
            r_round_rect(r, x, 24, w, 44, 22, 0.96f, 0.97f, 0.99f, 1.0f, 0, 0, 0, 0, 0);
            r_text(r, 1, NAV_LABELS[i], x, 24, w, 44, 1, 0.04f, 0.05f, 0.09f, 1.0f);
        } else if (active_view) {
            r_round_rect(r, x, 24, w, 44, 22, 1.0f, 1.0f, 1.0f, 0.14f, 1.0f, 1.0f, 1.0f, 0.25f, 1.2f);
            r_text(r, 1, NAV_LABELS[i], x, 24, w, 44, 1, 0.96f, 0.97f, 0.99f, 1.0f);
        } else if (i == 7) {
            r_round_rect(r, x, 24, w, 44, 22, 0.0f, 0.78f, 0.92f, 0.14f, 0.0f, 0.78f, 0.92f, 0.35f, 1.0f);
            r_text(r, 1, NAV_LABELS[i], x, 24, w, 44, 1, 0.65f, 0.95f, 0.99f, 1.0f);
        } else {
            r_text(r, 1, NAV_LABELS[i], x, 24, w, 44, 1, 0.64f, 0.69f, 0.78f, 1.0f);
        }
        x += w + 10.0f;
    }

    /* Right status: Volume + Clock */
    int vol = 0, mem = 0; bool muted = false, sw = false;
    if (M.host.get_top_status) M.host.get_top_status(&vol, &muted, &mem, &sw);
    char vbuf[32];
    snprintf(vbuf, sizeof(vbuf), muted ? "MUTED" : "VOL %d", vol);
    r_round_rect(r, 1590, 26, 114, 40, 20, 1.0f, 1.0f, 1.0f, 0.06f, 1.0f, 1.0f, 1.0f, 0.12f, 1.0f);
    r_text(r, 0, vbuf, 1590, 26, 114, 40, 1, muted ? 0.99f : 0.85f, muted ? 0.45f : 0.89f, muted ? 0.50f : 0.95f, 1.0f);

    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    int hr = tm.tm_hour % 12; if (hr == 0) hr = 12;
    char tbuf[24];
    snprintf(tbuf, sizeof(tbuf), "%d:%02d %s", hr, tm.tm_min, tm.tm_hour >= 12 ? "PM" : "AM");
    r_text(r, 4, tbuf, 1716, 24, 132, 44, 2, 0.95f, 0.96f, 0.99f, 1.0f);
}

static void draw_card(Renderer *r, const MediaCard *c, float x, float y, float w, float h, bool focused) {
    float lift = focused ? -6.0f : 0.0f;
    float cy = y + lift;
    float art_h = h - 56.0f;

    /* Card backdrop & artwork */
    r_round_rect(r, x, cy, w, art_h, 14, 0.09f, 0.11f, 0.17f, 1.0f,
                 focused ? 0.98f : 1.0f, focused ? 0.98f : 1.0f, focused ? 1.0f : 1.0f,
                 focused ? 0.92f : 0.10f, focused ? 3.0f : 1.0f);

    GLuint tex = astc_pool_get(M.pool, c->art_url, NULL, NULL);
    if (tex) {
        r_quad_tex(r, tex, x + 2, cy + 2, w - 4, art_h - 4, 0, 0, 1, 1, 1, 1, 1, 1);
    } else {
        r_text_fit(r, 1, c->title, x + 12, cy + art_h * 0.4f, w - 24, 30, 1, 0.55f, 0.62f, 0.74f, 0.85f);
    }

    /* Crisp focus outline ON TOP of artwork */
    if (focused) {
        r_round_rect(r, x - 2, cy - 2, w + 4, art_h + 4, 15, 0, 0, 0, 0,
                     0.98f, 0.98f, 1.0f, 0.96f, 3.2f);
    }

    /* Source badge (Kids YT teal / Adults Channel crimson / Fav gold / Plex amber / Jellyseerr cyan) */
    bool is_ch = strcmp(c->source, "yt_channel") == 0;
    bool is_yt = strcmp(c->source, "youtube") == 0;
    bool is_seerr = strcmp(c->source, "jellyseerr") == 0;
    if (is_ch) {
        if (c->is_kids) {
            r_round_rect(r, x + 10, cy + 10, 76, 24, 7, 0.04f, 0.16f, 0.14f, 0.90f, 0.20f, 0.86f, 0.68f, 0.70f, 1.2f);
            r_text(r, 0, "KIDS YT", x + 10, cy + 10, 76, 24, 1, 0.52f, 0.98f, 0.82f, 1.0f);
        } else if (c->is_favorite) {
            r_round_rect(r, x + 10, cy + 10, 60, 24, 7, 0.16f, 0.12f, 0.02f, 0.90f, 0.98f, 0.76f, 0.22f, 0.72f, 1.2f);
            r_text(r, 0, "FAV", x + 10, cy + 10, 60, 24, 1, 1.0f, 0.86f, 0.36f, 1.0f);
        } else {
            r_round_rect(r, x + 10, cy + 10, 78, 22, 6, 0.12f, 0.03f, 0.05f, 0.86f, 1.0f, 0.28f, 0.35f, 0.55f, 1.0f);
            r_text(r, 0, "CHANNEL", x + 10, cy + 10, 78, 22, 1, 1.0f, 0.54f, 0.58f, 1.0f);
        }
    } else if (is_yt) {
        r_round_rect(r, x + 10, cy + 10, 46, 22, 6, 0.12f, 0.03f, 0.05f, 0.82f, 1.0f, 0.25f, 0.32f, 0.55f, 1.0f);
        r_text(r, 0, "YT", x + 10, cy + 10, 46, 22, 1, 1.0f, 0.45f, 0.50f, 1.0f);
    } else if (is_seerr) {
        const char *st = c->can_request ? "+ REQ" : (c->status[0] ? c->status : "OK");
        r_round_rect(r, x + 10, cy + 10, 96, 24, 6, 0.04f, 0.11f, 0.18f, 0.86f, 0.22f, 0.75f, 0.98f, 0.55f, 1.0f);
        r_text(r, 0, st, x + 10, cy + 10, 96, 24, 1, 0.55f, 0.88f, 1.0f, 1.0f);
    } else {
        r_round_rect(r, x + 10, cy + 10, 56, 22, 6, 0.10f, 0.08f, 0.03f, 0.80f, 0.96f, 0.65f, 0.14f, 0.45f, 1.0f);
        r_text(r, 0, "PLEX", x + 10, cy + 10, 56, 22, 1, 0.98f, 0.76f, 0.24f, 1.0f);
    }

    /* Duration pill on wide cards */
    if (c->is_wide && c->duration_ms > 0) {
        char dbuf[20]; fmt_mmss(c->duration_ms, dbuf, sizeof(dbuf));
        r_round_rect(r, x + w - 74, cy + art_h - 32, 64, 24, 6, 0.03f, 0.04f, 0.06f, 0.82f, 0, 0, 0, 0, 0);
        r_text(r, 0, dbuf, x + w - 74, cy + art_h - 32, 64, 24, 1, 0.95f, 0.96f, 0.99f, 1.0f);
    }

    /* Progress bar */
    if (c->progress > 0.01f) {
        r_round_rect(r, x + 4, cy + art_h - 7, w - 8, 5, 2, 1.0f, 1.0f, 1.0f, 0.20f, 0, 0, 0, 0, 0);
        r_round_rect(r, x + 4, cy + art_h - 7, (w - 8) * c->progress, 5, 2,
                     is_yt ? 1.0f : 0.96f, is_yt ? 0.23f : 0.65f, is_yt ? 0.29f : 0.14f, 1.0f, 0, 0, 0, 0, 0);
    }

    /* Title + Subtitle underneath (exact glyph-fit ellipsis — never overflows neighbor cards!) */
    r_text_fit(r, 1, c->title, x + 2, cy + art_h + 6, w - 4, 24, 0,
               focused ? 0.99f : 0.84f, focused ? 0.99f : 0.87f, focused ? 1.0f : 0.92f, 1.0f);
    r_text_fit(r, 0, c->subtitle, x + 2, cy + art_h + 31, w - 4, 20, 0, 0.54f, 0.60f, 0.72f, 1.0f);
}

static void draw_carousel_row_shaped(Renderer *r, const char *title, const MediaCard *cards, int count,
                                     int sel_col, bool row_focused, float y, int shape /* 0=poster, 1=wide, 2=square */) {
    r_text(r, 4, title, 72, y, 760, 32, 0,
           row_focused ? 0.98f : 0.76f, row_focused ? 0.98f : 0.80f, row_focused ? 1.0f : 0.88f, 1.0f);
    if (count <= 0) {
        r_text(r, 1, "Empty", 72, y + 40, 300, 26, 0, 0.45f, 0.50f, 0.60f, 1.0f);
        return;
    }
    float cw = (shape == 1) ? 332.0f : (shape == 2 ? 236.0f : 202.0f);
    float ch = (shape == 1) ? 244.0f : (shape == 2 ? 292.0f : 356.0f);
    float gap = 22.0f;
    int vis = (shape == 1) ? 5 : (shape == 2 ? 7 : 8);
    int start = sel_col - 1;
    if (start < 0) start = 0;
    if (count > vis && start > count - vis) start = count - vis;

    float x = 72.0f;
    for (int i = start; i < count && x < 1880.0f; i++) {
        draw_card(r, &cards[i], x, y + 40.0f, cw, ch, row_focused && i == sel_col);
        x += cw + gap;
    }
}

static void draw_carousel_row(Renderer *r, const char *title, const MediaCard *cards, int count,
                              int sel_col, bool row_focused, float y, bool wide) {
    draw_carousel_row_shaped(r, title, cards, count, sel_col, row_focused, y, wide ? 1 : 0);
}

static void draw_wrapped_text(Renderer *r, int font_slot, const char *txt, float x, float y, float w, int max_lines, float lh) {
    if (!txt || !txt[0]) return;
    int approx_chars = (int)(w / 10.5f);
    if (approx_chars < 20) approx_chars = 20;
    const char *p = txt;
    for (int ln = 0; ln < max_lines && *p; ln++) {
        while (*p == ' ') p++;
        if (!*p) break;
        char line[180];
        int len = (int)strlen(p);
        int take = len < approx_chars ? len : approx_chars;
        if (take < len) {
            int sp = take;
            while (sp > 10 && p[sp] != ' ') sp--;
            if (sp > 10) take = sp;
        }
        if (take >= (int)sizeof(line) - 1) take = (int)sizeof(line) - 1;
        memcpy(line, p, (size_t)take);
        line[take] = '\0';
        r_text(r, font_slot, line, x, y + ln * lh, w, lh, 0, 0.72f, 0.77f, 0.86f, 1.0f);
        p += take;
    }
}

static void render_home_or_kids(Renderer *r, bool is_kids) {
    const char *h_titles[4] = { "Continue Watching", "YouTube Subscriptions (Adults)", "Recently Added Movies", "Recently Added TV" };
    const char *k_titles[4] = { "Kids Continue Watching", "Kids YouTube Channels", "Kids Movies", "Kids TV Shows" };
    int row_cnt = 4;
    int cur_row = is_kids ? M.kids_row : M.home_row;

    /* Show 2 or 3 rows starting around cur_row so screen stays airy & uncluttered */
    int first_r = (cur_row >= 2) ? (cur_row - 1) : 0;
    float y = 114.0f;
    for (int ri = first_r; ri < row_cnt && y < 960.0f; ri++) {
        const MediaCard *cards = is_kids ? M.kids_rows[ri] : M.home_rows[ri];
        int cnt = is_kids ? M.kids_counts[ri] : M.home_counts[ri];
        if (cnt == 0 && !M.loading) continue;
        int shape = is_kids ? (ri == 0 ? 1 : (ri == 1 ? 2 : 0)) : (ri < 2 ? 1 : 0);
        int sel = is_kids ? M.kids_col[ri] : M.home_col[ri];
        draw_carousel_row_shaped(r, is_kids ? k_titles[ri] : h_titles[ri], cards, cnt, sel,
                                 !M.focus_nav && ri == cur_row, y, shape);
        y += (shape == 1) ? 306.0f : (shape == 2 ? 354.0f : 418.0f);
    }
    if (M.loading) {
        r_text(r, 2, "Loading from Gateway…", 72, 520, 1776, 36, 1, 0.65f, 0.72f, 0.84f, 1.0f);
    }
}

static void render_grid_view(Renderer *r) {
    bool is_yt = (M.view == MEDIA_VIEW_YOUTUBE);
    bool yt_chan_mode = is_yt && (M.yt_tab_idx == 0 || M.yt_tab_idx == 1);
    bool has_catbar = is_yt && (M.yt_tab_idx == 0);
    const char *plex_pills[5] = { "Recently Added", "A - Z", "Year", M.grid_unwatched ? "Unwatched: ON" : "Unwatched: All", "Stock Plex" };
    const char *yt_pills[6]   = { "Adults Channels", "Kids Channels", "Latest (Adults)", "Kids Videos", "Home Feed", "Stock YouTube" };
    const char *yt_cats[8]    = { "Active", "Favorites", "Woodworking", "Home & Design", "Tech & AI", "Cars & Gear", "Podcasts", "All A - Z" };

    int sub_cnt = is_yt ? 6 : 5;
    float px = 72.0f;
    for (int i = 0; i < sub_cnt; i++) {
        const char *lbl = is_yt ? yt_pills[i] : plex_pills[i];
        float pw = is_yt ? 178.0f : 166.0f;
        bool active = is_yt ? (M.yt_tab_idx == i) : ((i < 3 && M.grid_sort_idx == i) || (i == 3 && M.grid_unwatched));
        bool foc = M.focus_subbar && M.subbar_idx == i;
        bool is_kids_pill = is_yt && (i == 1 || i == 3);
        if (foc) {
            r_round_rect(r, px, 110, pw, 38, 19, 0.96f, 0.97f, 0.99f, 1.0f, 0, 0, 0, 0, 0);
            r_text(r, 0, lbl, px, 110, pw, 38, 1, 0.04f, 0.05f, 0.09f, 1.0f);
        } else if (active && is_kids_pill) {
            r_round_rect(r, px, 110, pw, 38, 19, 0.14f, 0.80f, 0.60f, 0.22f, 0.20f, 0.86f, 0.68f, 0.62f, 1.2f);
            r_text(r, 0, lbl, px, 110, pw, 38, 1, 0.56f, 0.98f, 0.82f, 1.0f);
        } else if (active) {
            r_round_rect(r, px, 110, pw, 38, 19, 1.0f, 1.0f, 1.0f, 0.15f, 1.0f, 1.0f, 1.0f, 0.30f, 1.2f);
            r_text(r, 0, lbl, px, 110, pw, 38, 1, 0.96f, 0.97f, 0.99f, 1.0f);
        } else if (is_kids_pill) {
            r_round_rect(r, px, 110, pw, 38, 19, 0.12f, 0.72f, 0.56f, 0.08f, 0.18f, 0.80f, 0.64f, 0.24f, 1.0f);
            r_text(r, 0, lbl, px, 110, pw, 38, 1, 0.58f, 0.88f, 0.78f, 1.0f);
        } else {
            r_round_rect(r, px, 110, pw, 38, 19, 1.0f, 1.0f, 1.0f, 0.05f, 1.0f, 1.0f, 1.0f, 0.10f, 1.0f);
            r_text(r, 0, lbl, px, 110, pw, 38, 1, 0.66f, 0.72f, 0.82f, 1.0f);
        }
        px += pw + 12.0f;
    }

    char cntbuf[64];
    snprintf(cntbuf, sizeof(cntbuf), "%d / %d", M.grid_count > 0 ? M.grid_idx + 1 : 0, M.grid_total);
    r_text(r, 1, cntbuf, 1660, 112, 188, 34, 2, 0.56f, 0.62f, 0.74f, 1.0f);

    /* Secondary Category Filter Strip for Adults YouTube Channels */
    float grid_top_y = 172.0f;
    if (has_catbar) {
        float cx = 72.0f;
        for (int ci = 0; ci < 8; ci++) {
            float cw_p = (ci == 2 || ci == 3) ? 156.0f : 132.0f;
            bool active = (M.yt_cat_idx == ci);
            bool foc = M.focus_catbar && (M.yt_cat_idx == ci);
            if (foc) {
                r_round_rect(r, cx, 160, cw_p, 34, 17, 0.96f, 0.97f, 0.99f, 1.0f, 0, 0, 0, 0, 0);
                r_text(r, 0, yt_cats[ci], cx, 160, cw_p, 34, 1, 0.04f, 0.05f, 0.09f, 1.0f);
            } else if (active) {
                r_round_rect(r, cx, 160, cw_p, 34, 17, 1.0f, 0.28f, 0.34f, 0.22f, 1.0f, 0.36f, 0.42f, 0.55f, 1.0f);
                r_text(r, 0, yt_cats[ci], cx, 160, cw_p, 34, 1, 1.0f, 0.72f, 0.76f, 1.0f);
            } else {
                r_round_rect(r, cx, 160, cw_p, 34, 17, 1.0f, 1.0f, 1.0f, 0.045f, 1.0f, 1.0f, 1.0f, 0.10f, 1.0f);
                r_text(r, 0, yt_cats[ci], cx, 160, cw_p, 34, 1, 0.62f, 0.68f, 0.78f, 1.0f);
            }
            cx += cw_p + 10.0f;
        }
        grid_top_y = 216.0f;
    } else if (is_yt && M.yt_tab_idx == 1) {
        r_text(r, 1, "Dedicated Kids YouTube Channels  \xC2\xB7  Select a channel to browse all videos ad-free (or open Kids tab)",
               72, 162, 1400, 28, 0, 0.56f, 0.90f, 0.78f, 0.95f);
        grid_top_y = 206.0f;
    }

    if (M.grid_count == 0) {
        const char *empty_msg = M.loading ? "Loading\xE2\x80\xA6" :
            (has_catbar && M.yt_cat_idx == 1 ? "No Favorites pinned yet \xE2\x80\x94 open any channel and select Pin Fav." : "No items found.");
        r_text(r, 2, empty_msg, 72, 480, 1776, 36, 1, 0.60f, 0.66f, 0.78f, 1.0f);
        return;
    }

    int cols = is_yt ? (yt_chan_mode ? 6 : 4) : 7;
    float cw = is_yt ? (yt_chan_mode ? 276.0f : 424.0f) : 234.0f;
    float ch = is_yt ? (yt_chan_mode ? 332.0f : 294.0f) : 400.0f;
    float gx = is_yt ? (yt_chan_mode ? 24.0f : 26.0f) : 22.0f;
    float gy = 22.0f;
    int sel_row = M.grid_idx / cols;
    int top_row = (sel_row > 0) ? (sel_row - 1) : 0;
    int vis_rows = is_yt ? (yt_chan_mode ? 3 : 3) : 2;

    for (int rr = 0; rr < vis_rows; rr++) {
        int row = top_row + rr;
        float y = grid_top_y + rr * (ch + gy);
        if (y > 1040.0f) break;
        for (int cc = 0; cc < cols; cc++) {
            int idx = row * cols + cc;
            if (idx >= M.grid_count) break;
            float x = 72.0f + cc * (cw + gx);
            draw_card(r, &M.grid_items[idx], x, y, cw, ch,
                      !M.focus_nav && !M.focus_subbar && !M.focus_catbar && idx == M.grid_idx);
        }
    }
}

static void render_yt_channel_view(Renderer *r) {
    /* Sleek Dedicated Channel Header */
    r_round_rect(r, 0, 0, 1920, 176, 0, 0.03f, 0.045f, 0.075f, 0.94f, 0, 0, 0, 0, 0);
    r_round_rect(r, 0, 175, 1920, 1, 0, 1.0f, 1.0f, 1.0f, 0.10f, 0, 0, 0, 0, 0);

    /* Channel Avatar */
    r_round_rect(r, 72, 34, 108, 108, 22, 0.09f, 0.11f, 0.17f, 1.0f,
                 M.yt_ch.is_kids ? 0.20f : 1.0f, M.yt_ch.is_kids ? 0.86f : 0.34f, M.yt_ch.is_kids ? 0.68f : 0.40f, 0.65f, 2.0f);
    GLuint av = astc_pool_get(M.pool, M.yt_ch.avatar_url, NULL, NULL);
    if (av) r_quad_tex(r, av, 75, 37, 102, 102, 0, 0, 1, 1, 1, 1, 1, 1);

    /* Channel Title + Audience Badge */
    r_text_fit(r, 5, M.yt_ch.title, 206, 36, 980, 50, 0, 0.98f, 0.99f, 1.0f, 1.0f);
    char badge[96];
    if (M.yt_ch.is_kids) {
        snprintf(badge, sizeof(badge), "KIDS YOUTUBE  \xC2\xB7  %d Videos  \xC2\xB7  Ad-Free", M.yt_ch.video_count);
        r_round_rect(r, 206, 98, 420, 36, 18, 0.14f, 0.82f, 0.60f, 0.18f, 0.20f, 0.86f, 0.68f, 0.55f, 1.2f);
        r_text_fit(r, 1, badge, 222, 98, 388, 36, 0, 0.56f, 0.98f, 0.82f, 1.0f);
    } else {
        snprintf(badge, sizeof(badge), "ADULTS  \xC2\xB7  %s  \xC2\xB7  %d Videos",
                 M.yt_ch.category_label[0] ? M.yt_ch.category_label : "YouTube", M.yt_ch.video_count);
        r_round_rect(r, 206, 98, 460, 36, 18, 1.0f, 0.25f, 0.32f, 0.16f, 1.0f, 0.34f, 0.40f, 0.48f, 1.2f);
        r_text_fit(r, 1, badge, 222, 98, 428, 36, 0, 1.0f, 0.74f, 0.78f, 1.0f);
    }

    /* 3 Focusable Header Action Pills: Back | Pin Fav | Move to Kids/Adults */
    const char *btns[3] = {
        "\xE2\x97\x80  Back",
        M.yt_ch.is_favorite ? "Fav: ON" : "Pin Fav",
        M.yt_ch.is_kids ? "Move to Adults" : "Move to Kids"
    };
    static const float BW[3] = { 148.0f, 152.0f, 214.0f };
    float bx = 1300.0f;
    for (int i = 0; i < 3; i++) {
        bool foc = M.yt_ch.focus_header && M.yt_ch.hdr_btn == i;
        if (foc) {
            r_round_rect(r, bx, 64, BW[i], 48, 24, 0.96f, 0.97f, 0.99f, 1.0f, 0, 0, 0, 0, 0);
            r_text(r, 1, btns[i], bx, 64, BW[i], 48, 1, 0.04f, 0.05f, 0.09f, 1.0f);
        } else {
            r_round_rect(r, bx, 64, BW[i], 48, 24, 1.0f, 1.0f, 1.0f, 0.09f, 1.0f, 1.0f, 1.0f, 0.22f, 1.2f);
            r_text(r, 1, btns[i], bx, 64, BW[i], 48, 1, 0.92f, 0.94f, 0.98f, 1.0f);
        }
        bx += BW[i] + 14.0f;
    }

    if (!M.yt_ch.loaded || M.loading) {
        r_text(r, 4, "Loading channel videos\xE2\x80\xA6", 72, 520, 1776, 40, 1, 0.72f, 0.78f, 0.88f, 1.0f);
        return;
    }
    if (M.yt_ch.video_count == 0) {
        r_text(r, 2, "No full-length videos found for this channel.", 72, 520, 1776, 36, 1, 0.62f, 0.68f, 0.80f, 1.0f);
        return;
    }

    int cols = 4;
    float cw = 424.0f, ch = 286.0f, gx = 26.0f, gy = 20.0f;
    int sel_row = M.yt_ch.vid_idx / cols;
    int top_row = (sel_row > 0) ? (sel_row - 1) : 0;
    for (int rr = 0; rr < 3; rr++) {
        int row = top_row + rr;
        float y = 206.0f + rr * (ch + gy);
        if (y > 1040.0f) break;
        for (int cc = 0; cc < cols; cc++) {
            int idx = row * cols + cc;
            if (idx >= M.yt_ch.video_count) break;
            float x = 72.0f + cc * (cw + gx);
            draw_card(r, &M.yt_ch.videos[idx], x, y, cw, ch, !M.yt_ch.focus_header && idx == M.yt_ch.vid_idx);
        }
    }
}

static void render_detail_view(Renderer *r) {
    /* Blurred obsidian ASTC backdrop */
    GLuint bg = astc_pool_get(M.pool, M.detail.backdrop_url, NULL, NULL);
    if (bg) {
        r_quad_tex(r, bg, 0, 0, 1920, 1080, 0, 0, 1, 1, 0.70f, 0.72f, 0.78f, 1.0f);
    }
    r_round_rect(r, 0, 0, 1920, 1080, 0, 0.03f, 0.04f, 0.07f, 0.72f, 0, 0, 0, 0, 0);

    if (!M.detail.loaded) {
        r_text(r, 4, "Loading details…", 72, 500, 1776, 40, 1, 0.85f, 0.88f, 0.94f, 1.0f);
        return;
    }

    /* Poster on left */
    r_round_rect(r, 84, 96, 300, 450, 18, 0.08f, 0.10f, 0.16f, 1.0f, 1.0f, 1.0f, 1.0f, 0.16f, 1.5f);
    GLuint pt = astc_pool_get(M.pool, M.detail.poster_url, NULL, NULL);
    if (pt) r_quad_tex(r, pt, 86, 98, 296, 446, 0, 0, 1, 1, 1, 1, 1, 1);

    /* Right metadata panel */
    r_text_fit(r, 5, M.detail.title, 428, 96, 1400, 50, 0, 0.98f, 0.99f, 1.0f, 1.0f);
    r_text_fit(r, 2, M.detail.subtitle, 428, 156, 1380, 28, 0, 0.68f, 0.74f, 0.84f, 1.0f);

    if (M.detail.media_badge[0]) {
        r_round_rect(r, 428, 198, 580, 36, 10, 0.14f, 0.82f, 0.56f, 0.14f, 0.20f, 0.83f, 0.60f, 0.42f, 1.0f);
        r_text(r, 1, M.detail.media_badge, 444, 198, 548, 36, 0, 0.56f, 0.95f, 0.74f, 1.0f);
    }

    draw_wrapped_text(r, 2, M.detail.summary, 428, 256, 1360, 5, 30.0f);

    /* Action CTA buttons (for movies / episodes) */
    bool playable = (strcmp(M.detail.type, "movie") == 0 || strcmp(M.detail.type, "episode") == 0);
    if (playable) {
        char lbls[4][64];
        if (M.detail.offset_ms > 5000) {
            char rb[20]; fmt_mmss(M.detail.offset_ms, rb, sizeof(rb));
            snprintf(lbls[0], sizeof(lbls[0]), "Resume (%s)", rb);
        } else {
            snprintf(lbls[0], sizeof(lbls[0]), "Play Now");
        }
        snprintf(lbls[1], sizeof(lbls[1]), "Start Over");
        snprintf(lbls[2], sizeof(lbls[2]), "Audio: %s",
                 M.detail.audio_count > 0 ? M.detail.audio[M.detail.sel_audio_idx].label : "Default");
        snprintf(lbls[3], sizeof(lbls[3]), "Sub: %s",
                 (M.detail.sel_sub_idx >= 0 && M.detail.sel_sub_idx < M.detail.sub_count)
                     ? M.detail.subs[M.detail.sel_sub_idx].label : "Off");

        float bx = 428.0f;
        for (int i = 0; i < 4; i++) {
            float bw = (i >= 2) ? 280.0f : 220.0f;
            bool foc = (M.detail_zone == 0 && M.detail_cta_idx == i);
            if (foc) {
                r_round_rect(r, bx, 456, bw, 56, 14, 0.97f, 0.98f, 1.0f, 1.0f, 0, 0, 0, 0, 0);
                r_text_fit(r, 1, lbls[i], bx + 10, 456, bw - 20, 56, 1, 0.04f, 0.05f, 0.09f, 1.0f);
            } else {
                r_round_rect(r, bx, 456, bw, 56, 14, 1.0f, 1.0f, 1.0f, 0.09f, 1.0f, 1.0f, 1.0f, 0.18f, 1.2f);
                r_text_fit(r, 1, lbls[i], bx + 10, 456, bw - 20, 56, 1, 0.92f, 0.94f, 0.98f, 1.0f);
            }
            bx += bw + 16.0f;
        }
    }

    /* Seasons / Episodes carousel underneath */
    if (M.detail.child_count > 0) {
        bool wide = (strcmp(M.detail.type, "season") == 0);
        draw_carousel_row(r, strcmp(M.detail.type, "show") == 0 ? "Seasons" : "Episodes",
                          M.detail.children, M.detail.child_count, M.detail_child_idx,
                          M.detail_zone == 1, 588.0f, wide);
    }

    r_text(r, 0, "BACK  Return    ·    OK  Select", 72, 1024, 1776, 26, 1, 0.56f, 0.62f, 0.74f, 1.0f);
}

static void render_search_view(Renderer *r) {
    /* Search input pill */
    r_round_rect(r, 72, 110, 1776, 54, 14, 1.0f, 1.0f, 1.0f, 0.07f, 1.0f, 1.0f, 1.0f, 0.22f, 1.5f);
    char qdisp[96];
    if (M.search_query[0]) snprintf(qdisp, sizeof(qdisp), "%s_", M.search_query);
    else snprintf(qdisp, sizeof(qdisp), "Type to search Plex + YouTube + Jellyseerr…");
    r_text(r, 4, qdisp, 96, 110, 1720, 54, 0,
           M.search_query[0] ? 0.98f : 0.48f, M.search_query[0] ? 0.99f : 0.54f, M.search_query[0] ? 1.0f : 0.64f, 1.0f);

    /* Compact 4x10 on-screen keyboard */
    float kx0 = 360.0f, ky0 = 180.0f, kw = 110.0f, kh = 46.0f, gap = 10.0f;
    for (int rr = 0; rr < 4; rr++) {
        for (int cc = 0; cc < 10; cc++) {
            char ch = KB_ROWS[rr][cc];
            const char *lbl = NULL;
            char one[2] = { ch, 0 };
            if (ch == '<') lbl = "DEL";
            else if (ch == '_') lbl = "CLR";
            else if (ch == '>') lbl = "GO";
            else if (ch == ' ') lbl = "SPACE";
            else lbl = one;
            bool foc = (!M.focus_nav && M.search_in_kb && M.kb_row == rr && M.kb_col == cc);
            float x = kx0 + cc * (kw + gap);
            float y = ky0 + rr * (kh + gap);
            if (foc) {
                r_round_rect(r, x, y, kw, kh, 10, 0.96f, 0.97f, 0.99f, 1.0f, 0, 0, 0, 0, 0);
                r_text(r, 1, lbl, x, y, kw, kh, 1, 0.04f, 0.05f, 0.09f, 1.0f);
            } else {
                r_round_rect(r, x, y, kw, kh, 10, 1.0f, 1.0f, 1.0f, 0.06f, 1.0f, 1.0f, 1.0f, 0.12f, 1.0f);
                r_text(r, 1, lbl, x, y, kw, kh, 1, 0.88f, 0.91f, 0.96f, 1.0f);
            }
        }
    }

    /* Results rows below keyboard (Plex / YouTube / Jellyseerr) */
    const char *rtitles[3] = { "In Your Plex Library", "YouTube (Ad-Free)", "Request via Jellyseerr (Press OK)" };
    int start_r = (!M.search_in_kb && M.search_res_row >= 1) ? M.search_res_row : 0;
    float ry = 424.0f;
    for (int ri = start_r; ri < 3 && ry < 980.0f; ri++) {
        if (M.search_counts[ri] == 0) continue;
        bool wide = (ri == 1);
        draw_carousel_row(r, rtitles[ri], M.search_rows[ri], M.search_counts[ri],
                          M.search_res_col[ri], !M.focus_nav && !M.search_in_kb && M.search_res_row == ri, ry, wide);
        ry += wide ? 300.0f : 400.0f;
    }
}

static void render_player_overlay(Renderer *r, int64_t now_us) {
    /* Clear to transparent so the hardware video plane shines through the active video rectangle! */
    renderer_begin_frame(r, 1920, 1080, true);

    pthread_mutex_lock(&M.mu);
    ActivePlayback pb = M.pb;
    bool loading = M.loading;
    pthread_mutex_unlock(&M.mu);

    bool video_live = (!loading && pb.video_revealed && pb.player_state >= TP_STATE_PLAYING && pb.video_w > 0 && pb.video_h > 0 &&
                       !pb.err_msg[0] && !pb.fallback_stock_yt);

    /* 0. PURE-BLACK CINEMA MATTE (100% opaque alpha=1.0):
     *    - While loading/preparing/buffering: solid #000000 full screen -> ZERO Samsung Home flash!
     *    - While playing widescreen (2.39:1 etc.) or 4:3: pure #000000 letterbox/pillarbox bars
     *      around the exact video rectangle -> ZERO Samsung Home bleed in letterbox! */
    if (!video_live) {
        r_round_rect(r, 0, 0, 1920, 1080, 0, 0.0f, 0.0f, 0.0f, 1.0f, 0, 0, 0, 0, 0);
    } else {
        double vid_ar = (double)pb.video_w / (double)pb.video_h;
        double scr_ar = 1920.0 / 1080.0;
        if (vid_ar > scr_ar + 0.015) {
            float vh = floorf(1920.0f / (float)vid_ar);
            float bar = ceilf((1080.0f - vh) * 0.5f) + 2.0f;
            if (bar > 0.0f && bar < 500.0f) {
                r_round_rect(r, 0, 0, 1920, bar, 0, 0.0f, 0.0f, 0.0f, 1.0f, 0, 0, 0, 0, 0);
                r_round_rect(r, 0, 1080.0f - bar, 1920, bar, 0, 0.0f, 0.0f, 0.0f, 1.0f, 0, 0, 0, 0, 0);
            }
        } else if (vid_ar < scr_ar - 0.015) {
            float vw = floorf(1080.0f * (float)vid_ar);
            float bar = ceilf((1920.0f - vw) * 0.5f) + 2.0f;
            if (bar > 0.0f && bar < 900.0f) {
                r_round_rect(r, 0, 0, bar, 1080, 0, 0.0f, 0.0f, 0.0f, 1.0f, 0, 0, 0, 0, 0);
                r_round_rect(r, 1920.0f - bar, 0, bar, 1080, 0, 0.0f, 0.0f, 0.0f, 1.0f, 0, 0, 0, 0, 0);
            }
        }
    }

    bool show_hud = (pb.hud_until_us > now_us) || pb.hud_focused || pb.paused || loading || pb.err_msg[0] || pb.fallback_stock_yt;

    /* 1. Native crisp FreeType Subtitles (rendered even when HUD is completely hidden!) */
    if (pb.active_sub_text[0]) {
        float sub_y = show_hud ? 706.0f : 924.0f;
        char copy[256];
        m_strlcpy(copy, pb.active_sub_text, sizeof(copy));
        char *nl = strchr(copy, '\n');
        if (nl) *nl++ = '\0';
        float box_h = nl ? 92.0f : 52.0f;
        r_round_rect(r, 260, sub_y, 1400, box_h, 14, 0.02f, 0.03f, 0.05f, 0.78f, 0, 0, 0, 0, 0);
        r_text_fit(r, 4, copy, 280, sub_y + 8, 1360, 36, 1, 0.99f, 0.99f, 1.0f, 1.0f);
        if (nl) r_text_fit(r, 4, nl, 280, sub_y + 46, 1360, 36, 1, 0.99f, 0.99f, 1.0f, 1.0f);
    }

    /* 2. Floating Skip Intro / Skip Credits / Next Episode pill */
    if (pb.skip_pill_text[0]) {
        float py = show_hud ? 736.0f : 936.0f;
        r_round_rect(r, 1500, py, 340, 62, 31, 0.96f, 0.97f, 0.99f, 0.97f, 0, 0, 0, 0, 0);
        r_text(r, 4, pb.skip_pill_text, 1500, py, 340, 62, 1, 0.04f, 0.05f, 0.09f, 1.0f);
    }

    /* 2b. Big Bold Centered Action Feedback Badge (-15s / +30s / PAUSED / PLAYING / CC) */
    if (pb.feedback_until_us > now_us && pb.feedback_txt[0] && video_live) {
        r_round_rect(r, 790, 468, 340, 108, 28, 0.03f, 0.045f, 0.08f, 0.88f, 1.0f, 1.0f, 1.0f, 0.22f, 1.5f);
        r_text_fit(r, 5, pb.feedback_txt, 810, 476, 300, 92, 1, 0.98f, 0.99f, 1.0f, 1.0f);
    }

    /* 3. Auto-hiding 10-Foot Cinema Playback HUD (Unmistakable Large Controls!) */
    if (show_hud) {
        /* Top Title Bar */
        r_round_rect(r, 64, 38, 1792, 92, 22, 0.03f, 0.045f, 0.08f, 0.88f, 1.0f, 1.0f, 1.0f, 0.16f, 1.2f);
        r_text_fit(r, 4, pb.title, 98, 48, 1180, 38, 0, 0.98f, 0.99f, 1.0f, 1.0f);
        r_text_fit(r, 2, pb.subtitle[0] ? pb.subtitle : pb.badge, 98, 88, 1180, 30, 0, 0.70f, 0.76f, 0.88f, 1.0f);
        if (pb.badge[0]) {
            r_round_rect(r, 1316, 60, 508, 46, 23, 0.14f, 0.82f, 0.56f, 0.16f, 0.20f, 0.83f, 0.60f, 0.44f, 1.0f);
            r_text_fit(r, 1, pb.badge, 1332, 60, 476, 46, 1, 0.62f, 0.96f, 0.78f, 1.0f);
        }

        if (pb.fallback_stock_yt) {
            r_round_rect(r, 460, 430, 1000, 190, 24, 0.04f, 0.055f, 0.095f, 0.96f, 1.0f, 0.30f, 0.36f, 0.55f, 2.0f);
            r_text(r, 4, "Protected YouTube Stream (DRM)", 490, 462, 940, 40, 1, 0.99f, 0.99f, 1.0f, 1.0f);
            r_text(r, 2, "Press OK to open in Stock YouTube  \xC2\xB7  BACK to return", 490, 524, 940, 36, 1, 0.80f, 0.85f, 0.94f, 1.0f);
        } else if (pb.err_msg[0]) {
            r_round_rect(r, 460, 444, 1000, 148, 24, 0.06f, 0.03f, 0.05f, 0.96f, 1.0f, 0.32f, 0.38f, 0.60f, 2.0f);
            r_text_fit(r, 4, pb.err_msg, 490, 490, 940, 48, 1, 1.0f, 0.74f, 0.78f, 1.0f);
        } else if (!video_live) {
            r_round_rect(r, 680, 480, 560, 100, 24, 0.05f, 0.07f, 0.12f, 0.96f, 1.0f, 1.0f, 1.0f, 0.18f, 1.4f);
            char load_msg[96];
            snprintf(load_msg, sizeof(load_msg), "Starting %.44s\xE2\x80\xA6", pb.title[0] ? pb.title : "Video");
            r_text_fit(r, 4, load_msg, 704, 502, 512, 54, 1, 0.96f, 0.97f, 0.99f, 1.0f);
        }

        /* Bottom 10-Foot Cinema Transport Bar (Big, High-Contrast Focusable Buttons!) */
        r_round_rect(r, 64, 816, 1792, 224, 26, 0.03f, 0.045f, 0.08f, 0.90f, 1.0f, 1.0f, 1.0f, 0.18f, 1.4f);

        char pbuf[24], dbuf[24], rbuf[28];
        fmt_mmss(pb.last_pos_ms, pbuf, sizeof(pbuf));
        fmt_mmss(pb.known_dur_ms, dbuf, sizeof(dbuf));
        int rem_ms = (pb.known_dur_ms > pb.last_pos_ms) ? (pb.known_dur_ms - pb.last_pos_ms) : 0;
        char rem_raw[24];
        fmt_mmss(rem_ms, rem_raw, sizeof(rem_raw));
        snprintf(rbuf, sizeof(rbuf), "-%s  /  %s", rem_raw, dbuf);

        /* Status dot + PAUSED/PLAYING + large legible timestamps */
        r_round_rect(r, 104, 845, 12, 12, 6,
                     pb.paused ? 1.0f : 0.24f, pb.paused ? 0.76f : 0.86f, pb.paused ? 0.24f : 0.62f, 1.0f, 0, 0, 0, 0, 0);
        r_text(r, 4, pb.paused ? "PAUSED" : "PLAYING", 126, 834, 160, 34, 0,
               pb.paused ? 1.0f : 0.36f, pb.paused ? 0.80f : 0.92f, pb.paused ? 0.30f : 0.68f, 1.0f);
        r_text(r, 4, pbuf, 290, 834, 150, 34, 0, 0.97f, 0.98f, 1.0f, 1.0f);
        r_text(r, 4, rbuf, 1480, 834, 336, 34, 2, 0.74f, 0.80f, 0.90f, 1.0f);

        /* Thick 14px Scrub Bar + Glowing 26px Playhead Knob */
        float bx = 104.0f, bw = 1712.0f, by = 886.0f;
        r_round_rect(r, bx, by, bw, 14, 7, 1.0f, 1.0f, 1.0f, 0.20f, 0, 0, 0, 0, 0);
        float frac = (pb.known_dur_ms > 0) ? ((float)pb.last_pos_ms / (float)pb.known_dur_ms) : 0.0f;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        bool is_yt = strcmp(pb.source, "youtube") == 0;
        float fill_w = bw * frac;
        if (fill_w > 4.0f) {
            r_round_rect(r, bx, by, fill_w, 14, 7,
                         is_yt ? 1.0f : 0.96f, is_yt ? 0.25f : 0.68f, is_yt ? 0.31f : 0.16f, 1.0f, 0, 0, 0, 0, 0);
        }
        r_round_rect(r, bx + fill_w - 13.0f, by - 6.0f, 26, 26, 13, 0.98f, 0.99f, 1.0f, 1.0f, 0, 0, 0, 0, 0);

        /* 5 Big Unmistakable Transport Pill Buttons (66px tall, 22px SemiBold text!) */
        const char *sub_short = (pb.cur_sub_idx >= 0 && pb.cur_sub_idx < pb.sub_count) ? pb.subs[pb.cur_sub_idx].label : "Off";
        char sub_btn_lbl[48];
        snprintf(sub_btn_lbl, sizeof(sub_btn_lbl), "CC: %.20s", sub_short);

        const char *btn_lbls[5] = {
            "\xE2\x97\x80\xE2\x97\x80  -15s",
            pb.paused ? "\xE2\x96\xB6  Resume" : "II  Pause",
            "+30s  \xE2\x96\xB6\xE2\x96\xB6",
            sub_btn_lbl,
            "Exit Video"
        };
        static const float BTN_W[5] = { 216.0f, 276.0f, 216.0f, 344.0f, 224.0f };
        const float GAP = 22.0f;
        float total_bw = 216 + 276 + 216 + 344 + 224 + GAP * 4.0f; /* 1364 px */
        float cur_bx = (1920.0f - total_bw) * 0.5f;
        float btn_y = 928.0f, btn_h = 66.0f;

        for (int bi = 0; bi < 5; bi++) {
            float w = BTN_W[bi];
            bool foc = (pb.hud_focused && pb.hud_btn_idx == bi) || (!pb.hud_focused && bi == 1);
            if (foc) {
                r_round_rect(r, cur_bx - 3.0f, btn_y - 3.0f, w + 6.0f, btn_h + 6.0f, 20,
                             0.96f, 0.97f, 0.99f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.5f);
                r_text_fit(r, 4, btn_lbls[bi], cur_bx + 12, btn_y, w - 24, btn_h, 1, 0.035f, 0.051f, 0.086f, 1.0f);
            } else {
                r_round_rect(r, cur_bx, btn_y, w, btn_h, 18,
                             1.0f, 1.0f, 1.0f, 0.11f, 1.0f, 1.0f, 1.0f, 0.24f, 1.2f);
                r_text_fit(r, 4, btn_lbls[bi], cur_bx + 12, btn_y, w - 24, btn_h, 1, 0.93f, 0.95f, 0.99f, 1.0f);
            }
            cur_bx += w + GAP;
        }

        /* Crisp 16px one-line footer cue for remote buttons */
        const char *cue = pb.hud_focused
            ? "\xE2\x97\x80 \xE2\x96\xB6  Choose Control     \xC2\xB7     OK  Select     \xC2\xB7     \xE2\x96\xB2  Hide Controls     \xC2\xB7     BACK  Exit"
            : "OK  Play/Pause     \xC2\xB7     \xE2\x97\x80 \xE2\x96\xB6  Seek     \xC2\xB7     \xE2\x96\xBC  Controls     \xC2\xB7     BACK  Exit";
        r_text(r, 1, cue, 104, 1004, 1712, 28, 1, 0.68f, 0.74f, 0.85f, 0.90f);
    }

    r_flush(r);
}

void media_os_render(Renderer *r, int64_t now_us) {
    if (!M.initialized || M.view == MEDIA_VIEW_NONE) return;

    if (M.view == MEDIA_VIEW_PLAYER) {
        render_player_overlay(r, now_us);
        return;
    }

    /* Browse views: opaque obsidian canvas */
    renderer_begin_frame(r, 1920, 1080, false);
    if (r->bg_base.id) r_quad_tex(r, r->bg_base.id, 0, 0, 1920, 1080, 0, 0, 1, 1, 1, 1, 1, 1);

    pthread_mutex_lock(&M.mu);
    if (M.view == MEDIA_VIEW_HOME) render_home_or_kids(r, false);
    else if (M.view == MEDIA_VIEW_KIDS) render_home_or_kids(r, true);
    else if (M.view == MEDIA_VIEW_MOVIES || M.view == MEDIA_VIEW_TV || M.view == MEDIA_VIEW_YOUTUBE) render_grid_view(r);
    else if (M.view == MEDIA_VIEW_DETAIL) render_detail_view(r);
    else if (M.view == MEDIA_VIEW_YT_CHANNEL) render_yt_channel_view(r);
    else if (M.view == MEDIA_VIEW_SEARCH) render_search_view(r);

    if (M.view != MEDIA_VIEW_DETAIL && M.view != MEDIA_VIEW_YT_CHANNEL) draw_top_nav(r);
    pthread_mutex_unlock(&M.mu);

    r_flush(r);
}

static void m_json_esc(char *dst, size_t cap, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 2 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = (char)c; }
        else if (c < 32) { dst[j++] = ' '; }
        else dst[j++] = (char)c;
    }
    dst[j] = 0;
}

void media_os_status_json(char *buf, size_t cap) {
    if (!M.initialized) {
        snprintf(buf, cap, "\"mediaOs\":{\"initialized\":false}");
        return;
    }
    int loaded = 0, pending = 0;
    astc_pool_stats(M.pool, &loaded, &pending);
    pthread_mutex_lock(&M.mu);
    int v = (int)M.view;
    bool hl = M.home_loaded, yts = M.yt_signed_in, kl = M.kids_loaded;
    int hc0 = M.home_counts[0], hc1 = M.home_counts[1], hc2 = M.home_counts[2], hc3 = M.home_counts[3];
    int kc0 = M.kids_counts[0], kc1 = M.kids_counts[1], kc2 = M.kids_counts[2], kc3 = M.kids_counts[3];
    int gc = M.grid_count, gt = M.grid_total, ytt = M.yt_tab_idx, ytc = M.yt_cat_idx;
    int ycv = M.yt_ch.video_count;
    int st = M.pb.player_state, pos = M.pb.last_pos_ms, dur = M.pb.known_dur_ms;
    int vw = M.pb.video_w, vh = M.pb.video_h;
    bool vrev = M.pb.video_revealed, hfoc = M.pb.hud_focused, paused = M.pb.paused;
    int hbtn = M.pb.hud_btn_idx;
    char ptitle[128], pmode[40], perr[128], psrc[16], ycid[64];
    m_json_esc(ptitle, sizeof(ptitle), M.pb.title);
    m_json_esc(pmode, sizeof(pmode), M.pb.mode);
    m_json_esc(perr, sizeof(perr), M.pb.err_msg);
    m_json_esc(psrc, sizeof(psrc), M.pb.source);
    m_json_esc(ycid, sizeof(ycid), M.yt_ch.channel_id);
    pthread_mutex_unlock(&M.mu);
    snprintf(buf, cap,
             "\"mediaOs\":{\"view\":%d,\"active\":%s,\"playing\":%s,\"homeLoaded\":%s,\"kidsLoaded\":%s,"
             "\"ytSignedIn\":%s,\"homeCounts\":[%d,%d,%d,%d],\"kidsCounts\":[%d,%d,%d,%d],"
             "\"ytTab\":%d,\"ytCat\":%d,\"ytChannelId\":\"%s\",\"ytChannelVideos\":%d,"
             "\"gridCount\":%d,\"gridTotal\":%d,"
             "\"astcLoaded\":%d,\"astcPending\":%d,\"player\":{\"source\":\"%s\",\"state\":%d,\"paused\":%s,"
             "\"mode\":\"%s\",\"title\":\"%s\",\"posMs\":%d,\"durMs\":%d,\"videoW\":%d,\"videoH\":%d,"
             "\"videoRevealed\":%s,\"hudFocused\":%s,\"hudBtn\":%d,\"err\":\"%s\"}}",
             v,
             (v != MEDIA_VIEW_NONE) ? "true" : "false",
             (v == MEDIA_VIEW_PLAYER) ? "true" : "false",
             hl ? "true" : "false", kl ? "true" : "false",
             yts ? "true" : "false", hc0, hc1, hc2, hc3, kc0, kc1, kc2, kc3,
             ytt, ytc, ycid, ycv, gc, gt,
             loaded, pending, psrc, st, paused ? "true" : "false", pmode, ptitle, pos, dur, vw, vh,
             vrev ? "true" : "false", hfoc ? "true" : "false", hbtn, perr);
}
