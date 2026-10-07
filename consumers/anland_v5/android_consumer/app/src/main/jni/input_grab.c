/*
 * inputgrab -- the root helper behind "immersive mode".
 *
 * Immersive mode hands the device over to the Linux desktop: the touchscreen,
 * the keyboard and any pointer stop reaching Android entirely and are streamed
 * to the app instead, which replays them onto the remote desktop.
 *
 * The app cannot read /dev/input itself (untrusted_app is denied
 * input_device:chr_file), so it launches this helper through `su -c`. The
 * helper opens and EVIOCGRABs the devices in the root context and forwards a
 * fixed-size record stream over a unix socket the app listens on -- the same
 * bridge pattern fd_helper.c uses to hand back the daemon connection. It is
 * shipped inside the APK as lib*.so so Android extracts it into the app's
 * nativeLibraryDir with execute permission.
 *
 *   usage: libinputgrab.so <bridge_socket_path> <toggle_scancode>
 *
 * Grabbing every input device is a good way to brick a tablet, so the escape
 * hatches are the design, in the order they matter:
 *
 *   1. EVIOCGRAB hangs off the open file description, so the kernel drops every
 *      grab as soon as this process dies -- including on SIGKILL.
 *   2. <toggle_scancode> is mandatory and is watched on every device: pressing
 *      it ungrabs and exits, so the user gets out even if the app is wedged.
 *   3. Records are sent non-blocking and dropped under back-pressure. A stalled
 *      app can therefore never park this process inside send() and stop it from
 *      ever reaching the toggle key -- which is what actually locks a device up.
 *   4. The app heartbeats over the same socket. Silence for HEARTBEAT_TIMEOUT_MS
 *      (or EOF, which the kernel guarantees when the app dies) releases
 *      everything. The app only heartbeats while its main thread is running, so
 *      an ANR frees the input too.
 *   5. Android hardware-button nodes stay with Android. A touchscreen that also
 *      reports Power can be grabbed only while a separate built-in power-button
 *      node remains available to Android. Otherwise it is listed as watch-only,
 *      with its real device class preserved.
 */
#define _GNU_SOURCE
#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "input_grab.h"
#include "socket_utils.h"

#define TAG "AnlandGrab"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

/* No heartbeat for this long => the app is gone or frozen; release everything. */
#define HEARTBEAT_TIMEOUT_MS 6000
/* How often to look for input devices that appeared since the session started.
 * Bluetooth mice and keyboards sleep and reconnect as brand-new nodes
 * mid-session; a one-time scan at start would leave them out of the grab and
 * at Android's mercy. */
#define RESCAN_INTERVAL_MS 2000
/* Consecutive dropped records before we give the input back rather than keep
 * grabbing for an app that is plainly not reading. */
#define MAX_DROPS 2000
/* Poll slice; also how often the timeout above is re-checked. */
#define POLL_SLICE_MS 250

#define GOLD_NAME_PREFIX "Gold Keyboardremaps"
#define GOLD_VENDOR 0xffff
#define GOLD_PRODUCT 0xffff
enum input_source { SOURCE_PHYSICAL, SOURCE_GOLD, SOURCE_COMBINED };
static enum input_source g_source = SOURCE_PHYSICAL;
static int selected_physical_node(const char *path);

