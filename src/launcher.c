#include "kilix_land_desktop.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define LAUNCH_PATH_CAPACITY 512
#define LAUNCH_COMMAND_MAX 8
#define LAUNCH_BINDING_CAPACITY 256
/* kitten @ --password-file P launch --type=tab --self --tab-title T
 * [--cwd D] -- <command...> NULL */
#define LAUNCH_ARGV_MAX (12 + LAUNCH_COMMAND_MAX + 1)

typedef struct target_entry {
    const char *name;
    const char *label;
    /* Whether servicing this row runs a program. Internal rows are
     * handled inside desk.c and never reach a spawn. */
    bool external;
} target_entry;

static const target_entry target_table[DESK_TARGET_COUNT] = {
    [DESK_TARGET_NONE] = { "", "", false },
    [DESK_TARGET_TERMINAL] = { "terminal", "Terminal", true },
    [DESK_TARGET_CODING_AGENTS] = { "coding-agents", "Coding agents",
                                    true },
    [DESK_TARGET_FILES] = { "files", "File manager", true },
    [DESK_TARGET_MANUALS] = { "manuals", "Manuals", true },
    [DESK_TARGET_MODELS] = { "models", "Models", true },
    [DESK_TARGET_GAMES] = { "games", "Games", true },
    [DESK_TARGET_MUSIC] = { "music", "Music player", true },
    [DESK_TARGET_VOICE] = { "voice", "Voice", true },
    [DESK_TARGET_TRASH] = { "trash", "Trash", true },
    [DESK_TARGET_MAILBOX] = { "mailbox", "Mailbox", true },
    [DESK_TARGET_MAINTENANCE] = { "maintenance", "Configuration", true },
    [DESK_TARGET_WARDROBE] = { "wardrobe", "Wardrobe", false },
    [DESK_TARGET_BED] = { "bed", "Bed", false },
    [DESK_TARGET_STATUS_BOARD] = { "status-board", "Status board", false },
    [DESK_TARGET_GATE_LOCKED] = { "gate-locked", "Locked gate", false },
    [DESK_TARGET_WALK_EDITOR] = { "walk-editor", "Walk editor", true },
    [DESK_TARGET_KETTLE] = { "kettle", "Kettle", false },
    [DESK_TARGET_BROWSER] = { "browser", "Text browser", true },
    [DESK_TARGET_SETTINGS] = { "settings", "Settings", true },
    [DESK_TARGET_UPDATE] = { "update", "Update", true },
    [DESK_TARGET_CATALOG] = { "catalog", "Programs", true },
    [DESK_TARGET_DICTATION] = { "dictation", "Dictation", true },
    [DESK_TARGET_VOICE_HELP] = { "voice-help", "Voice status", true },
    [DESK_TARGET_PTY] = { "sessions", "Sessions", true },
    [DESK_TARGET_TMUX] = { "tmux", "Tmux manager", true },
    [DESK_TARGET_MUX] = { "mux", "Shared session", true },
    [DESK_TARGET_TEMPS] = { "temps", "Thermals", true },
    [DESK_TARGET_PASSWORD] = { "password", "Password", true },
    [DESK_TARGET_MANUAL] = { "manual", "System manual", true },
    [DESK_TARGET_RECOVERY] = { "recovery", "Recovery guide", true },
    [DESK_TARGET_WEB] = { "web", "Web browser", true },
    [DESK_TARGET_POWER_LOGOUT] = { "power-logout", "Log out", true },
    [DESK_TARGET_POWER_REBOOT] = { "power-reboot", "Restart", true },
    [DESK_TARGET_POWER_POWEROFF] = { "power-poweroff", "Shut down", true }
};

desk_target desk_target_from_string(const char *name)
{
    int index;
    if (!name || name[0] == '\0') return DESK_TARGET_NONE;
    for (index = 1; index < DESK_TARGET_COUNT; ++index) {
        if (strcmp(target_table[index].name, name) == 0)
            return (desk_target)index;
    }
    return DESK_TARGET_NONE;
}

const char *desk_target_name(desk_target target)
{
    if ((int)target < 0 || (int)target >= DESK_TARGET_COUNT) return "";
    return target_table[target].name;
}

const char *desk_target_label(desk_target target)
{
    if ((int)target < 0 || (int)target >= DESK_TARGET_COUNT) return "";
    return target_table[target].label;
}

bool desk_target_is_external(desk_target target)
{
    if ((int)target < 0 || (int)target >= DESK_TARGET_COUNT) return false;
    return target_table[target].external;
}

/* Power actions are the one family that does not open a tab: the machine
 * is on its way down, and a tab would close before anyone could read a
 * refusal. They run detached and report through the toast. */
static bool target_is_power(desk_target target)
{
    return target == DESK_TARGET_POWER_LOGOUT ||
           target == DESK_TARGET_POWER_REBOOT ||
           target == DESK_TARGET_POWER_POWEROFF;
}

void desk_launcher_init(desk_launcher *launcher)
{
    const char *kill_switch;
    if (!launcher) return;
    (void)memset(launcher, 0, sizeof *launcher);
    kill_switch = getenv("KILIX_LAND_DESKTOP_EXTERNAL_APPS");
    launcher->external_enabled =
        !(kill_switch && strcmp(kill_switch, "0") == 0);
}

static void set_status(desk_launcher *launcher, desk_state *state,
                       const char *text)
{
    (void)snprintf(launcher->status, sizeof launcher->status, "%s", text);
    (void)snprintf(state->toast, sizeof state->toast, "%s", text);
    state->toast_ticks = DESK_TOAST_TICKS;
}

/* Manual PATH scan; an empty component means the current directory. */
static bool which(const char *name, char *path, size_t size)
{
    const char *search = getenv("PATH");
    if (!name || name[0] == '\0' || !path || size == 0u) return false;
    if (!search || search[0] == '\0') return false;
    for (;;) {
        const char *end = strchr(search, ':');
        size_t dir_length = end ? (size_t)(end - search) : strlen(search);
        int written;
        if (dir_length == 0u)
            written = snprintf(path, size, "./%s", name);
        else
            written = snprintf(path, size, "%.*s/%s", (int)dir_length,
                               search, name);
        if (written > 0 && (size_t)written < size &&
            access(path, X_OK) == 0)
            return true;
        if (!end) return false;
        search = end + 1;
    }
}

