#include "tizen_ffi.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

TizenFFI G;

#define LOAD_SYM(lib, name) do { G.name = dlsym(lib, #name); if (!G.name) fprintf(stderr, "[ffi] missing %s\n", #name); } while (0)
#define LOAD_SYM_OPT(lib, name) do { G.name = dlsym(lib, #name); } while (0)

bool ffi_load_graphics(void) {
    memset(&G, 0, sizeof(G));

    void *wl = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_GLOBAL);
    void *wlegl = dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_GLOBAL);
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    void *gl = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    void *text = dlopen("libtizen-extension-client.so.0", RTLD_NOW);
    void *ft = dlopen("libfreetype.so.6", RTLD_NOW);
    if (!wl || !wlegl || !egl || !gl || !text || !ft) {
        fprintf(stderr, "[ffi] core dlopen failed: %s\n", dlerror());
        return false;
    }

    LOAD_SYM(wl, wl_display_connect);
    LOAD_SYM(wl, wl_display_disconnect);
    LOAD_SYM(wl, wl_display_get_fd);
    LOAD_SYM(wl, wl_display_dispatch);
    LOAD_SYM(wl, wl_display_dispatch_pending);
    LOAD_SYM(wl, wl_display_prepare_read);
    LOAD_SYM(wl, wl_display_read_events);
    LOAD_SYM(wl, wl_display_cancel_read);
    LOAD_SYM(wl, wl_display_flush);
    LOAD_SYM(wl, wl_display_roundtrip);
    LOAD_SYM(wl, wl_proxy_marshal_constructor);
    LOAD_SYM(wl, wl_proxy_marshal_constructor_versioned);
    LOAD_SYM(wl, wl_proxy_marshal);
    LOAD_SYM(wl, wl_proxy_add_listener);
    LOAD_SYM(wl, wl_proxy_destroy);

    G.wl_registry_iface      = dlsym(wl, "wl_registry_interface");
    G.wl_compositor_iface    = dlsym(wl, "wl_compositor_interface");
    G.wl_surface_iface       = dlsym(wl, "wl_surface_interface");
    G.wl_region_iface        = dlsym(wl, "wl_region_interface");
    G.wl_shell_iface         = dlsym(wl, "wl_shell_interface");
    G.wl_shell_surface_iface = dlsym(wl, "wl_shell_surface_interface");
    G.wl_seat_iface          = dlsym(wl, "wl_seat_interface");
    G.wl_keyboard_iface      = dlsym(wl, "wl_keyboard_interface");
    G.wl_callback_iface      = dlsym(wl, "wl_callback_interface");
    G.tizen_policy_iface     = dlsym(text, "tizen_policy_interface");
    G.tizen_keyrouter_iface  = dlsym(text, "tizen_keyrouter_interface");
    G.tizen_surface_iface    = dlsym(text, "tizen_surface_interface");
    G.tizen_resource_iface   = dlsym(text, "tizen_resource_interface");

    LOAD_SYM(wlegl, wl_egl_window_create);
    LOAD_SYM(wlegl, wl_egl_window_destroy);
    LOAD_SYM(wlegl, wl_egl_window_resize);

    LOAD_SYM(egl, eglGetDisplay);
    LOAD_SYM(egl, eglInitialize);
    LOAD_SYM(egl, eglBindAPI);
    LOAD_SYM(egl, eglChooseConfig);
    LOAD_SYM(egl, eglCreateContext);
    LOAD_SYM(egl, eglCreateWindowSurface);
    LOAD_SYM(egl, eglMakeCurrent);
    LOAD_SYM(egl, eglSwapInterval);
    LOAD_SYM(egl, eglSwapBuffers);
    LOAD_SYM(egl, eglQuerySurface);
    LOAD_SYM(egl, eglGetProcAddress);
    if (G.eglGetProcAddress) {
        G.eglSetDamageRegionKHR = G.eglGetProcAddress("eglSetDamageRegionKHR");
        G.eglSwapBuffersWithDamageKHR = G.eglGetProcAddress("eglSwapBuffersWithDamageKHR");
    }

    LOAD_SYM(gl, glGetString);
    LOAD_SYM(gl, glViewport);
    LOAD_SYM(gl, glScissor);
    LOAD_SYM(gl, glEnable);
    LOAD_SYM(gl, glDisable);
    LOAD_SYM(gl, glBlendFunc);
    LOAD_SYM_OPT(gl, glBlendFuncSeparate);
    LOAD_SYM(gl, glClearColor);
    LOAD_SYM(gl, glClear);
    LOAD_SYM(gl, glCreateShader);
    LOAD_SYM(gl, glShaderSource);
    LOAD_SYM(gl, glCompileShader);
    LOAD_SYM(gl, glGetShaderiv);
    LOAD_SYM(gl, glGetShaderInfoLog);
    LOAD_SYM(gl, glCreateProgram);
    LOAD_SYM(gl, glAttachShader);
    LOAD_SYM(gl, glBindAttribLocation);
    LOAD_SYM(gl, glLinkProgram);
    LOAD_SYM(gl, glGetProgramiv);
    LOAD_SYM(gl, glUseProgram);
    LOAD_SYM(gl, glGetUniformLocation);
    LOAD_SYM(gl, glUniform1i);
    LOAD_SYM(gl, glUniform2f);
    LOAD_SYM(gl, glGenBuffers);
    LOAD_SYM(gl, glBindBuffer);
    LOAD_SYM(gl, glBufferData);
    LOAD_SYM(gl, glEnableVertexAttribArray);
    LOAD_SYM(gl, glVertexAttribPointer);
    LOAD_SYM(gl, glDrawArrays);
    LOAD_SYM(gl, glGenTextures);
    LOAD_SYM(gl, glDeleteTextures);
    LOAD_SYM(gl, glActiveTexture);
    LOAD_SYM(gl, glBindTexture);
    LOAD_SYM(gl, glTexParameteri);
    LOAD_SYM(gl, glPixelStorei);
    LOAD_SYM(gl, glTexImage2D);
    LOAD_SYM(gl, glTexSubImage2D);
    LOAD_SYM(gl, glCompressedTexImage2D);
    LOAD_SYM(gl, glFinish);

    LOAD_SYM(ft, FT_Init_FreeType);
    LOAD_SYM(ft, FT_New_Face);
    LOAD_SYM(ft, FT_Set_Pixel_Sizes);
    LOAD_SYM(ft, FT_Load_Char);
    return true;
}