#define BITS_PER_LONG  (8 * (int)sizeof(unsigned long))
#define NBITS(x)       ((((x) - 1) / BITS_PER_LONG) + 1)
#define TEST_BIT(bit, arr) \
    (((arr)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

struct dev_entry {
    int  fd;
    int  cls;        /* IGRAB_CLASS_* */
    int  grabbed;    /* 0 =>
                        watched only (toggle detection), events not forwarded */
    int  gold;
    int  waiting_idle; /* finish Android's held keys before taking Gold output */
    int  announced;  /* the app has received this entry's DEVICE record */
    int  alpha;      /* a full keyboard, not just a node that happens to have keys */
    int  watch_only; /* classified as tracked-but-never-grabbed */
    int  power;      /* advertises KEY_POWER or KEY_POWER2 */
    int  power_button; /* a separate built-in, key-only power-button node */
    unsigned int bus; /* bustype: how the device is attached, e.g. USB / Bluetooth */
    int  multitouch;
    int  clickpad;   /* one button under the pad: BTN_LEFT alone means "a click" */
    int  min_x, max_x, min_y, max_y;
    char name[80];
};

static struct dev_entry g_devs[IGRAB_MAX_DEVICES];
static int g_ndevs = 0;
static int g_has_power_button = 0;
static volatile sig_atomic_t g_quit = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/*
 * Connect to the app's listening socket, which lives in the abstract namespace
 * (that is what android.net.LocalServerSocket creates). Abstract means there is
 * no file to find, chmod or clean up, and root needs one SELinux permission
 * fewer than for a socket inside the app's data directory. socket_utils'
 * connect_unix() only speaks filesystem paths, hence this one.
 */
static int connect_abstract(const char *name)
{
    size_t len = strlen(name);
    struct sockaddr_un addr;
    if (len + 1 > sizeof(addr.sun_path))
        return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';            /* leading NUL == abstract namespace */
    memcpy(addr.sun_path + 1, name, len);
    socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + len);

    if (connect(fd, (struct sockaddr *)&addr, alen) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Release every grab and close. Called on every exit path; the kernel would do
 * it anyway when the process dies, but doing it explicitly keeps the window
 * where Android sees no input as short as possible. */
static void release_all(void)
{
    for (int i = 0; i < g_ndevs; i++) {
        if (g_devs[i].fd < 0)
            continue;
        if (g_devs[i].grabbed)
            ioctl(g_devs[i].fd, EVIOCGRAB, 0);
        close(g_devs[i].fd);
        g_devs[i].fd = -1;
    }
    g_ndevs = 0;
}

static int pending_gold(void)
{
    for (int i = 0; i < g_ndevs; i++)
        if (g_devs[i].fd >= 0 && g_devs[i].waiting_idle)
            return 1;
    return 0;
}

static int has_gold_output(void)
{
    for (int i = 0; i < g_ndevs; i++)
        if (g_devs[i].fd >= 0 && g_devs[i].gold)
            return 1;
    return 0;
}

/* ---------------- record transport ---------------- */

/* Tail of a record that only went out partially. A stream socket can accept
 * fewer than 32 bytes, and losing the rest would desync the framing for good,
 * so the remainder is held here and flushed before anything else. */
static uint8_t g_pending[IGRAB_REC_SIZE];
static size_t  g_pending_len = 0;
static int     g_drops = 0;
static int     g_need_resync = 0;

static int writable(int fd)
{
    struct pollfd p = { .fd = fd, .events = POLLOUT };
    return poll(&p, 1, 0) > 0 && (p.revents & POLLOUT);
}

/* Flush a partial record. 1 = nothing pending anymore, 0 = still pending. */
static int flush_pending(int fd)
{
    while (g_pending_len > 0) {
        if (!writable(fd))
            return 0;
        ssize_t n = send(fd, g_pending + (IGRAB_REC_SIZE - g_pending_len),
                         g_pending_len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            g_pending_len -= (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return 0;
        return -1;
    }
    return 1;
}

/*
 * Send one record, never blocking. Under back-pressure the record is dropped
 * and counted: input that cannot be delivered is worth less than the ability to
 * keep polling for the toggle key. Returns 0 on success or a drop, -1 when the
 * socket is dead.
 */
static int send_rec(int fd, const struct igrab_rec *rec)
{
    int fp = flush_pending(fd);
    if (fp < 0)
        return -1;
    if (fp == 0) {
        g_drops++;
        g_need_resync = 1;
        return 0;
    }

    if (!writable(fd)) {
        g_drops++;
        g_need_resync = 1;
        return 0;
    }

    ssize_t n = send(fd, rec, IGRAB_REC_SIZE, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n == IGRAB_REC_SIZE) {
        g_drops = 0;
        return 0;
    }
    if (n > 0) {
        /* Partial: stash the tail so the next call restores the framing. */
        memcpy(g_pending, rec, IGRAB_REC_SIZE);
        g_pending_len = IGRAB_REC_SIZE - (size_t)n;
        g_drops = 0;
        return 0;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        g_drops++;
        g_need_resync = 1;
        return 0;
    }
    return -1;
}

/* Tell the app that records were lost, so it can release every key/contact it
 * still believes is held instead of leaving them stuck on the desktop. Mirrors
 * what the kernel does with SYN_DROPPED. */
static int send_resync(int fd)
{
    struct igrab_rec rec;
    memset(&rec, 0, sizeof(rec));
    rec.rtype = IGRAB_REC_EVENT;
    rec.dev   = IGRAB_DEV_ALL;
    rec.etype = EV_SYN;
    rec.code  = SYN_DROPPED;
    if (send_rec(fd, &rec) < 0)
        return -1;
    /* Only clear once it actually left: a dropped resync marker is worse than
     * useless, it would hide the drop it was meant to report. */
    if (g_drops == 0 && g_pending_len == 0)
        g_need_resync = 0;
    return 0;
}

/*
 * Finish a partial record and, after any loss, deliver the global SYN_DROPPED
 * marker before normal traffic resumes. This is also driven by POLLOUT while
 * idle: waiting for another input event leaves a final KEY_UP or SYN_REPORT
 * stranded forever when the user stops moving.
 *
 *  1: normal records may be sent; 0: socket still back-pressured; -1: dead.
 */
static int recover_output(int fd)
{
    int flushed = flush_pending(fd);
    if (flushed < 0)
        return -1;
    if (flushed == 0)
        return 0;
    if (!g_need_resync)
        return 1;
    if (send_resync(fd) < 0)
        return -1;
    return (!g_need_resync && g_pending_len == 0) ? 1 : 0;
}

static int send_bye(int fd, int reason)
{
    /* Finish any half-written record first: leaving one truncated would put the
     * app's parser out of step with the stream, and this is the one record it
     * really has to be able to read. */
    if (g_pending_len > 0) {
        send_all(fd, g_pending + (IGRAB_REC_SIZE - g_pending_len), g_pending_len);
        g_pending_len = 0;
    }

    struct igrab_rec rec;
    memset(&rec, 0, sizeof(rec));
    rec.rtype = IGRAB_REC_BYE;
    rec.value = reason;
    /* Blocking on purpose: this is the last record and the socket has a send
     * timeout, so it cannot hang. Losing it would leave the app waiting for a
     * stream that has already ended. */
    return send_all(fd, &rec, sizeof(rec));
}

/* ---------------- device discovery ---------------- */

static int any_key_bit(const unsigned long *key)
{
    for (int i = 0; i <= KEY_MAX; i++) {
        if (TEST_BIT(i, key))
            return 1;
    }
    return 0;
}

/* Full keyboards may advertise KEY_POWER alongside their ordinary keys. */
static int has_alpha_keys(const unsigned long *key)
{
    static const int letters[] = {
        KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y,
        KEY_A, KEY_S, KEY_D, KEY_F, KEY_G,
        KEY_Z, KEY_X, KEY_C, KEY_V,
    };
    for (size_t i = 0; i < sizeof(letters) / sizeof(letters[0]); i++) {
        if (!TEST_BIT(letters[i], key))
            return 0;
    }
    return 1;
}

static int has_power_key(const unsigned long *key)
{
    if (TEST_BIT(KEY_POWER, key))
        return 1;
#ifdef KEY_POWER2
    if (TEST_BIT(KEY_POWER2, key))
        return 1;
#endif
    return 0;
}

static int is_physical_android_key(int code)
{
    switch (code) {
    case KEY_POWER:
#ifdef KEY_POWER2
    case KEY_POWER2:
#endif
    case KEY_WAKEUP:
    case KEY_SLEEP:
    case KEY_VOLUMEUP:
    case KEY_VOLUMEDOWN:
    case KEY_MUTE:
        return 1;
    default:
        return 0;
    }
}

static int has_only_physical_android_keys(const unsigned long *key)
{
    int any = 0;
    for (int i = 0; i <= KEY_MAX; i++) {
        if (!TEST_BIT(i, key))
            continue;
        any = 1;
        if (!is_physical_android_key(i))
            return 0;
    }
    return any;
}

static int query_input(int fd, unsigned long request, void *data)
{
    int result;
    do {
        result = ioctl(fd, request, data);
    } while (result < 0 && errno == EINTR);
    return result;
}

static int read_capabilities(int fd, const char *path, int type, void *bits, size_t size)
{
    if (query_input(fd, EVIOCGBIT(type, size), bits) >= 0)
        return 0;
    LOGE("%s: EVIOCGBIT(%d) failed: %s", path, type, strerror(errno));
    return -1;
}

static void read_abs_range(int fd, const char *path, int axis, int *min, int *max)
{
    struct input_absinfo info;
    if (query_input(fd, EVIOCGABS(axis), &info) < 0) {
        LOGE("%s: EVIOCGABS(%#x) failed: %s", path, axis, strerror(errno));
        return;
    }
    if (info.maximum <= info.minimum) {
        LOGE("%s: invalid axis %#x range %d..%d", path, axis,
             info.minimum, info.maximum);
        return;
    }
    *min = info.minimum;
    *max = info.maximum;
}

static int is_builtin_bus(unsigned int bus)
{
    return bus == 0 || bus == BUS_HOST || bus == BUS_I8042
            || bus == BUS_I2C || bus == BUS_SPI;
}

/* Preserve Android's matching key releases before switching the output owner. */
static int grab_idle_gold(struct dev_entry *entry)
{
    unsigned long held[NBITS(KEY_MAX + 1)] = {0};
    if (query_input(entry->fd, EVIOCGKEY(sizeof(held)), held) < 0)
        return -1;
    for (size_t i = 0; i < sizeof(held) / sizeof(held[0]); i++)
        if (held[i])
            return 0;
    if (ioctl(entry->fd, EVIOCGRAB, 1) < 0)
        return -1;
    entry->grabbed = 1;
    entry->waiting_idle = 0;
    return 1;
}

/* Classify by capabilities and identity, then take only the requested source. */
static int inspect_device(const char *path, struct dev_entry *out, int do_grab)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        LOGE("open(%s) failed: %s", path, strerror(errno));
        return -1;
    }

    unsigned long ev[NBITS(EV_MAX + 1)];
    unsigned long key[NBITS(KEY_MAX + 1)];
    unsigned long abs[NBITS(ABS_MAX + 1)];
    unsigned long rel[NBITS(REL_MAX + 1)];
    unsigned long prop[NBITS(INPUT_PROP_MAX + 1)];
    memset(ev, 0, sizeof(ev));
    memset(key, 0, sizeof(key));
    memset(abs, 0, sizeof(abs));
    memset(rel, 0, sizeof(rel));
    memset(prop, 0, sizeof(prop));

    if (read_capabilities(fd, path, 0, ev, sizeof(ev)) < 0
            || (TEST_BIT(EV_KEY, ev)
                && read_capabilities(fd, path, EV_KEY, key, sizeof(key)) < 0)
            || (TEST_BIT(EV_ABS, ev)
                && read_capabilities(fd, path, EV_ABS, abs, sizeof(abs)) < 0)
            || (TEST_BIT(EV_REL, ev)
                && read_capabilities(fd, path, EV_REL, rel, sizeof(rel)) < 0)) {
        close(fd);
        return -1;
    }
    /* Older drivers may not expose properties; contact capabilities still let
     * us classify them. Required capability queries above must not silently
     * turn a failed touch-axis query into an apparently valid button node. */
    if (query_input(fd, EVIOCGPROP(sizeof(prop)), prop) < 0) {
        LOGI("%s: input properties unavailable (%s); using contact capabilities",
             path, strerror(errno));
        memset(prop, 0, sizeof(prop));
    }

    memset(out, 0, sizeof(*out));
    out->fd = fd;
    if (query_input(fd, EVIOCGNAME(sizeof(out->name)), out->name) < 0)
        out->name[0] = '\0';
    out->name[sizeof(out->name) - 1] = '\0';

    int mt      = TEST_BIT(ABS_MT_POSITION_X, abs) && TEST_BIT(ABS_MT_POSITION_Y, abs);
    int abs_xy  = TEST_BIT(ABS_X, abs) && TEST_BIT(ABS_Y, abs);
    int rel_xy  = TEST_BIT(REL_X, rel) && TEST_BIT(REL_Y, rel);
    int direct  = TEST_BIT(INPUT_PROP_DIRECT, prop);
    int pointer = TEST_BIT(INPUT_PROP_POINTER, prop);
    int touch   = TEST_BIT(BTN_TOUCH, key);
    int click   = TEST_BIT(BTN_LEFT, key);
    int pen     = TEST_BIT(BTN_TOOL_PEN, key) || TEST_BIT(BTN_STYLUS, key)
            || TEST_BIT(BTN_TOOL_RUBBER, key);
    int finger  = TEST_BIT(BTN_TOOL_FINGER, key);
    int alpha   = has_alpha_keys(key);
    int power   = has_power_key(key);
    out->alpha = alpha;
    out->power = power;

    /* How the device is attached. The same keyboard can be offered over more
     * than one transport, and which one it is tells the user which node is the
     * one they are actually typing on. */
    struct input_id id;
    int have_id = query_input(fd, EVIOCGID, &id) == 0;
    if (have_id)
        out->bus = id.bustype;
    out->gold = have_id && id.vendor == GOLD_VENDOR && id.product == GOLD_PRODUCT &&
            strncmp(out->name, GOLD_NAME_PREFIX, sizeof(GOLD_NAME_PREFIX) - 1) == 0;
    if (out->gold && g_source == SOURCE_PHYSICAL) {
        close(fd);
        return -1;
    }
    int keys    = any_key_bit(key);
    out->power_button = power && !alpha && have_id && is_builtin_bus(out->bus)
            && !TEST_BIT(EV_ABS, ev) && !TEST_BIT(EV_REL, ev);

    /* Skip a pen-only digitizer, but keep nodes that also carry finger contacts.
     * Shared pen/touch nodes are decoded by tool type on the app side. */
    if (pen && !mt && !finger) {
        close(fd);
        return -1;
    }

    /* Contacts, not axes: a gamepad also reports ABS_X/ABS_Y, and its sticks
     * must not be mistaken for a finger. */
    int contacts = mt || (abs_xy && (direct || pointer || touch || finger));

    if (contacts) {
        /* The property bits say it outright when they are set. Panels that set
         * neither are told apart by the clickpad button: a touchscreen has no
         * BTN_LEFT, an integrated pad almost always does. */
        if (direct)
            out->cls = IGRAB_CLASS_TOUCHSCREEN;
        else if (pointer || click)
            out->cls = IGRAB_CLASS_TOUCHPAD;
        else
            out->cls = IGRAB_CLASS_TOUCHSCREEN;
    } else if (rel_xy) {
        out->cls = IGRAB_CLASS_MOUSE;
    } else if (keys) {
        out->cls = IGRAB_CLASS_KEYBOARD;
    } else {
        close(fd);
        return -1;
    }

    out->multitouch = mt;
    /* A clickpad has one button under the whole surface, so every press comes in
     * as BTN_LEFT and the app has to work out left vs right from where the
     * finger is. INPUT_PROP_BUTTONPAD is the authoritative bit; a pad that
     * offers no BTN_RIGHT at all is one in practice too. */
    out->clickpad = TEST_BIT(INPUT_PROP_BUTTONPAD, prop)
            || (click && !TEST_BIT(BTN_RIGHT, key));
    if (out->cls == IGRAB_CLASS_TOUCHSCREEN || out->cls == IGRAB_CLASS_TOUCHPAD) {
        if (mt) {
            read_abs_range(fd, path, ABS_MT_POSITION_X, &out->min_x, &out->max_x);
            read_abs_range(fd, path, ABS_MT_POSITION_Y, &out->min_y, &out->max_y);
        }
        /* Single-touch panels only have ABS_X/ABS_Y; multitouch ones still use
         * them as a fallback when the MT axes report nothing usable. */
        if (out->max_x <= out->min_x && TEST_BIT(ABS_X, abs))
            read_abs_range(fd, path, ABS_X, &out->min_x, &out->max_x);
        if (out->max_y <= out->min_y && TEST_BIT(ABS_Y, abs))
            read_abs_range(fd, path, ABS_Y, &out->min_y, &out->max_y);
        if (out->max_x <= out->min_x || out->max_y <= out->min_y) {
            LOGE("%s '%s': no usable touch coordinate range", path, out->name);
            close(fd);
            return -1;
        }
    }

    /* Some panels advertise KEY_POWER for screen-off gestures. That does not
     * make them keyboards. They can be taken while a separate built-in power
     * button stays with Android; otherwise retain their class and report the
     * restriction so Settings can explain it instead of hiding the panel. */
    out->watch_only = !out->gold && ((power && !alpha
            && !(out->cls == IGRAB_CLASS_TOUCHSCREEN && g_has_power_button))
            || (out->cls == IGRAB_CLASS_KEYBOARD && has_only_physical_android_keys(key)));

    if (do_grab && !out->gold &&
            (g_source == SOURCE_GOLD || !selected_physical_node(path) ||
             (g_source == SOURCE_COMBINED && out->cls == IGRAB_CLASS_KEYBOARD &&
              !out->watch_only))) {
        close(fd);
        return -1;
    }

    if (!do_grab || out->watch_only) {
        /* Enumeration or a protected node. The caller owns this fd, but Android
         * keeps receiving its events. */
        out->grabbed = 0;
        return 0;
    }

    if (out->gold) {
        /* These are already remapped events. Taking the output leaves Gold's
         * physical grab and Anland profile intact, while Android gets no keys.
         * If the entry key is still held, let its UP reach Android first. */
        out->waiting_idle = 1;
        if (grab_idle_gold(out) >= 0)
            return 0;
        close(fd);
        return -1;
    }

    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        /* Someone else already owns it exclusively. Watching it would double up
         * with Android's own delivery, so let it go entirely. */
        LOGE("grab %s '%s' failed: %s", path, out->name, strerror(errno));
        close(fd);
        return -1;
    }
    out->grabbed = 1;
    return 0;
}

/* ---------------- node names the caller asked to keep or leave alone -------- */

#define IGRAB_NODE_NAME_MAX 16
#define IGRAB_MAX_NODE_ARG 32

struct node_list {
    char names[IGRAB_MAX_NODE_ARG][IGRAB_NODE_NAME_MAX];
    int count;
};

static struct node_list g_selected;   /* nodes= ...; empty means "auto" */
static struct node_list g_excluded;   /* exclude= ... */

/*
 * "event" followed by digits. The strict form is the grammar Gold itself emits
 * and admits no leading zeros beyond the bare "event0"; the loose form is what a
 * preference saved by an older build may contain, so "event01" is still legal
 * there.
 */
static int valid_node_name(const char *name, int strict)
{
    if (strncmp(name, "event", 5) != 0)
        return 0;
    const char *digits = name + 5;
    if (*digits == '\0')
        return 0;
    if (strict && digits[0] == '0')
        return digits[1] == '\0';
    for (const char *p = digits; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return 0;
    }
    return 1;
}

/*
 * Parses a comma separated node list. Returns 0 on success, -1 when the argument
 * is not one we are willing to act on: an empty list means "not given", and a
 * repeated name is refused rather than silently collapsed.
 */
static int parse_node_argument(const char *value, struct node_list *out, int strict)
{
    out->count = 0;
    if (value == NULL || *value == '\0')
        return 0;

    const char *cursor = value;
    for (;;) {
        const char *comma = strchr(cursor, ',');
        size_t length = comma ? (size_t)(comma - cursor) : strlen(cursor);
        if (length == 0 || length >= IGRAB_NODE_NAME_MAX)
            return -1;
        char name[IGRAB_NODE_NAME_MAX];
        memcpy(name, cursor, length);
        name[length] = '\0';

        if (!valid_node_name(name, strict) || out->count >= IGRAB_MAX_NODE_ARG)
            return -1;
        for (int i = 0; i < out->count; i++) {
            if (strcmp(out->names[i], name) == 0)
                return -1;
        }
        memcpy(out->names[out->count], name, length + 1);
        out->count++;

        if (comma == NULL)
            return 0;
        cursor = comma + 1;
    }
}

static int node_list_contains(const struct node_list *list, const char *name)
{
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->names[i], name) == 0)
            return 1;
    }
    return 0;
}