/* Installed launcher first, then the source checkout, then PATH; NULL when
 * no kilix launcher can be found anywhere. */
static const char *resolve_kilix(char *path, size_t size)
{
    const char *base = getenv("KILIX_HOME");
    int written;
    if (base && base[0] != '\0') {
        written = snprintf(path, size, "%s/kilix", base);
        if (written > 0 && (size_t)written < size &&
            access(path, X_OK) == 0)
            return path;
    }
    base = getenv("GPU_TERMINAL_SOURCE_HOME");
    if (base && base[0] != '\0') {
        written = snprintf(path, size, "%s/kilix/kilix", base);
        if (written > 0 && (size_t)written < size &&
            access(path, X_OK) == 0)
            return path;
    }
    /* Return the verified absolute path: the command after "--" runs in the
     * kitty process, whose PATH may differ from ours. */
    if (which("kilix", path, size)) return path;
    return NULL;
}

static size_t kilix_command(char *path, size_t size, const char *subcommand,
                            const char **command)
{
    const char *kilix = resolve_kilix(path, size);
    if (!kilix) return 0u;
    command[0] = kilix;
    command[1] = subcommand;
    return 2u;
}

static size_t tool_or_kilix(const char *tool, const char *subcommand,
                            char *path, size_t size, const char **command)
{
    if (which(tool, path, size)) {
        command[0] = path;
        return 1u;
    }
    return kilix_command(path, size, subcommand, command);
}

/* The stack's own text-native file manager wins; general TUI file managers
 * are the fallback. All of them accept a start directory as argv[1]. */
static bool resolve_file_manager(char *path, size_t size)
{
    static const char *const candidates[] = {
        "kilix-file", "mc", "ranger", "nnn", "lf"
    };
    size_t index;
    for (index = 0u; index < sizeof candidates / sizeof candidates[0];
         ++index) {
        if (which(candidates[index], path, size)) return true;
    }
    return false;
}

/* Documentation shelves: the first readable candidate wins, exactly like
 * the file-manager table, so a source checkout, an installed prefix and a
 * relocated copy all resolve without the desktop knowing which it has. */
static bool first_readable(const char *const *candidates, size_t count,
                           char *path, size_t size)
{
    size_t index;
    for (index = 0u; index < count; ++index) {
        if (!candidates[index] || candidates[index][0] == '\0') continue;
        if ((size_t)snprintf(path, size, "%s", candidates[index]) >= size)
            continue;
        if (access(path, R_OK) == 0) return true;
    }
    return false;
}

static bool resolve_pager(char *path, size_t size)
{
    return which("less", path, size) || which("more", path, size);
}

/* $KILIX_HOME/README.md and friends: the stack's own manual, whichever
 * copy this machine has. */
static bool resolve_manual(char *path, size_t size)
{
    char installed[LAUNCH_PATH_CAPACITY];
    char source[LAUNCH_PATH_CAPACITY];
    char desktop[LAUNCH_PATH_CAPACITY];
    const char *candidates[4];
    const char *base;
    size_t count = 0u;

    base = getenv("KILIX_HOME");
    if (base && base[0] != '\0' &&
        (size_t)snprintf(installed, sizeof installed, "%s/README.md",
                         base) < sizeof installed)
        candidates[count++] = installed;
    base = getenv("GPU_TERMINAL_SOURCE_HOME");
    if (base && base[0] != '\0' &&
        (size_t)snprintf(source, sizeof source, "%s/kilix/README.md",
                         base) < sizeof source)
        candidates[count++] = source;
    candidates[count++] = "/usr/local/share/doc/kilix/README.md";
    base = getenv("KILIX_LAND_DESKTOP_ASSETS");
    if (!base || base[0] == '\0') base = ".";
    if ((size_t)snprintf(desktop, sizeof desktop, "%s/README.md", base) <
        sizeof desktop)
        candidates[count++] = desktop;
    return first_readable(candidates, count, path, size);
}

/* Pleb's recovery guide, in the same installed-then-source order the
 * kilix-95 Start menu uses. */
static bool resolve_recovery_guide(char *path, size_t size)
{
    char source[LAUNCH_PATH_CAPACITY];
    const char *candidates[3];
    const char *base;
    size_t count = 0u;

    base = getenv("PLEB_RECOVERY_DOC_DST");
    if (base && base[0] != '\0') candidates[count++] = base;
    candidates[count++] = "/usr/local/share/doc/pleb/RECOVERY.md";
    base = getenv("GPU_TERMINAL_SOURCE_HOME");
    if (base && base[0] != '\0' &&
        (size_t)snprintf(source, sizeof source, "%s/pleb/docs/RECOVERY.md",
                         base) < sizeof source)
        candidates[count++] = source;
    return first_readable(candidates, count, path, size);
}

/* A tool shipped beside this desktop (tools/<name>). Same convention as
 * main.c's asset_root(): the env names the checkout root that CONTAINS
 * assets/ and tools/. */
static bool resolve_own_tool(const char *name, char *path, size_t size)
{
    const char *root = getenv("KILIX_LAND_DESKTOP_ASSETS");
    int written;
    if (!root || root[0] == '\0') root = ".";
    written = snprintf(path, size, "%s/tools/%s", root, name);
    return written > 0 && (size_t)written < size &&
           access(path, R_OK) == 0;
}

/* Open only a private, user-owned binding store. The editor creates this
 * directory as 0700 and bindings.conf as 0600; the runtime independently
 * verifies that contract so a hand-written symlink or writable replacement
 * cannot become an argv source. Relative overrides intentionally fall back
 * to HOME, matching tools/land_config.py. */
