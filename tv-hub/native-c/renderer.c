#include "renderer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* Vertex format: pos(2), uv(2), color(4), extra(4), mode(1) = 13 floats = 52 bytes */
typedef struct {
    float x, y;
    float u, v;
    float r, g, b, a;
    float e0, e1, e2, e3; /* mode 2: halfW, halfH, radius, borderW */
    float mode;           /* 0=RGBA/ASTC tex, 1=Alpha glyph, 2=SDF rounded fill, 3=SDF rounded border */
} Vertex;

#define MAX_QUADS 512
#define MAX_VERTS (MAX_QUADS * 6)
static Vertex VBUF[MAX_VERTS];
static int VCOUNT = 0;
static GLuint CUR_TEX = 0;
static int DRAW_CALLS = 0;

static const char *VS_SRC =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "attribute vec4 a_col;\n"
    "attribute vec4 a_ext;\n"
    "attribute float a_mode;\n"
    "uniform vec2 u_res;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_col;\n"
    "varying vec4 v_ext;\n"
    "varying float v_mode;\n"
    "void main() {\n"
    "  vec2 ndc = (a_pos / u_res) * 2.0 - 1.0;\n"
    "  gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);\n"
    "  v_uv = a_uv; v_col = a_col; v_ext = a_ext; v_mode = a_mode;\n"
    "}\n";

static const char *FS_SRC =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_col;\n"
    "varying vec4 v_ext;\n"
    "varying float v_mode;\n"
    "void main() {\n"
    "  if (v_mode < 0.5) {\n"
    "    gl_FragColor = texture2D(u_tex, v_uv) * v_col;\n"
    "  } else if (v_mode < 1.5) {\n"
    "    float a = texture2D(u_tex, v_uv).a;\n"
    "    gl_FragColor = vec4(v_col.rgb, v_col.a * a);\n"
    "  } else {\n"
    "    vec2 halfSz = v_ext.xy;\n"
    "    float rad = v_ext.z;\n"
    "    float bw = v_ext.w;\n"
    "    vec2 p = abs(v_uv) - (halfSz - vec2(rad));\n"
    "    float d = length(max(p, 0.0)) + min(max(p.x, p.y), 0.0) - rad;\n"
    "    if (v_mode < 2.5) {\n"
    "      float a = clamp(0.5 - d, 0.0, 1.0);\n"
    "      gl_FragColor = vec4(v_col.rgb, v_col.a * a);\n"
    "    } else {\n"
    "      float outer = clamp(0.5 - d, 0.0, 1.0);\n"
    "      float inner = clamp(0.5 - (d + bw), 0.0, 1.0);\n"
    "      gl_FragColor = vec4(v_col.rgb, v_col.a * max(outer - inner, 0.0));\n"
    "    }\n"
    "  }\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = G.glCreateShader(type);
    G.glShaderSource(s, 1, &src, NULL);
    G.glCompileShader(s);
    GLint ok = 0;
    G.glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        G.glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "[gl] shader compile err: %s\n", log);
        return 0;
    }
    return s;
}

GpuTex r_load_astc_mem(const unsigned char *buf, size_t len) {
    GpuTex t = {0, 0, 0};
    if (!buf || len <= 16) return t;
    /* 16-byte ASTC header: magic(4) blockdim_x(1) blockdim_y(1) blockdim_z(1) xsize(3) ysize(3) zsize(3) */
    if (buf[0] != 0x13 || buf[1] != 0xAB || buf[2] != 0xA1 || buf[3] != 0x5C) return t;
    int bx = buf[4], by = buf[5];
    int w = buf[7] | (buf[8] << 8) | (buf[9] << 16);
    int h = buf[10] | (buf[11] << 8) | (buf[12] << 16);
    GLenum fmt = (bx == 6 && by == 6) ? GL_COMPRESSED_RGBA_ASTC_6x6_KHR : GL_COMPRESSED_RGBA_ASTC_4x4_KHR;

    G.glGenTextures(1, &t.id);
    G.glBindTexture(GL_TEXTURE_2D, t.id);
    G.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    G.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    G.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    G.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    G.glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, (GLsizei)(len - 16), buf + 16);
    t.w = w; t.h = h;
    return t;
}

static GpuTex load_astc(const char *path) {
    GpuTex t = {0, 0, 0};
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[astc] missing %s\n", path); return t; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 16) { fclose(f); return t; }
    unsigned char *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return t; }
    fclose(f);
    t = r_load_astc_mem(buf, (size_t)sz);
    free(buf);
    return t;
}

