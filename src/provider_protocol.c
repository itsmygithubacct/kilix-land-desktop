#include "provider_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

extern char **environ;

enum {
    KILIX_PROVIDER_EXIT_USAGE = 2,
    KILIX_PROVIDER_EXIT_INVALID_REQUEST = 3,
    KILIX_PROVIDER_EXIT_UNAVAILABLE = 4,
    KILIX_PROVIDER_EXIT_INTERNAL_ERROR = 70,
    KILIX_STORAGE_TIMEOUT_MILLISECONDS = 5000
};

static int safe_command_path(const char *path)
{
    char copy[PATH_MAX];
    struct stat info;
    size_t length;
    if (path == NULL || path[0] != '/') return 0;
    length = strlen(path);
    if (length == 0u || length >= sizeof copy) return 0;
    (void)memcpy(copy, path, length + 1u);
    for (char *cursor = copy + 1;; ++cursor) {
        char saved;
        if (*cursor != '/' && *cursor != '\0') continue;
        saved = *cursor;
        *cursor = '\0';
        if (lstat(copy, &info) != 0 || S_ISLNK(info.st_mode)) {
            *cursor = saved;
            return 0;
        }
        *cursor = saved;
        if (saved == '\0') break;
    }
    return S_ISREG(info.st_mode) &&
           (info.st_uid == 0u || info.st_uid == geteuid()) &&
           (info.st_mode & 0022) == 0 &&
           access(path, X_OK) == 0;
}

static int contract_command(char *output, size_t output_size)
{
    const char *configured = getenv("KILIX_DESKTOP_CONTRACT_COMMAND");
    const char *prefix;
    int written;
    if (output == NULL || output_size == 0u) return 0;
    if (configured != NULL && configured[0] != '\0') {
        written = snprintf(output, output_size, "%s", configured);
    } else {
        prefix = getenv("KILIX_DESKTOP_SDK_PREFIX");
        if (prefix == NULL || prefix[0] != '/') return 0;
        written = snprintf(output, output_size,
                           "%s/bin/kilix-desktop-contract", prefix);
    }
    return written >= 0 && (size_t)written < output_size &&
           safe_command_path(output);
}

static int migration_record_present(void)
{
    const char *state = getenv("XDG_STATE_HOME");
    const char *home;
    char path[PATH_MAX];
    struct stat info;
    int written;
    if (state != NULL && state[0] != '\0') {
        if (state[0] != '/') return 1;
        written = snprintf(path, sizeof path,
                           "%s/kilix/desktops/migration-v1.json", state);
    } else {
        home = getenv("HOME");
        if (home == NULL || home[0] != '/') return 1;
        written = snprintf(path, sizeof path,
                           "%s/.local/state/kilix/desktops/migration-v1.json",
                           home);
    }
    if (written < 0 || (size_t)written >= sizeof path) return 1;
    if (lstat(path, &info) == 0) return 1;
    return errno != ENOENT;
}

bool kilix_provider_v1_storage_available(void)
{
    char command[PATH_MAX];
    return contract_command(command, sizeof command) != 0;
}

bool kilix_provider_v1_storage_required(void)
{
    const char *command = getenv("KILIX_DESKTOP_CONTRACT_COMMAND");
    const char *prefix = getenv("KILIX_DESKTOP_SDK_PREFIX");
    return (command != NULL && command[0] != '\0') ||
           (prefix != NULL && prefix[0] != '\0') ||
           migration_record_present();
}

static int exec_storage(size_t argument_count, const char *const *arguments)
{
    char command[PATH_MAX];
    char *child_argv[16];
    size_t index;
    if (argument_count + 3u > sizeof child_argv / sizeof child_argv[0] ||
        !contract_command(command, sizeof command))
        return KILIX_PROVIDER_EXIT_UNAVAILABLE;
    child_argv[0] = command;
    child_argv[1] = (char *)"storage";
    for (index = 0u; index < argument_count; ++index)
        child_argv[index + 2u] = (char *)arguments[index];
    child_argv[argument_count + 2u] = NULL;
    execve(command, child_argv, environ);
    (void)fprintf(stderr, "desktop-contract storage exec failed: %s\n",
                  strerror(errno));
    return KILIX_PROVIDER_EXIT_UNAVAILABLE;
}

static long long monotonic_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (long long)now.tv_sec * 1000ll + now.tv_nsec / 1000000ll;
}

static void kill_and_reap(pid_t child)
{
    int status;
    (void)kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
}

static int wait_child_until(pid_t child, long long deadline)
{
    int status;
    struct timespec pause = {0, 10000000L};
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (result < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (monotonic_milliseconds() < 0 ||
            monotonic_milliseconds() >= deadline) {
            kill_and_reap(child);
            errno = ETIMEDOUT;
            return 0;
        }
        while (nanosleep(&pause, &pause) < 0 && errno == EINTR) {}
        pause.tv_sec = 0;
        pause.tv_nsec = 10000000L;
    }
}

