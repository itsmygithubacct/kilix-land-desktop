/* state_store.c — the one place that resolves the desktop's config home.
 * Moved out of desk.c so profile.state and world.state open their stores
 * identically without duplicating the environment rules. */

#include "state_store.h"

#include <stdio.h>
#include <stdlib.h>

#define DESK_STATE_APP_ID "kilix-land-desktop"

bool desk_state_store_open(kilixstate_store *store, const char *filename,
                           size_t max_payload)
{
    kilixstate_options options;
    char base[KILIXSTATE_PATH_CAPACITY];
    char absolute[KILIXSTATE_PATH_CAPACITY];
    const char *override_dir;
    const char *home;
    int written;

    if (!store || !filename || filename[0] == '\0') return false;
    kilixstate_options_init(&options);
    options.app_id = DESK_STATE_APP_ID;
    options.filename = filename;
    options.max_payload = max_payload;
    override_dir = getenv("KILIX_LAND_DESKTOP_CONFIG_HOME");
    if (override_dir && override_dir[0] != '\0') {
        /* kilixstate rejects relative absolute_paths anyway; surface the
         * misconfiguration once instead of silently losing persistence. */
        if (override_dir[0] != '/') {
            static bool warned = false;

            if (!warned) {
                (void)fprintf(stderr, "kilix-land-desktop: "
                              "KILIX_LAND_DESKTOP_CONFIG_HOME must be an "
                              "absolute path; state persistence is off\n");
                warned = true;
            }
            return false;
        }
        written = snprintf(absolute, sizeof absolute, "%s/%s", override_dir,
                           filename);
        if (written < 0 || (size_t)written >= sizeof absolute) return false;
        options.absolute_path = absolute;
    } else {
        home = getenv("HOME");
        if (!home || home[0] != '/') return false;
        written = snprintf(base, sizeof base, "%s/.local/gpu_terminal", home);
        if (written < 0 || (size_t)written >= sizeof base) return false;
        options.base_directory = base;
    }
    return kilixstate_store_init(store, &options) == KILIXSTATE_OK;
}