static int open_bindings_file(void)
{
    char directory[LAUNCH_PATH_CAPACITY];
    const char *override_dir = getenv("KILIX_LAND_DESKTOP_CONFIG_HOME");
    const char *home = getenv("HOME");
    struct stat status;
    int directory_fd;
    int file_fd;
    int written;

    if (override_dir && override_dir[0] == '/') {
        if (strcmp(override_dir, "/") == 0) return -1;
        written = snprintf(directory, sizeof directory, "%s", override_dir);
    } else if (home && home[0] == '/') {
        written = snprintf(directory, sizeof directory,
                           "%s/.local/gpu_terminal/kilix-land-desktop",
                           home);
    } else {
        return -1;
    }
    if (written < 0 || (size_t)written >= sizeof directory) return -1;
    directory_fd = open(directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY |
                        O_NOFOLLOW);
    if (directory_fd < 0) return -1;
    if (fstat(directory_fd, &status) != 0 ||
        !S_ISDIR(status.st_mode) ||
        status.st_uid != getuid() ||
        (status.st_mode & 0077) != 0) {
        (void)close(directory_fd);
        return -1;
    }
    file_fd = openat(directory_fd, "bindings.conf",
                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    (void)close(directory_fd);
    if (file_fd < 0) return -1;
    if (fstat(file_fd, &status) != 0 ||
        !S_ISREG(status.st_mode) ||
        status.st_uid != getuid() ||
        (status.st_mode & 0022) != 0) {
        (void)close(file_fd);
        return -1;
    }
    return file_fd;
}

/* <config-home>/bindings.conf: written by tools/land_config.py, read on
 * every activation so edits apply immediately. Returns true and fills
 * kind/value when the object has an override. */
static bool lookup_binding(const desk_world *world, const desk_state *state,
                           const char *object_id, char *kind, size_t kind_size,
                           char *value, size_t value_size)
{
    char key[2 * DESK_ID_CAPACITY + 2];
    char line[LAUNCH_BINDING_CAPACITY + 2 * DESK_ID_CAPACITY + 16];
    FILE *handle;
    int file_fd;
    int written;
    bool found = false;
    if (!world || !object_id || object_id[0] == '\0') return false;
    if (state->room < 0 || state->room >= world->room_count) return false;
    written = snprintf(key, sizeof key, "%s.%s",
                       world->rooms[state->room].id, object_id);
    if (written < 0 || (size_t)written >= sizeof key) return false;
    file_fd = open_bindings_file();
    if (file_fd < 0) return false;
    handle = fdopen(file_fd, "r");
    if (!handle) {
        (void)close(file_fd);
        return false;
    }
    while (fgets(line, (int)sizeof line, handle)) {
        char *cursor = line;
        char *equals;
        char *entry_value;
        char *binding;
        size_t line_length;
        size_t key_length;
        size_t value_length;
        bool app_binding;

        line_length = strlen(line);
        if (line_length > 0u && line[line_length - 1u] != '\n' &&
            !feof(handle)) {
            int character;
            do {
                character = fgetc(handle);
            } while (character != '\n' && character != EOF);
            continue;
        }
        if (strchr(line, '\r') != NULL) continue;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor == '#' || *cursor == '\n' || *cursor == '\0') continue;
        equals = strchr(cursor, '=');
        if (!equals) continue;
        key_length = (size_t)(equals - cursor);
        while (key_length > 0u && (cursor[key_length - 1u] == ' ' ||
                                   cursor[key_length - 1u] == '\t'))
            --key_length;
        if (key_length != strlen(key) ||
            strncmp(cursor, key, key_length) != 0)
            continue;
        entry_value = equals + 1;
        while (*entry_value == ' ' || *entry_value == '\t') ++entry_value;
        entry_value[strcspn(entry_value, "\n")] = '\0';
        if (strncmp(entry_value, "app ", 4u) == 0) {
            app_binding = true;
            binding = entry_value + 4;
            (void)snprintf(kind, kind_size, "app");
        } else if (strncmp(entry_value, "folder ", 7u) == 0) {
            app_binding = false;
            binding = entry_value + 7;
            (void)snprintf(kind, kind_size, "folder");
        } else {
            continue;
        }
        value_length = strlen(binding);
        if (value_length == 0u || value_length >= value_size ||
            value_length >= LAUNCH_BINDING_CAPACITY ||
            binding[value_length - 1u] == ' ' ||
            binding[value_length - 1u] == '\t' ||
            (!app_binding && binding[0] != '/'))
            continue;
        for (size_t index = 0u; index < value_length; ++index) {
            unsigned char character = (unsigned char)binding[index];
            if (character == 127u ||
                (character < 32u &&
                 !(app_binding && character == (unsigned char)'\t'))) {
                value_length = 0u;
                break;
            }
        }
        if (value_length == 0u) continue;
        (void)snprintf(value, value_size, "%s", binding);
        found = true;
        break;
    }
    (void)fclose(handle);
    return found;
}

/* Whitespace split into an argv — no shell, no quoting, no expansion, the
 * documented bindings contract. */
static size_t split_command(char *value, const char **command,
                            size_t command_max)
{
    size_t count = 0u;
    char *cursor = value;
    for (;;) {
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor == '\0') return count;
        if (count >= command_max) return command_max + 1u;
        command[count++] = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t')
            ++cursor;
        if (*cursor == '\0') return count;
        *cursor++ = '\0';
    }
}

/* Children we have spawned but not yet reaped. Only these exact pids are
 * ever waited on: the audio runtime may own children of its own, so a
 * waitpid(-1) sweep would steal their exit statuses. */
#define LAUNCH_PENDING_MAX 8
static pid_t pending_children[LAUNCH_PENDING_MAX];

static void reap_pending_children(void)
{
    size_t index;
    for (index = 0u; index < LAUNCH_PENDING_MAX; ++index) {
        pid_t pid = pending_children[index];
        if (pid <= 0) continue;
        if (waitpid(pid, NULL, WNOHANG) != 0)
            pending_children[index] = 0;
    }
}