static int wait_child(pid_t child)
{
    long long now = monotonic_milliseconds();
    if (now < 0) {
        kill_and_reap(child);
        return 0;
    }
    return wait_child_until(
        child, now + KILIX_STORAGE_TIMEOUT_MILLISECONDS);
}

static int read_child_output(pid_t child, int descriptor,
                             char *output, size_t output_size,
                             size_t *used, long long deadline)
{
    struct pollfd watched = {.fd = descriptor, .events = POLLIN | POLLHUP};
    int flags = fcntl(descriptor, F_GETFL);
    if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        kill_and_reap(child);
        return 0;
    }
    for (;;) {
        ssize_t count = read(descriptor, output + *used,
                             output_size - *used - 1u);
        if (count > 0) {
            *used += (size_t)count;
            if (*used + 1u >= output_size) {
                kill_and_reap(child);
                errno = EOVERFLOW;
                return 0;
            }
            continue;
        }
        if (count == 0) return wait_child_until(child, deadline);
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            kill_and_reap(child);
            return 0;
        }
        long long now = monotonic_milliseconds();
        if (now < 0 || now >= deadline) {
            kill_and_reap(child);
            errno = ETIMEDOUT;
            return 0;
        }
        int remaining = (int)(deadline - now);
        int ready = poll(&watched, 1u, remaining);
        if (ready > 0) continue;
        if (ready < 0 && errno == EINTR) continue;
        kill_and_reap(child);
        if (ready == 0) errno = ETIMEDOUT;
        return 0;
    }
}

static int capture_storage(size_t argument_count,
                           const char *const *arguments,
                           char *output, size_t output_size)
{
    char command[PATH_MAX];
    char *child_argv[16];
    int pipes[2];
    pid_t child;
    size_t used = 0u;
    long long deadline;
    if (output == NULL || output_size < 2u ||
        argument_count + 3u > sizeof child_argv / sizeof child_argv[0] ||
        !contract_command(command, sizeof command) || pipe(pipes) != 0)
        return 0;
    child_argv[0] = command;
    child_argv[1] = (char *)"storage";
    for (size_t index = 0u; index < argument_count; ++index)
        child_argv[index + 2u] = (char *)arguments[index];
    child_argv[argument_count + 2u] = NULL;
    child = fork();
    if (child < 0) {
        close(pipes[0]);
        close(pipes[1]);
        return 0;
    }
    if (child == 0) {
        int sink;
        close(pipes[0]);
        if (dup2(pipes[1], STDOUT_FILENO) < 0) _exit(126);
        close(pipes[1]);
        sink = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (sink < 0 || dup2(sink, STDERR_FILENO) < 0) _exit(126);
        close(sink);
        execve(command, child_argv, environ);
        _exit(126);
    }
    close(pipes[1]);
    deadline = monotonic_milliseconds();
    if (deadline >= 0)
        deadline += KILIX_STORAGE_TIMEOUT_MILLISECONDS;
    if (deadline < 0 ||
        !read_child_output(child, pipes[0], output, output_size,
                           &used, deadline)) {
        close(pipes[0]);
        return 0;
    }
    close(pipes[0]);
    if (used == 0u || used >= output_size ||
        output[used - 1u] != '\n')
        return 0;
    output[--used] = '\0';
    if (used == 0u || strchr(output, '\n') != NULL ||
        strchr(output, '\r') != NULL)
        return 0;
    return 1;
}

static int run_storage_quiet(size_t argument_count,
                             const char *const *arguments)
{
    char command[PATH_MAX];
    char *child_argv[16];
    pid_t child;
    if (argument_count + 3u > sizeof child_argv / sizeof child_argv[0] ||
        !contract_command(command, sizeof command))
        return 0;
    child_argv[0] = command;
    child_argv[1] = (char *)"storage";
    for (size_t index = 0u; index < argument_count; ++index)
        child_argv[index + 2u] = (char *)arguments[index];
    child_argv[argument_count + 2u] = NULL;
    child = fork();
    if (child < 0) return 0;
    if (child == 0) {
        int sink = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (sink < 0 || dup2(sink, STDOUT_FILENO) < 0) _exit(126);
        close(sink);
        execve(command, child_argv, environ);
        _exit(126);
    }
    return wait_child(child);
}

bool kilix_provider_v1_storage_path(const char *provider_id,
                                    const char *category,
                                    char *output, size_t output_size)
{
    const char *arguments[] = {"path", provider_id, category};
    return provider_id != NULL && category != NULL &&
           capture_storage(3u, arguments, output, output_size) != 0;
}