/* Look for a separate built-in power button before deciding whether a panel's
 * gesture KEY_POWER is a reason to leave it alone. Selection limits grabs, but
 * an unselected power button still protects Android. Explicit exclusions are
 * respected even during this read-only probe (Gold may own those devices).
 * USB, Bluetooth and virtual power keys cannot stand in for the physical button.
 */
static void refresh_power_button(void)
{
    g_has_power_button = 0;
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0
                || node_list_contains(&g_excluded, ent->d_name))
            continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        struct dev_entry entry;
        if (inspect_device(path, &entry, 0) < 0)
            continue;
        int power_button = entry.power_button;
        close(entry.fd);
        if (power_button) {
            g_has_power_button = 1;
            break;
        }
    }
    closedir(dir);
}

/*
 * Whether a node belongs in the session. Excluded nodes are never opened or
 * inspected, including by the power-button probe. Unselected nodes may be
 * probed but are never grabbed, announced or assigned a device slot.
 *
 * An empty selection means "auto" -- take whatever is capable, as before.
 */
static int node_is_allowed(const char *name)
{
    if (node_list_contains(&g_excluded, name))
        return 0;
    if (g_source != SOURCE_COMBINED && g_selected.count > 0 &&
            !node_list_contains(&g_selected, name))
        return 0;
    return 1;
}

