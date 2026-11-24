#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <dlfcn.h>
#include <stdio.h>

/* ── Minimal Wayland client ABI (exact match to libwayland-client.so.0.22) ── */
struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_surface;
struct wl_region;
struct wl_shell;
struct wl_shell_surface;
struct wl_seat;
struct wl_keyboard;
struct wl_callback;
struct wl_egl_window;
struct wl_proxy;
struct wl_interface;

struct wl_registry_listener {
    void (*global)(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version);
    void (*global_remove)(void *data, struct wl_registry *reg, uint32_t name);
};

struct wl_seat_listener {
    void (*capabilities)(void *data, struct wl_seat *seat, uint32_t caps);
    void (*name)(void *data, struct wl_seat *seat, const char *name);
};

struct wl_keyboard_listener {
    void (*keymap)(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size);
    void (*enter)(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *surface, void *keys);
    void (*leave)(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *surface);
    void (*key)(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
    void (*modifiers)(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lck, uint32_t grp);
    void (*repeat_info)(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay);
};

struct wl_callback_listener {
    void (*done)(void *data, struct wl_callback *cb, uint32_t callback_data);
};

struct wl_shell_surface_listener {
    void (*ping)(void *data, struct wl_shell_surface *ss, uint32_t serial);
    void (*configure)(void *data, struct wl_shell_surface *ss, uint32_t edges, int32_t width, int32_t height);
    void (*popup_done)(void *data, struct wl_shell_surface *ss);
};

/* Standard Wayland opcodes */
#define WL_DISPLAY_GET_REGISTRY         1
#define WL_REGISTRY_BIND                0
#define WL_COMPOSITOR_CREATE_SURFACE    0
#define WL_COMPOSITOR_CREATE_REGION     1
#define WL_REGION_DESTROY               0
#define WL_REGION_ADD                   1
#define WL_SURFACE_DESTROY              0
#define WL_SURFACE_FRAME                3
#define WL_SURFACE_SET_OPAQUE_REGION    4
#define WL_SURFACE_SET_INPUT_REGION     5
#define WL_SURFACE_COMMIT               6
#define WL_SHELL_GET_SHELL_SURFACE      0
#define WL_SHELL_SURFACE_PONG           0
#define WL_SHELL_SURFACE_SET_TOPLEVEL   3
#define WL_SHELL_SURFACE_SET_FULLSCREEN 5
#define WL_SHELL_SURFACE_SET_TITLE      8
#define WL_SEAT_GET_KEYBOARD            1

/* Exact Tizen Wayland extension opcodes verified from libtizen-extension-client.so.0 */
#define TIZEN_POLICY_ACTIVATE           2
#define TIZEN_POLICY_RAISE              4
#define TIZEN_POLICY_LOWER              5
#define TIZEN_POLICY_SET_FOCUS_SKIP     7
#define TIZEN_POLICY_UNSET_FOCUS_SKIP   8
#define TIZEN_POLICY_SET_TYPE          10
#define TIZEN_POLICY_SET_OPAQUE_STATE  21
#define TIZEN_POLICY_SHOW              40
#define TIZEN_POLICY_HIDE              41

#define TIZEN_KEYROUTER_SET_KEYGRAB     0
#define TIZEN_KEYROUTER_UNSET_KEYGRAB   1
#define TIZEN_KEYGRAB_OVERRIDE_EXCL     3

#define TIZEN_SURFACE_GET_TIZEN_RESOURCE 0

struct tizen_resource_listener {
    void (*resource_id)(void *data, struct wl_proxy *res, uint32_t id);
};

/* ── Minimal EGL & GLES2 types/constants ── */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef unsigned int EGLBoolean;
typedef int32_t EGLint;
typedef unsigned int EGLenum;

#define EGL_NO_DISPLAY       ((EGLDisplay)0)
#define EGL_NO_SURFACE       ((EGLSurface)0)
#define EGL_NO_CONTEXT       ((EGLContext)0)
#define EGL_OPENGL_ES_API    0x30A0
#define EGL_SURFACE_TYPE     0x3033
#define EGL_WINDOW_BIT       0x0004
#define EGL_RENDERABLE_TYPE  0x3040
#define EGL_OPENGL_ES2_BIT   0x0004
#define EGL_RED_SIZE         0x3024
#define EGL_GREEN_SIZE       0x3023
#define EGL_BLUE_SIZE        0x3022
#define EGL_ALPHA_SIZE       0x3021
#define EGL_DEPTH_SIZE       0x3025
#define EGL_STENCIL_SIZE     0x3026
#define EGL_NONE             0x3038
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_BUFFER_AGE_KHR   0x313D

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

#define GL_COLOR_BUFFER_BIT        0x00004000
#define GL_BLEND                   0x0BE2
#define GL_SRC_ALPHA               0x0302
#define GL_ONE_MINUS_SRC_ALPHA     0x0303
#define GL_ONE                     1
#define GL_DEPTH_TEST              0x0B71
#define GL_CULL_FACE               0x0B44
#define GL_SCISSOR_TEST            0x0C11
#define GL_TEXTURE_2D              0x0DE1
#define GL_TEXTURE0                0x84C0
#define GL_TEXTURE_MIN_FILTER      0x2801
#define GL_TEXTURE_MAG_FILTER      0x2800
#define GL_TEXTURE_WRAP_S          0x2802
#define GL_TEXTURE_WRAP_T          0x2803
#define GL_LINEAR                  0x2601
#define GL_CLAMP_TO_EDGE           0x812F
#define GL_UNPACK_ALIGNMENT        0x0CF5
#define GL_RGBA                    0x1908
#define GL_ALPHA                   0x1906
#define GL_UNSIGNED_BYTE           0x1401
#define GL_FLOAT                   0x1406
#define GL_TRIANGLES               0x0004
#define GL_ARRAY_BUFFER            0x8892
#define GL_DYNAMIC_DRAW            0x88E8
#define GL_VERTEX_SHADER           0x8B31
#define GL_FRAGMENT_SHADER         0x8B30
#define GL_COMPILE_STATUS          0x8B81
#define GL_LINK_STATUS             0x8B82
#define GL_RENDERER                0x1F01
#define GL_VERSION                 0x1F02
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0
#define GL_COMPRESSED_RGBA_ASTC_6x6_KHR 0x93B4

/* ── Minimal FreeType2 ABI (32-bit ARM exact layout) ── */
typedef int32_t FT_Error;
typedef int32_t FT_Int;
typedef uint32_t FT_UInt;
typedef int32_t FT_Long;
typedef uint32_t FT_ULong;
typedef int32_t FT_Pos;
typedef void *FT_Library;
typedef struct FT_FaceRec_ *FT_Face;

typedef struct {
    uint32_t rows;
    uint32_t width;
    int32_t  pitch;
    unsigned char *buffer;
    uint16_t num_grays;
    unsigned char pixel_mode;
    unsigned char palette_mode;
    void *palette;
} FT_Bitmap;

typedef struct { FT_Pos x, y; } FT_Vector;

/* We use FT_Load_Char + access glyph slot via exact offsets checked at startup or FT_Glyph_To_Bitmap */
typedef struct FT_GlyphSlotRec_ {
    FT_Library library;
    FT_Face    face;
    struct FT_GlyphSlotRec_ *next;
    FT_UInt    glyph_index;
    struct { void *data; void (*finalizer)(void *); } generic;
    /* FT_Glyph_Metrics (8 * 4 = 32 bytes) */
    FT_Pos m_width, m_height, m_horiBearingX, m_horiBearingY, m_horiAdvance;
    FT_Pos m_vertBearingX, m_vertBearingY, m_vertAdvance;
    FT_Long linearHoriAdvance;
    FT_Long linearVertAdvance;
    FT_Vector advance;
    uint32_t format;
    FT_Bitmap bitmap;
    FT_Int bitmap_left;
    FT_Int bitmap_top;
} FT_GlyphSlotRec;

typedef struct FT_FaceRec_ {
    FT_Long num_faces;
    FT_Long face_index;
    FT_Long face_flags;
    FT_Long style_flags;
    FT_Long num_glyphs;
    char *family_name;
    char *style_name;
    FT_Int num_fixed_sizes;
    void *available_sizes;
    FT_Int num_charmaps;
    void *charmaps;
    struct { void *data; void (*finalizer)(void *); } generic;
    FT_Pos bbox[4];
    uint16_t units_per_EM;
    int16_t ascender;
    int16_t descender;
    int16_t height;
    int16_t max_advance_width;
    int16_t max_advance_height;
    int16_t underline_position;
    int16_t underline_thickness;
    FT_GlyphSlotRec *glyph;
} FT_FaceRec;

#define FT_LOAD_RENDER (1L << 2)

/* ── Runtime dlopen symbol table ── */
typedef struct {
    /* Wayland client */
    struct wl_display *(*wl_display_connect)(const char *);
    void (*wl_display_disconnect)(struct wl_display *);
    int (*wl_display_get_fd)(struct wl_display *);
    int (*wl_display_dispatch)(struct wl_display *);
    int (*wl_display_dispatch_pending)(struct wl_display *);
    int (*wl_display_prepare_read)(struct wl_display *);
    int (*wl_display_read_events)(struct wl_display *);
    void (*wl_display_cancel_read)(struct wl_display *);
    int (*wl_display_flush)(struct wl_display *);
    int (*wl_display_roundtrip)(struct wl_display *);
    struct wl_proxy *(*wl_proxy_marshal_constructor)(struct wl_proxy *, uint32_t, const struct wl_interface *, ...);
    struct wl_proxy *(*wl_proxy_marshal_constructor_versioned)(struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, ...);
    void (*wl_proxy_marshal)(struct wl_proxy *, uint32_t, ...);
    int (*wl_proxy_add_listener)(struct wl_proxy *, void (**)(void), void *);
    void (*wl_proxy_destroy)(struct wl_proxy *);
    const struct wl_interface *wl_registry_iface;
    const struct wl_interface *wl_compositor_iface;
    const struct wl_interface *wl_surface_iface;
    const struct wl_interface *wl_region_iface;
    const struct wl_interface *wl_shell_iface;
    const struct wl_interface *wl_shell_surface_iface;
    const struct wl_interface *wl_seat_iface;
    const struct wl_interface *wl_keyboard_iface;
    const struct wl_interface *wl_callback_iface;
    const struct wl_interface *tizen_policy_iface;
    const struct wl_interface *tizen_keyrouter_iface;
    const struct wl_interface *tizen_surface_iface;
    const struct wl_interface *tizen_resource_iface;

    /* Wayland EGL */
    struct wl_egl_window *(*wl_egl_window_create)(struct wl_surface *, int, int);
    void (*wl_egl_window_destroy)(struct wl_egl_window *);
    void (*wl_egl_window_resize)(struct wl_egl_window *, int, int, int, int);

    /* EGL */
    EGLDisplay (*eglGetDisplay)(void *);
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*eglBindAPI)(EGLenum);
    EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLSurface (*eglCreateWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *);
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*eglSwapInterval)(EGLDisplay, EGLint);
    EGLBoolean (*eglSwapBuffers)(EGLDisplay, EGLSurface);
    EGLBoolean (*eglQuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
    void *(*eglGetProcAddress)(const char *);
    EGLBoolean (*eglSetDamageRegionKHR)(EGLDisplay, EGLSurface, EGLint *, EGLint);
    EGLBoolean (*eglSwapBuffersWithDamageKHR)(EGLDisplay, EGLSurface, EGLint *, EGLint);

    /* GLES2 */
    const unsigned char *(*glGetString)(GLenum);
    void (*glViewport)(GLint, GLint, GLsizei, GLsizei);
    void (*glScissor)(GLint, GLint, GLsizei, GLsizei);
    void (*glEnable)(GLenum);
    void (*glDisable)(GLenum);
    void (*glBlendFunc)(GLenum, GLenum);
    void (*glBlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
    void (*glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*glClear)(GLenum);
    GLuint (*glCreateShader)(GLenum);
    void (*glShaderSource)(GLuint, GLsizei, const GLchar **, const GLint *);
    void (*glCompileShader)(GLuint);
    void (*glGetShaderiv)(GLuint, GLenum, GLint *);
    void (*glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    GLuint (*glCreateProgram)(void);
    void (*glAttachShader)(GLuint, GLuint);
    void (*glBindAttribLocation)(GLuint, GLuint, const GLchar *);
    void (*glLinkProgram)(GLuint);
    void (*glGetProgramiv)(GLuint, GLenum, GLint *);
    void (*glUseProgram)(GLuint);
    GLint (*glGetUniformLocation)(GLuint, const GLchar *);
    void (*glUniform1i)(GLint, GLint);
    void (*glUniform2f)(GLint, GLfloat, GLfloat);
    void (*glGenBuffers)(GLsizei, GLuint *);
    void (*glBindBuffer)(GLenum, GLuint);
    void (*glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
    void (*glEnableVertexAttribArray)(GLuint);
    void (*glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
    void (*glDrawArrays)(GLenum, GLint, GLsizei);
    void (*glGenTextures)(GLsizei, GLuint *);
    void (*glDeleteTextures)(GLsizei, const GLuint *);
    void (*glActiveTexture)(GLenum);
    void (*glBindTexture)(GLenum, GLuint);
    void (*glTexParameteri)(GLenum, GLenum, GLint);
    void (*glPixelStorei)(GLenum, GLint);
    void (*glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
    void (*glCompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
    void (*glFinish)(void);

    /* FreeType */
    FT_Error (*FT_Init_FreeType)(FT_Library *);
    FT_Error (*FT_New_Face)(FT_Library, const char *, FT_Long, FT_Face *);
    FT_Error (*FT_Set_Pixel_Sizes)(FT_Face, FT_UInt, FT_UInt);
    FT_Error (*FT_Load_Char)(FT_Face, FT_ULong, FT_Long);

    /* Tizen TV APIs */
    int (*app_control_create)(void **);
    int (*app_control_set_app_id)(void *, const char *);
    int (*app_control_send_launch_request)(void *, void *, void *);
    int (*app_control_destroy)(void *);

    int (*app_manager_is_running)(const char *, bool *);
    int (*app_manager_get_app_context)(const char *, void **);
    int (*app_manager_request_terminate_bg_app)(void *);
    int (*app_manager_terminate_app_without_restarting)(void *);
    int (*app_context_destroy)(void *);

    char *(*get_source_list)(void);
    bool (*connect_source)(int);
    bool (*connect_source_with_uniquekey)(int, const char *);
    int (*ep_source_get_source)(int *, int, int);
    int (*ep_source_get_signal_status)(int *, int);

    int (*sound_manager_get_volume)(int, int *);
    int (*sound_manager_set_volume)(int, int);
    int (*sound_manager_get_mute)(int, bool *);
    int (*sound_manager_set_mute)(int, bool);

    /* Samsung TV real 0..100 master/speaker volume (libcapi-media-sound-manager-tv.so.0) */
    int (*sound_manager_get_current_speaker_volume)(int *);
    int (*sound_manager_set_current_speaker_volume)(int);
    int (*sound_manager_get_current_speaker_mute)(bool *);
    int (*sound_manager_set_current_speaker_mute)(bool);
    int (*sound_manager_get_tv_master_volume)(int *);
    int (*sound_manager_set_tv_master_volume)(int);
    int (*sound_manager_get_tv_master_mute)(bool *);
    int (*sound_manager_set_tv_master_mute)(bool);

    int (*cec_get_device_power_status_by_hdmi_port)(int, int *);
} TizenFFI;

extern TizenFFI G;

bool ffi_load_graphics(void);
void ffi_load_services(void);
bool ffi_load_all(void);