static void track_pending_child(pid_t pid)
{
    size_t index;
    for (index = 0u; index < LAUNCH_PENDING_MAX; ++index) {
        if (pending_children[index] <= 0) {
            pending_children[index] = pid;
            return;
        }
    }
    /* Full table: block briefly on the oldest slot rather than leak it. */
    (void)waitpid(pending_children[0], NULL, 0);
    pending_children[0] = pid;
}

static bool tab_session_ready(void);

/* A direct detached spawn for the laptop: the command is a fixed argv
 * (never a shell string) with stdio on /dev/null. `kilix laptop open`
 * (or the fallback `kilix --session`) opens the profile's own kilix
 * window; `kilix <provider>` relies on the session environment this
 * desktop already runs in to place the provider tab. The bounded wait
 * mirrors spawn_tab so an immediate failure surfaces as one instead of a
 * false "Opened" toast. */
static bool spawn_detached(const char *const *command, size_t command_count)
{
    char *argv[LAUNCH_COMMAND_MAX + 1];
    size_t index;
    posix_spawn_file_actions_t actions;
    pid_t pid = -1;
    int result;

    if (command_count == 0u || command_count > LAUNCH_COMMAND_MAX)
        return false;
    for (index = 0u; index < command_count; ++index)
        argv[index] = (char *)command[index];
    argv[command_count] = NULL;
    if (posix_spawn_file_actions_init(&actions) != 0) return false;
    if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                         "/dev/null", O_RDONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                         "/dev/null", O_WRONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                         "/dev/null", O_WRONLY, 0) != 0) {
        (void)posix_spawn_file_actions_destroy(&actions);
        return false;
    }
    result = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    (void)posix_spawn_file_actions_destroy(&actions);
    if (result != 0) return false;
    for (index = 0u; index < 20u; ++index) {
        int status = 0;
        pid_t reaped = waitpid(pid, &status, WNOHANG);
        if (reaped == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (reaped < 0) return true;
        {
            struct timespec delay = { 0, 10L * 1000L * 1000L };
            (void)nanosleep(&delay, NULL);
        }
    }
    track_pending_child(pid);
    return true;
}

/* Like spawn_detached, but for a process that must STAY alive: the
 * un-detached session window whose pid the run registry records. Here a
 * quick exit — even a clean one — means the window never came up, so it
 * is a failure rather than a success. */
static bool spawn_tracked(const char *const *command, size_t command_count,
                          pid_t *out_pid)
{
    char *argv[LAUNCH_COMMAND_MAX + 1];
    size_t index;
    posix_spawn_file_actions_t actions;
    pid_t pid = -1;
    int result;

    if (out_pid == NULL || command_count == 0u ||
        command_count > LAUNCH_COMMAND_MAX)
        return false;
    *out_pid = -1;
    for (index = 0u; index < command_count; ++index)
        argv[index] = (char *)command[index];
    argv[command_count] = NULL;
    if (posix_spawn_file_actions_init(&actions) != 0) return false;
    if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                         "/dev/null", O_RDONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                         "/dev/null", O_WRONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                         "/dev/null", O_WRONLY, 0) != 0) {
        (void)posix_spawn_file_actions_destroy(&actions);
        return false;
    }
    result = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    (void)posix_spawn_file_actions_destroy(&actions);
    if (result != 0) return false;
    for (index = 0u; index < 20u; ++index) {
        int status = 0;
        pid_t reaped = waitpid(pid, &status, WNOHANG);
        if (reaped == pid) return false; /* the window never came up */
        if (reaped < 0) break;
        {
            struct timespec delay = { 0, 10L * 1000L * 1000L };
            (void)nanosleep(&delay, NULL);
        }
    }
    track_pending_child(pid);
    *out_pid = pid;
    return true;
}

/* Whether this host's kilix knows the `laptop` verb, probed the way the
 * reference desktop's games module probes `kilix games play`: run
 * `kilix laptop help`, bounded, and require the usage token — never
 * assume, so an old host keeps this desktop's own path working, and
 * never trust exit codes alone, because an old launcher forwards unknown
 * words to the terminal engine. Probed once per process. */
static bool laptop_host_verb_available(const char *kilix)
{
    static int cached = -1;
    int pipe_fds[2];
    posix_spawn_file_actions_t actions;
    pid_t pid = -1;
    char output[256];
    size_t output_length = 0u;
    int status = 0;
    bool exited = false;
    size_t waited;
    const char *argv_words[3] = { "laptop", "help", NULL };
    char *argv[4];

    if (cached >= 0) return cached == 1;
    cached = 0;
    if (kilix == NULL || pipe(pipe_fds) != 0) return false;
    argv[0] = (char *)kilix;
    argv[1] = (char *)argv_words[0];
    argv[2] = (char *)argv_words[1];
    argv[3] = NULL;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }
    if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                         "/dev/null", O_RDONLY, 0) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1],
                                         STDOUT_FILENO) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                         "/dev/null", O_WRONLY, 0) != 0 ||
        posix_spawn_file_actions_addclose(&actions, pipe_fds[0]) != 0 ||
        posix_spawnp(&pid, kilix, &actions, NULL, argv, environ) != 0) {
        (void)posix_spawn_file_actions_destroy(&actions);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }
    (void)posix_spawn_file_actions_destroy(&actions);
    close(pipe_fds[1]);
    (void)fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
    /* Up to two seconds: read what arrives, then collect the exit. */
    for (waited = 0u; waited < 200u; ++waited) {
        ssize_t got = read(pipe_fds[0], output + output_length,
                           sizeof output - 1u - output_length);
        if (got > 0 && output_length < sizeof output - 1u)
            output_length += (size_t)got;
        if (!exited) {
            pid_t reaped = waitpid(pid, &status, WNOHANG);
            if (reaped == pid) exited = true;
            else if (reaped < 0) break;
        }
        if (exited && (got == 0 || output_length >= sizeof output - 1u))
            break;
        if (!exited || got < 0) {
            struct timespec delay = { 0, 10L * 1000L * 1000L };
            (void)nanosleep(&delay, NULL);
        }
    }
    close(pipe_fds[0]);
    if (!exited) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, &status, 0);
        return false;
    }
    output[output_length] = '\0';
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        strstr(output, "open PROFILE") != NULL)
        cached = 1;
    return cached == 1;
}

