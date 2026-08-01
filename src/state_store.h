#ifndef KILIX_LAND_DESKTOP_STATE_STORE_H
#define KILIX_LAND_DESKTOP_STATE_STORE_H

#include "kilix_game_kit.h"

#include <stdbool.h>
#include <stddef.h>

/* Opens one named kilix-state record in the desktop's config home:
 * KILIX_LAND_DESKTOP_CONFIG_HOME when set (must be absolute), otherwise
 * $HOME/.local/gpu_terminal/kilix-land-desktop. profile.state and
 * world.state share this resolver so identity and world recovery stay
 * separate records under one roof. */
bool desk_state_store_open(kilixstate_store *store, const char *filename,
                           size_t max_payload);

#endif