static bool pack_glyph(FT_Face face, uint32_t cp, unsigned char *atlas, int aw, int ah,
                       int *pen_x, int *pen_y, int *row_h, GlyphInfo *out) {
    if (G.FT_Load_Char(face, cp, FT_LOAD_RENDER) != 0) return false;
    FT_GlyphSlotRec *sl = face->glyph;
    int gw = (int)sl->bitmap.width;
    int gh = (int)sl->bitmap.rows;
    if (*pen_x + gw + 2 >= aw) {
        *pen_x = 1;
        *pen_y += *row_h + 2;
        *row_h = 0;
    }
    if (*pen_y + gh + 2 >= ah) return false;
    for (int r = 0; r < gh; r++) {
        memcpy(atlas + (*pen_y + r) * aw + *pen_x, sl->bitmap.buffer + r * sl->bitmap.pitch, (size_t)gw);
    }
    out->u0 = (float)(*pen_x) / (float)aw;
    out->v0 = (float)(*pen_y) / (float)ah;
    out->u1 = (float)(*pen_x + gw) / (float)aw;
    out->v1 = (float)(*pen_y + gh) / (float)ah;
    out->w  = (float)gw;
    out->h  = (float)gh;
    out->bx = (float)sl->bitmap_left;
    out->by = (float)sl->bitmap_top;
    out->adv = (float)(sl->advance.x >> 6);
    *pen_x += gw + 2;
    if (gh > *row_h) *row_h = gh;
    return true;
}

static bool build_font_atlas(Renderer *r) {
    FT_Library ft = NULL;
    if (G.FT_Init_FreeType(&ft) != 0) { fprintf(stderr, "[ft] init failed\n"); return false; }

    const char *files[FONT_SLOT_COUNT] = {
        "/usr/share/fonts/SamsungOneUI-600.ttf",
        "/usr/share/fonts/SamsungOneUI-500.ttf",
        "/usr/share/fonts/SamsungOneUI-500.ttf",
        "/usr/share/fonts/SamsungOneUI-400.ttf",
        "/usr/share/fonts/SamsungOneUI-600.ttf",
        "/usr/share/fonts/SamsungOneUI-300.ttf",
    };
    const int sizes[FONT_SLOT_COUNT] = { 14, 16, 19, 21, 22, 36 };

    const int AW = 1024, AH = 1024;
    unsigned char *atlas = calloc((size_t)(AW * AH), 1);
    int pen_x = 1, pen_y = 1, row_h = 0;

    for (int s = 0; s < FONT_SLOT_COUNT; s++) {
        FT_Face face = NULL;
        if (G.FT_New_Face(ft, files[s], 0, &face) != 0) {
            if (G.FT_New_Face(ft, "/usr/share/fonts/SamsungOneUI-400.ttf", 0, &face) != 0) continue;
        }
        G.FT_Set_Pixel_Sizes(face, 0, (FT_UInt)sizes[s]);
        r->fonts[s].px = (float)sizes[s];
        r->fonts[s].ascender = (float)sizes[s] * 0.78f;
        for (uint32_t c = 32; c < 127; c++)
            pack_glyph(face, c, atlas, AW, AH, &pen_x, &pen_y, &row_h, &r->fonts[s].g[c]);
        for (uint32_t c = 0x00A0; c <= 0x00FF; c++)
            pack_glyph(face, c, atlas, AW, AH, &pen_x, &pen_y, &row_h, &r->fonts[s].g_latin[c - 0x00A0]);
        pack_glyph(face, 0x00B7, atlas, AW, AH, &pen_x, &pen_y, &row_h, &r->fonts[s].g_dot);
        pack_glyph(face, 0x2026, atlas, AW, AH, &pen_x, &pen_y, &row_h, &r->fonts[s].g_ell);
        pack_glyph(face, 0x2212, atlas, AW, AH, &pen_x, &pen_y, &row_h, &r->fonts[s].g_min);
        pack_glyph(face, 0x25C0, atlas, AW, AH, &pen_x, &pen_y, &row_h, &r->fonts[s].g_lt);
        pack_glyph(face, 0x25B6, atlas, AW, AH, &pen_x, &pen_y, &row_h, &r->fonts[s].g_rt);
        pack_glyph(face, 0x25B2, atlas, AW, AH, &pen_x, &pen_y, &row_h, &r->fonts[s].g_up);
        pack_glyph(face, 0x25BC, atlas, AW, AH, &pen_x, &pen_y, &row_h, &r->fonts[s].g_dn);
    }

    if (r->fonts[0].g['A'].w > 0) {
        fprintf(stderr, "[ft] atlas ready (1024x1024, 'A'@14px=%dx%d adv=%.0f pen_y=%d)\n",
                (int)r->fonts[0].g['A'].w, (int)r->fonts[0].g['A'].h, r->fonts[0].g['A'].adv, pen_y);
    } else {
        fprintf(stderr, "[ft] WARNING: 'A' glyph empty (pen_y=%d)\n", pen_y);
    }

    G.glGenTextures(1, &r->glyph_tex);
    G.glBindTexture(GL_TEXTURE_2D, r->glyph_tex);
    G.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    G.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    G.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    G.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    G.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    G.glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, AW, AH, 0, GL_ALPHA, GL_UNSIGNED_BYTE, atlas);
    free(atlas);
    return true;
}

