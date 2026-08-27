#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <uae/uae.h>
#include <fs/emu.h>
#include <fs/log.h>
#include <fs/thread.h>
#include "cmdfifo.h"
#include "fs-uae.h"

#ifndef WINDOWS
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/* Input events are queued as action | (state << 16), the same encoding used
 * by the netplay receiver, and are drained on the emulation thread. State is
 * recovered as a signed char, so it must fit in 8 bits. */
#define CMD_STATE_MIN (-128)
#define CMD_STATE_MAX 127

#define CMD_MAX_PENDING 64
#define CMD_MAX_LINE 512
#define CMD_DEFAULT_TAP_FRAMES 2

/* Name -> action table, generated from the input event definitions so it
 * cannot drift from the enum. */
typedef struct {
    const char *name;
    int action;
} cmd_event;

#undef DEFEVENT
#undef DEFEVENT2
#define DEFEVENT(A, ...) { #A, INPUTEVENT_ ## A },
#define DEFEVENT2(A, ...) { #A, INPUTEVENT_ ## A },

static const cmd_event g_cmd_events[] = {
#include "../inputevents.def"
    { NULL, 0 }
};

#undef DEFEVENT
#undef DEFEVENT2

typedef struct {
    int action;
    int frame;
    int used;
} cmd_pending_release;

static cmd_pending_release g_pending[CMD_MAX_PENDING];
static fs_mutex *g_cmd_mutex;
static char *g_cmd_fifo_path;
static volatile int g_cmd_fifo_stop;
static int g_cmd_frame;

static int cmd_clamp_state(long value)
{
    if (value < CMD_STATE_MIN) {
        return CMD_STATE_MIN;
    }
    if (value > CMD_STATE_MAX) {
        return CMD_STATE_MAX;
    }
    return (int) value;
}

/* Queues action | (state << 16) on the input event queue, which is drained on
 * the emulation thread. This does not depend on the window having host input
 * focus. */
static void cmd_queue_action(int action, int state)
{
    fs_emu_queue_action(action, state);
}

/* Names are matched case-insensitively, with or without the INPUTEVENT_
 * prefix. Returns 0 (INPUTEVENT_ZERO) when there is no match. */
static int cmd_lookup_event(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    if (strncasecmp(name, "INPUTEVENT_", 11) == 0) {
        name += 11;
    }
    for (int i = 0; g_cmd_events[i].name != NULL; i++) {
        if (strcasecmp(g_cmd_events[i].name, name) == 0) {
            return g_cmd_events[i].action;
        }
    }
    return 0;
}

/* Schedule a release without blocking the reader thread. */
static void cmd_schedule_release(int action, int frames)
{
    if (frames < 1) {
        frames = 1;
    }
    fs_mutex_lock(g_cmd_mutex);
    int frame = g_cmd_frame + frames;
    int scheduled = 0;
    for (int i = 0; i < CMD_MAX_PENDING; i++) {
        if (!g_pending[i].used) {
            g_pending[i].action = action;
            g_pending[i].frame = frame;
            g_pending[i].used = 1;
            scheduled = 1;
            break;
        }
    }
    fs_mutex_unlock(g_cmd_mutex);
    if (!scheduled) {
        fs_log("cmdfifo: too many pending releases, releasing immediately\n");
        cmd_queue_action(action, 0);
    }
}

void fs_uae_cmd_fifo_update(int frame)
{
    if (g_cmd_mutex == NULL) {
        return;
    }
    int due[CMD_MAX_PENDING];
    int count = 0;
    fs_mutex_lock(g_cmd_mutex);
    g_cmd_frame = frame;
    for (int i = 0; i < CMD_MAX_PENDING; i++) {
        if (g_pending[i].used && frame >= g_pending[i].frame) {
            due[count++] = g_pending[i].action;
            g_pending[i].used = 0;
        }
    }
    fs_mutex_unlock(g_cmd_mutex);
    for (int i = 0; i < count; i++) {
        cmd_queue_action(due[i], 0);
    }
}

static void cmd_handle_key(char *name, char *state_str)
{
    if (name == NULL || state_str == NULL) {
        fs_log("cmdfifo: key needs a name and a state\n");
        return;
    }
    int action = cmd_lookup_event(name);
    if (action == 0) {
        fs_log("cmdfifo: unknown input event \"%s\"\n", name);
        return;
    }
    int state = cmd_clamp_state(strtol(state_str, NULL, 10));
    cmd_queue_action(action, state);
}

static void cmd_handle_tap(char *name, char *frames_str)
{
    if (name == NULL) {
        fs_log("cmdfifo: tap needs a name\n");
        return;
    }
    int action = cmd_lookup_event(name);
    if (action == 0) {
        fs_log("cmdfifo: unknown input event \"%s\"\n", name);
        return;
    }
    int frames = CMD_DEFAULT_TAP_FRAMES;
    if (frames_str != NULL) {
        frames = (int) strtol(frames_str, NULL, 10);
    }
    cmd_queue_action(action, 1);
    cmd_schedule_release(action, frames);
}