/* The config home the state stores use, created 0700 on demand so the
 * generated session file has a private place to live. */
static bool laptop_session_home(char *path, size_t size)
{
    const char *override_dir = getenv("KILIX_LAND_DESKTOP_CONFIG_HOME");
    const char *home = getenv("HOME");
    int written;
    if (override_dir && override_dir[0] == '/')
        written = snprintf(path, size, "%s", override_dir);
    else if (home && home[0] == '/')
        written = snprintf(path, size,
                           "%s/.local/gpu_terminal/kilix-land-desktop",
                           home);
    else
        return false;
    if (written < 0 || (size_t)written >= size) return false;
    {
        /* Create each missing level 0700; existing directories are left
         * exactly as found. */
        char partial[LAUNCH_PATH_CAPACITY];
        size_t offset = 1u;
        size_t length = strlen(path);
        if (length >= sizeof partial) return false;
        while (offset <= length) {
            if (path[offset] == '/' || path[offset] == '\0') {
                (void)memcpy(partial, path, offset);
                partial[offset] = '\0';
                if (mkdir(partial, 0700) != 0 && errno != EEXIST)
                    return false;
            }
            ++offset;
        }
    }
    return true;
}

/* One chosen laptop profile. The host's `kilix laptop open` is preferred
 * when the verb exists — it owns the run registry, so a session opened
 * here shows as running on every other surface. On an old host the
 * fallback keeps everything working: a desktop profile becomes `kilix
 * <provider>`; a pane profile becomes a generated kitty --session file
 * spawned un-detached so this desktop can record the window's own pid in
 * the registry itself. */
static void service_laptop(desk_launcher *launcher, desk_state *state,
                           const char *profile_id)
{
    char kilix_path[LAUNCH_PATH_CAPACITY];
    char message[DESK_TOAST_CAPACITY];
    char error[DESK_LAPTOP_ERROR_CAPACITY];
    desk_laptop_profile profile;
    const char *command[LAUNCH_COMMAND_MAX];
    const char *desktop_arguments[2] = { NULL, NULL };
    size_t command_count = 0u;
    size_t desktop_argument_count;
    const char *kilix;

    if (!launcher->external_enabled) {
        set_status(launcher, state, "External apps are disabled.");
        return;
    }
    if (!tab_session_ready()) {
        set_status(launcher, state,
                   "Kilix session not detected - launch inside kilix");
        return;
    }
    if (!desk_laptop_load(profile_id, &profile, error, sizeof error)) {
        set_status(launcher, state, error);
        return;
    }
    kilix = resolve_kilix(kilix_path, sizeof kilix_path);
    if (!kilix) {
        set_status(launcher, state, "Kilix is required for the laptop");
        return;
    }
    if (profile.desktop[0] == '\0' &&
        desk_laptop_run_status(profile.id, NULL) == 1) {
        (void)snprintf(message, sizeof message, "%s is already running",
                       profile.name);
        set_status(launcher, state, message);
        return;
    }
    if (laptop_host_verb_available(kilix)) {
        /* The host verb spawns the session and records its pid in the
         * shared run registry; this desktop only has to ask. */
        command[command_count++] = kilix;
        command[command_count++] = "laptop";
        command[command_count++] = "open";
        command[command_count++] = profile_id;
        if (spawn_detached(command, command_count)) {
            if (profile.desktop[0] == '\0') state->laptop_on = true;
            (void)snprintf(message, sizeof message, "Opened %s",
                           profile.name);
        } else {
            (void)snprintf(message, sizeof message, "Could not open %s",
                           profile.name);
        }
        set_status(launcher, state, message);
        return;
    }
    command[command_count++] = kilix;
    desktop_argument_count =
        desk_laptop_desktop_arguments(&profile, desktop_arguments);
    if (desktop_argument_count > 0u) {
        size_t index;
        for (index = 0u; index < desktop_argument_count; ++index)
            command[command_count++] = desktop_arguments[index];
    } else {
        static char session_path[LAUNCH_PATH_CAPACITY];
        char session_home[LAUNCH_PATH_CAPACITY];
        pid_t session_pid = -1;
        int written;
        if (!laptop_session_home(session_home, sizeof session_home)) {
            set_status(launcher, state,
                       "The laptop session file has no home");
            return;
        }
        written = snprintf(session_path, sizeof session_path,
                           "%s/laptop-%s.session", session_home,
                           profile.id);
        if (written < 0 || (size_t)written >= sizeof session_path) {
            set_status(launcher, state,
                       "The laptop session path is too long");
            return;
        }
        if (!desk_laptop_write_session(&profile, session_path, error,
                                       sizeof error)) {
            set_status(launcher, state, error);
            return;
        }
        command[command_count++] = "--session";
        command[command_count++] = session_path;
        /* The laptop opens a machine, not a window: the session takes
         * the screen the way this desktop did. kilix forwards anything
         * it does not recognise straight to kitty. Desktop profiles skip
         * this — a provider owns its own presentation. Un-detached, so
         * the spawned pid IS the session window and the run registry
         * records the truth; the pending-children reaper collects it. */
        command[command_count++] = "--start-as=fullscreen";
        if (spawn_tracked(command, command_count, &session_pid)) {
            (void)desk_laptop_run_record(profile.id, (long)session_pid);
            state->laptop_on = true;
            (void)snprintf(message, sizeof message, "Opened %s",
                           profile.name);
        } else {
            (void)snprintf(message, sizeof message, "Could not open %s",
                           profile.name);
        }
        set_status(launcher, state, message);
        return;
    }
    if (spawn_detached(command, command_count))
        (void)snprintf(message, sizeof message, "Opened %s", profile.name);
    else
        (void)snprintf(message, sizeof message, "Could not open %s",
                       profile.name);
    set_status(launcher, state, message);
}

