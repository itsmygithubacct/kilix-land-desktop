/* laptop.c — laptop profile discovery, parsing, session emission.
 * Contract: src/laptop.h; format spec: docs/APPS.md ("The laptop").
 *
 * Everything here is deliberately strict: a profile that fails any rule is
 * rejected whole rather than partially honored, because these files feed a
 * process launch. Values never pass through a shell — panes are emitted as
 * kitty --session lines whose quoting rules are enforced at parse time by
 * refusing the two characters (double quote, control bytes) that could
 * change how kitty splits them.
 */
#include "laptop.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DESK_LAPTOP_FILE_CAPACITY (16 * 1024)

static bool copy_text(char *dst, size_t size, const char *src)
{
    int n;
    if (dst == NULL || size == 0 || src == NULL) return false;
    n = snprintf(dst, size, "%s", src);
    return n >= 0 && (size_t)n < size;
}

/* The format here is always one of this file's string literals carrying
 * only %s conversions; the wrapper exists so every caller keeps the same
 * bounds-and-truncation check. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
static bool format_text(char *dst, size_t size, const char *format,
                        const char *a, const char *b)
{
    int n = snprintf(dst, size, format, a, b);
    return n >= 0 && (size_t)n < size;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0)
        (void)snprintf(error, error_size, "%s", message);
}

bool desk_laptop_directory(char *path, size_t size)
{
    const char *override = getenv("KILIX_LAPTOP_PROFILES");
    const char *home;
    if (path == NULL || size == 0) return false;
    if (override != NULL && override[0] != '\0') {
        if (override[0] != '/') return false;
        return copy_text(path, size, override);
    }
    home = getenv("HOME");
    if (home == NULL || home[0] != '/') return false;
    return format_text(path, size, "%s/.local/gpu_terminal/laptop", home,
                       NULL);
}

static bool valid_id(const char *id)
{
    size_t i;
    if (id == NULL || id[0] == '\0' || id[0] == '.') return false;
    if (strlen(id) >= DESK_LAPTOP_ID_CAPACITY) return false;
    for (i = 0; id[i] != '\0'; i++) {
        char c = id[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            continue;
        return false;
    }
    return true;
}

/* Directories on the way to the profile root are created 0700; anything
 * already present is left exactly as found. */
static bool ensure_directory(const char *path)
{
    struct stat info;
    if (stat(path, &info) == 0) return S_ISDIR(info.st_mode);
    if (errno != ENOENT) return false;
    {
        char parent[PATH_MAX];
        const char *slash = strrchr(path, '/');
        if (slash == NULL || slash == path) return false;
        if ((size_t)(slash - path) >= sizeof parent) return false;
        memcpy(parent, path, (size_t)(slash - path));
        parent[slash - path] = '\0';
        if (!ensure_directory(parent)) return false;
    }
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

static bool read_small_file(const char *path, char *buffer, size_t size,
                            size_t *length)
{
    struct stat info;
    ssize_t got;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || (size_t)info.st_size >= size) {
        close(fd);
        return false;
    }
    got = read(fd, buffer, (size_t)info.st_size);
    close(fd);
    if (got < 0 || (size_t)got != (size_t)info.st_size) return false;
    buffer[got] = '\0';
    *length = (size_t)got;
    return true;
}

/* 0600 same-directory temp file + rename: a reader sees the old file or
 * the whole new one, never a prefix. */
static bool write_private_file(const char *path, const char *data,
                               size_t length)
{
    char temp[PATH_MAX];
    int fd;
    ssize_t put;
    if (!format_text(temp, sizeof temp, "%s.tmp", path, NULL)) return false;
    fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
              0600);
    if (fd < 0) return false;
    put = write(fd, data, length);
    if (put < 0 || (size_t)put != length || close(fd) != 0) {
        if (put >= 0 && (size_t)put != length) close(fd);
        unlink(temp);
        return false;
    }
    if (rename(temp, path) != 0) {
        unlink(temp);
        return false;
    }
    return true;
}

static void seed_examples(const char *directory, const char *seed_directory)
{
    DIR *seeds;
    struct dirent *entry;
    if (seed_directory == NULL) return;
    seeds = opendir(seed_directory);
    if (seeds == NULL) return;
    while ((entry = readdir(seeds)) != NULL) {
        char source[PATH_MAX];
        char destination[PATH_MAX];
        char contents[DESK_LAPTOP_FILE_CAPACITY];
        size_t length;
        const char *dot = strrchr(entry->d_name, '.');
        if (dot == NULL || strcmp(dot, ".profile") != 0) continue;
        if (!format_text(source, sizeof source, "%s/%s", seed_directory,
                         entry->d_name) ||
            !format_text(destination, sizeof destination, "%s/%s",
                         directory, entry->d_name))
            continue;
        if (!read_small_file(source, contents, sizeof contents, &length))
            continue;
        (void)write_private_file(destination, contents, length);
    }
    closedir(seeds);
}

