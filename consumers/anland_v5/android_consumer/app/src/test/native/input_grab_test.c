/* Exercise the production helper with fake evdev nodes; no real input is opened
 * or grabbed. Run with: sh app/src/test/native/run_input_grab_tests.sh */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int fake_open(const char *path, int flags, ...);
static int fake_close(int fd);
static ssize_t fake_read(int fd, void *buffer, size_t size);
static int fake_ioctl(int fd, unsigned long request, ...);
static DIR *fake_opendir(const char *path);
static struct dirent *fake_readdir(DIR *dir);
static int fake_closedir(DIR *dir);

#define open fake_open
#define close fake_close
#define read fake_read
#define ioctl fake_ioctl
#define opendir fake_opendir
#define readdir fake_readdir
#define closedir fake_closedir
#define main input_grab_main
#include "../../main/jni/input_grab.c"
#undef open
#undef close
#undef read
#undef ioctl
#undef opendir
#undef readdir
#undef closedir
#undef main

struct fake_device {
    unsigned long ev[NBITS(EV_MAX + 1)];
    unsigned long key[NBITS(KEY_MAX + 1)];
    unsigned long abs[NBITS(ABS_MAX + 1)];
    unsigned long rel[NBITS(REL_MAX + 1)];
    unsigned long prop[NBITS(INPUT_PROP_MAX + 1)];
    struct input_absinfo ranges[ABS_MAX + 1];
    unsigned int bus;
    unsigned int vendor, product;
    char name[80];
    unsigned long held[NBITS(KEY_MAX + 1)];
    struct input_event events[8];
    size_t event_count, event_cursor;
    int grab_error;
    int present, opens, grabs, grabbed;
    int open_error, fail_nr, interrupt_once;
};

static struct fake_device devices[4];
static int device_count;
static struct {
    int device, open, grabbed;
} handles[128];
static int handle_count;

struct fake_dir {
    int used, next;
    struct dirent entry;
};
static struct fake_dir directories[2];

int __android_log_print(int priority, const char *tag, const char *format, ...)
{
    (void)priority;
    (void)tag;
    (void)format;
    return 0;
}

static void set_bit(unsigned long *bits, int bit)
{
    bits[bit / BITS_PER_LONG] |= 1UL << (bit % BITS_PER_LONG);
}

static void reset(int count)
{
    release_all();
    for (int i = 0; i < handle_count; i++)
        assert(!handles[i].open);
    memset(devices, 0, sizeof(devices));
    memset(handles, 0, sizeof(handles));
    handle_count = 0;
    memset(directories, 0, sizeof(directories));
    memset(&g_selected, 0, sizeof(g_selected));
    memset(&g_excluded, 0, sizeof(g_excluded));
    device_count = count;
    g_has_power_button = 0;
    g_source = SOURCE_PHYSICAL;
    g_pending_len = 0;
    g_need_resync = 0;
    g_drops = 0;
    for (int i = 0; i < count; i++) {
        devices[i].present = 1;
        devices[i].bus = BUS_HOST;
        devices[i].fail_nr = -1;
    }
}

static void key(int device, int code)
{
    set_bit(devices[device].ev, EV_KEY);
    set_bit(devices[device].key, code);
}

static void axis(int device, int code, int maximum)
{
    set_bit(devices[device].ev, EV_ABS);
    set_bit(devices[device].abs, code);
    devices[device].ranges[code].maximum = maximum;
}

static void panel(int device, int multitouch)
{
    key(device, BTN_TOUCH);
    set_bit(devices[device].prop, INPUT_PROP_DIRECT);
    axis(device, multitouch ? ABS_MT_POSITION_X : ABS_X, 1200);
    axis(device, multitouch ? ABS_MT_POSITION_Y : ABS_Y, 2000);
    if (multitouch) {
        axis(device, ABS_MT_SLOT, 9);
        axis(device, ABS_MT_TRACKING_ID, 65535);
    }
}