static void cmd_handle_mouse(char *dx_str, char *dy_str)
{
    if (dx_str == NULL || dy_str == NULL) {
        fs_log("cmdfifo: mouse needs dx and dy\n");
        return;
    }
    int dx = cmd_clamp_state(strtol(dx_str, NULL, 10));
    int dy = cmd_clamp_state(strtol(dy_str, NULL, 10));
    if (dx != 0) {
        cmd_queue_action(INPUTEVENT_MOUSE1_HORIZ, dx);
    }
    if (dy != 0) {
        cmd_queue_action(INPUTEVENT_MOUSE1_VERT, dy);
    }
}

static void cmd_handle_click(char *frames_str)
{
    int frames = CMD_DEFAULT_TAP_FRAMES;
    if (frames_str != NULL) {
        frames = (int) strtol(frames_str, NULL, 10);
    }
    cmd_queue_action(INPUTEVENT_JOY1_FIRE_BUTTON, 1);
    cmd_schedule_release(INPUTEVENT_JOY1_FIRE_BUTTON, frames);
}

/* Unknown or malformed lines are logged and ignored. */
static void cmd_handle_line(char *line)
{
    char *save = NULL;
    char *command = strtok_r(line, " \t\r\n", &save);
    if (command == NULL || command[0] == '#') {
        return;
    }
    if (strcasecmp(command, "key") == 0) {
        char *name = strtok_r(NULL, " \t\r\n", &save);
        char *state = strtok_r(NULL, " \t\r\n", &save);
        cmd_handle_key(name, state);
    } else if (strcasecmp(command, "tap") == 0) {
        char *name = strtok_r(NULL, " \t\r\n", &save);
        char *frames = strtok_r(NULL, " \t\r\n", &save);
        cmd_handle_tap(name, frames);
    } else if (strcasecmp(command, "mouse") == 0) {
        char *dx = strtok_r(NULL, " \t\r\n", &save);
        char *dy = strtok_r(NULL, " \t\r\n", &save);
        cmd_handle_mouse(dx, dy);
    } else if (strcasecmp(command, "click") == 0) {
        char *frames = strtok_r(NULL, " \t\r\n", &save);
        cmd_handle_click(frames);
    } else if (strcasecmp(command, "quit") == 0) {
        fs_log("cmdfifo: quit requested\n");
        g_cmd_fifo_stop = 1;
        fs_emu_quit();
    } else {
        fs_log("cmdfifo: ignoring unknown command \"%s\"\n", command);
    }
}

#ifndef WINDOWS

static void *cmd_fifo_thread(void *data)
{
    char line[CMD_MAX_LINE];
    while (!g_cmd_fifo_stop) {
        /* Blocks until a writer opens the pipe. Reopening after end of file
         * lets several senders connect one after another. */
        FILE *f = fopen(g_cmd_fifo_path, "r");
        if (f == NULL) {
            fs_log("cmdfifo: could not open %s (%s)\n", g_cmd_fifo_path,
                    strerror(errno));
            usleep(100 * 1000);
            continue;
        }
        while (!g_cmd_fifo_stop && fgets(line, sizeof(line), f) != NULL) {
            cmd_handle_line(line);
        }
        fclose(f);
    }
    fs_log("cmdfifo: reader thread finished\n");
    return NULL;
}

void fs_uae_init_cmd_fifo(void)
{
    const char *path = getenv("FSUAE_CMD_FIFO");
    if (path == NULL || path[0] == '\0') {
        path = fs_config_get_const_string("cmd_fifo");
    }
    if (path == NULL || path[0] == '\0') {
        path = fs_config_get_const_string("cmd-fifo");
    }
    if (path == NULL || path[0] == '\0') {
        return;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        if (mkfifo(path, 0600) != 0) {
            fs_log("cmdfifo: could not create fifo %s (%s)\n", path,
                    strerror(errno));
            return;
        }
        fs_log("cmdfifo: created fifo %s\n", path);
    } else if (!S_ISFIFO(st.st_mode)) {
        fs_log("cmdfifo: %s exists and is not a fifo, ignoring\n", path);
        return;
    }

    g_cmd_fifo_path = strdup(path);
    g_cmd_mutex = fs_mutex_create();
    fs_log("cmdfifo: listening on %s\n", g_cmd_fifo_path);
    fs_thread_create("cmdfifo", cmd_fifo_thread, NULL);
}

#else

void fs_uae_init_cmd_fifo(void)
{
    if (getenv("FSUAE_CMD_FIFO") != NULL) {
        fs_log("cmdfifo: not supported on this platform\n");
    }
}

#endif