static int selected_physical_node(const char *path)
{
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    return g_selected.count == 0 || node_list_contains(&g_selected, name);
}

/*
 * Whether a node is one of the input devices a session actually wants: a
 * keyboard, a pointer, a touch surface.
 *
 * A node that merely carries a few buttons -- a powerkey, a headset jack, or the
 * media strip a modern keyboard exposes as its own HID collection -- is not one.
 * Grabbing it would take the device's own volume and brightness keys away from
 * Android for the whole session and give nothing back: those keys are not
 * something a desktop wants forwarded while the user is trying to change the
 * volume of the tablet in their hands.
 *
 * Consulted only when the caller named no nodes of its own; an explicit
 * selection is the caller's business, not this function's.
 */
static int node_is_common_input(const struct dev_entry *entry)
{
    if (entry->gold)
        return 1;
    if (entry->watch_only)
        return 0;
    /* Classified as a keyboard but carrying no letters: a button cluster. */
    if (entry->cls == IGRAB_CLASS_KEYBOARD && !entry->alpha)
        return 0;
    return 1;
}

/* Give back a node inspect_device opened (and maybe grabbed) that we decided
 * not to keep. Releasing the grab before closing shortens the window in which
 * Android cannot see the device. */
static void drop_inspected(struct dev_entry *entry)
{
    if (entry->fd < 0)
        return;
    if (entry->grabbed)
        ioctl(entry->fd, EVIOCGRAB, 0);
    close(entry->fd);
    entry->fd = -1;
    entry->grabbed = 0;
}

