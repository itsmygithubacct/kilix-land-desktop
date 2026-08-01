#include "kilix_land_desktop.h"

#include <fcntl.h>
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
} target_entry;

static const target_entry target_table[DESK_TARGET_COUNT] = {
    [DESK_TARGET_NONE] = { "", "" },
    [DESK_TARGET_TERMINAL] = { "terminal", "Terminal" },
    [DESK_TARGET_CODING_AGENTS] = { "coding-agents", "Coding agents" },
    [DESK_TARGET_FILES] = { "files", "File manager" },
    [DESK_TARGET_MANUALS] = { "manuals", "Manuals" },
    [DESK_TARGET_MODELS] = { "models", "Models" },
    [DESK_TARGET_GAMES] = { "games", "Games" },
    [DESK_TARGET_MUSIC] = { "music", "Music player" },
    [DESK_TARGET_VOICE] = { "voice", "Voice" },
    [DESK_TARGET_TRASH] = { "trash", "Trash" },
    [DESK_TARGET_MAILBOX] = { "mailbox", "Mailbox" },
    [DESK_TARGET_MAINTENANCE] = { "maintenance", "Configuration" },
    [DESK_TARGET_WARDROBE] = { "wardrobe", "Wardrobe" },
    [DESK_TARGET_BED] = { "bed", "Bed" },
    [DESK_TARGET_STATUS_BOARD] = { "status-board", "Status board" },
    [DESK_TARGET_GATE_LOCKED] = { "gate-locked", "Locked gate" },
    [DESK_TARGET_WALK_EDITOR] = { "walk-editor", "Walk editor" },
    [DESK_TARGET_KETTLE] = { "kettle", "Kettle" }
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
    return (target >= DESK_TARGET_TERMINAL &&
            target <= DESK_TARGET_MAINTENANCE) ||
           target == DESK_TARGET_WALK_EDITOR;
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
    case DESK_TARGET_GAMES: {
        /* Same convention as main.c's asset_root(): the env names the
         * checkout root that CONTAINS assets/ and tools/. */
        const char *root = getenv("KILIX_LAND_DESKTOP_ASSETS");
        int written;
        if (!root || root[0] == '\0') root = ".";
        written = snprintf(resolved, sizeof resolved,
                           "%s/tools/land_games.py", root);
        if (written > 0 && (size_t)written < sizeof resolved &&
            access(resolved, R_OK) == 0) {
            command[command_count++] = "python3";
            command[command_count++] = resolved;
        }
        break;
    }
    case DESK_TARGET_MUSIC:
        command_count = tool_or_kilix("kilix-amp", "amp",
                                      resolved, sizeof resolved, command);
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
    case DESK_TARGET_MAINTENANCE: {
        /* The shed opens the desktop's own configuration TUI. */
        const char *root = getenv("KILIX_LAND_DESKTOP_ASSETS");
        int written;
        if (!root || root[0] == '\0') root = ".";
        written = snprintf(resolved, sizeof resolved,
                           "%s/tools/land_config.py", root);
        if (written > 0 && (size_t)written < sizeof resolved &&
            access(resolved, R_OK) == 0) {
            command[command_count++] = "python3";
            command[command_count++] = resolved;
        }
        break;
    }
    case DESK_TARGET_WALK_EDITOR: {
        static const char *const style_directories[DESK_CAST_COUNT] = {
            "legend", "chumrunner", "fantasy", "pleb-bound"
        };
        const char *root = getenv("KILIX_LAND_DESKTOP_ASSETS");
        int written;
        if (!root || root[0] == '\0') root = ".";
        written = snprintf(resolved, sizeof resolved,
                           "%s/tools/walk_editor.py", root);
        if (written > 0 && (size_t)written < sizeof resolved &&
            access(resolved, R_OK) == 0) {
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
