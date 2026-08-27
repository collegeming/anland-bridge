#define _GNU_SOURCE
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <ctype.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <jni.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include "anw_hidden.h"
#include "camera_service.h"
#include "display_consumer.h"
#include "native_audio.h"
#include "protocol.h"
#include "socket_utils.h"
#include "tracy_zones.h"

#define TAG "Anland"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define PIXEL_FORMAT_RGBA_8888 1
#define MAX_COLLECT_BUFS 8
#define FANOUT_RING_SIZE 3

#ifndef EGL_RECORDABLE_ANDROID
#define EGL_RECORDABLE_ANDROID 0x3142
#endif

enum fanout_slot_state {
    FANOUT_SLOT_FREE = 0,
    FANOUT_SLOT_COPYING,
    FANOUT_SLOT_READY,
    FANOUT_SLOT_ENCODING,
};

struct fanout_slot {
    GLuint texture;
    GLuint framebuffer;
    enum fanout_slot_state state;
    uint64_t serial;
    int64_t timestamp_ns;
};

struct fanout_state {
    pthread_mutex_t lock;
    pthread_cond_t cond;

    bool enabled;
    bool encoder_stop;
    bool encoder_thread_joinable;
    bool encoder_init_done;
    bool encoder_init_ok;
    bool encoder_failed;
    pthread_t encoder_thread;

    ANativeWindow *encoder_window;
    EGLDisplay display;
    EGLConfig config;
    EGLContext source_context;
    EGLContext encoder_context;
    EGLSurface source_surface;
    EGLSurface encoder_surface;

    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;
    PFNEGLPRESENTATIONTIMEANDROIDPROC presentation_time;

    GLuint program;
    GLint position_attr;
    GLint texcoord_attr;
    GLint sampler_uniform;
    struct fanout_slot slots[FANOUT_RING_SIZE];
    uint64_t next_serial;

    int visible_width;
    int visible_height;
    int encoded_width;
    int encoded_height;
    int fps;
    int64_t frame_interval_ns;
    int64_t next_copy_ns;
    uint64_t generation;
};

/* Saved JVM reference for event-thread JNI callbacks. Process-global (the JVM is);
 * the per-thread env is attached as needed. The activity callback target is
 * per-instance -> consumer_state.clipboard_obj. */
static JavaVM *g_jvm = NULL;

/* ANativeWindow hidden-API function pointers: loaded once, read-only afterwards, so
 * safe to share across instances. */
static struct anw_api api;
static pthread_once_t api_once = PTHREAD_ONCE_INIT;
static atomic_bool api_available = ATOMIC_VAR_INIT(false);

static void load_anw_api_once(void)
{
    atomic_store_explicit(&api_available, anw_api_load(&api) == 0,
                          memory_order_release);
}

static bool ensure_anw_api(void)
{
    pthread_once(&api_once, load_anw_api_once);
    return atomic_load_explicit(&api_available, memory_order_acquire);
}

static void on_fallback(void *userdata);
static void on_exit_fallback(void *userdata);

struct consumer_state;
static void disconnect_consumer_window(struct consumer_state *s);

struct consumer_state {
    pthread_mutex_t lock;
    /* Serializes ctx replacement/disconnect against JNI input/clipboard writers. */
    pthread_mutex_t ctx_lock;
    ANativeWindow *window;
    int connected_window_api;
    display_ctx *ctx;
    pthread_t render_thread;
    atomic_bool running;
    int connect_cancel_efd;

    // Daemon reconnect request; separate from the display library fallback state.
    atomic_bool need_reconnect;
    /* Mirrors display_ctx fallback callbacks so the render loop can distinguish
     * refresh_done()'s legitimate no-fence -1 from its fallback/error -1. */
    atomic_bool in_fallback;

    int buf_count;
    int dmabuf_fds[MAX_COLLECT_BUFS];
    struct buf_info dmabuf_infos[MAX_COLLECT_BUFS];
    ANativeWindowBuffer *buf_anb[MAX_COLLECT_BUFS];

    int screen_w;
    int screen_h;
    bool remote_mode;
    int remote_display_w;
    int remote_display_h;
    int remote_encoded_w;
    int remote_encoded_h;
    int remote_fps;

    /* Simultaneous local/encoder output. The render thread owns the source EGL
     * context; the encoder thread owns its shared context and window surface. */
    struct fanout_state fanout;

    // Latest display refresh rate (milli-Hz) reported from Java. Read on
    // (re)connect to seed the producer; updated live by nativeSetRefreshRate.
    atomic_uint_least32_t refresh_mhz;

    // Event (output) thread
    pthread_t event_thread;
    atomic_bool event_running;
    /* True while event_thread holds a started-but-not-yet-joined thread. stop only
     * signals (never joins -- on_fallback can run ON the event thread); the join is
     * deferred to the next start_event_thread() (create time), which runs on the
     * render thread and so cannot self-join. */
    bool event_thread_joinable;

    /* Connection config, set from Java via nativeConfigure() and read on each
     * (re)connect in do_connect(). Guarded by cfg_lock. Per-instance. */
    pthread_mutex_t cfg_lock;
    char cfg_socket_path[256];
    bool cfg_use_root;
    char cfg_helper_path[512];
    char cfg_bridge_path[512];
    int  cfg_custom_width;
    int  cfg_custom_height;
    bool cfg_topapp_enable;
    char cfg_topapp_path[512];
    int cfg_topapp_mode;
    char cfg_topapp_stops[192];
    /* Snapshot used by the helper lifecycle after do_connect() drops cfg_lock. */
    bool topapp_run_enable;
    char topapp_run_path[512];
    int topapp_run_mode;
    char topapp_run_stops[192];

    /* Foreground-scheduling state, serialized by topapp_lock. topapp_root_pid
     * is the HOST pid of the producer's session tree root (find_tree_root via
     * UNIX_DIAG): mode 2 promotes its whole tree, mode 1 uses it both as the
     * namespace anchor for the O(1) "set" switches and as the restore root --
     * on disconnect/fallback the whole tree goes back to the root cgroups in
     * one sweep, covering every pid mode 1 boosted. Written by the topapp_*
     * helpers / the event thread's scheduling handler from arbitrary
     * threads. */
    pthread_mutex_t topapp_lock;
    pid_t topapp_root_pid;

    /* Pointer-motion delta tracking (per-instance). */
    bool  motion_has_last;
    float motion_last_x, motion_last_y;

    /* Clipboard callback target: the Java object whose nativeSetClipboardBytes /
     * nativeClipListening / nativeClipboardSync the event thread calls (per-instance). */
    jobject clipboard_obj;

    /* Owning MainActivity: on_fallback() calls its onFallback() when the display lib
     * drops the connection, so Java can probe the daemon socket and close the window
     * if the daemon is gone (per-instance global ref). */
    jobject activity_obj;

    /* Per-instance audio bridge (own AAudio streams, own producer). */
    audio_bridge *audio;

    /* Per-instance camera service registration; userdata points back at this state
     * so the camera layer can tell instances apart (see camera_service.c). */
    struct service_info camera_svc;
};

static void topapp_handle_scheduling_event(struct consumer_state *s,
                                           const struct OutputEvent *event);
static void clear_java_targets(JNIEnv *env, struct consumer_state *s)
{
    if (!env || !s)
        return;
    if (s->clipboard_obj) {
        (*env)->DeleteGlobalRef(env, s->clipboard_obj);
        s->clipboard_obj = NULL;
    }
    if (s->activity_obj) {
        (*env)->DeleteGlobalRef(env, s->activity_obj);
        s->activity_obj = NULL;
    }
}

static bool extension_has_token(const char *extensions, const char *name)
{
    if (!extensions || !name || name[0] == '\0' || strchr(name, ' '))
        return false;
    size_t length = strlen(name);
    const char *at = extensions;
    while ((at = strstr(at, name)) != NULL) {
        bool starts_token = at == extensions || at[-1] == ' ';
        bool ends_token = at[length] == '\0' || at[length] == ' ';
        if (starts_token && ends_token)
            return true;
        at += length;
    }
    return false;
}

static int64_t monotonic_time_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

/* sync_file fds signal POLLIN through poll. On success the fd is closed and reset.
 * On failure it remains owned by the caller so cancelBuffer can take the still-valid
 * fence. Runtime producer/acquire waits have no deadline; only startup preflight uses
 * a bounded timeout so Java can cleanly choose remote-direct. */
static bool wait_fence(int *fence_fd, const char *which, atomic_bool *running,
                       int timeout_ms)
{
    if (!fence_fd || *fence_fd < 0)
        return true;

    struct pollfd pfd = { .fd = *fence_fd, .events = POLLIN };
    int waited_ms = 0;
    while (!running || atomic_load_explicit(running, memory_order_acquire)) {
        int poll_ms = timeout_ms >= 0 && timeout_ms - waited_ms < 100
                    ? timeout_ms - waited_ms : 100;
        if (poll_ms < 0)
            poll_ms = 0;
        int result = poll(&pfd, 1, poll_ms);
        if (result < 0 && errno == EINTR)
            continue;
        if (result == 0) {
            if (timeout_ms >= 0) {
                waited_ms += poll_ms;
                if (waited_ms >= timeout_ms) {
                    LOGE("%s fence wait timed out after %d ms", which, timeout_ms);
                    return false;
                }
            }
            continue;
        }

        bool signaled = result > 0 && (pfd.revents & POLLIN) &&
                        !(pfd.revents & (POLLERR | POLLNVAL));
        if (!signaled) {
            LOGE("%s fence wait failed: result=%d revents=0x%x errno=%s",
                 which, result, pfd.revents, strerror(errno));
            return false;
        }
        close(*fence_fd);
        *fence_fd = -1;
        return true;
    }

    LOGI("%s fence wait interrupted by stop", which);
    return false;
}