/*
 * Enumerates every input node and prints one line each. Nothing is grabbed and
 * the caller's selection is ignored: Settings has to be able to show a node even
 * when a session has been told to leave it alone.
 */
static int list_devices(void)
{
    refresh_power_button();
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        LOGE("opendir(/dev/input): %s", strerror(errno));
        return 1;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

        struct dev_entry entry;
        if (inspect_device(path, &entry, 0) < 0)
            continue;
        /* node, human name, class, bustype, flags. The app splits on tabs, and
         * the helper ships inside the same APK as the app that reads it, so the
         * two are always the same version and there is no older shape to keep
         * working with.
         *
         * The flags are what stop the list from lying: without them every node
         * that merely carries a few keys reports the same class as a real
         * keyboard, and "gpio-keys" would be offered as a keyboard alongside the
         * one the user actually types on.
         *   a = has a full set of letter keys (a keyboard, not a button node)
         *   w = classified watch-only, so a session never grabs it */
        char flags[3];
        int next = 0;
        if (entry.alpha)
            flags[next++] = 'a';
        if (entry.watch_only)
            flags[next++] = 'w';
        flags[next] = '\0';
        printf("%s\t%s\t%d\t%04x\t%s\n", ent->d_name, entry.name, entry.cls,
               entry.bus, next == 0 ? "-" : flags);
        fflush(stdout);
        close(entry.fd);
    }
    closedir(dir);
    return 0;
}

