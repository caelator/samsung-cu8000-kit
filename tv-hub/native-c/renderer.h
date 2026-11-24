#pragma once
#include "tizen_ffi.h"

typedef struct {
    GLuint id;
    int w, h;
} GpuTex;

typedef struct {
    float u0, v0, u1, v1;
    float w, h, bx, by, adv;
} GlyphInfo;

#define FONT_SLOT_COUNT 6
/* Font slots: 0=14px SemiBold, 1=16px Medium, 2=18px Medium, 3=20px Regular, 4=22px SemiBold, 5=36px Light */
typedef struct {
    GlyphInfo g[128];      /* ASCII 0..127 */
    GlyphInfo g_latin[96]; /* Latin-1 Supplement U+00A0..U+00FF (accents, ñ, ¿, ¡, ·) */
    GlyphInfo g_dot;       /* U+00B7 middle dot */
    GlyphInfo g_ell;       /* U+2026 ellipsis */
    GlyphInfo g_min;       /* U+2212 minus */
    GlyphInfo g_lt;        /* U+25C0 left tri */
    GlyphInfo g_rt;        /* U+25B6 right tri */
    GlyphInfo g_up;        /* U+25B2 up tri */
    GlyphInfo g_dn;        /* U+25BC down tri */
    float ascender;
    float px;
} FontAtlasSlot;

typedef struct {
    GLuint prog;
    GLuint vbo;
    GLint u_res;
    GLint u_tex;
    GLuint glyph_tex;
    FontAtlasSlot fonts[FONT_SLOT_COUNT];

    /* Hardware ASTC textures (2.2 MiB total Mali VRAM, ZERO PNG decode!) */
    GpuTex bg_base;
    GpuTex halo[3];
    GpuTex card_idle[3];
    GpuTex card_focus[3];
    GpuTex dock_rail;
    GpuTex icons_atlas;
} Renderer;

bool renderer_init(Renderer *r, const char *asset_dir);
GpuTex r_load_astc_mem(const unsigned char *buf, size_t len);
void renderer_begin_frame(Renderer *r, int w, int h, bool clear_transparent);
void renderer_set_scissor(int x, int y, int w, int h);
void renderer_clear_scissor(void);

/* Immediate batched draws (automatically flush when switching texture) */
void r_quad_tex(Renderer *r, GLuint tex, float x, float y, float w, float h,
                float u0, float v0, float u1, float v1,
                float cr, float cg, float cb, float ca);
void r_icon(Renderer *r, int icon_idx, float x, float y, float w, float h,
            float cr, float cg, float cb, float ca);
void r_round_rect(Renderer *r, float x, float y, float w, float h, float radius,
                  float cr, float cg, float cb, float ca,
                  float br, float bg, float bb, float ba, float bw);
void r_text(Renderer *r, int font_slot, const char *utf8, float x, float y, float box_w, float box_h,
            int align /* 0=left, 1=center, 2=right */, float cr, float cg, float cb, float ca);
void r_text_fit(Renderer *r, int font_slot, const char *utf8, float x, float y, float box_w, float box_h,
                int align, float cr, float cg, float cb, float ca);
void r_text_spaced(Renderer *r, int font_slot, const char *utf8, float x, float y, float box_w, float box_h,
                   int align, float extra_spacing, float cr, float cg, float cb, float ca);
void r_flush(Renderer *r);
int  r_get_draw_calls(void);