static GLuint fanout_compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    if (!shader)
        return 0;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        char log[512] = {0};
        glGetShaderInfoLog(shader, sizeof(log) - 1, NULL, log);
        LOGE("fanout shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool fanout_create_gl_resources(struct fanout_state *f)
{
    static const char vertex_source[] =
        "attribute vec2 aPosition;\n"
        "attribute vec2 aTexCoord;\n"
        "varying vec2 vTexCoord;\n"
        "void main() {\n"
        "  gl_Position = vec4(aPosition, 0.0, 1.0);\n"
        "  vTexCoord = aTexCoord;\n"
        "}\n";
    static const char fragment_source[] =
        "precision mediump float;\n"
        "uniform sampler2D uTexture;\n"
        "varying vec2 vTexCoord;\n"
        "void main() { gl_FragColor = texture2D(uTexture, vTexCoord); }\n";

    const char *gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (!extension_has_token(gl_extensions, "GL_OES_EGL_image")) {
        LOGE("fanout unavailable: GL_OES_EGL_image missing");
        return false;
    }

    GLint max_texture_size = 0;
    GLint max_viewport[2] = {0, 0};
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, max_viewport);
    if (f->visible_width > max_texture_size || f->visible_height > max_texture_size ||
        f->encoded_width > max_viewport[0] || f->encoded_height > max_viewport[1]) {
        LOGE("fanout unavailable: %dx%d/%dx%d exceeds texture=%d viewport=%dx%d",
             f->visible_width, f->visible_height, f->encoded_width, f->encoded_height,
             max_texture_size, max_viewport[0], max_viewport[1]);
        return false;
    }

    GLuint vertex = fanout_compile_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = fanout_compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex || !fragment) {
        if (vertex)
            glDeleteShader(vertex);
        if (fragment)
            glDeleteShader(fragment);
        return false;
    }

    f->program = glCreateProgram();
    glAttachShader(f->program, vertex);
    glAttachShader(f->program, fragment);
    glLinkProgram(f->program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(f->program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[512] = {0};
        glGetProgramInfoLog(f->program, sizeof(log) - 1, NULL, log);
        LOGE("fanout shader link failed: %s", log);
        return false;
    }

    f->position_attr = glGetAttribLocation(f->program, "aPosition");
    f->texcoord_attr = glGetAttribLocation(f->program, "aTexCoord");
    f->sampler_uniform = glGetUniformLocation(f->program, "uTexture");
    if (f->position_attr < 0 || f->texcoord_attr < 0 || f->sampler_uniform < 0) {
        LOGE("fanout shader locations unavailable");
        return false;
    }

    for (int i = 0; i < FANOUT_RING_SIZE; i++) {
        struct fanout_slot *slot = &f->slots[i];
        glGenTextures(1, &slot->texture);
        glBindTexture(GL_TEXTURE_2D, slot->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, f->visible_width, f->visible_height,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

        glGenFramebuffers(1, &slot->framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, slot->framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               slot->texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("fanout ring framebuffer %d is incomplete", i);
            return false;
        }
        slot->state = FANOUT_SLOT_FREE;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        LOGE("fanout GL resource setup failed: 0x%x", error);
        return false;
    }
    return true;
}

static void fanout_set_quad(struct fanout_state *f, bool native_buffer_source)
{
    static const GLfloat positions[] = {
        -1.0f, -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f,  1.0f,  1.0f,
    };
    /* Android native buffers are top-left-oriented when imported as EGLImages. Flip
     * once while copying into the ordinary GL texture ring. Ring-to-codec rendering
     * then uses the normal bottom-left GL texture orientation. */
    static const GLfloat native_texcoords[] = {
        0.0f, 1.0f,  1.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,
    };
    static const GLfloat ring_texcoords[] = {
        0.0f, 0.0f,  1.0f, 0.0f,
        0.0f, 1.0f,  1.0f, 1.0f,
    };

    glUseProgram(f->program);
    glVertexAttribPointer((GLuint)f->position_attr, 2, GL_FLOAT, GL_FALSE, 0, positions);
    glEnableVertexAttribArray((GLuint)f->position_attr);
    glVertexAttribPointer((GLuint)f->texcoord_attr, 2, GL_FLOAT, GL_FALSE, 0,
                          native_buffer_source ? native_texcoords : ring_texcoords);
    glEnableVertexAttribArray((GLuint)f->texcoord_attr);
    glUniform1i(f->sampler_uniform, 0);
}

static bool fanout_copy_anb_to_framebuffer(struct fanout_state *f,
                                           ANativeWindowBuffer *anb,
                                           GLuint framebuffer)
{
    while (glGetError() != GL_NO_ERROR) {
    }

    const EGLint image_attributes[] = {
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
        EGL_NONE,
    };
    EGLImageKHR image = f->create_image(f->display, EGL_NO_CONTEXT,
                                        EGL_NATIVE_BUFFER_ANDROID,
                                        (EGLClientBuffer)anb, image_attributes);
    if (image == EGL_NO_IMAGE_KHR) {
        LOGE("fanout source: native buffer import failed: 0x%x", eglGetError());
        return false;
    }

    GLuint source_texture = 0;
    glGenTextures(1, &source_texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    f->image_target_texture(GL_TEXTURE_2D, (GLeglImageOES)image);

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, f->visible_width, f->visible_height);
    glDisable(GL_BLEND);
    fanout_set_quad(f, true);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    /* This is also the source-buffer lifetime barrier: do not destroy the image,
     * publish the ring slot, or release the dequeued ANB before the read completes. */
    glFinish();
    bool copied = glGetError() == GL_NO_ERROR;
    if (!copied)
        LOGE("fanout source copy failed with a GL error");

    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &source_texture);
    f->destroy_image(f->display, image);
    return copied;
}

static bool fanout_preflight_source(struct consumer_state *s)
{
    struct fanout_state *f = &s->fanout;
    ANativeWindowBuffer *anb = NULL;
    int fence = -1;
    bool copied = false;

    disconnect_consumer_window(s);
    if (anw_api_connect(s->window, ANW_API_EGL) != 0) {
        LOGE("fanout unavailable: local window EGL API connection failed");
        return false;
    }
    s->connected_window_api = ANW_API_EGL;

    if (ANativeWindow_setBuffersGeometry(s->window, f->visible_width, f->visible_height,
                                         AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM) != 0) {
        LOGE("fanout unavailable: local window geometry setup failed");
        goto done;
    }

    int min_undequeued = 0;
    if (api.query(s->window, ANATIVEWINDOW_QUERY_MIN_UNDEQUEUED_BUFFERS,
                  &min_undequeued) != 0 || min_undequeued < 0 ||
        min_undequeued + 2 > MAX_COLLECT_BUFS ||
        api.setBufferCount(s->window, (size_t)(min_undequeued + 2)) != 0) {
        LOGE("fanout unavailable: local window buffer-count setup failed (min=%d)",
             min_undequeued);
        goto done;
    }

    if (api.dequeueBuffer(s->window, &anb, &fence) != 0 || !anb) {
        LOGE("fanout unavailable: local buffer preflight dequeue failed");
        if (fence >= 0)
            close(fence);
        fence = -1;
        goto done;
    }
    if (!wait_fence(&fence, "fanout preflight acquire", NULL, 2000))
        goto done;

    copied = fanout_copy_anb_to_framebuffer(f, anb, f->slots[0].framebuffer);

done:
    if (anb)
        api.cancelBuffer(s->window, anb, fence);
    else if (fence >= 0)
        close(fence);
    disconnect_consumer_window(s);
    if (!copied)
        LOGE("fanout unavailable: local native-buffer import/copy preflight failed");
    return copied;
}

static void notify_fanout_failure(struct consumer_state *s, uint64_t generation)
{
    if (!g_jvm || !s->activity_obj || generation == 0)
        return;

    JNIEnv *env = NULL;
    bool attached = false;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) == 0)
            attached = true;
    }
    if (env) {
        jclass cls = (*env)->GetObjectClass(env, s->activity_obj);
        jmethodID method = cls ? (*env)->GetMethodID(
            env, cls, "onNativeFanoutFailed", "(J)V") : NULL;
        if (method) {
            (*env)->CallVoidMethod(env, s->activity_obj, method, (jlong)generation);
        } else if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        if (cls)
            (*env)->DeleteLocalRef(env, cls);
    }
    if (attached)
        (*g_jvm)->DetachCurrentThread(g_jvm);
}