static int scan_devices(void)
{
    refresh_power_button();
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        LOGE("opendir(/dev/input): %s", strerror(errno));
        return -1;
    }

    struct dirent *ent;
    int grabbed = 0;
    while ((ent = readdir(dir)) != NULL && g_ndevs < IGRAB_MAX_DEVICES) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;
        /* Decided from the name, before the node is opened, so a node we were
         * told to leave alone costs nothing and takes no slot. */
        if (!node_is_allowed(ent->d_name))
            continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        if (inspect_device(path, &g_devs[g_ndevs], 1) < 0)
            continue;
        if (g_selected.count == 0 && !node_is_common_input(&g_devs[g_ndevs])) {
            /* Auto-selection does not want it. Closing releases any grab
             * inspect_device took, and nothing was announced or slotted. */
            drop_inspected(&g_devs[g_ndevs]);
            continue;
        }
        LOGI("%s '%s' class=%d %s", path, g_devs[g_ndevs].name,
             g_devs[g_ndevs].cls, g_devs[g_ndevs].grabbed ? "GRABBED" : "watch-only");
        if (g_devs[g_ndevs].grabbed)
            grabbed++;
        g_ndevs++;
    }
    closedir(dir);
    return grabbed;
}

/* ---------------- mid-session device hotplug ---------------- */

/* Device number of an event node; -1 when it is gone or not a device. */
static int node_identity(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode))
        return -1;
    return (int)st.st_rdev;
}

/* Whether a tracked device still owns this node (dead fds do not count: the
 * device was unplugged, and a reconnect with the same number must be treated
 * as new). */
static int already_tracked(int rdev)
{
    for (int i = 0; i < g_ndevs; i++) {
        if (g_devs[i].fd < 0)
            continue;
        struct stat st;
        if (fstat(g_devs[i].fd, &st) == 0 && (int)st.st_rdev == rdev)
            return 1;
    }
    return 0;
}

static void fill_device_record(struct igrab_rec *rec, int idx)
{
    const struct dev_entry *de = &g_devs[idx];
    memset(rec, 0, sizeof(*rec));
    rec->rtype = IGRAB_REC_DEVICE;
    rec->dev   = (uint16_t)idx;
    rec->etype = (uint16_t)de->cls;
    rec->aux[IGRAB_AUX_MIN_X] = de->min_x;
    rec->aux[IGRAB_AUX_MAX_X] = de->max_x;
    rec->aux[IGRAB_AUX_MIN_Y] = de->min_y;
    rec->aux[IGRAB_AUX_MAX_Y] = de->max_y;
    rec->aux[IGRAB_AUX_FLAGS] =
        (de->grabbed ? IGRAB_DEV_GRABBED : 0) |
        (de->multitouch ? IGRAB_DEV_MULTITOUCH : 0) |
        (de->clickpad ? IGRAB_DEV_CLICKPAD : 0);
}

/* Reuse a detached device id before growing the table. The Java side only sees
 * a replacement DEVICE record after the global resync sent on removal, so the
 * old state cannot be confused with the new node. */
static int find_free_device_slot(void)
{
    for (int i = 0; i < g_ndevs; i++) {
        if (g_devs[i].fd < 0)
            return i;
    }
    return g_ndevs < IGRAB_MAX_DEVICES ? g_ndevs : -1;
}

/* A hotplugged device is grabbed before it can leak input to Android, but its
 * events stay local until its DEVICE record is definitely on the stream. The
 * non-blocking transport may drop a record under pressure, so retry on every
 * writable turn instead of treating that one announcement as expendable. */
static int announce_pending_devices(int sock)
{
    for (int i = 0; i < g_ndevs; i++) {
        if (g_devs[i].fd < 0 || g_devs[i].announced)
            continue;

        int ready = recover_output(sock);
        if (ready <= 0)
            return ready;

        struct igrab_rec rec;
        fill_device_record(&rec, i);
        if (send_rec(sock, &rec) < 0)
            return -1;
        if (g_pending_len != 0 || g_need_resync)
            return 0;
        g_devs[i].announced = 1;
        LOGI("announced device %d '%s'", i, g_devs[i].name);
    }
    return 1;
}

static int has_unannounced_devices(void)
{
    for (int i = 0; i < g_ndevs; i++) {
        if (g_devs[i].fd >= 0 && !g_devs[i].announced)
            return 1;
    }
    return 0;
}

/*
 * Grab any input device that appeared since the last scan. A Bluetooth mouse
 * that went to sleep reconnects as a brand-new node mid-session; without this
 * it would keep feeding Android, and the app would forward it unaccelerated
 * and ungated. New devices get the same classification rules as at session
 * start, a DEVICE record announces them to the app, and the toggle key is
 * watched on them like on everything else.
 */
static int scan_new_devices(void)
{
    refresh_power_button();
    if (!g_has_power_button) {
        for (int i = 0; i < g_ndevs; i++) {
            const struct dev_entry *entry = &g_devs[i];
            if (entry->fd >= 0 && entry->grabbed && entry->power && !entry->alpha
                    && entry->cls == IGRAB_CLASS_TOUCHSCREEN) {
                LOGE("power-button path disappeared; releasing '%s'", entry->name);
                return -1;
            }
        }
    }
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;
        /* Same rule as the initial scan: a node the caller left out is never
         * opened, so a device plugged in later cannot slip past the selection. */
        if (!node_is_allowed(ent->d_name))
            continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int rdev = node_identity(path);
        if (rdev < 0 || already_tracked(rdev))
            continue;

        int idx = find_free_device_slot();
        if (idx < 0)
            break;

        struct dev_entry de;
        if (inspect_device(path, &de, 1) < 0)
            continue;   /* uninteresting, ungrabbable, or already grabbed by us */
        if (g_selected.count == 0 && !node_is_common_input(&de)) {
            /* Same rule as the initial scan: a device plugged in later must not
             * slip past it. Nothing was slotted or announced yet. */
            drop_inspected(&de);
            continue;
        }
        g_devs[idx] = de;
        if (idx == g_ndevs)
            g_ndevs++;
        LOGI("new device %s '%s' class=%d %s", path, de.name, de.cls,
             de.grabbed ? "GRABBED (awaiting DEVICE)" : "watch-only (awaiting DEVICE)");
    }
    closedir(dir);
    return 0;
}