/* Closing a running session: the host verb owns the registry when it
 * exists (`kilix laptop close` SIGTERMs the recorded pid and clears the
 * file); otherwise this desktop signals the recorded pid itself and the
 * per-second refresh clears the entry once the process is gone. Either
 * way the lid follows the registry, not the click. */
static void service_laptop_close(desk_launcher *launcher, desk_state *state,
                                 const char *profile_id)
{
    char kilix_path[LAUNCH_PATH_CAPACITY];
    char message[DESK_TOAST_CAPACITY];
    const char *kilix;

    kilix = launcher->external_enabled
                ? resolve_kilix(kilix_path, sizeof kilix_path)
                : NULL;
    if (kilix != NULL && laptop_host_verb_available(kilix)) {
        const char *command[4];
        command[0] = kilix;
        command[1] = "laptop";
        command[2] = "close";
        command[3] = profile_id;
        if (spawn_detached(command, 4u)) {
            (void)snprintf(message, sizeof message, "Closing %s",
                           profile_id);
            set_status(launcher, state, message);
            return;
        }
    }
    if (desk_laptop_run_terminate(profile_id))
        (void)snprintf(message, sizeof message, "Closing %s", profile_id);
    else
        (void)snprintf(message, sizeof message, "Could not close %s",
                       profile_id);
    set_status(launcher, state, message);
}

/* Session and machine power. The argv list is the fleet's one list —
 * kilix-tui-utils' privileged.py names exactly these three — mirrored
 * rather than imported because a C desktop cannot import Python. Nothing
 * here confirms: desk.c already asked, in the house's own words. */
static void service_power(desk_launcher *launcher, desk_state *state,
                          desk_target target)
{
    char resolved[LAUNCH_PATH_CAPACITY];
    const char *command[LAUNCH_COMMAND_MAX];
    size_t command_count = 0u;
    const char *done;
    const char *failed;

    if (target == DESK_TARGET_POWER_LOGOUT) {
        const char *session = getenv("XDG_SESSION_ID");
        if (!session || session[0] == '\0') {
            set_status(launcher, state, "There is no session to leave.");
            return;
        }
        if (!which("loginctl", resolved, sizeof resolved)) {
            set_status(launcher, state, "loginctl is not installed.");
            return;
        }
        command[command_count++] = resolved;
        command[command_count++] = "terminate-session";
        command[command_count++] = session;
        done = "Logging out.";
        failed = "The session would not end.";
    } else {
        if (!which("systemctl", resolved, sizeof resolved)) {
            set_status(launcher, state, "systemctl is not installed.");
            return;
        }
        command[command_count++] = resolved;
        if (target == DESK_TARGET_POWER_REBOOT) {
            command[command_count++] = "reboot";
            done = "Waking anew.";
            failed = "The house would not restart.";
        } else {
            command[command_count++] = "poweroff";
            done = "Powering down the house.";
            failed = "The house would not power down.";
        }
    }
    set_status(launcher, state,
               spawn_detached(command, command_count) ? done : failed);
}

/* The Plebian-OS default-password helper, asked once at start-up. True
 * only when it CONFIRMS the login password is still the shipped one; a
 * missing helper, a missing sudo rule, a timeout or an error all read as
 * false, so the note never appears spuriously. */
bool desk_launcher_password_is_default(void)
{
    static const char helper[] = "/usr/local/sbin/plebian-os-passwd";
    char sudo_path[LAUNCH_PATH_CAPACITY];
    char *argv[5];
    posix_spawn_file_actions_t actions;
    pid_t pid = -1;
    size_t attempt;

    if (access(helper, X_OK) != 0) return false;
    if (!which("sudo", sudo_path, sizeof sudo_path)) return false;
    argv[0] = sudo_path;
    argv[1] = (char *)"-n";
    argv[2] = (char *)helper;
    argv[3] = (char *)"check";
    argv[4] = NULL;
    if (posix_spawn_file_actions_init(&actions) != 0) return false;
    if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                         "/dev/null", O_RDONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                         "/dev/null", O_WRONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                         "/dev/null", O_WRONLY, 0) != 0) {
        (void)posix_spawn_file_actions_destroy(&actions);
        return false;
    }
    if (posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ) != 0) {
        (void)posix_spawn_file_actions_destroy(&actions);
        return false;
    }
    (void)posix_spawn_file_actions_destroy(&actions);
    /* Two seconds, then give up and answer "cannot confirm". */
    for (attempt = 0u; attempt < 200u; ++attempt) {
        int status = 0;
        pid_t reaped = waitpid(pid, &status, WNOHANG);
        struct timespec delay = { 0, 10L * 1000L * 1000L };
        if (reaped == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (reaped < 0) return false;
        (void)nanosleep(&delay, NULL);
    }
    track_pending_child(pid);
    return false;
}

/* Checked on every activation, never cached. */
static bool tab_session_ready(void)
{
    const char *listen = getenv("KITTY_LISTEN_ON");
    const char *password = getenv("KILIX_RC_PASSWORD_FILE");
    return listen && listen[0] != '\0' && password && password[0] != '\0' &&
           access(password, R_OK) == 0;
}