bool kilix_provider_v1_storage_value(const char *provider_id,
                                     const char *key,
                                     char *output, size_t output_size)
{
    const char *arguments[] = {"value", provider_id, key};
    return provider_id != NULL && key != NULL &&
           capture_storage(3u, arguments, output, output_size) != 0;
}

bool kilix_provider_v1_storage_set(const char *provider_id,
                                   const char *key, const char *value)
{
    const char *arguments[] = {"set", provider_id, key, value};
    return provider_id != NULL && key != NULL && value != NULL &&
           run_storage_quiet(4u, arguments) != 0;
}

static int emit_description(const struct kilix_provider_v1 *provider)
{
    return printf(
        "{\"capabilities\":%s,\"config_schema\":"
        "\"kilix.desktop.config.provider.v1\",\"contract_version\":1,"
        "\"display_modes\":%s,\"provider_id\":\"%s\","
        "\"provider_version\":\"%s\",\"required_capabilities\":%s,"
        "\"schema_version\":1}\n",
        provider->capabilities_json, provider->display_modes_json,
        provider->provider_id, provider->provider_version,
        provider->required_capabilities_json) < 0
               ? KILIX_PROVIDER_EXIT_INTERNAL_ERROR
               : 0;
}

static int emit_check(const struct kilix_provider_v1 *provider)
{
    const char *check_status = provider->check_ready ? "pass" : "unavailable";
    const char *status = provider->check_ready ? "ready" : "unavailable";
    const char *summary = provider->check_ready
                              ? provider->ready_summary
                              : provider->unavailable_summary;
    const char *check_summary = provider->check_ready
                                    ? provider->check_pass_summary
                                    : provider->check_unavailable_summary;
    return printf(
        "{\"checks\":[{\"id\":\"%s\",\"required\":true,"
        "\"status\":\"%s\",\"summary\":\"%s\"}],"
        "\"contract_version\":1,\"provider_id\":\"%s\","
        "\"schema_version\":1,\"status\":\"%s\",\"summary\":\"%s\"}\n",
        provider->check_id, check_status, check_summary,
        provider->provider_id, status, summary) < 0
               ? KILIX_PROVIDER_EXIT_INTERNAL_ERROR
               : 0;
}

static int emit_config_schema(const struct kilix_provider_v1 *provider)
{
    return printf(
        "{\"$id\":\"https://schemas.kilix.org/desktop/config/%s/v1\","
        "\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
        "\"additionalProperties\":true,\"properties\":{},\"type\":\"object\","
        "\"x-kilix-contract-version\":1,\"x-kilix-provider-id\":\"%s\"}\n",
        provider->provider_id, provider->provider_id) < 0
               ? KILIX_PROVIDER_EXIT_INTERNAL_ERROR
               : 0;
}

static int emit_config(const struct kilix_provider_v1 *provider)
{
    return printf(
        "{\"contract_version\":1,\"provider_id\":\"%s\",\"revision\":0,"
        "\"schema_version\":1,\"values\":{}}\n",
        provider->provider_id) < 0
               ? KILIX_PROVIDER_EXIT_INTERNAL_ERROR
               : 0;
}

static int ascii_alphanumeric(unsigned char character)
{
    return (character >= (unsigned char)'A' &&
            character <= (unsigned char)'Z') ||
           (character >= (unsigned char)'a' &&
            character <= (unsigned char)'z') ||
           (character >= (unsigned char)'0' &&
            character <= (unsigned char)'9');
}

static int valid_session_id(const char *value)
{
    size_t index;
    size_t length;
    if (value == NULL) return 0;
    length = strlen(value);
    if (length == 0u || length > 128u || !ascii_alphanumeric((unsigned char)value[0]))
        return 0;
    for (index = 1u; index < length; ++index) {
        unsigned char character = (unsigned char)value[index];
        if (!ascii_alphanumeric(character) && character != (unsigned char)'.' &&
            character != (unsigned char)'_' && character != (unsigned char)'-')
            return 0;
    }
    return 1;
}

static int valid_version(const char *value)
{
    size_t index;
    size_t length;
    if (value == NULL) return 0;
    length = strlen(value);
    if (length == 0u || length > 64u) return 0;
    for (index = 0u; index < length; ++index) {
        unsigned char character = (unsigned char)value[index];
        if (character < 0x20u || character == 0x7fu) return 0;
    }
    return 1;
}

static int unavailable(const char *provider_id, const char *feature)
{
    (void)fprintf(stderr, "%s: %s is unavailable\n", provider_id, feature);
    return KILIX_PROVIDER_EXIT_UNAVAILABLE;
}