static void *fanout_encoder_thread_func(void *arg)
{
    struct consumer_state *s = arg;
    struct fanout_state *f = &s->fanout;
    bool current = eglMakeCurrent(f->display, f->encoder_surface, f->encoder_surface,
                                  f->encoder_context) == EGL_TRUE;

    bool shared_resources = false;
    if (current) {
        /* Texture sharing is the core cross-context requirement; validate it on the
         * context that will consume the ring before nativeStartFanout can return true. */
        while (glGetError() != GL_NO_ERROR) {
        }
        shared_resources = glIsTexture(f->slots[0].texture) == GL_TRUE &&
                           glGetError() == GL_NO_ERROR;
    }

    pthread_mutex_lock(&f->lock);
    f->encoder_init_ok = current && shared_resources;
    f->encoder_init_done = true;
    if (!f->encoder_init_ok)
        f->encoder_failed = true;
    pthread_cond_broadcast(&f->cond);
    pthread_mutex_unlock(&f->lock);

    if (!current) {
        LOGE("fanout encoder: eglMakeCurrent failed: 0x%x", eglGetError());
        return NULL;
    }
    if (!shared_resources) {
        LOGE("fanout encoder: shared texture validation failed");
        eglMakeCurrent(f->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return NULL;
    }

    LOGI("fanout encoder thread started");
    while (true) {
        pthread_mutex_lock(&f->lock);
        int newest = -1;
        while (!f->encoder_stop) {
            uint64_t newest_serial = 0;
            for (int i = 0; i < FANOUT_RING_SIZE; i++) {
                if (f->slots[i].state == FANOUT_SLOT_READY &&
                    (newest < 0 || f->slots[i].serial > newest_serial)) {
                    newest = i;
                    newest_serial = f->slots[i].serial;
                }
            }
            if (newest >= 0)
                break;
            pthread_cond_wait(&f->cond, &f->lock);
        }
        if (f->encoder_stop) {
            pthread_mutex_unlock(&f->lock);
            break;
        }

        /* Consume only the newest complete copy. Older ready frames have no value to
         * the real-time encoder and freeing them keeps the ring bounded/nonblocking. */
        for (int i = 0; i < FANOUT_RING_SIZE; i++) {
            if (i != newest && f->slots[i].state == FANOUT_SLOT_READY)
                f->slots[i].state = FANOUT_SLOT_FREE;
        }
        f->slots[newest].state = FANOUT_SLOT_ENCODING;
        GLuint texture = f->slots[newest].texture;
        int64_t timestamp_ns = f->slots[newest].timestamp_ns;
        pthread_mutex_unlock(&f->lock);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_BLEND);
        glViewport(0, 0, f->encoded_width, f->encoded_height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        /* Put the visible image at the conventional top-left of the aligned codec
         * buffer; any right/bottom alignment padding remains opaque black. */
        glViewport(0, f->encoded_height - f->visible_height,
                   f->visible_width, f->visible_height);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        fanout_set_quad(f, false);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish();

        pthread_mutex_lock(&f->lock);
        bool stop_before_swap = f->encoder_stop;
        pthread_mutex_unlock(&f->lock);
        bool frame_ok = stop_before_swap ||
                        (glGetError() == GL_NO_ERROR &&
                         f->presentation_time(f->display, f->encoder_surface,
                                              timestamp_ns) == EGL_TRUE &&
                         eglSwapBuffers(f->display, f->encoder_surface) == EGL_TRUE);

        pthread_mutex_lock(&f->lock);
        f->slots[newest].state = FANOUT_SLOT_FREE;
        if (!frame_ok) {
            f->encoder_failed = true;
            f->encoder_stop = true;
        }
        pthread_cond_broadcast(&f->cond);
        pthread_mutex_unlock(&f->lock);

        if (!frame_ok) {
            LOGE("fanout encoder render/swap failed: egl=0x%x", eglGetError());
            break;
        }
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    eglMakeCurrent(f->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    pthread_mutex_lock(&f->lock);
    bool failed = f->encoder_failed;
    uint64_t generation = f->generation;
    pthread_mutex_unlock(&f->lock);
    if (failed)
        notify_fanout_failure(s, generation);
    LOGI("fanout encoder thread stopped");
    return NULL;
}

static void fanout_delete_gl_resources(struct fanout_state *f)
{
    for (int i = 0; i < FANOUT_RING_SIZE; i++) {
        if (f->slots[i].framebuffer)
            glDeleteFramebuffers(1, &f->slots[i].framebuffer);
        if (f->slots[i].texture)
            glDeleteTextures(1, &f->slots[i].texture);
        f->slots[i].framebuffer = 0;
        f->slots[i].texture = 0;
        f->slots[i].state = FANOUT_SLOT_FREE;
    }
    if (f->program)
        glDeleteProgram(f->program);
    f->program = 0;
}

static void fanout_request_stop(struct consumer_state *s)
{
    struct fanout_state *f = &s->fanout;
    pthread_mutex_lock(&f->lock);
    f->encoder_stop = true;
    pthread_cond_broadcast(&f->cond);
    pthread_mutex_unlock(&f->lock);
}

static void fanout_shutdown(struct consumer_state *s)
{
    struct fanout_state *f = &s->fanout;

    fanout_request_stop(s);
    pthread_mutex_lock(&f->lock);
    bool joinable = f->encoder_thread_joinable;
    pthread_mutex_unlock(&f->lock);

    if (joinable) {
        pthread_join(f->encoder_thread, NULL);
        pthread_mutex_lock(&f->lock);
        f->encoder_thread_joinable = false;
        pthread_mutex_unlock(&f->lock);
    }

    EGLDisplay previous_display = eglGetCurrentDisplay();
    EGLContext previous_context = eglGetCurrentContext();
    EGLSurface previous_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface previous_read = eglGetCurrentSurface(EGL_READ);

    if (f->display != EGL_NO_DISPLAY && f->source_context != EGL_NO_CONTEXT &&
        f->source_surface != EGL_NO_SURFACE &&
        eglMakeCurrent(f->display, f->source_surface, f->source_surface,
                       f->source_context) == EGL_TRUE) {
        fanout_delete_gl_resources(f);
        glFinish();
    }

    if (previous_display != EGL_NO_DISPLAY && previous_context != EGL_NO_CONTEXT) {
        eglMakeCurrent(previous_display, previous_draw, previous_read, previous_context);
    } else if (f->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(f->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    if (f->display != EGL_NO_DISPLAY) {
        if (f->encoder_surface != EGL_NO_SURFACE)
            eglDestroySurface(f->display, f->encoder_surface);
        if (f->source_surface != EGL_NO_SURFACE)
            eglDestroySurface(f->display, f->source_surface);
        if (f->encoder_context != EGL_NO_CONTEXT)
            eglDestroyContext(f->display, f->encoder_context);
        if (f->source_context != EGL_NO_CONTEXT)
            eglDestroyContext(f->display, f->source_context);
        /* EGLDisplay is process-global. Do not eglTerminate it here: another local
         * window/session may be using the same display. */
    }
    if (f->encoder_window)
        ANativeWindow_release(f->encoder_window);

    pthread_mutex_lock(&f->lock);
    f->enabled = false;
    f->encoder_stop = false;
    f->encoder_init_done = false;
    f->encoder_init_ok = false;
    f->encoder_failed = false;
    f->encoder_window = NULL;
    f->display = EGL_NO_DISPLAY;
    f->config = NULL;
    f->source_context = EGL_NO_CONTEXT;
    f->encoder_context = EGL_NO_CONTEXT;
    f->source_surface = EGL_NO_SURFACE;
    f->encoder_surface = EGL_NO_SURFACE;
    f->create_image = NULL;
    f->destroy_image = NULL;
    f->image_target_texture = NULL;
    f->presentation_time = NULL;
    f->next_serial = 0;
    f->next_copy_ns = 0;
    f->generation = 0;
    pthread_mutex_unlock(&f->lock);
}

static bool fanout_init(struct consumer_state *s, JNIEnv *env, jobject encoder_surface,
                        int visible_width, int visible_height,
                        int encoded_width, int encoded_height, int fps,
                        uint64_t generation)
{
    struct fanout_state *f = &s->fanout;
    if (!encoder_surface || visible_width <= 0 || visible_height <= 0 ||
        encoded_width < visible_width || encoded_height < visible_height || fps <= 0) {
        LOGE("fanout unavailable: invalid surface or geometry %dx%d -> %dx%d @ %d",
             visible_width, visible_height, encoded_width, encoded_height, fps);
        return false;
    }

    f->visible_width = visible_width;
    f->visible_height = visible_height;
    f->encoded_width = encoded_width;
    f->encoded_height = encoded_height;
    f->fps = fps;
    f->frame_interval_ns = 1000000000LL / fps;
    f->generation = generation;
    f->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (f->display == EGL_NO_DISPLAY || !eglInitialize(f->display, NULL, NULL)) {
        LOGE("fanout unavailable: EGL display initialization failed: 0x%x", eglGetError());
        f->display = EGL_NO_DISPLAY;
        return false;
    }

    const char *egl_extensions = eglQueryString(f->display, EGL_EXTENSIONS);
    bool extensions_ok =
        extension_has_token(egl_extensions, "EGL_KHR_image_base") &&
        extension_has_token(egl_extensions, "EGL_ANDROID_image_native_buffer") &&
        extension_has_token(egl_extensions, "EGL_ANDROID_presentation_time") &&
        extension_has_token(egl_extensions, "EGL_ANDROID_recordable");
    f->create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    f->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    f->image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    f->presentation_time = (PFNEGLPRESENTATIONTIMEANDROIDPROC)
        eglGetProcAddress("eglPresentationTimeANDROID");
    if (!extensions_ok || !f->create_image || !f->destroy_image ||
        !f->image_target_texture || !f->presentation_time) {
        LOGE("fanout unavailable: required EGL image/presentation extensions missing");
        fanout_shutdown(s);
        return false;
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        LOGE("fanout unavailable: eglBindAPI failed: 0x%x", eglGetError());
        fanout_shutdown(s);
        return false;
    }

    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RECORDABLE_ANDROID, EGL_TRUE,
        EGL_NONE,
    };
    EGLint config_count = 0;
    if (!eglChooseConfig(f->display, config_attributes, &f->config, 1, &config_count) ||
        config_count != 1) {
        LOGE("fanout unavailable: no recordable GLES2 EGL config");
        fanout_shutdown(s);
        return false;
    }

    const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    f->source_context = eglCreateContext(f->display, f->config, EGL_NO_CONTEXT,
                                         context_attributes);
    if (f->source_context == EGL_NO_CONTEXT) {
        LOGE("fanout unavailable: source context creation failed: 0x%x", eglGetError());
        fanout_shutdown(s);
        return false;
    }
    f->encoder_context = eglCreateContext(f->display, f->config, f->source_context,
                                          context_attributes);
    if (f->encoder_context == EGL_NO_CONTEXT) {
        LOGE("fanout unavailable: shared encoder context creation failed: 0x%x", eglGetError());
        fanout_shutdown(s);
        return false;
    }

    const EGLint pbuffer_attributes[] = {
        EGL_WIDTH, 1,
        EGL_HEIGHT, 1,
        EGL_NONE,
    };
    f->source_surface = eglCreatePbufferSurface(f->display, f->config, pbuffer_attributes);
    if (f->source_surface == EGL_NO_SURFACE) {
        LOGE("fanout unavailable: source pbuffer creation failed: 0x%x", eglGetError());
        fanout_shutdown(s);
        return false;
    }

    f->encoder_window = ANativeWindow_fromSurface(env, encoder_surface);
    if (!f->encoder_window) {
        LOGE("fanout unavailable: encoder ANativeWindow creation failed");
        fanout_shutdown(s);
        return false;
    }
    f->encoder_surface = eglCreateWindowSurface(f->display, f->config,
                                                f->encoder_window, NULL);
    if (f->encoder_surface == EGL_NO_SURFACE) {
        LOGE("fanout unavailable: encoder EGLSurface creation failed: 0x%x", eglGetError());
        fanout_shutdown(s);
        return false;
    }
    EGLint surface_width = 0;
    EGLint surface_height = 0;
    if (!eglQuerySurface(f->display, f->encoder_surface, EGL_WIDTH, &surface_width) ||
        !eglQuerySurface(f->display, f->encoder_surface, EGL_HEIGHT, &surface_height) ||
        surface_width != encoded_width || surface_height != encoded_height) {
        LOGE("fanout unavailable: encoder surface is %dx%d, expected %dx%d",
             surface_width, surface_height, encoded_width, encoded_height);
        fanout_shutdown(s);
        return false;
    }

    EGLDisplay previous_display = eglGetCurrentDisplay();
    EGLContext previous_context = eglGetCurrentContext();
    EGLSurface previous_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface previous_read = eglGetCurrentSurface(EGL_READ);
    if (!eglMakeCurrent(f->display, f->source_surface, f->source_surface,
                        f->source_context) || !fanout_create_gl_resources(f) ||
        !fanout_preflight_source(s)) {
        LOGE("fanout unavailable: source GL/import initialization failed: 0x%x",
             eglGetError());
        if (previous_display != EGL_NO_DISPLAY && previous_context != EGL_NO_CONTEXT)
            eglMakeCurrent(previous_display, previous_draw, previous_read, previous_context);
        else
            eglMakeCurrent(f->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        fanout_shutdown(s);
        return false;
    }
    glFinish();
    if (previous_display != EGL_NO_DISPLAY && previous_context != EGL_NO_CONTEXT)
        eglMakeCurrent(previous_display, previous_draw, previous_read, previous_context);
    else
        eglMakeCurrent(f->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    pthread_mutex_lock(&f->lock);
    f->enabled = true;
    f->encoder_stop = false;
    f->encoder_init_done = false;
    f->encoder_init_ok = false;
    f->encoder_failed = false;
    pthread_mutex_unlock(&f->lock);

    if (pthread_create(&f->encoder_thread, NULL, fanout_encoder_thread_func, s) != 0) {
        LOGE("fanout unavailable: encoder thread creation failed");
        fanout_shutdown(s);
        return false;
    }
    pthread_mutex_lock(&f->lock);
    f->encoder_thread_joinable = true;
    while (!f->encoder_init_done)
        pthread_cond_wait(&f->cond, &f->lock);
    bool encoder_ready = f->encoder_init_ok;
    pthread_mutex_unlock(&f->lock);
    if (!encoder_ready) {
        fanout_shutdown(s);
        return false;
    }

    LOGI("fanout initialized: visible=%dx%d encoded=%dx%d fps=%d ring=%d",
         visible_width, visible_height, encoded_width, encoded_height, fps,
         FANOUT_RING_SIZE);
    return true;
}

static bool fanout_copy_source(struct consumer_state *s, ANativeWindowBuffer *anb,
                               int64_t timestamp_ns)
{
    struct fanout_state *f = &s->fanout;
    int slot_index = -1;

    pthread_mutex_lock(&f->lock);
    if (!f->enabled || f->encoder_stop || f->encoder_failed ||
        timestamp_ns < f->next_copy_ns) {
        pthread_mutex_unlock(&f->lock);
        return false;
    }
    f->next_copy_ns = timestamp_ns + f->frame_interval_ns;
    for (int i = 0; i < FANOUT_RING_SIZE; i++) {
        if (f->slots[i].state == FANOUT_SLOT_FREE) {
            slot_index = i;
            f->slots[i].state = FANOUT_SLOT_COPYING;
            break;
        }
    }
    pthread_mutex_unlock(&f->lock);

    /* Ring full: intentionally drop the remote copy without waiting for MediaCodec. */
    if (slot_index < 0)
        return false;

    bool copied = false;
    if (eglGetCurrentContext() != f->source_context &&
        eglMakeCurrent(f->display, f->source_surface, f->source_surface,
                       f->source_context) != EGL_TRUE) {
        LOGE("fanout source: eglMakeCurrent failed: 0x%x", eglGetError());
    } else {
        copied = fanout_copy_anb_to_framebuffer(f, anb,
                                                f->slots[slot_index].framebuffer);
    }

    pthread_mutex_lock(&f->lock);
    if (copied) {
        f->slots[slot_index].serial = ++f->next_serial;
        f->slots[slot_index].timestamp_ns = timestamp_ns;
        f->slots[slot_index].state = FANOUT_SLOT_READY;
        pthread_cond_signal(&f->cond);
    } else {
        f->slots[slot_index].state = FANOUT_SLOT_FREE;
        f->encoder_failed = true;
        f->encoder_stop = true;
        pthread_cond_broadcast(&f->cond);
    }
    pthread_mutex_unlock(&f->lock);
    return copied;
}

static void disconnect_consumer_window(struct consumer_state *s)
{
    if (s->window && s->connected_window_api != 0) {
        anw_api_disconnect(s->window, s->connected_window_api);
        s->connected_window_api = 0;
    }
}

static bool queue_consumer_buffer(struct consumer_state *s,
                                  ANativeWindowBuffer *anb, int fence_fd)
{
    if (api.queueBuffer(s->window, anb, fence_fd) == 0)
        return true;

    /* queueBuffer owns fence_fd once called. If the buffer is still dequeued after
     * the error, cancel it without reusing that fd; if it was already returned, the
     * cancel simply fails harmlessly. */
    LOGE("queueBuffer failed; cancelling dequeued buffer");
    api.cancelBuffer(s->window, anb, -1);
    return false;
}

static int collect_dmabufs(struct consumer_state *s)
{
    ANativeWindow *win = s->window;
    int target = s->buf_count;
    int found = 0;

    LOGI("collecting %d dma-bufs via dequeue/queue", target);

    for (int attempt = 0; attempt < target * 4 && found < target; attempt++) {
        ANativeWindowBuffer *anb = NULL;
        int fence = -1;
        if (api.dequeueBuffer(win, &anb, &fence) != 0 || !anb) {
            LOGE("dequeueBuffer failed on attempt %d", attempt);
            if (fence >= 0)
                close(fence);
            break;
        }
        /* Enumeration never touches the pixels, so return the acquire fence to the
         * queue rather than waiting or discarding the dependency. */
        int queue_fence = fence;

        if (!anb->handle || anb->handle->numFds < 1) {
            LOGE("dequeued buffer has no dma-buf handle on attempt %d", attempt);
            api.cancelBuffer(win, anb, queue_fence);
            continue;
        }

        int fd = anb->handle->data[0];   /* first fd backs the dma-buf */
        int stride = anb->stride, width = anb->width, height = anb->height;

        /* deduplicate by ANativeWindowBuffer pointer (stable per queue slot) */
        bool dup_found = false;
        for (int i = 0; i < found; i++) {
            if (s->buf_anb[i] == anb) {
                dup_found = true;
                break;
            }
        }

        int dup_fd = dup_found ? -1 : dup(fd);
        /* Post it back so the next dequeue rotates to another slot. queueBuffer takes
         * ownership of queue_fence on both success and BufferQueue error paths. */
        queue_consumer_buffer(s, anb, queue_fence);

        if (dup_found)
            continue;
        if (dup_fd < 0)
            continue;

        s->buf_anb[found] = anb;
        s->dmabuf_fds[found] = dup_fd;
        s->dmabuf_infos[found].stride = stride * 4;
        s->dmabuf_infos[found].width  = width;
        s->dmabuf_infos[found].height = height;
        s->dmabuf_infos[found].format = PIXEL_FORMAT_RGBA_8888;
        s->dmabuf_infos[found].modifier = 0;
        s->dmabuf_infos[found].offset = 0;
        LOGI("  buf[%d]: anb=%p fd=%d dup=%d %dx%d stride=%d",
             found, (void *)anb, fd, dup_fd, width, height, stride);
        found++;
    }

    if (found < target) {
        LOGE("only collected %d/%d", found, target);
        for (int i = 0; i < found; i++) {
            close(s->dmabuf_fds[i]);
            s->dmabuf_fds[i] = -1;
        }
        return -1;
    }

    s->buf_count = found;
    LOGI("collected %d dma-bufs", found);
    return 0;
}

static void cleanup_dmabufs(struct consumer_state *s)
{
    for (int i = 0; i < s->buf_count; i++) {
        if (s->dmabuf_fds[i] >= 0) {
            close(s->dmabuf_fds[i]);
            s->dmabuf_fds[i] = -1;
        }
    }
    s->buf_count = 0;
}

/* Report the current display refresh rate to the producer over the data
 * channel, reusing the InputEvent framing (see INPUT_TYPE_DISPLAY_REFRESH).
 * No-op when disconnected or rate unknown. */
static int push_event_locked(struct consumer_state *s, const struct InputEvent *event)
{
    int result = 0;
    pthread_mutex_lock(&s->ctx_lock);
    if (s->ctx)
        result = push_input_event(s->ctx, event);
    pthread_mutex_unlock(&s->ctx_lock);
    return result;
}

static int push_event_with_length_locked(struct consumer_state *s,
                                         const struct InputEvent *event,
                                         void *payload, size_t size)
{
    int result = 0;
    pthread_mutex_lock(&s->ctx_lock);
    if (s->ctx)
        result = push_input_event_with_length(s->ctx, event, payload, size);
    pthread_mutex_unlock(&s->ctx_lock);
    return result;
}

static void send_refresh_rate(struct consumer_state *s)
{
    uint32_t refresh = atomic_load_explicit(&s->refresh_mhz, memory_order_acquire);
    pthread_mutex_lock(&s->ctx_lock);
    if (s->ctx && refresh != 0) {
        struct InputEvent ev = {
            .type = INPUT_TYPE_DISPLAY_REFRESH,
            .display = { .refresh_mhz = refresh },
        };
        push_input_event(s->ctx, &ev);
    }
    pthread_mutex_unlock(&s->ctx_lock);
}

/*
 * Event thread: listens for output events (clipboard, etc.) from the producer
 * on the data_fd. Runs while s->event_running is true.
 */
static void *event_thread_func(void *arg)
{
    struct consumer_state *s = arg;
    LOGI("event thread started");

    JNIEnv *env = NULL;
    if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != 0) {
        LOGE("event thread: AttachCurrentThread failed");
        return NULL;
    }

    /* Keep the payload as ordinary UTF-8 bytes across JNI. Java performs strict
     * decoding (REPORT), size/NUL validation, and constructs the UTF-16 String. */
    jclass ctxClass = (*env)->GetObjectClass(env, s->clipboard_obj);
    jmethodID setClipMethod = (*env)->GetMethodID(
        env, ctxClass, "nativeSetClipboardBytes", "([B)V");
    (*env)->DeleteLocalRef(env, ctxClass);
    if (!setClipMethod) {
        LOGE("event thread: nativeSetClipboardBytes not found");
        (*g_jvm)->DetachCurrentThread(g_jvm);
        return NULL;
    }

    /* CONSUMER_VAR_* callbacks land on the owning MainActivity (var, value). */
    jmethodID setVarMethod = NULL;
    if (s->activity_obj) {
        jclass actClass = (*env)->GetObjectClass(env, s->activity_obj);
        setVarMethod = (*env)->GetMethodID(env, actClass, "nativeSetConsumerVar", "(II)V");
        (*env)->DeleteLocalRef(env, actClass);
        if (!setVarMethod)
            LOGE("event thread: nativeSetConsumerVar not found");
    }

    while (atomic_load_explicit(&s->event_running, memory_order_acquire)) {
        if (!s->ctx) {
            usleep(50000);
            continue;
        }

        struct OutputEvent ev;
        TracyCZoneN(zPoll, "poll_output_event", 1);
        int ret = poll_output_event(s->ctx, &ev, 500);
        TracyCZoneEnd(zPoll);
        if (ret <= 0)
            continue;

        if (ev.type == OUTPUT_TYPE_RESOURCES_REQUEST) {
            /* Producer is asking for a service's fds (e.g. camera). The display lib
             * matches the type against the registered services and sends the
             * pre-created fds back over SCM_RIGHTS. */
            handle_resource_request(s->ctx, &ev);
        } else if (ev.type == OUTPUT_TYPE_CLIPBOARD) {
            uint32_t size = ev.clipboard.size;
            if (size > 1024u * 1024u) {
                LOGE("event thread: rejecting oversized clipboard (%u bytes)", size);
                display_consumer_fail_transport(s->ctx);
                continue;
            }
            jbyteArray bytes = (*env)->NewByteArray(env, (jsize)size);
            if (!bytes)
                continue;

            bool received = true;
            if (size > 0) {
                char *buf = malloc(size);
                if (!buf) {
                    (*env)->DeleteLocalRef(env, bytes);
                    continue;
                }
                received = poll_output_event_extend_data(s->ctx, buf, size, 5000) == 1;
                if (received)
                    (*env)->SetByteArrayRegion(env, bytes, 0, (jsize)size, (jbyte *)buf);
                free(buf);
            }
            if (received)
                (*env)->CallVoidMethod(env, s->clipboard_obj, setClipMethod, bytes);
            (*env)->DeleteLocalRef(env, bytes);
        } else if (ev.type == OUTPUT_TYPE_SET_CONSUMER_VAR) {
            /* Producer asserts a transient runtime override. CONSUMER_VAR_CAPTURE_MOUSE
             * forces pointer capture on for Wayland pointer lock (games); 0 releases.
             * Forwarded to MainActivity, which marshals to the UI thread. */
            if (setVarMethod && s->activity_obj)
                (*env)->CallVoidMethod(env, s->activity_obj, setVarMethod,
                                       (jint)ev.set_consumer_var.var, (jint)ev.set_consumer_var.value);
        } else if (ev.type == OUTPUT_TYPE_SCHEDULING) {
            topapp_handle_scheduling_event(s, &ev);
        } else {
            LOGE("event thread: rejecting unknown output event type=%u", ev.type);
            display_consumer_fail_transport(s->ctx);
        }
    }

    (*g_jvm)->DetachCurrentThread(g_jvm);
    LOGI("event thread stopped");
    return NULL;
}

static void join_event_thread(struct consumer_state *s)
{
    /* Idempotent. MUST be called only from a non-event thread (render / JNI teardown);
     * never from on_fallback (which may run on the event thread). */
    if (s->event_thread_joinable) {
        pthread_join(s->event_thread, NULL);
        s->event_thread_joinable = false;
    }
}

static void start_event_thread(struct consumer_state *s)
{
    if (atomic_load_explicit(&s->event_running, memory_order_acquire))
        return;
    /* Reap the previous stopped-but-unjoined thread before spawning a new one. Runs on
     * the render thread (on_exit_fallback), so this join can't self-deadlock. */
    join_event_thread(s);
    atomic_store_explicit(&s->event_running, true, memory_order_release);
    if (pthread_create(&s->event_thread, NULL, event_thread_func, s) == 0)
        s->event_thread_joinable = true;
    else
        atomic_store_explicit(&s->event_running, false, memory_order_release);
}

static void stop_event_thread(struct consumer_state *s)
{
    /* Signal only -- do NOT join here. enter_fallback()->on_fallback() can execute on
     * the event thread itself, so joining would self-deadlock. The handle stays in
     * event_thread (event_thread_joinable) and is reaped at create time by the next
     * start_event_thread() (or do_connect's reconnect path, both on the render
     * thread). */
    atomic_store_explicit(&s->event_running, false, memory_order_release);
}

static void signal_connect_cancel(struct consumer_state *s)
{
    if (s && s->connect_cancel_efd >= 0) {
        eventfd_t value = 1;
        if (eventfd_write(s->connect_cancel_efd, value) < 0 && errno != EAGAIN)
            LOGE("connect cancellation signal failed: %s", strerror(errno));
    }
}

static void drain_connect_cancel(struct consumer_state *s)
{
    if (!s || s->connect_cancel_efd < 0)
        return;
    eventfd_t value;
    while (eventfd_read(s->connect_cancel_efd, &value) == 0) {
    }
    if (errno != EAGAIN)
        LOGE("connect cancellation drain failed: %s", strerror(errno));
}

static void detach_and_abort_transport(struct consumer_state *s)
{
    audio_set_ctx(s->audio, NULL);
    pthread_mutex_lock(&s->ctx_lock);
    if (s->ctx)
        display_consumer_abort_io(s->ctx);
    pthread_mutex_unlock(&s->ctx_lock);
}

/* Return the exact byte count (excluding NUL) for POSIX shell single-quoting.
 * A literal apostrophe becomes '\'' (close quote, escaped quote, reopen quote). */
static bool shell_single_quoted_size(const char *arg, size_t *size_out)
{
    if (!arg || !size_out)
        return false;
    size_t size = 2; /* opening and closing apostrophes */
    for (const unsigned char *p = (const unsigned char *)arg; *p; p++) {
        size_t add = *p == '\'' ? 4u : 1u;
        if (size > SIZE_MAX - add)
            return false;
        size += add;
    }
    *size_out = size;
    return true;
}

static bool append_shell_single_quoted(char *dst, size_t capacity,
                                       size_t *length, const char *arg)
{
    size_t needed = 0;
    if (!dst || !length || !shell_single_quoted_size(arg, &needed) ||
        *length >= capacity || needed > capacity - *length - 1)
        return false;

    size_t at = *length;
    dst[at++] = '\'';
    for (const unsigned char *p = (const unsigned char *)arg; *p; p++) {
        if (*p == '\'') {
            static const char escaped_apostrophe[] = "'\\''";
            memcpy(dst + at, escaped_apostrophe, sizeof(escaped_apostrophe) - 1);
            at += sizeof(escaped_apostrophe) - 1;
        } else {
            dst[at++] = (char)*p;
        }
    }
    dst[at++] = '\'';
    dst[at] = '\0';
    *length = at;
    return true;
}

/*
 * "Connect with root" handshake. The app cannot connect() to a root-owned
 * daemon socket directly, so it listens on a bridge socket, launches the bundled
 * helper through `su -c`, and the helper (as root) connects to the daemon and
 * passes the connected fd back over the bridge. Returns the received fd (caller
 * owns it) or -1 on failure.
 */
static int wait_child_bounded(pid_t pid, int timeout_ms)
{
    int status = 0;
    int waited_ms = 0;
    while (waited_ms < timeout_ms) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid)
            return status;
        if (result < 0 && errno != EINTR)
            return -1;
        usleep(10000);
        waited_ms += 10;
    }
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return status;
}

static void terminate_helper(pid_t pid)
{
    kill(-pid, SIGTERM);
    kill(pid, SIGTERM);
}

static bool bridge_wait_cancelled(int cancel_fd, short revents)
{
    return cancel_fd >= 0 &&
           (revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
}

static int receive_helper_fd(int connection_fd, int cancel_fd, int64_t deadline_ns,
                             bool *cancelled)
{
    for (;;) {
        int64_t remaining_ns = deadline_ns - monotonic_time_ns();
        if (remaining_ns <= 0)
            return -1;
        int remaining_ms = (int)((remaining_ns + 999999LL) / 1000000LL);
        struct pollfd pfds[2] = {
            { .fd = connection_fd, .events = POLLIN },
            { .fd = cancel_fd, .events = POLLIN | POLLHUP | POLLERR },
        };
        nfds_t count = cancel_fd >= 0 ? 2 : 1;
        int result = poll(pfds, count, remaining_ms);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (result == 0)
            return -1;
        if (count == 2 && bridge_wait_cancelled(cancel_fd, pfds[1].revents)) {
            *cancelled = true;
            return -1;
        }
        if (pfds[0].revents & (POLLERR | POLLNVAL))
            return -1;
        if (!(pfds[0].revents & POLLIN)) {
            if (pfds[0].revents & POLLHUP)
                return -1;
            continue;
        }

        char marker;
        struct iovec iov = { .iov_base = &marker, .iov_len = 1 };
        union {
            char bytes[CMSG_SPACE(sizeof(int))];
            struct cmsghdr align;
        } control;
        memset(&control, 0, sizeof(control));
        struct msghdr message = {
            .msg_iov = &iov,
            .msg_iovlen = 1,
            .msg_control = control.bytes,
            .msg_controllen = sizeof(control.bytes),
        };
        ssize_t bytes = recvmsg(connection_fd, &message,
                                MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (bytes < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
        int fd = -1;
        if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
            memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
        }
        if (bytes != 1 || (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) || !cmsg ||
            cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(int)) || fd < 0 ||
            fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            if (fd >= 0)
                close(fd);
            return -1;
        }
        return fd;
    }
}

static int recv_fd_via_root_helper_timeout(const char *daemon_sock,
                                           const char *helper_path,
                                           const char *bridge_path,
                                           int cancel_fd, int timeout_ms)
{
    if (!daemon_sock || !helper_path || !bridge_path || daemon_sock[0] == '\0' ||
        helper_path[0] == '\0' || bridge_path[0] == '\0') {
        LOGE("root helper: daemon/helper/bridge path not configured");
        return -1;
    }
    struct sockaddr_un addr;
    size_t bridge_length = strlen(bridge_path);
    if (bridge_length >= sizeof(addr.sun_path)) {
        LOGE("root helper: handoff socket path is too long");
        return -1;
    }

    unlink(bridge_path);

    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) {
        LOGE("root helper: socket() failed: %s", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, bridge_path, bridge_length + 1);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("root helper: bind(%s) failed: %s", bridge_path, strerror(errno));
        close(lfd);
        return -1;
    }
    /* The handoff socket is in the app-private cache directory. Root can traverse
     * it; no other untrusted app needs permission. */
    if (chmod(bridge_path, 0600) < 0) {
        LOGE("root helper: chmod(%s) failed: %s", bridge_path, strerror(errno));
        close(lfd);
        unlink(bridge_path);
        return -1;
    }

    if (listen(lfd, 1) < 0) {
        LOGE("root helper: listen() failed: %s", strerror(errno));
        close(lfd);
        unlink(bridge_path);
        return -1;
    }

    /* su -c necessarily parses one shell command. Quote each argv element exactly;
     * never interpolate configurable paths as raw shell syntax. */
    size_t helper_size = 0;
    size_t daemon_size = 0;
    size_t bridge_size = 0;
    if (!shell_single_quoted_size(helper_path, &helper_size) ||
        !shell_single_quoted_size(daemon_sock, &daemon_size) ||
        !shell_single_quoted_size(bridge_path, &bridge_size) ||
        helper_size > SIZE_MAX - daemon_size ||
        helper_size + daemon_size > SIZE_MAX - bridge_size ||
        helper_size + daemon_size + bridge_size > SIZE_MAX - 3) {
        LOGE("root helper: command arguments are too long");
        close(lfd);
        unlink(bridge_path);
        return -1;
    }
    size_t inner_size = helper_size + daemon_size + bridge_size + 3; /* spaces + NUL */
    char *inner = malloc(inner_size);
    size_t inner_length = 0;
    if (!inner ||
        !append_shell_single_quoted(inner, inner_size, &inner_length, helper_path) ||
        inner_length + 1 >= inner_size) {
        LOGE("root helper: command allocation/quoting failed");
        free(inner);
        close(lfd);
        unlink(bridge_path);
        return -1;
    }
    inner[inner_length++] = ' ';
    inner[inner_length] = '\0';
    if (!append_shell_single_quoted(inner, inner_size, &inner_length, daemon_sock) ||
        inner_length + 1 >= inner_size) {
        LOGE("root helper: command quoting failed");
        free(inner);
        close(lfd);
        unlink(bridge_path);
        return -1;
    }
    inner[inner_length++] = ' ';
    inner[inner_length] = '\0';
    if (!append_shell_single_quoted(inner, inner_size, &inner_length, bridge_path) ||
        inner_length + 1 != inner_size) {
        LOGE("root helper: command quoting length mismatch");
        free(inner);
        close(lfd);
        unlink(bridge_path);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOGE("root helper: fork() failed: %s", strerror(errno));
        free(inner);
        close(lfd);
        unlink(bridge_path);
        return -1;
    }
    if (pid == 0) {
        if (setsid() < 0)
            _exit(126);
        execlp("su", "su", "-c", inner, (char *)NULL);
        _exit(127);   /* su not found / exec failed */
    }
    free(inner);

    if (timeout_ms <= 0)
        timeout_ms = 10000;
    int fd = -1;
    bool cancelled = false;
    int64_t deadline_ns = monotonic_time_ns() + (int64_t)timeout_ms * 1000000LL;
    while (fd < 0 && !cancelled) {
        int64_t remaining_ns = deadline_ns - monotonic_time_ns();
        if (remaining_ns <= 0)
            break;
        int remaining_ms = (int)((remaining_ns + 999999LL) / 1000000LL);
        struct pollfd pfds[2] = {
            { .fd = lfd, .events = POLLIN },
            { .fd = cancel_fd, .events = POLLIN | POLLHUP | POLLERR },
        };
        nfds_t count = cancel_fd >= 0 ? 2 : 1;
        int result = poll(pfds, count, remaining_ms);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (result == 0)
            break;
        if (count == 2 && bridge_wait_cancelled(cancel_fd, pfds[1].revents)) {
            cancelled = true;
            break;
        }
        if (pfds[0].revents & (POLLHUP | POLLERR | POLLNVAL))
            break;
        if (pfds[0].revents & POLLIN) {
            int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (cfd < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                break;
            }
            fd = receive_helper_fd(cfd, cancel_fd, deadline_ns, &cancelled);
            close(cfd);
        }
    }

    if (cancelled)
        LOGI("root helper: connection attempt cancelled");
    else if (fd < 0)
        LOGE("root helper: timed out or failed waiting for helper connection");
    if (fd < 0)
        terminate_helper(pid);
    int status = wait_child_bounded(pid, 1000);
    close(lfd);
    unlink(bridge_path);

    if (fd < 0)
        LOGE("root helper: did not receive daemon fd (su status=%d)", status);
    return fd;
}

/*
 * Foreground scheduling ("启用前台调度 (root)"). No resident helper: each
 * transition is one `su -c libsettopapp.so ...` invocation.
 *
 * Mode 1 (focused app): at exit-fallback the consumer asks the helper for the
 * HOST pid of the session anchor (UNIX_DIAG lookup, "noset"); that anchor
 * both pins the producer's PID namespace and its /proc/<pid>/root/proc.
 * Every SCHEDULING report from the producer (producer subtree at init, then
 * the focused client subtree on each activation change) becomes an O(1) "set"
 * call that setns()es into the anchor's PID namespace and writes the
 * container pid straight into the cgroup files. The producer orders the
 * reports self-contained (off before on), so no consumer-side tracking is
 * needed.
 *
 * Mode 2 (whole session): unchanged legacy behaviour -- promote the whole
 * producer tree at exit-fallback.
 *
 * Both modes restore identically on fallback/stop: one "restore" sweep over
 * the whole tree rooted at the scanned session root, which covers every pid
 * either mode boosted.
 */

static bool safe_helper_path(const char *path)
{
    if (!path || path[0] != '/')
        return false;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (!(isalnum(*p) || *p == '/' || *p == '.' || *p == '_'
              || *p == '-' || *p == '+' || *p == '=' || *p == ':'
              || *p == '~')) {
            return false;
        }
    }
    return true;
}

/* Run `su -c "<cmd>"`, capture its first stdout line. Returns the parsed pid
 * (>0), or -1 on any failure (no su, helper missing, no output). Blocks for
 * the duration of the helper; called from the render/event threads where a
 * short stall at a session boundary is acceptable. */
static pid_t run_su_capture_pid(const char *cmd)
{
    int link[2];
    if (pipe(link) < 0) {
        LOGE("settopapp: pipe failed: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOGE("settopapp: fork failed: %s", strerror(errno));
        close(link[0]);
        close(link[1]);
        return -1;
    }
    if (pid == 0) {
        /* su prompt/progress output must not pollute the pid line. */
        close(link[0]);
        dup2(link[1], STDOUT_FILENO);
        close(link[1]);
        int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("su", "su", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(link[1]);

    char line[32];
    ssize_t n = read(link[0], line, sizeof(line) - 1);
    close(link[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        LOGE("settopapp: helper failed (status=%d, read=%zd)", status, n);
        return -1;
    }
    line[n] = '\0';
    int got = atoi(line);
    return got > 0 ? (pid_t)got : -1;
}

/* Run `su -c "<cmd>"` ignoring its output; returns the su exit status, or -1
 * on fork/exec failure. Used for the mode-1 "set" switches, which report
 * success through logcat rather than stdout. */
static int run_su_status(const char *cmd)
{
    pid_t pid = fork();
    if (pid < 0) {
        LOGE("settopapp: fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("su", "su", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Resolve the producer (holder of the peer of data socket `data_fd`) to its
 * session-tree-root HOST pid via the helper. promote=false only discovers
 * ("noset"); promote=true additionally moves the whole tree into top-app
 * (mode 2), honouring the optional custom stop-name list. Returns the root
 * pid, or -1. Called at exit-fallback, i.e. the producer is connected right
 * now, so the diag lookup has a live peer; data_fd is live for this whole
 * call (enter_fallback closes it only after this returns). In mode 1 the
 * returned anchor pins the producer's PID namespace (/proc/<pid>/ns/pid) and
 * its /proc/<pid>/root/proc for the later "set" calls. */
static pid_t topapp_discover_root(struct consumer_state *s, int data_fd,
                                  bool promote)
{
    struct stat st;
    if (data_fd < 0 || fstat(data_fd, &st) < 0 || st.st_ino == 0
        || !safe_helper_path(s->topapp_run_path)) {
        LOGE("settopapp: data fd not resolvable");
        return -1;
    }

    /* The inode identifies our end of the data socketpair; the helper resolves
     * it to the peer the producer holds. The optional stop-name list bounds
     * the upward PPID scan in both modes: mode 2 uses the scanned root as the
     * promote tree root, mode 1 as the namespace anchor (and restore root). */
    char cmd[sizeof(s->topapp_run_path) + sizeof(s->topapp_run_stops) + 32];
    if (s->topapp_run_stops[0] != '\0' && promote)
        snprintf(cmd, sizeof(cmd), "%s %llu stops=%s", s->topapp_run_path,
                 (unsigned long long)st.st_ino, s->topapp_run_stops);
    else if (s->topapp_run_stops[0] != '\0')
        snprintf(cmd, sizeof(cmd), "%s %llu stops=%s noset", s->topapp_run_path,
                 (unsigned long long)st.st_ino, s->topapp_run_stops);
    else if (promote)
        snprintf(cmd, sizeof(cmd), "%s %llu", s->topapp_run_path,
                 (unsigned long long)st.st_ino);
    else
        snprintf(cmd, sizeof(cmd), "%s %llu noset", s->topapp_run_path,
                 (unsigned long long)st.st_ino);

    pid_t root = run_su_capture_pid(cmd);
    if (root > 0)
        LOGI("settopapp: tree root %d %s", (int)root,
             promote ? "promoted to top-app" : "resolved (anchor)");
    return root;
}

/* Mode 1: O(1) switch of one container pid (or its subtree) between top-app
 * and the root groups, via the anchor's PID namespace. */
static int topapp_set(struct consumer_state *s, pid_t container_pid,
                      bool on, bool settree)
{
    pthread_mutex_lock(&s->topapp_lock);
    const pid_t anchor = s->topapp_root_pid;
    pthread_mutex_unlock(&s->topapp_lock);
    if (anchor <= 0)
        return -1;

    char cmd[sizeof(s->topapp_run_path) + 96];
    const int length = snprintf(cmd, sizeof(cmd), "%s set %d %d opt=%s settree=%s",
                                s->topapp_run_path, (int)anchor, (int)container_pid,
                                on ? "on" : "off", settree ? "true" : "false");
    if (length <= 0 || (size_t)length >= sizeof(cmd))
        return -1;
    return run_su_status(cmd);
}

/* Undo every mode-1 boost of this connection: the focused pid (if any) and
 * KWin's own subtree back to the root groups. */
/* Undo every boost of this connection: restore the whole scanned tree rooted
 * at topapp_root_pid back to the root cgroups. Shared by both modes -- the
 * anchor tree covers KWin's subtree and every focused client subtree mode 1
 * boosted, and mode 2 promoted exactly that tree in the first place. Safe to
 * call with the producer already gone: restore walks the root's tree as it
 * exists NOW (leftover processes still move back; dead ones skip). */
static void topapp_restore_tree(struct consumer_state *s)
{
    pthread_mutex_lock(&s->topapp_lock);
    pid_t root = s->topapp_root_pid;
    s->topapp_root_pid = 0;
    pthread_mutex_unlock(&s->topapp_lock);

    if (root <= 0)
        return;

    char cmd[sizeof(s->topapp_run_path) + 32];
    snprintf(cmd, sizeof(cmd), "%s %d restore", s->topapp_run_path, (int)root);
    if (run_su_capture_pid(cmd) > 0)
        LOGI("settopapp: tree root %d restored", (int)root);
}

static void topapp_handle_scheduling_event(struct consumer_state *s,
                                           const struct OutputEvent *event)
{
    /* Only mode 1 consumes per-pid reports (mode 2 promoted the whole tree
     * once at exit-fallback), and only when the feature is enabled for this
     * connection -- with foreground scheduling off the producer may still
     * send events (it cannot know the Android-side setting), and they must
     * not spawn su invocations. */
    if (s->topapp_run_mode != 1 || !s->topapp_run_enable)
        return;

    pthread_mutex_lock(&s->topapp_lock);
    const bool have_anchor = s->topapp_root_pid > 0;
    pthread_mutex_unlock(&s->topapp_lock);
    if (!have_anchor)
        return;

    /* Self-contained switch: the producer already ordered the events so an
     * "off" for the previous client arrives before the "on" for the next.
     * Every event maps 1:1 onto one O(1) helper "set" invocation. */
    const bool on = event->scheduling.flags & SCHEDULING_FLAG_ON;
    const bool settree = event->scheduling.flags & SCHEDULING_FLAG_SETTREE;

    if (event->scheduling.pid == 0 && !on)
        return;   /* nothing to restore; no focused client reported */

    if (topapp_set(s, event->scheduling.pid, on, settree) != 0)
        LOGE("settopapp: set pid=%d on=%d tree=%d failed",
             (int)event->scheduling.pid, on, settree);
    else
        LOGI("settopapp: pid %d %s (%s)", (int)event->scheduling.pid,
             on ? "promoted" : "restored",
             settree ? "tree" : "process");
}

static int do_connect(struct consumer_state *s)
{
    atomic_store_explicit(&s->in_fallback, true, memory_order_release);
    /* Snapshot the connection config for this attempt. */
    pthread_mutex_lock(&s->cfg_lock);
    bool use_root = s->cfg_use_root;
    char sock_path[sizeof(s->cfg_socket_path)];
    char helper_path[sizeof(s->cfg_helper_path)];
    char bridge_path[sizeof(s->cfg_bridge_path)];
    memcpy(sock_path, s->cfg_socket_path, sizeof(sock_path));
    memcpy(helper_path, s->cfg_helper_path, sizeof(helper_path));
    memcpy(bridge_path, s->cfg_bridge_path, sizeof(bridge_path));
    s->topapp_run_enable = s->cfg_topapp_enable;
    memcpy(s->topapp_run_path, s->cfg_topapp_path, sizeof(s->topapp_run_path));
    s->topapp_run_mode = s->cfg_topapp_mode == 2 ? 2 : 1;
    memcpy(s->topapp_run_stops, s->cfg_topapp_stops,
           sizeof(s->topapp_run_stops));
    pthread_mutex_unlock(&s->cfg_lock);

    const char *sock = sock_path;

    /* A boosted producer from the previous session must not keep its top-app
     * priority across a reconnect cycle; restore the whole tree before the
     * old ctx dies. */
    topapp_restore_tree(s);

    if (s->ctx) {
        audio_set_ctx(s->audio, NULL);
        display_consumer_abort_io(s->ctx);
        stop_event_thread(s);
        join_event_thread(s);
        pthread_mutex_lock(&s->ctx_lock);
        display_ctx *old_ctx = s->ctx;
        s->ctx = NULL;
        if (old_ctx)
            disconnect(old_ctx);
        pthread_mutex_unlock(&s->ctx_lock);
    }
    cleanup_dmabufs(s);

    ANativeWindow *win = s->window;
    pthread_mutex_lock(&s->cfg_lock);
    int cw = s->cfg_custom_width;
    int ch = s->cfg_custom_height;
    pthread_mutex_unlock(&s->cfg_lock);

    if (s->fanout.enabled) {
        s->screen_w = s->fanout.visible_width;
        s->screen_h = s->fanout.visible_height;
    } else if (s->remote_mode) {
        s->screen_w = s->remote_encoded_w;
        s->screen_h = s->remote_encoded_h;
    } else if (cw > 0 && ch > 0) {
        s->screen_w = cw;
        s->screen_h = ch;
    } else {
        s->screen_w = ANativeWindow_getWidth(win);
        s->screen_h = ANativeWindow_getHeight(win);
    }

    /* dequeueBuffer needs the window connected to an API first (ANativeWindow_lock
     * did this internally). Disconnect first so reconnect is idempotent. */
    int window_api = (s->remote_mode || s->fanout.enabled) ? ANW_API_EGL : ANW_API_CPU;
    disconnect_consumer_window(s);
    if (anw_api_connect(win, window_api) != 0) {
        LOGE("api_connect(%s) failed", window_api == ANW_API_EGL ? "EGL" : "CPU");
        return -1;
    }
    s->connected_window_api = window_api;

    if (ANativeWindow_setBuffersGeometry(win, s->screen_w, s->screen_h,
                                         AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM) != 0) {
        LOGE("setBuffersGeometry failed for %dx%d", s->screen_w, s->screen_h);
        return -1;
    }

    int min_undequeued = 0;
    if (api.query(win, ANATIVEWINDOW_QUERY_MIN_UNDEQUEUED_BUFFERS,
                  &min_undequeued) != 0 || min_undequeued < 0 ||
        min_undequeued + 2 > MAX_COLLECT_BUFS) {
        LOGE("unsupported window min-undequeued count: %d", min_undequeued);
        return -1;
    }
    int total = min_undequeued + 2;
    if (api.setBufferCount(win, (size_t)total) != 0) {
        LOGE("setBufferCount(%d) failed", total);
        return -1;
    }

    s->buf_count = total;
    if (collect_dmabufs(s) < 0)
        return -1;

    LOGI("connecting to %s (%dx%d, %d bufs, root=%d)", sock,
         s->screen_w, s->screen_h, s->buf_count, use_root);

    display_ctx *new_ctx = NULL;
    if (use_root) {
        int ctrl_fd = recv_fd_via_root_helper_timeout(
            sock, helper_path, bridge_path, s->connect_cancel_efd, 30000);
        if (ctrl_fd < 0) {
            LOGE("root helper connect failed");
            return -1;
        }
        if (!atomic_load_explicit(&s->running, memory_order_acquire)) {
            close(ctrl_fd);
            return -1;
        }
        if (connect_to_deamon_with_fd(&new_ctx, ctrl_fd) < 0) {
            LOGE("connect_to_deamon_with_fd failed");
            return -1;
        }
    } else if (connect_to_deamon(&new_ctx, sock) < 0) {
        LOGE("connect_to_deamon failed");
        return -1;
    }

    if (!atomic_load_explicit(&s->running, memory_order_acquire)) {
        disconnect(new_ctx);
        return -1;
    }

    int display_w = s->remote_mode ? s->remote_display_w : s->screen_w;
    int display_h = s->remote_mode ? s->remote_display_h : s->screen_h;
    if (set_screen_info(new_ctx, display_w, display_h, PIXEL_FORMAT_RGBA_8888,
            atomic_load_explicit(&s->refresh_mhz, memory_order_acquire)) < 0) {
        disconnect(new_ctx);
        return -1;
    }
    if (push_dmabufs(new_ctx, s->dmabuf_fds, s->dmabuf_infos, s->buf_count) < 0) {
        disconnect(new_ctx);
        return -1;
    }

    /* Register the camera service only when it was initialised (i.e. the user
     * enabled it in settings and granted CAMERA). The service_info lives in this
     * per-instance state (outlives the ctx) and carries userdata=s so the camera
     * layer knows which instance's client to serve. The producer drives it via
     * RESOURCES_REQUEST (handled on the event thread). */
    if (camera_service_is_ready()) {
        s->camera_svc.type = SERVICE_TYPE_CAMERA;
        s->camera_svc.allocate_resource = camera_allocate_resource;
        s->camera_svc.free_resource = camera_free_resource;
        s->camera_svc.userdata = s;
        allocate_services(new_ctx, &s->camera_svc, 1);
    }

    set_fallback_callback(new_ctx, on_fallback, s);
    set_exit_fallback_callback(new_ctx, on_exit_fallback, s);
    if (!atomic_load_explicit(&s->running, memory_order_acquire)) {
        disconnect(new_ctx);
        return -1;
    }
    pthread_mutex_lock(&s->ctx_lock);
    s->ctx = new_ctx;
    pthread_mutex_unlock(&s->ctx_lock);

    audio_set_ctx(s->audio, new_ctx);

    atomic_store_explicit(&s->need_reconnect, false, memory_order_release);
    LOGI("connected");
    return 0;
}

static void on_fallback(void *userdata)
{
    struct consumer_state *s = userdata;
    atomic_store_explicit(&s->in_fallback, true, memory_order_release);
    LOGI("fallback triggered");

    /* The producer is gone (or the link broke): its top-app boost must end
     * now, before any process can leak foreground priority for a whole
     * session. One tree sweep covers every pid either mode boosted. */
    topapp_restore_tree(s);

    audio_set_ctx(s->audio, NULL);   /* the lib has closed the audio fd; stop touching it */

    /* Let the owning MainActivity probe the daemon socket and close the window if the
     * daemon is gone. onFallback() marshals itself to the UI thread on the Java side. */
    if (g_jvm && s->activity_obj) {
        JNIEnv *env = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
            if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) == 0)
                attached = true;
        }
        if (env) {
            jclass cls = (*env)->GetObjectClass(env, s->activity_obj);
            jmethodID mid = (*env)->GetMethodID(env, cls, "onFallback", "()V");
            if (mid)
                (*env)->CallVoidMethod(env, s->activity_obj, mid);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }

    // Disable clip listener on Java side before stopping event thread
    if (g_jvm && s->clipboard_obj) {
        JNIEnv *env = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
            if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) == 0)
                attached = true;
        }
        if (env) {
            jclass cls = (*env)->GetObjectClass(env, s->clipboard_obj);
            jmethodID mid = (*env)->GetMethodID(env, cls, "nativeClipListening", "(Z)V");
            if (mid)
                (*env)->CallVoidMethod(env, s->clipboard_obj, mid, JNI_FALSE);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }

    stop_event_thread(s);
}

static void on_exit_fallback(void *userdata)
{
    struct consumer_state *s = userdata;
    atomic_store_explicit(&s->in_fallback, false, memory_order_release);
    LOGI("exit fallback triggered");

    /* Producer (re)connected. Mode 2 promotes the whole tree right here;
     * mode 1 only discovers the namespace anchor now -- the per-pid boosts
     * happen when the event thread drains KWin's producer-identity and
     * active-window reports queued on the data socket. Runs before anything
     * JNI below so a failure here never breaks the session restart.
     * cfg_topapp_enable is read locklessly like every other cfg read on this
     * path (Java only reconfigures between connections). */
    if (s->topapp_run_enable && s->topapp_run_path[0] != '\0') {
        pid_t root = topapp_discover_root(s, get_data_fd(s->ctx),
                                          s->topapp_run_mode == 2);
        if (root > 0) {
            pthread_mutex_lock(&s->topapp_lock);
            s->topapp_root_pid = root;
            pthread_mutex_unlock(&s->topapp_lock);
        }
    }

    audio_set_ctx(s->audio, s->ctx);
    send_refresh_rate(s);

    JNIEnv *env = NULL;
    if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != 0) {
        LOGE("on_exit_fallback: AttachCurrentThread failed");
        return;
    }

    // Enable clip listener on Java side
    jclass cls = (*env)->GetObjectClass(env, s->clipboard_obj);
    jmethodID listenMid = (*env)->GetMethodID(env, cls, "nativeClipListening", "(Z)V");
    if (listenMid)
        (*env)->CallVoidMethod(env, s->clipboard_obj, listenMid, JNI_TRUE);

    start_event_thread(s);

    // Initial clipboard sync: read current system clipboard and send to producer
    jmethodID syncMethod = (*env)->GetMethodID(env, cls, "nativeClipboardSync", "()V");
    if (syncMethod)
        (*env)->CallVoidMethod(env, s->clipboard_obj, syncMethod);

    (*g_jvm)->DetachCurrentThread(g_jvm);
}

static void *render_thread_func(void *arg)
{
    struct consumer_state *s = arg;
    LOGI("render thread started");

    while (atomic_load_explicit(&s->running, memory_order_acquire)) {
        if (!atomic_load_explicit(&s->need_reconnect, memory_order_acquire)) {
            pthread_mutex_lock(&s->ctx_lock);
            bool control_dead = s->ctx && display_consumer_needs_reconnect(s->ctx);
            pthread_mutex_unlock(&s->ctx_lock);
            if (control_dead) {
                LOGI("daemon control transport closed; scheduling full reconnect");
                atomic_store_explicit(&s->need_reconnect, true, memory_order_release);
            }
        }
        if (atomic_load_explicit(&s->need_reconnect, memory_order_acquire)) {
            LOGI("reconnecting...");
            TracyCZoneN(zConnect, "do_connect", 1);
            int rc = do_connect(s);
            TracyCZoneEnd(zConnect);
            if (rc < 0) {
                usleep(500000);
                continue;
            }
        }

        ANativeWindowBuffer *anb = NULL;
        int acqfence = -1;
        TracyCZoneN(zDequeue, "dequeueBuffer", 1);
        int dq = api.dequeueBuffer(s->window, &anb, &acqfence);
        TracyCZoneEnd(zDequeue);
        if (dq != 0 || !anb) {
            if (anb)
                api.cancelBuffer(s->window, anb, acqfence);
            else if (acqfence >= 0)
                close(acqfence);
            usleep(16000);
            continue;
        }
        /* Emulate ANativeWindow_lock: CPU-wait the acquire fence so the buffer is
         * safe to write before handing it to the producer. */
        TracyCZoneN(zAcqFence, "acquire fence wait", 1);
        bool acquire_ready = wait_fence(&acqfence, "acquire", &s->running, -1);
        TracyCZoneEnd(zAcqFence);
        if (!acquire_ready) {
            api.cancelBuffer(s->window, anb, acqfence);
            usleep(16000);
            continue;
        }

        int idx = -1;
        for (int i = 0; i < s->buf_count; i++) {
            if (s->buf_anb[i] == anb) {
                idx = i;
                break;
            }
        }

        if (idx < 0) {
            queue_consumer_buffer(s, anb, -1);
            usleep(16000);
            continue;
        }

        TracyCZoneN(zSelect, "select_dmabuf", 1);
        int sel = select_dmabuf(s->ctx, idx);
        TracyCZoneEnd(zSelect);
        if (sel < 0) {
            queue_consumer_buffer(s, anb, -1);
            usleep(16000);
            continue;
        }

        TracyCZoneN(zRefresh, "refresh_done (producer render)", 1);
        int rfence = refresh_done(s->ctx);
        TracyCZoneEnd(zRefresh);
        int64_t timestamp_ns = monotonic_time_ns();

        if (atomic_load_explicit(&s->in_fallback, memory_order_acquire)) {
            /* refresh_done failed before confirming producer completion. Re-posting
             * this buffer ready-now would expose a potentially in-flight render. */
            api.cancelBuffer(s->window, anb, rfence);
            usleep(16000);
            continue;
        }

        if (s->fanout.enabled) {
            /* The source remains dequeued and owned here. KGSL fence export between
             * our contexts is intentionally avoided: CPU-wait producer completion,
             * import/copy, glFinish, then post the local source ready-now. */
            if (!wait_fence(&rfence, "producer", &s->running, -1)) {
                api.cancelBuffer(s->window, anb, rfence);
                usleep(16000);
                continue;
            }
            fanout_copy_source(s, anb, timestamp_ns);
            api.setBuffersTimestamp(s->window, timestamp_ns);
            queue_consumer_buffer(s, anb, -1);
        } else {
            /* Direct modes preserve the existing producer-fence handoff. */
            if (s->remote_mode)
                api.setBuffersTimestamp(s->window, timestamp_ns);
            queue_consumer_buffer(s, anb, rfence);
        }
        TracyCFrameMark;
    }

    if (s->fanout.enabled && eglGetCurrentContext() == s->fanout.source_context)
        eglMakeCurrent(s->fanout.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    LOGI("render thread stopped");
    return NULL;
}

/* ---------- JNI ---------- */

static void copy_jstring(JNIEnv *env, jstring js, char *dst, size_t dstsz)
{
    if (!js) {
        dst[0] = '\0';
        return;
    }
    const char *s = (*env)->GetStringUTFChars(env, js, NULL);
    if (s) {
        strncpy(dst, s, dstsz - 1);
        dst[dstsz - 1] = '\0';
        (*env)->ReleaseStringUTFChars(env, js, s);
    } else {
        dst[0] = '\0';
    }
}

/* Every JNI entry point below takes a jlong handle -- the consumer_state* returned
 * by nativeCreate -- so multiple instances (windows) coexist in one process. */
#define STATE(h) ((struct consumer_state *)(uintptr_t)(h))

JNIEXPORT jlong JNICALL
Java_com_anland_consumer_Native_nativeCreate(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    struct consumer_state *s = calloc(1, sizeof(*s));
    if (!s)
        return 0;
    s->connect_cancel_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (s->connect_cancel_efd < 0) {
        free(s);
        return 0;
    }
    pthread_mutex_init(&s->lock, NULL);
    pthread_mutex_init(&s->ctx_lock, NULL);
    pthread_mutex_init(&s->cfg_lock, NULL);
    pthread_mutex_init(&s->topapp_lock, NULL);
    s->cfg_topapp_mode = 1;
    pthread_mutex_init(&s->fanout.lock, NULL);
    pthread_cond_init(&s->fanout.cond, NULL);
    s->fanout.display = EGL_NO_DISPLAY;
    s->fanout.source_context = EGL_NO_CONTEXT;
    s->fanout.encoder_context = EGL_NO_CONTEXT;
    s->fanout.source_surface = EGL_NO_SURFACE;
    s->fanout.encoder_surface = EGL_NO_SURFACE;
    atomic_init(&s->running, false);
    atomic_init(&s->need_reconnect, false);
    atomic_init(&s->in_fallback, true);
    atomic_init(&s->event_running, false);
    atomic_init(&s->refresh_mhz, 0);
    for (int i = 0; i < MAX_COLLECT_BUFS; i++)
        s->dmabuf_fds[i] = -1;
    strncpy(s->cfg_socket_path, "/data/local/tmp/display_daemon.sock",
            sizeof(s->cfg_socket_path) - 1);
    s->audio = audio_create();
    LOGI("instance %p created", (void *)s);
    return (jlong)(uintptr_t)s;
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeDestroy(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    /* Stop the transport (mirrors nativeStop), release the camera client + audio
     * bridge, then free. */
    pthread_mutex_lock(&s->lock);
    if (atomic_load_explicit(&s->running, memory_order_acquire)) {
        atomic_store_explicit(&s->running, false, memory_order_release);
        signal_connect_cancel(s);
        fanout_request_stop(s);
        detach_and_abort_transport(s);
        pthread_mutex_unlock(&s->lock);
        pthread_join(s->render_thread, NULL);
        pthread_mutex_lock(&s->lock);
    }
    fanout_shutdown(s);
    detach_and_abort_transport(s);
    if (s->ctx) {
        display_consumer_abort_io(s->ctx);
        stop_event_thread(s);
        join_event_thread(s);
        topapp_restore_tree(s);
        disconnect(s->ctx);
        s->ctx = NULL;
    } else {
        topapp_restore_tree(s);
        pthread_mutex_lock(&s->ctx_lock);
        display_ctx *old_ctx = s->ctx;
        s->ctx = NULL;
        if (old_ctx)
            disconnect(old_ctx);
        pthread_mutex_unlock(&s->ctx_lock);
    }
    cleanup_dmabufs(s);
    if (s->window) {
        disconnect_consumer_window(s);
        ANativeWindow_release(s->window);
        s->window = NULL;
    }
    pthread_mutex_unlock(&s->lock);

    audio_destroy(s->audio);
    s->audio = NULL;
    camera_release_client(s);   /* window gone: tear down its camera channels */

    if (s->clipboard_obj && g_jvm) {
        JNIEnv *e = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&e, JNI_VERSION_1_6) == JNI_EDETACHED)
            attached = ((*g_jvm)->AttachCurrentThread(g_jvm, &e, NULL) == 0);
        if (e) {
            (*e)->DeleteGlobalRef(e, s->clipboard_obj);
            if (s->activity_obj)
                (*e)->DeleteGlobalRef(e, s->activity_obj);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }
    s->activity_obj = NULL;
    pthread_mutex_destroy(&s->topapp_lock);
    clear_java_targets(env, s);
    pthread_cond_destroy(&s->fanout.cond);
    pthread_mutex_destroy(&s->fanout.lock);
    pthread_mutex_destroy(&s->cfg_lock);
    pthread_mutex_destroy(&s->ctx_lock);
    pthread_mutex_destroy(&s->lock);
    close(s->connect_cancel_efd);
    s->connect_cancel_efd = -1;
    LOGI("instance %p destroyed", (void *)s);
    free(s);
}

/* Mark this instance focused (real camera frames go to the focused instance; others
 * get blank frames). Called from Java on window focus gain. */
JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetFocused(
    JNIEnv *env, jclass clazz, jlong handle, jboolean focused)
{
    (void)env; (void)clazz;
    struct consumer_state *s = STATE(handle);
    if (s && focused)
        camera_set_focus(s);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeConfigure(
    JNIEnv *env, jclass clazz, jlong handle, jstring socketPath, jboolean useRoot,
    jstring helperPath, jstring bridgePath, jboolean topappEnable, jstring topappPath,
    jint topappMode, jstring topappStops)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    pthread_mutex_lock(&s->cfg_lock);
    char tmp[sizeof(s->cfg_socket_path)];
    copy_jstring(env, socketPath, tmp, sizeof(tmp));
    if (tmp[0] != '\0')
        memcpy(s->cfg_socket_path, tmp, sizeof(s->cfg_socket_path));
    s->cfg_use_root = (useRoot == JNI_TRUE);
    copy_jstring(env, helperPath, s->cfg_helper_path, sizeof(s->cfg_helper_path));
    copy_jstring(env, bridgePath, s->cfg_bridge_path, sizeof(s->cfg_bridge_path));
    s->cfg_topapp_enable = (topappEnable == JNI_TRUE);
    copy_jstring(env, topappPath, s->cfg_topapp_path, sizeof(s->cfg_topapp_path));
    s->cfg_topapp_mode = topappMode == 2 ? 2 : 1;
    copy_jstring(env, topappStops, s->cfg_topapp_stops,
                 sizeof(s->cfg_topapp_stops));
    for (char *p = s->cfg_topapp_stops; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == 0x7f) {
            s->cfg_topapp_stops[0] = '\0';
            break;
        }
    }
    pthread_mutex_unlock(&s->cfg_lock);
    LOGI("configured: socket=%s root=%d helper=%s bridge=%s topapp=%d mode=%d stops=%s",
         s->cfg_socket_path, s->cfg_use_root, s->cfg_helper_path, s->cfg_bridge_path,
         s->cfg_topapp_enable, s->cfg_topapp_mode, s->cfg_topapp_stops);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetCustomResolution(
    JNIEnv* env, jclass clazz, jlong handle, jint width, jint height)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    pthread_mutex_lock(&s->cfg_lock);
    s->cfg_custom_width = width;
    s->cfg_custom_height = height;
    pthread_mutex_unlock(&s->cfg_lock);
    LOGI("custom resolution: %dx%d", width, height);
}

static bool start_consumer(
    JNIEnv *env, jlong handle, jobject surface, jobject encoder_surface,
    jobject clipboardTarget, jobject activityTarget, bool remote_mode,
    bool preserve_local_audio, int display_width, int display_height,
    int encoded_width, int encoded_height, int fps, uint64_t fanout_generation)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !surface)
        return false;

    if (!ensure_anw_api()) {
        LOGE("failed to load ANativeWindow hidden API");
        return false;
    }

    pthread_mutex_lock(&s->lock);

    if (atomic_load_explicit(&s->running, memory_order_acquire)) {
        atomic_store_explicit(&s->running, false, memory_order_release);
        signal_connect_cancel(s);
        fanout_request_stop(s);
        detach_and_abort_transport(s);
        pthread_mutex_unlock(&s->lock);
        pthread_join(s->render_thread, NULL);
        pthread_mutex_lock(&s->lock);
    }
    fanout_shutdown(s);
    detach_and_abort_transport(s);

    if (s->ctx) {
        stop_event_thread(s);
        join_event_thread(s);
        pthread_mutex_lock(&s->ctx_lock);
        display_ctx *old_ctx = s->ctx;
        s->ctx = NULL;
        if (old_ctx)
            disconnect(old_ctx);
        pthread_mutex_unlock(&s->ctx_lock);
    }
    drain_connect_cancel(s);
    s->motion_has_last = false;
    cleanup_dmabufs(s);

    if (s->window) {
        disconnect_consumer_window(s);
        ANativeWindow_release(s->window);
        s->window = NULL;
    }

    s->window = ANativeWindow_fromSurface(env, surface);
    if (!s->window) {
        LOGE("ANativeWindow_fromSurface failed");
        pthread_mutex_unlock(&s->lock);
        return false;
    }

    s->remote_mode = remote_mode;
    s->remote_display_w = display_width;
    s->remote_display_h = display_height;
    s->remote_encoded_w = encoded_width;
    s->remote_encoded_h = encoded_height;
    s->remote_fps = fps;
    if (remote_mode)
        atomic_store_explicit(&s->refresh_mhz, (uint32_t)fps * 1000,
                              memory_order_release);

    if (!g_jvm)
        (*env)->GetJavaVM(env, &g_jvm);
    clear_java_targets(env, s);
    s->clipboard_obj = (*env)->NewGlobalRef(env, clipboardTarget);
    s->activity_obj = activityTarget ? (*env)->NewGlobalRef(env, activityTarget) : NULL;

    /* Finish all device-only EGL/GL validation before opening the anland transport.
     * A false return leaves Java free to start its existing remote-direct fallback. */
    if (encoder_surface &&
        !fanout_init(s, env, encoder_surface, display_width, display_height,
                     encoded_width, encoded_height, fps, fanout_generation)) {
        disconnect_consumer_window(s);
        ANativeWindow_release(s->window);
        s->window = NULL;
        clear_java_targets(env, s);
        pthread_mutex_unlock(&s->lock);
        return false;
    }

    atomic_store_explicit(&s->running, true, memory_order_release);
    atomic_store_explicit(&s->need_reconnect, true, memory_order_release);
    if (pthread_create(&s->render_thread, NULL, render_thread_func, s) != 0) {
        LOGE("render thread creation failed");
        atomic_store_explicit(&s->running, false, memory_order_release);
        fanout_shutdown(s);
        disconnect_consumer_window(s);
        ANativeWindow_release(s->window);
        s->window = NULL;
        clear_java_targets(env, s);
        pthread_mutex_unlock(&s->lock);
        return false;
    }

    if (!remote_mode || preserve_local_audio)
        audio_start(s->audio);

    pthread_mutex_unlock(&s->lock);
    return true;
}

JNIEXPORT jboolean JNICALL
Java_com_anland_consumer_Native_nativeStart(
    JNIEnv *env, jclass clazz, jlong handle, jobject surface, jobject clipboardTarget,
    jobject activityTarget)
{
    (void)clazz;
    return start_consumer(env, handle, surface, NULL, clipboardTarget, activityTarget,
                          false, true, 0, 0, 0, 0, 0, 0)
         ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_anland_consumer_Native_nativeStartRemote(
    JNIEnv *env, jclass clazz, jlong handle, jobject surface, jobject clipboardTarget,
    jint display_width, jint display_height, jint encoded_width, jint encoded_height,
    jint fps, jboolean preserve_local_audio)
{
    (void)clazz;
    return start_consumer(env, handle, surface, NULL, clipboardTarget, NULL,
                          true, preserve_local_audio == JNI_TRUE,
                          display_width, display_height, encoded_width, encoded_height, fps, 0)
         ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_anland_consumer_Native_nativeStartFanout(
    JNIEnv *env, jclass clazz, jlong handle, jobject local_surface,
    jobject encoder_surface, jobject clipboard_target, jobject activity_target,
    jint display_width, jint display_height, jint encoded_width, jint encoded_height,
    jint fps, jlong generation)
{
    (void)clazz;
    return start_consumer(env, handle, local_surface, encoder_surface,
                          clipboard_target, activity_target, true, true,
                          display_width, display_height, encoded_width, encoded_height,
                          fps, (uint64_t)generation) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeStop(
    JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    pthread_mutex_lock(&s->lock);

    if (atomic_load_explicit(&s->running, memory_order_acquire)) {
        atomic_store_explicit(&s->running, false, memory_order_release);
        signal_connect_cancel(s);
        fanout_request_stop(s);
        detach_and_abort_transport(s);
        pthread_mutex_unlock(&s->lock);
        pthread_join(s->render_thread, NULL);
        pthread_mutex_lock(&s->lock);
    }
    /* The encoder can still be blocked in eglSwapBuffers after the source/render
     * thread is gone. Join it before destroying either shared context or window. */
    fanout_shutdown(s);

    /* Stop audio before the ctx (and its fd) is torn down. */
    detach_and_abort_transport(s);
    audio_stop(s->audio);

    if (s->ctx) {
        stop_event_thread(s);
        join_event_thread(s);
        topapp_restore_tree(s); /* stopping the pipeline ends the boost */
        disconnect(s->ctx);
        s->ctx = NULL;
    } else {
        topapp_restore_tree(s);
        pthread_mutex_lock(&s->ctx_lock);
        display_ctx *old_ctx = s->ctx;
        s->ctx = NULL;
        if (old_ctx)
            disconnect(old_ctx);
        pthread_mutex_unlock(&s->ctx_lock);
    }

    // Disable clip listener on Java side
    if (g_jvm && s->clipboard_obj) {
        JNIEnv *env2 = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&env2, JNI_VERSION_1_6) == JNI_EDETACHED) {
            if ((*g_jvm)->AttachCurrentThread(g_jvm, &env2, NULL) == 0)
                attached = true;
        }
        if (env2) {
            jclass cls = (*env2)->GetObjectClass(env2, s->clipboard_obj);
            jmethodID mid = (*env2)->GetMethodID(env2, cls, "nativeClipListening", "(Z)V");
            if (mid)
                (*env2)->CallVoidMethod(env2, s->clipboard_obj, mid, JNI_FALSE);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }

    cleanup_dmabufs(s);

    if (s->window) {
        disconnect_consumer_window(s);
        ANativeWindow_release(s->window);
        s->window = NULL;
    }
    clear_java_targets(env, s);

    pthread_mutex_unlock(&s->lock);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetRefreshRate(
    JNIEnv *env, jclass clazz, jlong handle, jfloat hz)
{
    struct consumer_state *s = STATE(handle);
    if (!s || hz <= 0.0f)
        return;
    atomic_store_explicit(&s->refresh_mhz,
                          (uint32_t)(hz * 1000.0f + 0.5f), memory_order_release);
    // Apply live if already connected; otherwise do_connect() seeds it.
    send_refresh_rate(s);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendTouch(
    JNIEnv *env, jclass clazz, jlong handle, jint action, jfloat x, jfloat y, jint pointer_id)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_TOUCH,
        .touch = { .action = action, .x = x, .y = y, .pointer_id = pointer_id },
    };
    push_event_locked(s, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendTouchFrame(
    JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_TOUCH_FRAME,
    };
    push_event_locked(s, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendKey(
    JNIEnv *env, jclass clazz, jlong handle, jint action, jint keycode)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_KEY,
        .key = { .action = action, .keycode = keycode },
    };
    push_event_locked(s, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendMouseMotion(
    JNIEnv *env, jclass clazz, jlong handle, jfloat x, jfloat y, jfloat dx, jfloat dy)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    if (dx == 0.0f && dy == 0.0f && s->motion_has_last) {
        dx = x - s->motion_last_x;
        dy = y - s->motion_last_y;
    }

    s->motion_last_x = x;
    s->motion_last_y = y;
    s->motion_has_last = true;

    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_MOTION,
        .pointer_motion = { .x = x, .y = y, .dx = dx, .dy = dy },
    };
    push_event_locked(s, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendMouseButton(
    JNIEnv *env, jclass clazz, jlong handle, jint button, jboolean pressed)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_BUTTON,
        .pointer_button = { .button = button, .pressed = pressed ? 1 : 0 },
    };
    push_event_locked(s, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendMouseScroll(
    JNIEnv *env, jclass clazz, jlong handle, jint axis, jfloat value)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_AXIS,
        .pointer_axis = { .axis = axis, .value = value, .discrete = 0 },
    };
    push_event_locked(s, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendClipboard(
    JNIEnv *env, jclass clazz, jlong handle, jbyteArray data)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !data)
        return;

    jsize len = (*env)->GetArrayLength(env, data);
    if (len < 0 || len > 1024 * 1024)
        return;
    char *buf = NULL;
    if (len > 0) {
        buf = malloc(len);
        if (!buf)
            return;
        (*env)->GetByteArrayRegion(env, data, 0, len, (jbyte *)buf);
    }

    struct InputEvent ev = {
        .type = INPUT_TYPE_CLIPBOARD,
        .clipboard = { .size = (uint32_t)len },
    };
    push_event_with_length_locked(s, &ev, buf, (size_t)len);
    free(buf);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendTextInput(
    JNIEnv *env, jclass clazz, jlong handle, jbyteArray data)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !data)
        return;

    jsize len = (*env)->GetArrayLength(env, data);
    if (len <= 0)
        return;

    char *buf = malloc(len);
    if (!buf)
        return;
    (*env)->GetByteArrayRegion(env, data, 0, len, (jbyte *)buf);

    struct InputEvent ev = {
        .type = INPUT_TYPE_TEXT_INPUT,
        .text_input = { .size = (uint32_t)len },
    };
    push_event_with_length_locked(s, &ev, buf, (size_t)len);
    free(buf);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetMicEnabled(
    JNIEnv *env, jclass clazz, jlong handle, jboolean enabled)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    audio_set_mic_enabled(s->audio, enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetAudioLatency(
    JNIEnv *env, jclass clazz, jlong handle, jint speakerMs, jint micMs)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    audio_set_latency(s->audio, speakerMs, micMs);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetAudioKeepalive(
    JNIEnv *env, jclass clazz, jlong handle, jboolean enabled)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    audio_set_keepalive(s->audio, enabled == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_com_anland_consumer_Native_nativeOpenBridgeSocket(
    JNIEnv *env, jclass clazz, jstring helper_path_j, jstring socket_path_j,
    jstring handoff_path_j, jint cancel_fd, jint timeout_ms)
{
    (void)clazz;
    char helper_path[512], socket_path[256], handoff_path[512];
    copy_jstring(env, helper_path_j, helper_path, sizeof(helper_path));
    copy_jstring(env, socket_path_j, socket_path, sizeof(socket_path));
    copy_jstring(env, handoff_path_j, handoff_path, sizeof(handoff_path));
    if (helper_path[0] == '\0' || socket_path[0] == '\0' || handoff_path[0] == '\0')
        return -1;
    return recv_fd_via_root_helper_timeout(socket_path, helper_path, handoff_path,
                                           cancel_fd, timeout_ms);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetBridgeSocketTimeout(
    JNIEnv *env, jclass clazz, jint fd, jint timeout_ms)
{
    (void)env; (void)clazz;
    if (fd < 0)
        return;
    struct timeval tv = {0};
    if (timeout_ms > 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetBridgeSocketReceiveTimeout(
    JNIEnv *env, jclass clazz, jint fd, jint timeout_ms)
{
    (void)env; (void)clazz;
    if (fd < 0)
        return;
    struct timeval tv = {0};
    if (timeout_ms > 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetBridgeSocketSendTimeout(
    JNIEnv *env, jclass clazz, jint fd, jint timeout_ms)
{
    (void)env; (void)clazz;
    if (fd < 0)
        return;
    struct timeval tv = {0};
    if (timeout_ms > 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
    }
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeShutdownBridgeSocket(
    JNIEnv *env, jclass clazz, jint fd)
{
    (void)env; (void)clazz;
    if (fd >= 0)
        shutdown(fd, SHUT_RDWR);
}