static bool spawn_tab(const char *label, const char *cwd,
                      const char *const *command, size_t command_count)
{
    const char *kitten = getenv("KILIX_KITTEN");
    const char *password = getenv("KILIX_RC_PASSWORD_FILE");
    char *argv[LAUNCH_ARGV_MAX];
    size_t argc = 0u;
    size_t index;
    posix_spawn_file_actions_t actions;
    pid_t pid = -1;
    int result;

    if (!password || password[0] == '\0') return false;
    if (command_count == 0u || command_count > LAUNCH_COMMAND_MAX)
        return false;
    if (!kitten || kitten[0] == '\0') kitten = "kitten";
    /* posix_spawnp takes non-const argv; the strings are never modified. */
    argv[argc++] = (char *)kitten;
    argv[argc++] = (char *)"@";
    argv[argc++] = (char *)"--password-file";
    argv[argc++] = (char *)password;
    argv[argc++] = (char *)"launch";
    argv[argc++] = (char *)"--type=tab";
    argv[argc++] = (char *)"--self";
    argv[argc++] = (char *)"--tab-title";
    argv[argc++] = (char *)label;
    if (cwd && cwd[0] != '\0') {
        argv[argc++] = (char *)"--cwd";
        argv[argc++] = (char *)cwd;
    }
    argv[argc++] = (char *)"--";
    for (index = 0u; index < command_count; ++index)
        argv[argc++] = (char *)command[index];
    argv[argc] = NULL;

    if (posix_spawn_file_actions_init(&actions) != 0) return false;
    if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                         "/dev/null", O_RDONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                         "/dev/null", O_WRONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                         "/dev/null", O_WRONLY, 0) != 0) {
        (void)posix_spawn_file_actions_destroy(&actions);
        return false;
    }
    result = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    (void)posix_spawn_file_actions_destroy(&actions);
    if (result != 0) return false;
    /* kitten normally exits within milliseconds of placing the tab. Give it
     * a bounded window so remote-control refusals surface as failures
     * instead of a false "Opened" toast; a slow child is tracked and reaped
     * on a later service pass. */
    for (index = 0u; index < 20u; ++index) {
        int status = 0;
        pid_t reaped = waitpid(pid, &status, WNOHANG);
        if (reaped == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (reaped < 0) return true;
        struct timespec delay = { 0, 10L * 1000L * 1000L };
        (void)nanosleep(&delay, NULL);
    }
    track_pending_child(pid);
    return true;
}