static int compare_ids(const void *left, const void *right)
{
    return strcmp((const char *)left, (const char *)right);
}

int desk_laptop_scan(const char *seed_directory, desk_laptop_list *list)
{
    char directory[PATH_MAX];
    struct stat info;
    DIR *entries;
    struct dirent *entry;

    if (list == NULL) return -1;
    memset(list, 0, sizeof *list);
    if (!desk_laptop_directory(directory, sizeof directory)) return -1;
    if (stat(directory, &info) != 0) {
        if (errno != ENOENT || !ensure_directory(directory)) return -1;
        seed_examples(directory, seed_directory);
    } else if (!S_ISDIR(info.st_mode)) {
        return -1;
    }
    entries = opendir(directory);
    if (entries == NULL) return -1;
    while ((entry = readdir(entries)) != NULL &&
           list->count < DESK_LAPTOP_PROFILES_MAX) {
        char id[DESK_LAPTOP_ID_CAPACITY];
        const char *dot = strrchr(entry->d_name, '.');
        if (dot == NULL || strcmp(dot, ".profile") != 0) continue;
        if ((size_t)(dot - entry->d_name) >= sizeof id) continue;
        memcpy(id, entry->d_name, (size_t)(dot - entry->d_name));
        id[dot - entry->d_name] = '\0';
        if (!valid_id(id)) continue;
        (void)copy_text(list->ids[list->count], sizeof list->ids[0], id);
        list->count++;
    }
    closedir(entries);
    qsort(list->ids, (size_t)list->count, sizeof list->ids[0], compare_ids);
    return list->count;
}

static bool valid_value(const char *value)
{
    size_t i;
    for (i = 0; value[i] != '\0'; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20u || c == 0x7fu || c == '"') return false;
    }
    return true;
}

static bool valid_ssh_destination(const char *value)
{
    size_t i;
    if (value[0] == '\0' || value[0] == '-') return false;
    for (i = 0; value[i] != '\0'; i++) {
        char c = value[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
            c == '@')
            continue;
        return false;
    }
    return true;
}

static bool known_desktop(const char *word)
{
    static const char *const providers[] = {
        "desktop", "95", "xp", "cap", "tui", "land"
    };
    size_t i;
    for (i = 0; i < sizeof providers / sizeof providers[0]; i++)
        if (strcmp(word, providers[i]) == 0) return true;
    return false;
}

typedef struct profile_parser {
    desk_laptop_profile *profile;
    bool saw_pane[DESK_LAPTOP_PANES_MAX];
    bool saw_pane_key;
    char *error;
    size_t error_size;
} profile_parser;

static bool parser_fail(profile_parser *parser, const char *message)
{
    set_error(parser->error, parser->error_size, message);
    return false;
}

static bool assign_pane_key(profile_parser *parser, const char *key,
                            const char *value)
{
    desk_laptop_profile *profile = parser->profile;
    char *end = NULL;
    long pane = strtol(key + 5, &end, 10);
    desk_laptop_pane *slot;
    if (end == NULL || *end != '.' || end == key + 5)
        return parser_fail(parser, "Unknown profile key.");
    if (pane < 1 || pane > DESK_LAPTOP_PANES_MAX)
        return parser_fail(parser, "Pane numbers run 1..8.");
    slot = &profile->panes[pane - 1];
    parser->saw_pane[pane - 1] = true;
    parser->saw_pane_key = true;
    if (pane > profile->pane_count) profile->pane_count = (int)pane;
    end++;
    if (strcmp(end, "title") == 0) {
        if (strlen(value) >= sizeof slot->title)
            return parser_fail(parser, "A pane title is too long.");
        return copy_text(slot->title, sizeof slot->title, value);
    }
    if (strcmp(end, "cwd") == 0) {
        if (strlen(value) >= sizeof slot->cwd)
            return parser_fail(parser, "A pane directory is too long.");
        return copy_text(slot->cwd, sizeof slot->cwd, value);
    }
    if (strcmp(end, "ssh") == 0) {
        if (strlen(value) >= sizeof slot->ssh)
            return parser_fail(parser, "A pane destination is too long.");
        if (!valid_ssh_destination(value))
            return parser_fail(parser,
                               "ssh destinations are [user@]host only.");
        return copy_text(slot->ssh, sizeof slot->ssh, value);
    }
    if (strcmp(end, "cmd") == 0) {
        if (strlen(value) >= sizeof slot->cmd)
            return parser_fail(parser, "A pane command is too long.");
        return copy_text(slot->cmd, sizeof slot->cmd, value);
    }
    return parser_fail(parser, "Unknown profile key.");
}