static int send_device_list(int sock, int grabbed)
{
    struct igrab_rec rec;

    memset(&rec, 0, sizeof(rec));
    rec.rtype  = IGRAB_REC_HELLO;
    rec.value  = IGRAB_PROTO_VERSION;
    rec.aux[0] = grabbed;
    rec.aux[1] = (int32_t)getpid();
    if (send_all(sock, &rec, sizeof(rec)) < 0)
        return -1;

    for (int i = 0; i < g_ndevs; i++) {
        fill_device_record(&rec, i);
        if (send_all(sock, &rec, sizeof(rec)) < 0)
            return -1;
        g_devs[i].announced = 1;
    }

    memset(&rec, 0, sizeof(rec));
    rec.rtype = IGRAB_REC_READY;
    rec.aux[0] = grabbed;
    return send_all(sock, &rec, sizeof(rec));
}

/* ---------------- main loop ---------------- */

/*
 * Drain one device. Returns 1 to keep going, 0 when the toggle key was pressed
 * and the session must end, -1 when the socket died.
 */
static int pump_device(int sock, int idx, int toggle)
{
    struct input_event evs[64];
    for (;;) {
        ssize_t n = read(g_devs[idx].fd, evs, sizeof(evs));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 1;   /* drained */
            return -2;      /* ENODEV: the device was unplugged */
        }
        if (n == 0)
            return -2;      /* would otherwise spin: poll keeps reporting POLLIN */

        if (g_devs[idx].waiting_idle) {
            int ready = grab_idle_gold(&g_devs[idx]);
            if (ready < 0)
                return -2;
            if (ready > 0)
                g_devs[idx].announced = 0;
            return 1; /* this batch belonged to Android before the grab */
        }

        int count = (int)(n / (ssize_t)sizeof(struct input_event));
        for (int i = 0; i < count; i++) {
            struct input_event *e = &evs[i];

            if (e->type == EV_KEY && e->code == toggle) {
                /* Press ends the session; the release and any auto-repeat are
                 * swallowed so the key never reaches the desktop. Checked before
                 * the grabbed test so a watch-only node can still trigger it. */
                if (e->value == 1)
                    return 0;
                continue;
            }

            if (!g_devs[idx].grabbed)
                continue;   /* Android still owns it; forwarding would duplicate */

            /* A hotplugged node stays silent until the Java side has its DEVICE
             * descriptor. Discarding a transition here requires a global reset:
             * otherwise its first delivered record could be a KEY_UP for a key
             * the desktop never saw go down. */
            if (!g_devs[idx].announced) {
                g_need_resync = 1;
                continue;
            }

            int ready = recover_output(sock);
            if (ready < 0)
                return -1;
            if (ready == 0) {
                g_drops++;
                g_need_resync = 1;
                if (g_drops > MAX_DROPS) {
                    LOGE("app is not reading (%d dropped records); giving input back",
                         g_drops);
                    return -3;
                }
                continue;
            }

            struct igrab_rec rec;
            memset(&rec, 0, sizeof(rec));
            rec.rtype = IGRAB_REC_EVENT;
            rec.dev   = (uint16_t)idx;
            rec.etype = e->type;
            rec.code  = e->code;
            rec.value = e->value;
            if (send_rec(sock, &rec) < 0)
                return -1;
            if (g_drops > MAX_DROPS) {
                LOGE("app is not reading (%d dropped records); giving input back",
                     g_drops);
                return -3;
            }
        }
    }
}

