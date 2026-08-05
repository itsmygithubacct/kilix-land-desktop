#ifndef KILIX_LAND_DESKTOP_LAPTOP_H
#define KILIX_LAND_DESKTOP_LAPTOP_H

#include <stdbool.h>
#include <stddef.h>

/* Laptop profiles: discovery, strict parsing, kitty session emission.
 *
 * One convention shared by every kilix desktop that ships a laptop item:
 * profiles are plain KEY=value files named <id>.profile in
 * ~/.local/gpu_terminal/laptop/ (override: KILIX_LAPTOP_PROFILES, an
 * absolute directory). A profile either names another desktop provider to
 * open, or describes one kilix terminal session — its pane layout, each
 * pane's working directory or ssh destination, and each pane's command.
 * Data can never smuggle shell text into a launch: values are validated
 * here, panes become lines of a kitty --session file, and the launch argv
 * is always a fixed vector (see docs/APPS.md, "The laptop").
 */

#define DESK_LAPTOP_PROFILES_MAX 16
#define DESK_LAPTOP_ID_CAPACITY 40
#define DESK_LAPTOP_NAME_CAPACITY 48
#define DESK_LAPTOP_PANES_MAX 8
#define DESK_LAPTOP_VALUE_CAPACITY 200
#define DESK_LAPTOP_DESKTOP_CAPACITY 12
#define DESK_LAPTOP_ERROR_CAPACITY 96

typedef struct desk_laptop_pane {
    char title[DESK_LAPTOP_NAME_CAPACITY]; /* optional; empty = default */
    char cwd[DESK_LAPTOP_VALUE_CAPACITY];  /* local dir, or remote w/ ssh */
    char ssh[DESK_LAPTOP_VALUE_CAPACITY];  /* [user@]host, or empty */
    char cmd[DESK_LAPTOP_VALUE_CAPACITY];  /* command, or empty = shell */
} desk_laptop_pane;

typedef struct desk_laptop_profile {
    char id[DESK_LAPTOP_ID_CAPACITY];
    char name[DESK_LAPTOP_NAME_CAPACITY];
    char desktop[DESK_LAPTOP_DESKTOP_CAPACITY]; /* provider, or empty */
    bool tabs;                                  /* layout=tabs */
    int pane_count;
    desk_laptop_pane panes[DESK_LAPTOP_PANES_MAX];
} desk_laptop_profile;

typedef struct desk_laptop_list {
    int count;
    char ids[DESK_LAPTOP_PROFILES_MAX][DESK_LAPTOP_ID_CAPACITY];
} desk_laptop_list;

/* The shared profile directory. False when no home is resolvable or the
 * KILIX_LAPTOP_PROFILES override is not an absolute path. */
bool desk_laptop_directory(char *path, size_t size);

/* Sorted profile ids. A missing directory is created once and seeded with
 * the bundled examples from seed_directory (NULL skips seeding); an
 * existing directory — even an emptied one — is never reseeded. Returns
 * the count, or -1 when the directory cannot be resolved or read. */
int desk_laptop_scan(const char *seed_directory, desk_laptop_list *list);

/* Strict parse of <directory>/<id>.profile. On failure the profile is
 * zeroed and error holds one short user-facing sentence. */
bool desk_laptop_load(const char *id, desk_laptop_profile *profile,
                      char *error, size_t error_size);

/* Writes the kitty --session file for a pane profile, 0600, via a
 * same-directory temp file and rename. Refuses desktop profiles. */
bool desk_laptop_write_session(const desk_laptop_profile *profile,
                               const char *path, char *error,
                               size_t error_size);

/* Fills argv words after "kilix" for a desktop profile ("cap" -> {"cap"};
 * "95" -> {"desktop", "95"}). Returns the word count, 0 for pane
 * profiles. */
size_t desk_laptop_desktop_arguments(const desk_laptop_profile *profile,
                                     const char *arguments[2]);

/* ---- authoring ----
 *
 * The in-desktop configuration pages build a profile in memory and hand
 * it here. Writing goes through the same rules the reader enforces, so a
 * profile the pages produced can never be one the loader rejects.
 */

/* No double quotes and no control characters — the two things that could
 * change how kitty splits a generated session line. */
bool desk_laptop_text_ok(const char *value);
/* [user@]host only: a destination can never smuggle options or commands. */
bool desk_laptop_host_ok(const char *value);
/* The providers a desktop profile may name, in menu order. */
size_t desk_laptop_provider_count(void);
const char *desk_laptop_provider(size_t index);

/* Whole-profile check: a desktop or panes (never both), a name that
 * fits, contiguous panes, and every value legal. */
bool desk_laptop_validate(const desk_laptop_profile *profile, char *error,
                          size_t error_size);

/* Turns a display name into a usable file id, avoiding collisions with
 * profiles that already exist. */
bool desk_laptop_make_id(const char *name, char *id, size_t size);

/* Writes <id>.profile 0600 via a same-directory temp file and rename.
 * Validates first; a rejected profile leaves the file untouched. */
bool desk_laptop_save(const desk_laptop_profile *profile, char *error,
                      size_t error_size);

/* Removes <id>.profile. A profile that is already gone counts as
 * removed. */
bool desk_laptop_delete(const char *id, char *error, size_t error_size);

/* Headless self-checks over a private temp directory. */
bool desk_laptop_selftest(void);

#endif /* KILIX_LAND_DESKTOP_LAPTOP_H */