static int fake_open(const char *path, int flags, ...)
{
    (void)flags;
    assert(strncmp(path, "/dev/input/event", 16) == 0);
    int index = atoi(path + 16);
    assert(index >= 0 && index < device_count);
    struct fake_device *device = &devices[index];
    device->opens++;
    if (!device->present || device->open_error) {
        errno = device->open_error ? device->open_error : ENOENT;
        return -1;
    }
    assert(handle_count < (int)(sizeof(handles) / sizeof(handles[0])));
    handles[handle_count].device = index;
    handles[handle_count].open = 1;
    return 100 + handle_count++;
}

static int fake_close(int fd)
{
    assert(fd >= 100 && fd < 100 + handle_count);
    int handle = fd - 100;
    assert(handles[handle].open);
    if (handles[handle].grabbed)
        devices[handles[handle].device].grabbed = 0;
    handles[handle].open = 0;
    return 0;
}

static int fake_ioctl(int fd, unsigned long request, ...)
{
    assert(fd >= 100 && fd < 100 + handle_count);
    int handle = fd - 100;
    assert(handles[handle].open);
    struct fake_device *device = &devices[handles[handle].device];
    va_list args;
    va_start(args, request);
    if (request == EVIOCGRAB) {
        int grabbing = va_arg(args, int) != 0;
        if (grabbing && device->grab_error) {
            va_end(args);
            errno = device->grab_error;
            return -1;
        }
        device->grabbed = grabbing;
        handles[handle].grabbed = device->grabbed;
        device->grabs += device->grabbed;
        va_end(args);
        return 0;
    }
    void *data = va_arg(args, void *);
    va_end(args);
    int nr = (int)_IOC_NR(request);
    if (device->interrupt_once) {
        device->interrupt_once = 0;
        errno = EINTR;
        return -1;
    }
    if (nr == device->fail_nr) {
        errno = EIO;
        return -1;
    }
    size_t size = _IOC_SIZE(request);
    if (nr >= 0x20 && nr <= 0x20 + EV_MAX) {
        const void *bits = NULL;
        switch (nr - 0x20) {
        case 0: bits = device->ev; break;
        case EV_KEY: bits = device->key; break;
        case EV_ABS: bits = device->abs; break;
        case EV_REL: bits = device->rel; break;
        default: assert(0);
        }
        memcpy(data, bits, size);
        return (int)size;
    }
    if (nr >= 0x40 && nr <= 0x40 + ABS_MAX) {
        int code = nr - 0x40;
        if (!TEST_BIT(code, device->abs)) {
            errno = EINVAL;
            return -1;
        }
        memcpy(data, &device->ranges[code], size);
        return 0;
    }
    switch (nr) {
    case 0x02:
        memset(data, 0, size);
        ((struct input_id *)data)->bustype = device->bus;
        ((struct input_id *)data)->vendor = device->vendor;
        ((struct input_id *)data)->product = device->product;
        return 0;
    case 0x06:
        if (device->name[0])
            snprintf(data, size, "%s", device->name);
        else
            snprintf(data, size, "Test input %d", handles[handle].device);
        return 0;
    case 0x18:
        memcpy(data, device->held, size);
        return (int)size;
    case 0x09:
        memcpy(data, device->prop, size);
        return (int)size;
    default:
        assert(0);
        return -1;
    }
}

static ssize_t fake_read(int fd, void *buffer, size_t size)
{
    assert(fd >= 100 && fd < 100 + handle_count);
    struct fake_device *device = &devices[handles[fd - 100].device];
    if (device->event_cursor == device->event_count) {
        errno = EAGAIN;
        return -1;
    }
    size_t count = device->event_count - device->event_cursor;
    if (count > size / sizeof(struct input_event))
        count = size / sizeof(struct input_event);
    memcpy(buffer, device->events + device->event_cursor, count * sizeof(struct input_event));
    device->event_cursor += count;
    return (ssize_t)(count * sizeof(struct input_event));
}

static DIR *fake_opendir(const char *path)
{
    assert(strcmp(path, "/dev/input") == 0);
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
        if (!directories[i].used) {
            directories[i].used = 1;
            directories[i].next = 0;
            return (DIR *)&directories[i];
        }
    }
    assert(0);
    return NULL;
}

static struct dirent *fake_readdir(DIR *dir)
{
    struct fake_dir *cursor = (struct fake_dir *)dir;
    while (cursor->next < device_count) {
        int index = cursor->next++;
        if (devices[index].present) {
            snprintf(cursor->entry.d_name, sizeof(cursor->entry.d_name), "event%d", index);
            return &cursor->entry;
        }
    }
    return NULL;
}