bool renderer_init(Renderer *r, const char *asset_dir) {
    memset(r, 0, sizeof(*r));
    GLuint vs = compile_shader(GL_VERTEX_SHADER, VS_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FS_SRC);
    if (!vs || !fs) return false;
    r->prog = G.glCreateProgram();
    G.glAttachShader(r->prog, vs);
    G.glAttachShader(r->prog, fs);
    G.glBindAttribLocation(r->prog, 0, "a_pos");
    G.glBindAttribLocation(r->prog, 1, "a_uv");
    G.glBindAttribLocation(r->prog, 2, "a_col");
    G.glBindAttribLocation(r->prog, 3, "a_ext");
    G.glBindAttribLocation(r->prog, 4, "a_mode");
    G.glLinkProgram(r->prog);
    GLint ok = 0;
    G.glGetProgramiv(r->prog, GL_LINK_STATUS, &ok);
    if (!ok) return false;

    r->u_res = G.glGetUniformLocation(r->prog, "u_res");
    r->u_tex = G.glGetUniformLocation(r->prog, "u_tex");
    G.glGenBuffers(1, &r->vbo);

    char p[512];
    snprintf(p, sizeof(p), "%s/bg_base.astc", asset_dir);      r->bg_base = load_astc(p);
    snprintf(p, sizeof(p), "%s/dock_rail.astc", asset_dir);    r->dock_rail = load_astc(p);
    snprintf(p, sizeof(p), "%s/icons_atlas.astc", asset_dir);  r->icons_atlas = load_astc(p);

    const char *names[3] = { "plex", "youtube", "switch2" };
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof(p), "%s/halo_%s.astc", asset_dir, names[i]);        r->halo[i] = load_astc(p);
        snprintf(p, sizeof(p), "%s/card_%s_idle.astc", asset_dir, names[i]);   r->card_idle[i] = load_astc(p);
        snprintf(p, sizeof(p), "%s/card_%s_focus.astc", asset_dir, names[i]);  r->card_focus[i] = load_astc(p);
    }

    build_font_atlas(r);
    return r->bg_base.id != 0;
}

void renderer_begin_frame(Renderer *r, int w, int h, bool clear_transparent) {
    VCOUNT = 0;
    CUR_TEX = 0;
    DRAW_CALLS = 0;
    G.glViewport(0, 0, w, h);
    G.glDisable(GL_DEPTH_TEST);
    G.glDisable(GL_CULL_FACE);
    G.glEnable(GL_BLEND);
    if (G.glBlendFuncSeparate) {
        G.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        G.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    if (clear_transparent) {
        G.glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        G.glClear(GL_COLOR_BUFFER_BIT);
    }
    G.glUseProgram(r->prog);
    G.glUniform2f(r->u_res, (float)w, (float)h);
    G.glUniform1i(r->u_tex, 0);
    G.glActiveTexture(GL_TEXTURE0);
    G.glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    G.glEnableVertexAttribArray(0);
    G.glEnableVertexAttribArray(1);
    G.glEnableVertexAttribArray(2);
    G.glEnableVertexAttribArray(3);
    G.glEnableVertexAttribArray(4);
    G.glVertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(Vertex), (void *)offsetof(Vertex, x));
    G.glVertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(Vertex), (void *)offsetof(Vertex, u));
    G.glVertexAttribPointer(2, 4, GL_FLOAT, 0, sizeof(Vertex), (void *)offsetof(Vertex, r));
    G.glVertexAttribPointer(3, 4, GL_FLOAT, 0, sizeof(Vertex), (void *)offsetof(Vertex, e0));
    G.glVertexAttribPointer(4, 1, GL_FLOAT, 0, sizeof(Vertex), (void *)offsetof(Vertex, mode));
}