int kilix_provider_v1_dispatch(int argc, char **argv,
                               const struct kilix_provider_v1 *provider)
{
    if (argc < 2 || strcmp(argv[1], "provider") != 0)
        return KILIX_PROVIDER_NOT_HANDLED;
    if (provider == NULL) return KILIX_PROVIDER_EXIT_INTERNAL_ERROR;
    if (kilix_provider_v1_storage_required() &&
        !kilix_provider_v1_storage_available())
        return unavailable(provider->provider_id,
                           "authoritative persistence resolver");

    if (argc == 4 && strcmp(argv[2], "describe") == 0 &&
        strcmp(argv[3], "--json") == 0)
        return emit_description(provider);
    if (argc == 4 && strcmp(argv[2], "check") == 0 &&
        strcmp(argv[3], "--json") == 0)
        return emit_check(provider);
    if (argc == 5 && strcmp(argv[2], "config") == 0 &&
        strcmp(argv[3], "schema") == 0 && strcmp(argv[4], "--json") == 0) {
        if (kilix_provider_v1_storage_available()) {
            const char *arguments[] = {"schema", provider->provider_id};
            return exec_storage(2u, arguments);
        }
        return emit_config_schema(provider);
    }
    if ((argc == 5 || argc == 6) && strcmp(argv[2], "config") == 0 &&
        strcmp(argv[3], "get") == 0 &&
        strcmp(argv[argc - 1], "--json") == 0 &&
        (argc == 5 || argv[4][0] != '\0')) {
        if (kilix_provider_v1_storage_available()) {
            const char *arguments[] = {"get", provider->provider_id, argv[4]};
            return exec_storage(argc == 5 ? 2u : 3u, arguments);
        }
        return emit_config(provider);
    }
    if (argc == 6 && strcmp(argv[2], "config") == 0 &&
        strcmp(argv[3], "set") == 0) {
        if (kilix_provider_v1_storage_available()) {
            const char *arguments[] = {
                "set", provider->provider_id, argv[4], argv[5]
            };
            return exec_storage(4u, arguments);
        }
        return unavailable(provider->provider_id,
                           "protocol configuration writes");
    }

    if (argc >= 3 && strcmp(argv[2], "launch") == 0) {
        if (argc == 3) return KILIX_PROVIDER_LAUNCH;
        if (argc == 5 && strcmp(argv[3], "--session-id") == 0) {
            if (!valid_session_id(argv[4])) {
                (void)fprintf(stderr, "%s: invalid provider session ID\n",
                              provider->provider_id);
                return KILIX_PROVIDER_EXIT_INVALID_REQUEST;
            }
            if (setenv("KILIX_DESKTOP_SESSION_ID", argv[4], 1) != 0)
                return KILIX_PROVIDER_EXIT_INTERNAL_ERROR;
            return KILIX_PROVIDER_LAUNCH;
        }
        (void)fprintf(stderr,
                      "%s: usage: provider launch [--session-id ID]\n",
                      provider->provider_id);
        return KILIX_PROVIDER_EXIT_USAGE;
    }

    if (argc >= 3 && strcmp(argv[2], "screenshot") == 0) {
        if (argc < 4 || argv[3][0] == '\0') {
            (void)fprintf(stderr,
                          "%s: usage: provider screenshot OUTPUT [OPTIONS...]\n",
                          provider->provider_id);
            return KILIX_PROVIDER_EXIT_USAGE;
        }
        if (!provider->screenshot_available)
            return unavailable(provider->provider_id, "headless screenshot");
        return KILIX_PROVIDER_SCREENSHOT;
    }

    if (argc >= 3 && strcmp(argv[2], "migrate") == 0) {
        if ((argc != 5 && argc != 6) || strcmp(argv[3], "--from") != 0 ||
            (argc == 6 && strcmp(argv[5], "--dry-run") != 0)) {
            (void)fprintf(stderr,
                          "%s: usage: provider migrate --from VERSION [--dry-run]\n",
                          provider->provider_id);
            return KILIX_PROVIDER_EXIT_USAGE;
        }
        if (!valid_version(argv[4])) {
            (void)fprintf(stderr, "%s: invalid migration source version\n",
                          provider->provider_id);
            return KILIX_PROVIDER_EXIT_INVALID_REQUEST;
        }
        if (kilix_provider_v1_storage_available()) {
            const char *arguments[] = {
                "migrate", provider->provider_id, "--from", argv[4], argv[5]
            };
            return exec_storage(argc == 5 ? 4u : 5u, arguments);
        }
        return unavailable(provider->provider_id,
                           "protocol persistence migration");
    }

    (void)fprintf(stderr, "%s: unknown provider protocol command\n",
                  provider->provider_id);
    return KILIX_PROVIDER_EXIT_USAGE;
}