static int fake_closedir(DIR *dir)
{
    ((struct fake_dir *)dir)->used = 0;
    return 0;
}

static void test_power_panel_keeps_its_identity(void)
{
    reset(1);
    panel(0, 1);
    key(0, KEY_POWER);
    struct dev_entry entry;
    assert(inspect_device("/dev/input/event0", &entry, 0) == 0);
    assert(entry.cls == IGRAB_CLASS_TOUCHSCREEN);
    assert(entry.multitouch && entry.max_x == 1200 && entry.max_y == 2000);
    assert(entry.watch_only && !entry.power_button);
    assert(!node_is_common_input(&entry));
    fake_close(entry.fd);
    assert(scan_devices() == 0);
    assert(devices[0].grabs == 0);
}

static void test_independent_power_allows_gesture_panel(void)
{
    reset(2);
    panel(0, 1);
    key(0, KEY_POWER);
    key(1, KEY_POWER);
    key(1, KEY_VOLUMEUP);
    assert(scan_devices() == 1);
    assert(g_ndevs == 1 && g_devs[0].cls == IGRAB_CLASS_TOUCHSCREEN);
    assert(devices[0].grabbed && devices[1].grabs == 0);
    refresh_power_button();
    assert(g_has_power_button && devices[0].grabbed);
    release_all();
    assert(!devices[0].grabbed);
    // A manually selected screen can still use an unselected power button.
    assert(parse_node_argument("event0", &g_selected, 0) == 0);
    assert(scan_devices() == 1);
    assert(devices[1].grabs == 0);
}

static void test_power_policy_does_not_trust_other_sources(void)
{
    const unsigned int buses[] = {BUS_USB, BUS_BLUETOOTH, BUS_VIRTUAL};
    for (size_t i = 0; i < sizeof(buses) / sizeof(buses[0]); i++) {
        reset(2);
        panel(0, 1);
        key(0, KEY_POWER);
        key(1, KEY_POWER);
        devices[1].bus = buses[i];
        assert(scan_devices() == 0);
        assert(devices[0].grabs == 0);
    }
    reset(2);
    panel(0, 1);
    key(0, KEY_POWER);
    key(1, KEY_POWER);
    assert(parse_node_argument("event1", &g_excluded, 1) == 0);
    assert(scan_devices() == 0);
    assert(devices[1].opens == 0);
    // A failed ID query cannot confirm that the power node is built-in.
    g_excluded.count = 0;
    devices[1].fail_nr = _IOC_NR(EVIOCGID);
    assert(scan_devices() == 0);
}

static void test_disappearing_power_ends_capture(void)
{
    reset(2);
    panel(0, 1);
    key(0, KEY_POWER);
    key(1, KEY_POWER);
    assert(scan_devices() == 1);
    devices[1].present = 0;
    assert(scan_new_devices() == -1);
    release_all();
    assert(!devices[0].grabbed);
}

static void test_shared_pen_and_touch(void)
{
    const int pen_keys[] = {BTN_TOOL_PEN, BTN_STYLUS, BTN_TOOL_RUBBER};
    for (size_t i = 0; i < sizeof(pen_keys) / sizeof(pen_keys[0]); i++) {
        reset(1);
        panel(0, 1);
        key(0, pen_keys[i]);
        assert(scan_devices() == 1);
        assert(g_devs[0].cls == IGRAB_CLASS_TOUCHSCREEN);
    }
    reset(1);
    panel(0, 0);
    key(0, BTN_TOOL_PEN);
    assert(scan_devices() == 0); // Pen-only ABS_X/Y digitizer.
    key(0, BTN_TOOL_FINGER);
    assert(scan_devices() == 1); // Shared single-touch finger/pen collection.
}