int main(int argc, char **argv)
{
    /* Enumeration mode: no socket, no toggle, and nothing grabbed. Used by
     * Settings, which has to list every node without disturbing a session. */
    if (argc >= 2 && strcmp(argv[1], "--list") == 0)
        return list_devices();

    if (argc < 3) {
        LOGE("usage: %s <bridge_socket> <toggle_scancode> [nodes=...] [exclude=...]",
             argv[0]);
        return 1;
    }

    const char *bridge = argv[1];
    int toggle = atoi(argv[2]);
    /* Everything past the toggle is a keyword argument. Unknown ones are
     * ignored so a newer app can talk to an older helper built before they
     * existed. A malformed list is refused outright: silently dropping a bad
     * exclusion would hand a node to this session that the caller believes it
     * kept away. */
    for (int i = 3; i < argc; i++) {
        if (strncmp(argv[i], "nodes=", 6) == 0) {
            if (parse_node_argument(argv[i] + 6, &g_selected, 0) != 0) {
                LOGE("refusing invalid nodes argument: %s", argv[i] + 6);
                return 2;
            }
        } else if (strncmp(argv[i], "exclude=", 8) == 0) {
            if (parse_node_argument(argv[i] + 8, &g_excluded, 1) != 0) {
                LOGE("refusing invalid exclude argument: %s", argv[i] + 8);
                return 2;
            }
        } else if (strncmp(argv[i], "source=", 7) == 0) {
            if (strcmp(argv[i] + 7, "gold") == 0)
                g_source = SOURCE_GOLD;
            else if (strcmp(argv[i] + 7, "combined") == 0)
                g_source = SOURCE_COMBINED;
            else if (strcmp(argv[i] + 7, "physical") == 0)
                g_source = SOURCE_PHYSICAL;
            else
                return 2;
        }
    }

    /* Gold-only leaves touch, Android gestures and hardware buttons available.
     * It can auto-enter without a binding; EOF, heartbeat and screen-off still
     * release it. Taking physical touch always requires an escape key. */
    if (toggle < 0 || toggle > KEY_MAX || (toggle == 0 && g_source != SOURCE_GOLD)) {
        LOGE("refusing invalid toggle scancode (%d)", toggle);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);

    int sock = connect_abstract(bridge);
    if (sock < 0) {
        LOGE("connect to bridge '%s' failed: %s", bridge, strerror(errno));
        return 3;
    }

    /* The hello/device records are sent blocking; bound them so a peer that
     * connects and then stops reading cannot wedge the handshake. A large send
     * buffer keeps the event stream from dropping during a UI hiccup. */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int sndbuf = 512 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    int grabbed = scan_devices();
    if ((grabbed <= 0 && !pending_gold()) ||
            (g_source != SOURCE_PHYSICAL && !has_gold_output())) {
        LOGE("no grabbable input devices");
        send_bye(sock, IGRAB_BYE_ERROR);
        release_all();
        close(sock);
        return 4;
    }

    if (send_device_list(sock, grabbed) < 0) {
        LOGE("handshake failed: %s", strerror(errno));
        release_all();
        close(sock);
        return 5;
    }
    LOGI("immersive session started: %d grabbed device(s), toggle=%d",
         grabbed, toggle);

    struct pollfd pfds[IGRAB_MAX_DEVICES + 1];
    long last_beat = now_ms();
    long last_scan = now_ms();
    int reason = IGRAB_BYE_PEER_GONE;

    for (;;) {
        if (g_quit) {
            /* SIGTERM comes from the app's own last-resort kill, i.e. it has
             * already given up on the session. */
            reason = IGRAB_BYE_PEER_GONE;
            break;
        }

        int nfds = 0;
        pfds[nfds].fd = sock;
        pfds[nfds].events = POLLIN;
        if (g_pending_len != 0 || g_need_resync || has_unannounced_devices())
            pfds[nfds].events |= POLLOUT;
        pfds[nfds].revents = 0;
        nfds++;
        for (int i = 0; i < g_ndevs; i++) {
            if (g_devs[i].fd < 0)
                continue;
            pfds[nfds].fd = g_devs[i].fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        int ret = poll(pfds, (nfds_t)nfds, POLL_SLICE_MS);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            reason = IGRAB_BYE_ERROR;
            break;
        }

        if (pfds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            reason = IGRAB_BYE_PEER_GONE;
            break;
        }
        if (pfds[0].revents & POLLIN) {
            char beat[64];
            ssize_t n = recv(sock, beat, sizeof(beat), MSG_DONTWAIT);
            if (n == 0) {
                reason = IGRAB_BYE_PEER_GONE;   /* app closed / died */
                break;
            }
            if (n > 0)
                last_beat = now_ms();
        }

        /* A frozen app keeps its socket open but stops beating. Treat that the
         * same as death: the input has to go back to Android either way. */
        if (now_ms() - last_beat > HEARTBEAT_TIMEOUT_MS) {
            LOGE("no heartbeat for %d ms; releasing", HEARTBEAT_TIMEOUT_MS);
            reason = IGRAB_BYE_STALLED;
            break;
        }

        /* Finish any partial stream work even when the user has stopped moving.
         * POLLOUT wakes this promptly; the unconditional call also handles a
         * race where the socket became writable between poll() and here. */
        int output_ready = recover_output(sock);
        if (output_ready < 0) {
            reason = IGRAB_BYE_PEER_GONE;
            break;
        }

        /* Bluetooth devices wake and reconnect as new nodes. A device is held
         * locally until announce_pending_devices() has put its DEVICE record on
         * the stream, so Android and the desktop can never both own it. */
        if (now_ms() - last_scan > RESCAN_INTERVAL_MS) {
            if (scan_new_devices() < 0) {
                reason = IGRAB_BYE_ERROR;
                break;
            }
            last_scan = now_ms();
        }

        if (output_ready > 0) {
            int announced = announce_pending_devices(sock);
            if (announced < 0) {
                reason = IGRAB_BYE_PEER_GONE;
                break;
            }
        }

        int stop = 0;
        for (int p = 1; p < nfds && !stop; p++) {
            if (!(pfds[p].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            int idx = -1;
            for (int i = 0; i < g_ndevs; i++) {
                if (g_devs[i].fd == pfds[p].fd) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0)
                continue;

            int r = pump_device(sock, idx, toggle);
            if (r == 0) {
                reason = IGRAB_BYE_TOGGLE;
                stop = 1;
            } else if (r == -1) {
                reason = IGRAB_BYE_PEER_GONE;
                stop = 1;
            } else if (r == -3) {
                reason = IGRAB_BYE_STALLED;
                stop = 1;
            } else if (r == -2) {
                /* Device vanished (unplugged dock/keyboard). Drop it and carry
                 * on -- the remaining devices are still grabbed. Release every
                 * remote state first; otherwise a key/button held at unplug stays
                 * pressed until the whole immersive session ends. */
                LOGI("device '%s' went away", g_devs[idx].name);
                if (g_devs[idx].grabbed && g_devs[idx].announced)
                    g_need_resync = 1;
                if (g_devs[idx].grabbed)
                    ioctl(g_devs[idx].fd, EVIOCGRAB, 0);
                close(g_devs[idx].fd);
                memset(&g_devs[idx], 0, sizeof(g_devs[idx]));
                g_devs[idx].fd = -1;
            }
        }
        if (stop)
            break;

        /* Every grabbed device is gone (dock unplugged mid-session). There is
         * nothing left to hold, and staying alive would only keep the app
         * believing it is still immersive. */
        int alive = 0;
        for (int i = 0; i < g_ndevs; i++) {
            if (g_devs[i].fd >= 0 && g_devs[i].grabbed)
                alive++;
        }
        if (alive == 0 && !pending_gold() && g_source != SOURCE_GOLD) {
            LOGI("all grabbed devices are gone; ending session");
            reason = IGRAB_BYE_ERROR;
            break;
        }
    }

    /* Ungrab first: the app is about to be told the session ended, and the
     * sooner Android has its input back the shorter the dead window. */
    release_all();
    send_bye(sock, reason);
    close(sock);
    LOGI("immersive session ended (reason=%d)", reason);
    return 0;
}