static bool assign_key(profile_parser *parser, const char *key,
                       const char *value)
{
    desk_laptop_profile *profile = parser->profile;
    if (!valid_value(value))
        return parser_fail(parser,
                           "Profile values cannot hold quotes or "
                           "control characters.");
    if (strcmp(key, "name") == 0) {
        if (value[0] == '\0' || strlen(value) >= sizeof profile->name)
            return parser_fail(parser, "The profile name will not fit.");
        return copy_text(profile->name, sizeof profile->name, value);
    }
    if (strcmp(key, "desktop") == 0) {
        if (!known_desktop(value))
            return parser_fail(parser,
                               "desktop= must name a kilix provider.");
        return copy_text(profile->desktop, sizeof profile->desktop, value);
    }
    if (strcmp(key, "layout") == 0) {
        if (strcmp(value, "splits") == 0) {
            profile->tabs = false;
            return true;
        }
        if (strcmp(value, "tabs") == 0) {
            profile->tabs = true;
            return true;
        }
        return parser_fail(parser, "layout= must be splits or tabs.");
    }
    if (strncmp(key, "pane.", 5) == 0)
        return assign_pane_key(parser, key, value);
    return parser_fail(parser, "Unknown profile key.");
}

static bool parse_profile(profile_parser *parser, char *contents)
{
    char *cursor = contents;
    while (cursor != NULL && *cursor != '\0') {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        char *equals;
        if (newline != NULL) {
            *newline = '\0';
            cursor = newline + 1;
        } else {
            cursor = NULL;
        }
        {
            size_t length = strlen(line);
            if (length > 0 && line[length - 1] == '\r')
                line[length - 1] = '\0';
        }
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '\0' || *line == '#') continue;
        equals = strchr(line, '=');
        if (equals == NULL)
            return parser_fail(parser, "Profile lines are KEY=value.");
        *equals = '\0';
        if (!assign_key(parser, line, equals + 1)) return false;
    }
    return true;
}

static bool finish_profile(profile_parser *parser)
{
    desk_laptop_profile *profile = parser->profile;
    int i;
    if (profile->desktop[0] != '\0') {
        if (parser->saw_pane_key)
            return parser_fail(parser,
                               "A profile is a desktop or panes, not "
                               "both.");
        return true;
    }
    if (profile->pane_count == 0)
        return parser_fail(parser, "A profile needs pane.1 or desktop=.");
    for (i = 0; i < profile->pane_count; i++)
        if (!parser->saw_pane[i])
            return parser_fail(parser, "Pane numbers must be contiguous.");
    return true;
}

bool desk_laptop_load(const char *id, desk_laptop_profile *profile, char *error,
                 size_t error_size)
{
    char directory[PATH_MAX];
    char path[PATH_MAX];
    char contents[DESK_LAPTOP_FILE_CAPACITY];
    size_t length;
    profile_parser parser;

    set_error(error, error_size, "");
    if (profile == NULL) return false;
    memset(profile, 0, sizeof *profile);
    if (!valid_id(id)) {
        set_error(error, error_size, "That profile name is not valid.");
        return false;
    }
    if (!desk_laptop_directory(directory, sizeof directory) ||
        !format_text(path, sizeof path, "%s/%s.profile", directory, id)) {
        set_error(error, error_size, "No laptop profile directory.");
        return false;
    }
    if (!read_small_file(path, contents, sizeof contents, &length)) {
        set_error(error, error_size, "That profile cannot be read.");
        return false;
    }
    (void)copy_text(profile->id, sizeof profile->id, id);
    (void)copy_text(profile->name, sizeof profile->name, id);
    memset(&parser, 0, sizeof parser);
    parser.profile = profile;
    parser.error = error;
    parser.error_size = error_size;
    if (!parse_profile(&parser, contents) || !finish_profile(&parser)) {
        memset(profile, 0, sizeof *profile);
        return false;
    }
    return true;
}