static void test_ordinary_devices_keep_their_policy(void)
{
    struct dev_entry entry;
    reset(1);
    panel(0, 0);
    assert(scan_devices() == 1); // Single-touch panel without any power capability.
    reset(1);
    key(0, KEY_VOLUMEUP);
    key(0, KEY_VOLUMEDOWN);
    assert(inspect_device("/dev/input/event0", &entry, 1) == 0);
    assert(entry.watch_only && !entry.grabbed);
    fake_close(entry.fd);
    reset(1);
    key(0, KEY_POWER);
    // A dock's full keyboard retains its existing Power-key exception.
    for (int code = KEY_ESC; code <= KEY_SPACE; code++)
        key(0, code);
    assert(scan_devices() == 1);
    assert(g_devs[0].cls == IGRAB_CLASS_KEYBOARD && !g_devs[0].watch_only);
    reset(1);
    key(0, BTN_LEFT);
    set_bit(devices[0].ev, EV_REL);
    set_bit(devices[0].rel, REL_X);
    set_bit(devices[0].rel, REL_Y);
    assert(scan_devices() == 1);
    assert(g_devs[0].cls == IGRAB_CLASS_MOUSE);
    release_all();
    key(0, KEY_POWER);
    assert(inspect_device("/dev/input/event0", &entry, 1) == 0);
    assert(entry.cls == IGRAB_CLASS_MOUSE && entry.watch_only && !entry.grabbed);
    fake_close(entry.fd);
    reset(1);
    axis(0, ABS_X, 32767);
    axis(0, ABS_Y, 32767);
    key(0, BTN_GAMEPAD);
    assert(scan_devices() == 0); // Stick axes alone must not look like contacts.
}

static void test_query_failures_and_legacy_fallbacks(void)
{
    struct dev_entry entry;
    reset(1);
    panel(0, 1);
    devices[0].open_error = EACCES;
    assert(inspect_device("/dev/input/event0", &entry, 0) == -1);
    devices[0].open_error = 0;
    devices[0].fail_nr = _IOC_NR(EVIOCGBIT(EV_ABS, 1));
    assert(inspect_device("/dev/input/event0", &entry, 0) == -1);
    devices[0].fail_nr = _IOC_NR(EVIOCGPROP(1));
    assert(inspect_device("/dev/input/event0", &entry, 0) == 0);
    assert(entry.cls == IGRAB_CLASS_TOUCHSCREEN && !entry.watch_only);
    fake_close(entry.fd);
    devices[0].fail_nr = -1;
    devices[0].interrupt_once = 1;
    assert(inspect_device("/dev/input/event0", &entry, 0) == 0);
    fake_close(entry.fd);
    devices[0].ranges[ABS_MT_POSITION_X].maximum = 0;
    devices[0].ranges[ABS_MT_POSITION_Y].maximum = 0;
    assert(inspect_device("/dev/input/event0", &entry, 0) == -1);
    axis(0, ABS_X, 600);
    axis(0, ABS_Y, 1000);
    assert(inspect_device("/dev/input/event0", &entry, 0) == 0);
    assert(entry.max_x == 600 && entry.max_y == 1000);
    fake_close(entry.fd);
}

static void gold_keyboard(int index)
{
    key(index, KEY_A);
    key(index, KEY_Z);
    key(index, KEY_SPACE);
    key(index, KEY_LEFTMETA);
    devices[index].bus = BUS_USB;
    devices[index].vendor = GOLD_VENDOR;
    devices[index].product = GOLD_PRODUCT;
    snprintf(devices[index].name, sizeof(devices[index].name), "%s event9", GOLD_NAME_PREFIX);
}