void renderer_set_scissor(int x, int y, int w, int h) {
    G.glEnable(GL_SCISSOR_TEST);
    G.glScissor(x, 1080 - (y + h), w, h);
}

void renderer_clear_scissor(void) {
    G.glDisable(GL_SCISSOR_TEST);
}

void r_flush(Renderer *r) {
    (void)r;
    if (VCOUNT == 0) return;
    if (CUR_TEX) G.glBindTexture(GL_TEXTURE_2D, CUR_TEX);
    G.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(VCOUNT * sizeof(Vertex)), VBUF, GL_DYNAMIC_DRAW);
    G.glDrawArrays(GL_TRIANGLES, 0, VCOUNT);
    DRAW_CALLS++;
    VCOUNT = 0;
}

int r_get_draw_calls(void) { return DRAW_CALLS; }

static void push_quad(Renderer *r, GLuint tex, float x, float y, float w, float h,
                      float u0, float v0, float u1, float v1,
                      float cr, float cg, float cb, float ca,
                      float e0, float e1, float e2, float e3, float mode) {
    if ((tex != 0 && tex != CUR_TEX) || VCOUNT + 6 > MAX_VERTS) {
        r_flush(r);
        if (tex != 0) CUR_TEX = tex;
    }
    Vertex v[6] = {
        { x,     y,     u0, v0, cr, cg, cb, ca, e0, e1, e2, e3, mode },
        { x + w, y,     u1, v0, cr, cg, cb, ca, e0, e1, e2, e3, mode },
        { x + w, y + h, u1, v1, cr, cg, cb, ca, e0, e1, e2, e3, mode },
        { x,     y,     u0, v0, cr, cg, cb, ca, e0, e1, e2, e3, mode },
        { x + w, y + h, u1, v1, cr, cg, cb, ca, e0, e1, e2, e3, mode },
        { x,     y + h, u0, v1, cr, cg, cb, ca, e0, e1, e2, e3, mode },
    };
    memcpy(&VBUF[VCOUNT], v, sizeof(v));
    VCOUNT += 6;
}

void r_quad_tex(Renderer *r, GLuint tex, float x, float y, float w, float h,
                float u0, float v0, float u1, float v1,
                float cr, float cg, float cb, float ca) {
    if (!tex) return;
    push_quad(r, tex, x, y, w, h, u0, v0, u1, v1, cr, cg, cb, ca, 0, 0, 0, 0, 0.0f);
}

void r_icon(Renderer *r, int icon_idx, float x, float y, float w, float h,
            float cr, float cg, float cb, float ca) {
    float u0 = (float)(icon_idx * 48) / 480.0f;
    float u1 = (float)((icon_idx + 1) * 48) / 480.0f;
    r_quad_tex(r, r->icons_atlas.id, x, y, w, h, u0, 0.0f, u1, 1.0f, cr, cg, cb, ca);
}

void r_round_rect(Renderer *r, float x, float y, float w, float h, float radius,
                  float cr, float cg, float cb, float ca,
                  float br, float bg, float bb, float ba, float bw) {
    float hw = w * 0.5f, hh = h * 0.5f;
    if (ca > 0.001f) {
        push_quad(r, 0, x, y, w, h, -hw, -hh, hw, hh, cr, cg, cb, ca, hw, hh, radius, 0.0f, 2.0f);
    }
    if (bw > 0.01f && ba > 0.001f) {
        push_quad(r, 0, x, y, w, h, -hw, -hh, hw, hh, br, bg, bb, ba, hw, hh, radius, bw, 3.0f);
    }
}