size_t desk_laptop_desktop_arguments(const desk_laptop_profile *profile,
                                const char *arguments[2])
{
    if (profile == NULL || arguments == NULL ||
        profile->desktop[0] == '\0')
        return 0;
    if (strcmp(profile->desktop, "desktop") == 0) {
        arguments[0] = "desktop";
        return 1;
    }
    if (strcmp(profile->desktop, "95") == 0) {
        arguments[0] = "desktop";
        arguments[1] = "95";
        return 2;
    }
    arguments[0] = profile->desktop;
    return 1;
}

typedef struct session_text {
    char text[8 * 1024];
    size_t length;
    bool overflow;
} session_text;

/* Formats are this file's literals with only %s conversions (see
 * format_text above). */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
static void session_append(session_text *session, const char *format,
                           const char *a, const char *b)
{
    int n;
    if (session->overflow) return;
    n = snprintf(session->text + session->length,
                 sizeof session->text - session->length, format, a, b);
    if (n < 0 || (size_t)n >= sizeof session->text - session->length) {
        session->overflow = true;
        return;
    }
    session->length += (size_t)n;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* A leading ~/ becomes the caller's home so kitty's cd sees an absolute
 * path; anything else is emitted as written. */
static const char *expanded_cwd(const char *cwd, char *buffer, size_t size)
{
    const char *home = getenv("HOME");
    if (cwd[0] == '~' && (cwd[1] == '\0' || cwd[1] == '/') &&
        home != NULL && home[0] == '/') {
        if (format_text(buffer, size, "%s%s", home, cwd + 1))
            return buffer;
    }
    return cwd;
}

static void append_pane(session_text *session, const desk_laptop_profile *profile,
                        const desk_laptop_pane *pane, int index)
{
    char expanded[DESK_LAPTOP_VALUE_CAPACITY + PATH_MAX];
    /* Splits alternate right/down so a profile tiles without nesting
     * knowledge; the first pane and every tab pane need no location. */
    const char *location = index % 2 == 1 ? "--location=vsplit "
                                          : "--location=hsplit ";
    if (profile->tabs) {
        session_append(session, "new_tab %s\n",
                       pane->title[0] != '\0' ? pane->title
                                              : profile->name, NULL);
        location = "";
    } else if (index == 0) {
        location = "";
    }
    if (pane->title[0] != '\0')
        session_append(session, "title %s\n", pane->title, NULL);
    if (pane->ssh[0] != '\0') {
        /* The remote pane: directory and command run on the far side.
         * Values were validated quote- and control-free, so the quoted
         * argument reaches ssh exactly as written here. */
        session_append(session, "launch %sssh -t %s", location, pane->ssh);
        if (pane->cmd[0] != '\0' && pane->cwd[0] != '\0')
            session_append(session, " \"cd %s && exec %s\"\n", pane->cwd,
                           pane->cmd);
        else if (pane->cmd[0] != '\0')
            session_append(session, " \"exec %s\"\n", pane->cmd, NULL);
        else if (pane->cwd[0] != '\0')
            session_append(session,
                           " \"cd %s && exec \\$SHELL -l\"\n",
                           pane->cwd, NULL);
        else
            session_append(session, "\n", NULL, NULL);
        return;
    }
    if (pane->cwd[0] != '\0')
        session_append(session, "cd %s\n",
                       expanded_cwd(pane->cwd, expanded, sizeof expanded),
                       NULL);
    if (pane->cmd[0] != '\0')
        session_append(session, "launch %ssh -lc \"%s\"\n", location,
                       pane->cmd);
    else if (location[0] != '\0')
        session_append(session, "launch %s\n", location, NULL);
    else
        session_append(session, "launch\n", NULL, NULL);
}

bool desk_laptop_write_session(const desk_laptop_profile *profile, const char *path,
                          char *error, size_t error_size)
{
    session_text session;
    int i;

    set_error(error, error_size, "");
    if (profile == NULL || path == NULL || path[0] != '/') {
        set_error(error, error_size, "No session path.");
        return false;
    }
    if (profile->desktop[0] != '\0' || profile->pane_count == 0) {
        set_error(error, error_size, "Not a pane profile.");
        return false;
    }
    memset(&session, 0, sizeof session);
    session_append(&session,
                   "# Generated by the kilix desktop laptop from "
                   "%s.profile; do not edit.\n", profile->id, NULL);
    session_append(&session, "os_window_title %s\n", profile->name, NULL);
    if (!profile->tabs) {
        session_append(&session, "new_tab %s\n", profile->name, NULL);
        session_append(&session, "layout splits\n", NULL, NULL);
    }
    for (i = 0; i < profile->pane_count; i++)
        append_pane(&session, profile, &profile->panes[i], i);
    if (session.overflow) {
        set_error(error, error_size, "The profile is too large.");
        return false;
    }
    if (!write_private_file(path, session.text, session.length)) {
        set_error(error, error_size, "The session file cannot be written.");
        return false;
    }
    return true;
}

/* ---- selftest ---- */

static bool expect(bool condition, const char *label)
{
    if (!condition) fprintf(stderr, "laptop selftest: FAIL %s\n", label);
    return condition;
}

static bool write_fixture(const char *directory, const char *name,
                          const char *contents)
{
    char path[PATH_MAX];
    if (!format_text(path, sizeof path, "%s/%s", directory, name))
        return false;
    return write_private_file(path, contents, strlen(contents));
}

static bool contains(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

/* Bounded two-level remover for the selftest scratch tree only. */
static void remove_test_tree(const char *root)
{
    DIR *entries = opendir(root);
    struct dirent *entry;
    if (entries == NULL) return;
    while ((entry = readdir(entries)) != NULL) {
        char path[PATH_MAX];
        struct stat info;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        if (!format_text(path, sizeof path, "%s/%s", root, entry->d_name))
            continue;
        if (lstat(path, &info) == 0 && S_ISDIR(info.st_mode))
            remove_test_tree(path);
        else
            (void)unlink(path);
    }
    closedir(entries);
    (void)rmdir(root);
}

bool desk_laptop_selftest(void)
{
    char root[] = "/tmp/kilix-land-laptop.XXXXXX";
    char seed[PATH_MAX];
    char session_path[PATH_MAX];
    char session[8 * 1024];
    char error[DESK_LAPTOP_ERROR_CAPACITY];
    size_t session_length = 0;
    desk_laptop_list list;
    desk_laptop_profile profile;
    const char *arguments[2] = {NULL, NULL};
    bool ok = true;

    if (mkdtemp(root) == NULL) return false;
    if (!format_text(seed, sizeof seed, "%s/seed", root, NULL) ||
        mkdir(seed, 0700) != 0)
        return false;
    ok &= expect(write_fixture(seed, "starter.profile",
                               "name=Starter\npane.1.cwd=~\n"),
                 "seed fixture");
    if (setenv("KILIX_LAPTOP_PROFILES", root, 1) != 0) return false;

    /* The override directory exists, so seeding must NOT run. */
    ok &= expect(desk_laptop_scan(seed, &list) == 0 && list.count == 0,
                 "existing directory is never reseeded");

    /* A missing directory is created and seeded exactly once. */
    {
        char fresh[PATH_MAX];
        ok &= expect(format_text(fresh, sizeof fresh, "%s/fresh", root,
                                 NULL),
                     "fresh path");
        if (setenv("KILIX_LAPTOP_PROFILES", fresh, 1) != 0) return false;
        ok &= expect(desk_laptop_scan(seed, &list) == 1 &&
                         strcmp(list.ids[0], "starter") == 0,
                     "seeding a first-run directory");
        ok &= expect(desk_laptop_load("starter", &profile, error,
                                 sizeof error) &&
                         strcmp(profile.name, "Starter") == 0 &&
                         profile.pane_count == 1,
                     "seeded profile loads");
        if (setenv("KILIX_LAPTOP_PROFILES", root, 1) != 0) return false;
    }

    ok &= expect(write_fixture(root, "dev.profile",
                               "# comment\n"
                               "name=Dev Bench\n"
                               "layout=splits\n"
                               "pane.1.title=editor\n"
                               "pane.1.cwd=~/projects\n"
                               "pane.2.cmd=htop\n"
                               "pane.3.ssh=user@build-host\n"
                               "pane.3.cwd=/srv\n"
                               "pane.3.cmd=tail -f service.log\n"),
                 "dev fixture");
    ok &= expect(write_fixture(root, "ops.profile",
                               "name=Ops\nlayout=tabs\n"
                               "pane.1.cmd=journalctl -f\n"
                               "pane.2.ssh=admin@gateway\n"),
                 "ops fixture");
    ok &= expect(write_fixture(root, "house.profile",
                               "name=House\ndesktop=land\n"),
                 "house fixture");
    ok &= expect(write_fixture(root, "not-a-profile.txt", "x=y\n"),
                 "stray fixture");

    ok &= expect(desk_laptop_scan(NULL, &list) == 3, "scan finds 3 profiles");
    ok &= expect(strcmp(list.ids[0], "dev") == 0 &&
                     strcmp(list.ids[1], "house") == 0 &&
                     strcmp(list.ids[2], "ops") == 0,
                 "scan sorts ids");

    ok &= expect(desk_laptop_load("dev", &profile, error, sizeof error),
                 "dev loads");
    ok &= expect(strcmp(profile.name, "Dev Bench") == 0 &&
                     !profile.tabs && profile.pane_count == 3 &&
                     strcmp(profile.panes[2].ssh, "user@build-host") == 0,
                 "dev fields");
    ok &= expect(desk_laptop_desktop_arguments(&profile, arguments) == 0,
                 "dev is not a desktop profile");
    ok &= expect(format_text(session_path, sizeof session_path,
                             "%s/dev.session", root, NULL),
                 "session path");
    ok &= expect(desk_laptop_write_session(&profile, session_path, error,
                                      sizeof error),
                 "dev session writes");
    ok &= expect(read_small_file(session_path, session, sizeof session,
                                 &session_length),
                 "dev session reads back");
    ok &= expect(contains(session, "os_window_title Dev Bench\n") &&
                     contains(session, "layout splits\n") &&
                     contains(session, "title editor\n") &&
                     contains(session, "/projects\n") &&
                     contains(session,
                              "launch --location=vsplit sh -lc "
                              "\"htop\"\n") &&
                     contains(session,
                              "launch --location=hsplit ssh -t "
                              "user@build-host \"cd /srv && exec tail -f "
                              "service.log\"\n"),
                 "dev session content");
    ok &= expect(!contains(session, "new_tab editor"),
                 "splits profiles stay in one tab");

    ok &= expect(desk_laptop_load("ops", &profile, error, sizeof error) &&
                     profile.tabs,
                 "ops loads");
    ok &= expect(desk_laptop_write_session(&profile, session_path, error,
                                      sizeof error) &&
                     read_small_file(session_path, session,
                                     sizeof session, &session_length) &&
                     contains(session, "new_tab journalctl") == false &&
                     contains(session, "new_tab Ops\n") &&
                     contains(session, "launch ssh -t admin@gateway\n"),
                 "ops session content");

    ok &= expect(desk_laptop_load("house", &profile, error, sizeof error),
                 "house loads");
    ok &= expect(desk_laptop_desktop_arguments(&profile, arguments) == 1 &&
                     strcmp(arguments[0], "land") == 0,
                 "house desktop argv");
    ok &= expect(!desk_laptop_write_session(&profile, session_path, error,
                                       sizeof error),
                 "desktop profiles have no session");

    /* Every rejection leaves the profile zeroed. */
    ok &= expect(write_fixture(root, "bad1.profile",
                               "pane.1.cmd=echo \"hi\"\n") &&
                     !desk_laptop_load("bad1", &profile, error, sizeof error) &&
                     profile.pane_count == 0,
                 "quotes are rejected");
    ok &= expect(write_fixture(root, "bad2.profile",
                               "desktop=cap\npane.1.cmd=htop\n") &&
                     !desk_laptop_load("bad2", &profile, error, sizeof error),
                 "desktop plus panes is rejected");
    ok &= expect(write_fixture(root, "bad3.profile",
                               "pane.1.cmd=a\npane.3.cmd=b\n") &&
                     !desk_laptop_load("bad3", &profile, error, sizeof error),
                 "pane gaps are rejected");
    ok &= expect(write_fixture(root, "bad4.profile", "desktop=gnome\n") &&
                     !desk_laptop_load("bad4", &profile, error, sizeof error),
                 "unknown providers are rejected");
    ok &= expect(write_fixture(root, "bad5.profile",
                               "pane.1.ssh=host; rm -rf /\n") &&
                     !desk_laptop_load("bad5", &profile, error, sizeof error),
                 "ssh destinations are charset-limited");
    ok &= expect(write_fixture(root, "bad6.profile", "shape=round\n") &&
                     !desk_laptop_load("bad6", &profile, error, sizeof error),
                 "unknown keys are rejected");
    ok &= expect(!desk_laptop_load("../escape", &profile, error, sizeof error),
                 "path traversal ids are rejected");

    unsetenv("KILIX_LAPTOP_PROFILES");
    remove_test_tree(root);
    return ok;
}