bool desk_launcher_service(desk_launcher *launcher, desk_state *state,
                           const desk_world *world)
{
    char resolved[LAUNCH_PATH_CAPACITY];
    char cwd_path[LAUNCH_PATH_CAPACITY];
    char message[DESK_TOAST_CAPACITY];
    char object_id[DESK_ID_CAPACITY];
    char binding_kind[8];
    char binding_value[LAUNCH_BINDING_CAPACITY];
    const char *command[LAUNCH_COMMAND_MAX];
    size_t command_count = 0u;
    const char *cwd = NULL;
    const char *label;
    desk_target target;

    if (!launcher || !state) return false;
    reap_pending_children();
    {
        char laptop_id[DESK_LAPTOP_ID_CAPACITY];
        if (desk_take_laptop_request(state, laptop_id)) {
            service_laptop(launcher, state, laptop_id);
            return true;
        }
        if (desk_take_laptop_close_request(state, laptop_id)) {
            service_laptop_close(launcher, state, laptop_id);
            return true;
        }
    }
    (void)snprintf(object_id, sizeof object_id, "%s",
                   state->pending_launch_object);
    target = desk_take_launch_request(state);
    if (target == DESK_TARGET_NONE) return false;
    if (!desk_target_is_external(target)) return true;
    label = desk_target_label(target);
    if (!launcher->external_enabled) {
        set_status(launcher, state, "External apps are disabled.");
        return true;
    }
    /* Power runs without a tab, so it also runs without the tab gate: a
     * desktop that has lost remote control must still be able to shut
     * the machine down. */
    if (target_is_power(target)) {
        service_power(launcher, state, target);
        return true;
    }
    if (!tab_session_ready()) {
        set_status(launcher, state,
                   "Kilix session not detected - launch inside kilix");
        return true;
    }
    /* Per-object overrides from the configuration TUI beat the registry. */
    if (lookup_binding(world, state, object_id, binding_kind,
                       sizeof binding_kind, binding_value,
                       sizeof binding_value)) {
        if (strcmp(binding_kind, "folder") == 0) {
            if (resolve_file_manager(resolved, sizeof resolved)) {
                command[command_count++] = resolved;
                command[command_count++] = binding_value;
            } else {
                set_status(launcher, state,
                           "No file manager is installed");
                return true;
            }
        } else {
            command_count = split_command(binding_value, command,
                                          LAUNCH_COMMAND_MAX);
            if (command_count == 0u) {
                set_status(launcher, state, "Binding has no command");
                return true;
            }
            if (command_count > LAUNCH_COMMAND_MAX) {
                set_status(launcher, state,
                           "Binding has too many arguments");
                return true;
            }
        }
        if (spawn_tab(label, NULL, command, command_count))
            (void)snprintf(message, sizeof message, "Opened %s", label);
        else
            (void)snprintf(message, sizeof message, "Could not open %s",
                           label);
        set_status(launcher, state, message);
        return true;
    }
    switch (target) {
    case DESK_TARGET_TERMINAL:
        command[command_count++] = "bash";
        command[command_count++] = "-l";
        break;
    case DESK_TARGET_CODING_AGENTS:
        command_count = tool_or_kilix("kilix-rollout-resume", "rollout",
                                      resolved, sizeof resolved, command);
        break;
    case DESK_TARGET_FILES:
        if (resolve_file_manager(resolved, sizeof resolved)) {
            const char *home = getenv("HOME");
            command[command_count++] = resolved;
            if (home && home[0] == '/') {
                command[command_count++] = home;
                cwd = home;
            }
        }
        break;
    case DESK_TARGET_MANUALS:
        command[command_count++] = "man";
        command[command_count++] = "man";
        break;
    case DESK_TARGET_MODELS:
        command_count = tool_or_kilix("kilix-bonsai", "bonsai",
                                      resolved, sizeof resolved, command);
        break;
    case DESK_TARGET_GAMES:
        if (resolve_own_tool("land_games.py", resolved, sizeof resolved)) {
            command[command_count++] = "python3";
            command[command_count++] = resolved;
        }
        break;
    case DESK_TARGET_MUSIC:
        command_count = tool_or_kilix("kilix-amp", "amp",
                                      resolved, sizeof resolved, command);
        break;
    case DESK_TARGET_BROWSER:
        /* No tool_or_kilix pair here: the browser has no installed command to
         * prefer. `kilix chawan` owns the pinned checkout and its first-run
         * build, and its binary lives inside that private prefix. */
        command_count = kilix_command(resolved, sizeof resolved, "chawan",
                                      command);
        break;
    case DESK_TARGET_VOICE:
        command_count = tool_or_kilix("kilix-tts", "tts",
                                      resolved, sizeof resolved, command);
        break;
    case DESK_TARGET_TRASH:
        if (resolve_file_manager(resolved, sizeof resolved)) {
            const char *home = getenv("HOME");
            command[command_count++] = resolved;
            if (home && home[0] != '\0') {
                int written = snprintf(cwd_path, sizeof cwd_path,
                                       "%s/.local/share/Trash/files", home);
                if (written > 0 && (size_t)written < sizeof cwd_path) {
                    command[command_count++] = cwd_path;
                    cwd = cwd_path;
                }
            }
        }
        break;
    case DESK_TARGET_MAILBOX:
        command_count = tool_or_kilix("kilix-memory", "memory",
                                      resolved, sizeof resolved, command);
        break;
    case DESK_TARGET_MAINTENANCE:
        /* The shed opens the desktop's own configuration TUI. */
        if (resolve_own_tool("land_config.py", resolved, sizeof resolved)) {
            command[command_count++] = "python3";
            command[command_count++] = resolved;
        }
        break;
    case DESK_TARGET_SETTINGS:
        command_count = tool_or_kilix("kilix-settings", "settings",
                                      resolved, sizeof resolved, command);
        break;
    case DESK_TARGET_UPDATE:
        /* The best updater this machine has: the Plebian-OS control TUI
         * owns the whole stack, and `kilix update` is the fallback for a
         * box that only installed Kilix. */
        if (which("plebian-os", resolved, sizeof resolved))
            command[command_count++] = resolved;
        else
            command_count = kilix_command(resolved, sizeof resolved,
                                          "update", command);
        break;
    case DESK_TARGET_CATALOG:
        /* A host-owned catalog wins when one exists; the desktop's own
         * tool is what makes a land-only box able to run anything at
         * all. No `kilix launcher` rung: an unknown subcommand would
         * consume the fallback. */
        if (which("kilix-launcher", resolved, sizeof resolved)) {
            command[command_count++] = resolved;
        } else if (resolve_own_tool("land_catalog.py", resolved,
                                    sizeof resolved)) {
            command[command_count++] = "python3";
            command[command_count++] = resolved;
        }
        break;
    case DESK_TARGET_DICTATION:
        command_count = tool_or_kilix("kilix-stt", "stt", resolved,
                                      sizeof resolved, command);
        break;
    case DESK_TARGET_VOICE_HELP:
        command_count = kilix_command(resolved, sizeof resolved, "voice",
                                      command);
        if (command_count > 0u) command[command_count++] = "status";
        break;
    case DESK_TARGET_PTY:
        command_count = tool_or_kilix("kilix-pty", "pty", resolved,
                                      sizeof resolved, command);
        break;
    case DESK_TARGET_TMUX:
        command_count = tool_or_kilix("tmux-tui", "tmux", resolved,
                                      sizeof resolved, command);
        break;
    case DESK_TARGET_MUX:
        command_count = kilix_command(resolved, sizeof resolved, "mux",
                                      command);
        break;
    case DESK_TARGET_TEMPS:
        command_count = tool_or_kilix("kilix-temps", "temps", resolved,
                                      sizeof resolved, command);
        break;
    case DESK_TARGET_PASSWORD:
        if (which("passwd", resolved, sizeof resolved))
            command[command_count++] = resolved;
        break;
    case DESK_TARGET_MANUAL:
    case DESK_TARGET_RECOVERY: {
        static char document[LAUNCH_PATH_CAPACITY];
        bool found = target == DESK_TARGET_MANUAL
                         ? resolve_manual(document, sizeof document)
                         : resolve_recovery_guide(document,
                                                  sizeof document);
        if (found && resolve_pager(resolved, sizeof resolved)) {
            command[command_count++] = resolved;
            command[command_count++] = document;
        }
        break;
    }
    case DESK_TARGET_WEB:
        /* `kilix open-url` picks a real browser when the machine has one
         * and renders in a pane when it does not — the same containment
         * kilix-95 uses by default. */
        command_count = kilix_command(resolved, sizeof resolved,
                                      "open-url", command);
        break;
    case DESK_TARGET_WALK_EDITOR: {
        static const char *const style_directories[DESK_CAST_COUNT] = {
            "legend", "chumrunner", "fantasy", "pleb-bound"
        };
        if (resolve_own_tool("walk_editor.py", resolved,
                             sizeof resolved)) {
            command[command_count++] = "python3";
            command[command_count++] = resolved;
            command[command_count++] = "--style";
            command[command_count++] =
                style_directories[state->profile.cast];
        }
        break;
    }
    default:
        break;
    }
    if (command_count == 0u) {
        if (target == DESK_TARGET_FILES || target == DESK_TARGET_TRASH)
            (void)snprintf(message, sizeof message,
                           "No file manager is installed");
        else if (target == DESK_TARGET_MANUAL)
            (void)snprintf(message, sizeof message,
                           "That shelf is empty on this machine");
        else if (target == DESK_TARGET_RECOVERY)
            (void)snprintf(message, sizeof message,
                           "The recovery guide is not installed");
        else
            (void)snprintf(message, sizeof message, "%s is not installed",
                           label);
        set_status(launcher, state, message);
        return true;
    }
    if (spawn_tab(label, cwd, command, command_count))
        (void)snprintf(message, sizeof message, "Opened %s", label);
    else
        (void)snprintf(message, sizeof message, "Could not open %s", label);
    set_status(launcher, state, message);
    return true;
}
