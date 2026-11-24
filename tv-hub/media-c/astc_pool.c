#include "astc_pool.h"
#include "http_client.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum {
    SLOT_EMPTY = 0,
    SLOT_QUEUED,
    SLOT_FETCHING,
    SLOT_READY_UPLOAD,
    SLOT_LOADED,
    SLOT_FAILED
} SlotState;

typedef struct {
    SlotState state;
    char url[384];
    uint64_t lru_tick;
    uint8_t *pending_buf;
    size_t pending_len;
    GpuTex tex;
} PoolSlot;

struct AstcPool {
    PoolSlot slots[ASTC_POOL_SLOTS];
    uint64_t tick;
    void (*gl_delete_textures)(GLsizei, const GLuint *);
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t worker;
    bool stop;
    int wake_pipe[2];
};

static void signal_wake(AstcPool *p) {
    if (p->wake_pipe[1] >= 0) {
        char b = 1;
        ssize_t ignored = write(p->wake_pipe[1], &b, 1);
        (void)ignored;
    }
}

static void *worker_main(void *arg) {
    AstcPool *p = (AstcPool *)arg;
    for (;;) {
        int pick = -1;
        char url[384];
        pthread_mutex_lock(&p->mu);
        while (!p->stop) {
            uint64_t best_tick = 0;
            for (int i = 0; i < ASTC_POOL_SLOTS; i++) {
                if (p->slots[i].state == SLOT_QUEUED && p->slots[i].lru_tick >= best_tick) {
                    best_tick = p->slots[i].lru_tick;
                    pick = i;
                }
            }
            if (pick >= 0) break;
            pthread_cond_wait(&p->cv, &p->mu);
        }
        if (p->stop) {
            pthread_mutex_unlock(&p->mu);
            break;
        }
        p->slots[pick].state = SLOT_FETCHING;
        strncpy(url, p->slots[pick].url, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
        pthread_mutex_unlock(&p->mu);

        HttpResponse resp;
        bool ok = http_get(url, 6000, 512 * 1024, &resp);

        pthread_mutex_lock(&p->mu);
        /* Verify slot wasn't recycled while fetching */
        if (p->slots[pick].state == SLOT_FETCHING && strcmp(p->slots[pick].url, url) == 0) {
            if (ok && resp.status == 200 && resp.len > 16 &&
                resp.body[0] == 0x13 && resp.body[1] == 0xAB &&
                resp.body[2] == 0xA1 && resp.body[3] == 0x5C) {
                free(p->slots[pick].pending_buf);
                p->slots[pick].pending_buf = resp.body;
                p->slots[pick].pending_len = resp.len;
                resp.body = NULL; /* transferred */
                p->slots[pick].state = SLOT_READY_UPLOAD;
                signal_wake(p);
            } else {
                p->slots[pick].state = SLOT_FAILED;
            }
        }
        pthread_mutex_unlock(&p->mu);
        http_response_free(&resp);
    }
    return NULL;
}

AstcPool *astc_pool_create(void (*gl_delete_textures_fn)(GLsizei, const GLuint *)) {
    AstcPool *p = calloc(1, sizeof(AstcPool));
    if (!p) return NULL;
    p->gl_delete_textures = gl_delete_textures_fn;
    p->wake_pipe[0] = p->wake_pipe[1] = -1;
    if (pipe(p->wake_pipe) == 0) {
        fcntl(p->wake_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(p->wake_pipe[1], F_SETFL, O_NONBLOCK);
    }
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    pthread_create(&p->worker, NULL, worker_main, p);
    return p;
}

void astc_pool_destroy(AstcPool *p) {
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    p->stop = true;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    pthread_join(p->worker, NULL);
    for (int i = 0; i < ASTC_POOL_SLOTS; i++) {
        free(p->slots[i].pending_buf);
    }
    if (p->wake_pipe[0] >= 0) close(p->wake_pipe[0]);
    if (p->wake_pipe[1] >= 0) close(p->wake_pipe[1]);
    pthread_cond_destroy(&p->cv);
    pthread_mutex_destroy(&p->mu);
    free(p);
}

int astc_pool_wake_fd(AstcPool *p) {
    return p ? p->wake_pipe[0] : -1;
}

int astc_pool_pump_gl(AstcPool *p) {
    if (!p) return 0;
    if (p->wake_pipe[0] >= 0) {
        char drain[64];
        while (read(p->wake_pipe[0], drain, sizeof(drain)) > 0) {}
    }
    int uploaded = 0;
    /* Upload up to 8 textures per frame to stay well inside 16.7 ms frame budget */
    for (int i = 0; i < ASTC_POOL_SLOTS && uploaded < 8; i++) {
        uint8_t *buf = NULL;
        size_t len = 0;
        GLuint old_id = 0;
        pthread_mutex_lock(&p->mu);
        if (p->slots[i].state == SLOT_READY_UPLOAD && p->slots[i].pending_buf) {
            buf = p->slots[i].pending_buf;
            len = p->slots[i].pending_len;
            p->slots[i].pending_buf = NULL;
            p->slots[i].pending_len = 0;
            old_id = p->slots[i].tex.id;
            p->slots[i].tex.id = 0;
        }
        pthread_mutex_unlock(&p->mu);
        if (!buf) continue;

        if (old_id && p->gl_delete_textures) p->gl_delete_textures(1, &old_id);
        GpuTex t = r_load_astc_mem(buf, len);
        free(buf);

        pthread_mutex_lock(&p->mu);
        p->slots[i].tex = t;
        p->slots[i].state = t.id ? SLOT_LOADED : SLOT_FAILED;
        pthread_mutex_unlock(&p->mu);
        if (t.id) uploaded++;
    }
    if (uploaded >= 8) signal_wake(p); /* Ensure remaining ready textures upload next frame */
    return uploaded;
}

GLuint astc_pool_get(AstcPool *p, const char *url, int *out_w, int *out_h) {
    if (!p || !url || !url[0]) return 0;
    pthread_mutex_lock(&p->mu);
    p->tick++;

    /* Hit check */
    for (int i = 0; i < ASTC_POOL_SLOTS; i++) {
        if (p->slots[i].state != SLOT_EMPTY && strcmp(p->slots[i].url, url) == 0) {
            p->slots[i].lru_tick = p->tick;
            if (p->slots[i].state == SLOT_LOADED && p->slots[i].tex.id) {
                GLuint id = p->slots[i].tex.id;
                if (out_w) *out_w = p->slots[i].tex.w;
                if (out_h) *out_h = p->slots[i].tex.h;
                pthread_mutex_unlock(&p->mu);
                return id;
            }
            pthread_mutex_unlock(&p->mu);
            return 0;
        }
    }

    /* Choose empty or oldest non-fetching victim slot */
    int victim = -1;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < ASTC_POOL_SLOTS; i++) {
        if (p->slots[i].state == SLOT_EMPTY) { victim = i; break; }
        if (p->slots[i].state != SLOT_FETCHING && p->slots[i].lru_tick < oldest) {
            oldest = p->slots[i].lru_tick;
            victim = i;
        }
    }
    GLuint evict_tex = 0;
    if (victim >= 0) {
        evict_tex = p->slots[victim].tex.id;
        p->slots[victim].tex.id = 0;
        p->slots[victim].tex.w = p->slots[victim].tex.h = 0;
        free(p->slots[victim].pending_buf);
        p->slots[victim].pending_buf = NULL;
        p->slots[victim].pending_len = 0;
        strncpy(p->slots[victim].url, url, sizeof(p->slots[victim].url) - 1);
        p->slots[victim].url[sizeof(p->slots[victim].url) - 1] = '\0';
        p->slots[victim].lru_tick = p->tick;
        p->slots[victim].state = SLOT_QUEUED;
        pthread_cond_signal(&p->cv);
    }
    pthread_mutex_unlock(&p->mu);

    if (evict_tex && p->gl_delete_textures) p->gl_delete_textures(1, &evict_tex);
    return 0;
}

void astc_pool_flush_all(AstcPool *p) {
    if (!p) return;
    GLuint to_delete[ASTC_POOL_SLOTS];
    int del_n = 0;
    pthread_mutex_lock(&p->mu);
    for (int i = 0; i < ASTC_POOL_SLOTS; i++) {
        if (p->slots[i].tex.id) {
            to_delete[del_n++] = p->slots[i].tex.id;
            p->slots[i].tex.id = 0;
        }
        free(p->slots[i].pending_buf);
        p->slots[i].pending_buf = NULL;
        p->slots[i].pending_len = 0;
        if (p->slots[i].state != SLOT_FETCHING) {
            p->slots[i].state = SLOT_EMPTY;
            p->slots[i].url[0] = '\0';
        }
    }
    pthread_mutex_unlock(&p->mu);
    if (del_n > 0 && p->gl_delete_textures) p->gl_delete_textures(del_n, to_delete);
}

void astc_pool_stats(AstcPool *p, int *loaded_slots, int *pending_slots) {
    int ld = 0, pd = 0;
    if (p) {
        pthread_mutex_lock(&p->mu);
        for (int i = 0; i < ASTC_POOL_SLOTS; i++) {
            if (p->slots[i].state == SLOT_LOADED) ld++;
            else if (p->slots[i].state == SLOT_QUEUED || p->slots[i].state == SLOT_FETCHING || p->slots[i].state == SLOT_READY_UPLOAD) pd++;
        }
        pthread_mutex_unlock(&p->mu);
    }
    if (loaded_slots) *loaded_slots = ld;
    if (pending_slots) *pending_slots = pd;
}