static const GlyphInfo *lookup_utf8(const FontAtlasSlot *f, const unsigned char **pp) {
    const unsigned char *p = *pp;
    unsigned char c = *p++;
    if (c < 128) { *pp = p; return &f->g[c]; }
    uint32_t cp = '?';
    if ((c & 0xE0) == 0xC0 && p[0]) {
        cp = ((c & 0x1F) << 6) | (p[0] & 0x3F);
        p += 1;
    } else if ((c & 0xF0) == 0xE0 && p[0] && p[1]) {
        cp = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
        p += 2;
    } else if ((c & 0xF8) == 0xF0 && p[0] && p[1] && p[2]) {
        cp = ((c & 0x07) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        p += 3;
    }
    *pp = p;
    if (cp == 0x00B7) return &f->g_dot;
    if (cp >= 0x00A0 && cp <= 0x00FF) return &f->g_latin[cp - 0x00A0];
    if (cp == 0x2026) return &f->g_ell;
    if (cp == 0x2212 || cp == 0x2013 || cp == 0x2014) return &f->g_min;
    if (cp == 0x2018 || cp == 0x2019) return &f->g['\''];
    if (cp == 0x201C || cp == 0x201D) return &f->g['"'];
    if (cp == 0x25C0) return &f->g_lt;
    if (cp == 0x25B6) return &f->g_rt;
    if (cp == 0x25B2) return &f->g_up;
    if (cp == 0x25BC) return &f->g_dn;
    return &f->g[' '];
}

void r_text_spaced(Renderer *r, int font_slot, const char *utf8, float x, float y, float box_w, float box_h,
                   int align, float extra_spacing, float cr, float cg, float cb, float ca) {
    if (!utf8 || !*utf8 || font_slot < 0 || font_slot >= FONT_SLOT_COUNT) return;
    const FontAtlasSlot *f = &r->fonts[font_slot];
    float total_w = 0.0f;
    int count = 0;
    const unsigned char *p = (const unsigned char *)utf8;
    while (*p) {
        const GlyphInfo *g = lookup_utf8(f, &p);
        total_w += g->adv;
        count++;
    }
    if (count > 1) total_w += extra_spacing * (float)(count - 1);
    float pen_x = x;
    if (align == 1) pen_x = x + (box_w - total_w) * 0.5f;
    else if (align == 2) pen_x = x + (box_w - total_w);
    float base_y = y + (box_h + f->ascender * 0.85f) * 0.5f;

    p = (const unsigned char *)utf8;
    while (*p) {
        const GlyphInfo *g = lookup_utf8(f, &p);
        if (g->w > 0 && g->h > 0) {
            float gx = floorf(pen_x + g->bx + 0.5f);
            float gy = floorf(base_y - g->by + 0.5f);
            push_quad(r, r->glyph_tex, gx, gy, g->w, g->h, g->u0, g->v0, g->u1, g->v1, cr, cg, cb, ca, 0, 0, 0, 0, 1.0f);
        }
        pen_x += g->adv + extra_spacing;
    }
}

void r_text(Renderer *r, int font_slot, const char *utf8, float x, float y, float box_w, float box_h,
            int align, float cr, float cg, float cb, float ca) {
    r_text_spaced(r, font_slot, utf8, x, y, box_w, box_h, align, 0.0f, cr, cg, cb, ca);
}

void r_text_fit(Renderer *r, int font_slot, const char *utf8, float x, float y, float box_w, float box_h,
                int align, float cr, float cg, float cb, float ca) {
    if (!utf8 || !*utf8 || font_slot < 0 || font_slot >= FONT_SLOT_COUNT) return;
    const FontAtlasSlot *f = &r->fonts[font_slot];
    float total_w = 0.0f;
    const unsigned char *p = (const unsigned char *)utf8;
    while (*p) {
        total_w += lookup_utf8(f, &p)->adv;
    }
    if (total_w <= box_w || box_w <= 24.0f) {
        r_text_spaced(r, font_slot, utf8, x, y, box_w, box_h, align, 0.0f, cr, cg, cb, ca);
        return;
    }
    float limit = box_w - (f->g_ell.adv > 1.0f ? f->g_ell.adv : 12.0f);
    char fit[256];
    size_t out_n = 0;
    float acc = 0.0f;
    p = (const unsigned char *)utf8;
    while (*p) {
        const unsigned char *prev = p;
        const GlyphInfo *g = lookup_utf8(f, &p);
        size_t step = (size_t)(p - prev);
        if (acc + g->adv > limit || out_n + step + 4 >= sizeof(fit)) break;
        memcpy(fit + out_n, prev, step);
        out_n += step;
        acc += g->adv;
    }
    while (out_n > 0 && fit[out_n - 1] == ' ') out_n--;
    memcpy(fit + out_n, "\xE2\x80\xA6", 3);
    fit[out_n + 3] = '\0';
    r_text_spaced(r, font_slot, fit, x, y, box_w, box_h, align, 0.0f, cr, cg, cb, ca);
}