void ffi_load_services(void) {
    /* Tizen TV service APIs (loaded off main thread so boot-to-first-pixel is ~340 ms!) */
    void *ac = dlopen("libcapi-appfw-app-control.so.0", RTLD_LAZY);
    if (ac) {
        LOAD_SYM_OPT(ac, app_control_create);
        LOAD_SYM_OPT(ac, app_control_set_app_id);
        LOAD_SYM_OPT(ac, app_control_send_launch_request);
        LOAD_SYM_OPT(ac, app_control_destroy);
    }
    void *am = dlopen("libcapi-appfw-app-manager.so.0", RTLD_LAZY);
    if (am) {
        LOAD_SYM_OPT(am, app_manager_is_running);
        LOAD_SYM_OPT(am, app_manager_get_app_context);
        LOAD_SYM_OPT(am, app_manager_request_terminate_bg_app);
        LOAD_SYM_OPT(am, app_manager_terminate_app_without_restarting);
        LOAD_SYM_OPT(am, app_context_destroy);
    }
    void *sa = dlopen("libsource-api.so", RTLD_LAZY);
    if (sa) {
        LOAD_SYM_OPT(sa, get_source_list);
        LOAD_SYM_OPT(sa, connect_source);
        LOAD_SYM_OPT(sa, connect_source_with_uniquekey);
    }
    void *ep = dlopen("libep-system-api.so", RTLD_LAZY);
    if (ep) {
        LOAD_SYM_OPT(ep, ep_source_get_source);
        LOAD_SYM_OPT(ep, ep_source_get_signal_status);
    }
    void *sm = dlopen("libcapi-media-sound-manager.so.0", RTLD_LAZY);
    if (sm) {
        LOAD_SYM_OPT(sm, sound_manager_get_volume);
        LOAD_SYM_OPT(sm, sound_manager_set_volume);
        LOAD_SYM_OPT(sm, sound_manager_get_mute);
        LOAD_SYM_OPT(sm, sound_manager_set_mute);
    }
    void *smtv = dlopen("libcapi-media-sound-manager-tv.so.0", RTLD_LAZY);
    if (smtv) {
        LOAD_SYM_OPT(smtv, sound_manager_get_current_speaker_volume);
        LOAD_SYM_OPT(smtv, sound_manager_set_current_speaker_volume);
        LOAD_SYM_OPT(smtv, sound_manager_get_current_speaker_mute);
        LOAD_SYM_OPT(smtv, sound_manager_set_current_speaker_mute);
        LOAD_SYM_OPT(smtv, sound_manager_get_tv_master_volume);
        LOAD_SYM_OPT(smtv, sound_manager_set_tv_master_volume);
        LOAD_SYM_OPT(smtv, sound_manager_get_tv_master_mute);
        LOAD_SYM_OPT(smtv, sound_manager_set_tv_master_mute);
    }
    void *cec = dlopen("libhdmicec-api.so.0", RTLD_LAZY);
    if (cec) {
        LOAD_SYM_OPT(cec, cec_get_device_power_status_by_hdmi_port);
    }
}

bool ffi_load_all(void) {
    if (!ffi_load_graphics()) return false;
    ffi_load_services();
    return true;
}
