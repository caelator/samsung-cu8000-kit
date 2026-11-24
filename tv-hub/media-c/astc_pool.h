#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../native-c/renderer.h"

#define ASTC_POOL_SLOTS 64

typedef struct AstcPool AstcPool;

AstcPool *astc_pool_create(void (*gl_delete_textures_fn)(GLsizei, const GLuint *));
void      astc_pool_destroy(AstcPool *pool);

/* Returns an event pipe read-fd that becomes readable when new ASTC bytes are ready to upload on GL thread. */
int       astc_pool_wake_fd(AstcPool *pool);

/* Call on GL thread whenever wake_fd fires (or before rendering): uploads ready ASTC buffers via r_load_astc_mem().
 * Returns number of newly uploaded textures. */
int       astc_pool_pump_gl(AstcPool *pool);

/* Request texture for URL (non-blocking). Returns valid GLuint if ready in GPU VRAM, else 0 (queues async fetch). */
GLuint    astc_pool_get(AstcPool *pool, const char *url, int *out_w, int *out_h);

/* Evict all dynamic textures (call on HDMI switch / Cold-Sleep to drop VRAM to 0). Must be called on GL thread. */
void      astc_pool_flush_all(AstcPool *pool);

/* Diagnostics */
void      astc_pool_stats(AstcPool *pool, int *loaded_slots, int *pending_slots);