static void gold_capture_scopes(void)
{
    reset(3);
    key(0, KEY_A); key(0, KEY_Z); key(0, KEY_SPACE);
    gold_keyboard(1);
    panel(2, 1);
    g_source = SOURCE_GOLD;
    assert(scan_devices() == 1);
    assert(has_gold_output());
    assert(g_ndevs == 1 && g_devs[0].gold && g_devs[0].grabbed);
    assert(!devices[0].grabbed && devices[1].grabbed && !devices[2].grabbed);
    release_all();
    assert(!devices[1].grabbed); // Android owns the output again after exit.

    reset(3);
    key(0, KEY_A); key(0, KEY_Z); key(0, KEY_SPACE);
    gold_keyboard(1);
    panel(2, 1);
    g_source = SOURCE_COMBINED;
    assert(parse_node_argument("event2", &g_selected, 0) == 0);
    assert(scan_devices() == 2);
    assert(!devices[0].grabbed && devices[1].grabbed && devices[2].grabbed);

    reset(1);
    gold_keyboard(0);
    assert(scan_devices() == 0); // Plain physical mode still leaves Gold alone.
    g_source = SOURCE_GOLD;
    devices[0].product = 0x3869;
    assert(scan_devices() == 0); // A matching name cannot substitute for the ids.
    devices[0].product = GOLD_PRODUCT;
    devices[0].grab_error = EBUSY;
    assert(scan_devices() == 0 && !has_gold_output());

    reset(2);
    gold_keyboard(0); gold_keyboard(1);
    g_source = SOURCE_GOLD;
    assert(parse_node_argument("event1", &g_selected, 0) == 0);
    assert(scan_devices() == 1);
    assert(!devices[0].grabbed && devices[1].grabbed);

    reset(1);
    gold_keyboard(0);
    memset(devices[0].key, 0, sizeof(devices[0].key));
    key(0, KEY_VOLUMEUP);
    g_source = SOURCE_GOLD;
    assert(scan_devices() == 1); // Gold's remapped output is not a physical hardware button.
    assert(!g_devs[0].watch_only);
}

static void gold_waits_for_android_release(void)
{
    reset(1);
    gold_keyboard(0);
    g_source = SOURCE_GOLD;
    set_bit(devices[0].held, KEY_LEFTMETA);
    assert(scan_devices() == 0);
    assert(pending_gold() && has_gold_output());
    assert(!devices[0].grabbed);

    memset(devices[0].held, 0, sizeof(devices[0].held));
    devices[0].events[0] = (struct input_event){.type = EV_KEY, .code = KEY_LEFTMETA, .value = 0};
    devices[0].events[1] = (struct input_event){.type = EV_SYN, .code = SYN_REPORT};
    devices[0].event_count = 2;
    g_devs[0].announced = 1;
    int stream[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, stream) == 0);
    assert(pump_device(stream[0], 0, KEY_F12) == 1);
    assert(devices[0].grabbed && !pending_gold() && !g_devs[0].announced);
    char byte;
    assert(recv(stream[1], &byte, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    close(stream[0]); close(stream[1]);
}

static void gold_raw_meta_reaches_the_stream(void)
{
    reset(1);
    gold_keyboard(0);
    g_source = SOURCE_GOLD;
    assert(scan_devices() == 1);
    g_devs[0].announced = 1;
    devices[0].events[0] = (struct input_event){.type = EV_KEY, .code = KEY_LEFTMETA, .value = 1};
    devices[0].events[1] = (struct input_event){.type = EV_SYN, .code = SYN_REPORT};
    devices[0].events[2] = (struct input_event){.type = EV_KEY, .code = KEY_LEFTMETA, .value = 0};
    devices[0].events[3] = (struct input_event){.type = EV_SYN, .code = SYN_REPORT};
    devices[0].event_count = 4;
    int stream[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, stream) == 0);
    int result = pump_device(stream[0], 0, KEY_F12);
    if (result != 1)
        fprintf(stderr, "raw stream failed: result=%d errno=%d cursor=%zu\n",
                result, errno, devices[0].event_cursor);
    assert(result == 1);
    struct igrab_rec records[4];
    assert(read(stream[1], records, sizeof(records)) == (ssize_t)sizeof(records));
    assert(records[0].rtype == IGRAB_REC_EVENT && records[0].etype == EV_KEY);
    assert(records[0].code == KEY_LEFTMETA && records[0].value == 1);
    assert(records[2].code == KEY_LEFTMETA && records[2].value == 0);
    assert(records[1].etype == EV_SYN && records[3].etype == EV_SYN);
    close(stream[0]); close(stream[1]);
}

int main(void)
{
    test_power_panel_keeps_its_identity();
    test_independent_power_allows_gesture_panel();
    test_power_policy_does_not_trust_other_sources();
    test_disappearing_power_ends_capture();
    test_shared_pen_and_touch();
    test_query_failures_and_legacy_fallbacks();
    test_ordinary_devices_keep_their_policy();
    release_all();
    gold_capture_scopes();
    gold_waits_for_android_release();
    gold_raw_meta_reaches_the_stream();
    release_all();
    puts("input-grab native regressions passed (10 groups)");
    return 0;
}
