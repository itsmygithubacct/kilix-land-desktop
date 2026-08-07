#include "kilix_land_desktop.h"
#include "json_reader.h"
#include "items.h"
#include "world_state.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Text is captured per frame so wizard NAME entry never drops keystrokes
 * when a frame runs multiple simulation steps. */
#include <limits.h>

#define DESK_TEXT_QUEUE_CAPACITY 8

typedef struct desk_key_input {
    int move_x;
    int move_y;
    /* Edge-triggered menu deltas: set on key PRESS only and cleared by
     * consume_edge_input, so cursors step once per keystroke while held
     * movement stays level-triggered for the room (land's menu_delta). */
    int menu_x;
    int menu_y;
    bool enter_pressed;
    bool space_pressed;
    bool cancel_pressed;
    /* Hotbar intents; desk.c accepts selection in room/inventory modes,
     * so digits typed into the wizard name field stay plain text. */
    int select_slot; /* -1 = none; 0..11 from 1..9, 0, -, = */
    int cycle_slot;  /* sum of [ (-1) and ] (+1) presses */
    bool drop_pressed;
    bool open_inventory;
    int backspace_count;
    int text_count;
    uint32_t text[DESK_TEXT_QUEUE_CAPACITY];
} desk_key_input;

static int hotbar_slot_for_codepoint(uint32_t codepoint)
{
    if (codepoint >= (uint32_t)'1' && codepoint <= (uint32_t)'9')
        return (int)(codepoint - (uint32_t)'1');
    if (codepoint == (uint32_t)'0') return 9;
    if (codepoint == (uint32_t)'-') return 10;
    if (codepoint == (uint32_t)'=') return 11;
    return -1;
}

static kittyts_session terminal_session;
static uint32_t last_movement_key;

static bool event_letter(const kittykb_event *event, char lower)
{
    char upper = (char)(lower - 'a' + 'A');
    return kittykb_event_matches_key(event, (uint32_t)(unsigned char)lower) ||
           kittykb_event_matches_key(event, (uint32_t)(unsigned char)upper);
}

static uint32_t normalize_movement_key(uint32_t key)
{
    if (key == (uint32_t)'w' || key == (uint32_t)'W') return KITTYKB_KEY_UP;
    if (key == (uint32_t)'s' || key == (uint32_t)'S') return KITTYKB_KEY_DOWN;
    if (key == (uint32_t)'a' || key == (uint32_t)'A') return KITTYKB_KEY_LEFT;
    if (key == (uint32_t)'d' || key == (uint32_t)'D') return KITTYKB_KEY_RIGHT;
    if (key == KITTYKB_KEY_UP || key == KITTYKB_KEY_DOWN ||
        key == KITTYKB_KEY_LEFT || key == KITTYKB_KEY_RIGHT)
        return key;
    return KITTYKB_KEY_NONE;
}

static bool movement_key_down(uint32_t key)
{
    switch (key) {
    case KITTYKB_KEY_UP:
        return kittyts_key_down(&terminal_session, KITTYKB_KEY_UP) ||
               kittyts_key_down(&terminal_session, (uint32_t)'w');
    case KITTYKB_KEY_DOWN:
        return kittyts_key_down(&terminal_session, KITTYKB_KEY_DOWN) ||
               kittyts_key_down(&terminal_session, (uint32_t)'s');
    case KITTYKB_KEY_LEFT:
        return kittyts_key_down(&terminal_session, KITTYKB_KEY_LEFT) ||
               kittyts_key_down(&terminal_session, (uint32_t)'a');
    case KITTYKB_KEY_RIGHT:
        return kittyts_key_down(&terminal_session, KITTYKB_KEY_RIGHT) ||
               kittyts_key_down(&terminal_session, (uint32_t)'d');
    default:
        return kittyts_key_down(&terminal_session, key);
    }
}

static void apply_movement_key(desk_key_input *input, uint32_t key)
{
    if (!input) return;
    if (key == KITTYKB_KEY_UP) input->move_y = -1;
    else if (key == KITTYKB_KEY_DOWN) input->move_y = 1;
    else if (key == KITTYKB_KEY_LEFT) input->move_x = -1;
    else if (key == KITTYKB_KEY_RIGHT) input->move_x = 1;
}

static bool poll_input(desk_key_input *input, bool *quit_requested)
{
    static const uint32_t directions[] = {
        KITTYKB_KEY_UP, KITTYKB_KEY_DOWN,
        KITTYKB_KEY_LEFT, KITTYKB_KEY_RIGHT
    };
    kittykb_event event;
    size_t index;

    if (!input || !quit_requested) {
        errno = EINVAL;
        return false;
    }
    (void)memset(input, 0, sizeof *input);
    input->select_slot = -1;
    if (kittyts_read_input(&terminal_session) < 0 && errno != EAGAIN &&
        errno != EWOULDBLOCK && errno != EINTR)
        return false;

    while (kittyts_next_key_event(&terminal_session, &event)) {
        uint32_t movement_key = normalize_movement_key(event.key);
        bool pressed = event.action == KITTYKB_ACTION_PRESS;
        if (movement_key != KITTYKB_KEY_NONE &&
            event.action != KITTYKB_ACTION_RELEASE)
            last_movement_key = movement_key;
        if (pressed && movement_key != KITTYKB_KEY_NONE) {
            if (movement_key == KITTYKB_KEY_UP) input->menu_y = -1;
            else if (movement_key == KITTYKB_KEY_DOWN) input->menu_y = 1;
            else if (movement_key == KITTYKB_KEY_LEFT) input->menu_x = -1;
            else input->menu_x = 1;
        }
        /* Auto-repeat reaches only the text field; every other edge stays
         * press-only for land parity. */
        if (!pressed && event.action != KITTYKB_ACTION_REPEAT) continue;
        if (event_letter(&event, 'c') &&
            (event.modifiers & KITTYKB_MOD_CTRL) != 0u) {
            if (pressed) *quit_requested = true;
        } else if (event.key == KITTYKB_KEY_ESCAPE) {
            if (pressed) input->cancel_pressed = true;
        } else if (event.key == KITTYKB_KEY_ENTER) {
            if (pressed) input->enter_pressed = true;
        } else if (event.key == KITTYKB_KEY_BACKSPACE) {
            input->backspace_count++;
        } else if ((event.modifiers &
                    (KITTYKB_MOD_CTRL | KITTYKB_MOD_ALT |
                     KITTYKB_MOD_SUPER)) == 0u) {
            uint32_t codepoint = event.text_length > 0u ?
                event.text[0] :
                (event.shifted_key != 0u ? event.shifted_key : event.key);
            if (codepoint == (uint32_t)' ' && pressed)
                input->space_pressed = true;
            if (pressed) {
                int slot = hotbar_slot_for_codepoint(codepoint);

                if (slot >= 0) input->select_slot = slot;
                if (codepoint == (uint32_t)'[') input->cycle_slot -= 1;
                if (codepoint == (uint32_t)']') input->cycle_slot += 1;
                if (codepoint == (uint32_t)'q' ||
                    codepoint == (uint32_t)'Q')
                    input->drop_pressed = true;
                if (codepoint == (uint32_t)'i' ||
                    codepoint == (uint32_t)'I')
                    input->open_inventory = true;
            }
            if (codepoint >= 32u && codepoint <= 126u &&
                input->text_count < DESK_TEXT_QUEUE_CAPACITY)
                input->text[input->text_count++] = codepoint;
        }
    }
    if (last_movement_key != KITTYKB_KEY_NONE &&
        movement_key_down(last_movement_key)) {
        apply_movement_key(input, last_movement_key);
        return true;
    }
    for (index = 0u; index < sizeof directions / sizeof directions[0];
         ++index) {
        if (movement_key_down(directions[index])) {
            last_movement_key = directions[index];
            apply_movement_key(input, directions[index]);
            break;
        }
    }
    return true;
}

static void merge_pending_input(desk_key_input *pending,
                                const desk_key_input *current)
{
    int index;
    pending->move_x = current->move_x;
    pending->move_y = current->move_y;
    if (current->menu_x != 0) pending->menu_x = current->menu_x;
    if (current->menu_y != 0) pending->menu_y = current->menu_y;
    pending->enter_pressed = pending->enter_pressed ||
                             current->enter_pressed;
    pending->space_pressed = pending->space_pressed ||
                             current->space_pressed;
    pending->cancel_pressed = pending->cancel_pressed ||
                              current->cancel_pressed;
    if (current->select_slot >= 0)
        pending->select_slot = current->select_slot;
    pending->cycle_slot += current->cycle_slot;
    pending->drop_pressed = pending->drop_pressed ||
                            current->drop_pressed;
    pending->open_inventory = pending->open_inventory ||
                              current->open_inventory;
    pending->backspace_count += current->backspace_count;
    for (index = 0; index < current->text_count; ++index) {
        if (pending->text_count >= DESK_TEXT_QUEUE_CAPACITY) break;
        pending->text[pending->text_count++] = current->text[index];
    }
}

static void consume_edge_input(desk_key_input *input)
{
    input->menu_x = 0;
    input->menu_y = 0;
    input->enter_pressed = false;
    input->space_pressed = false;
    input->cancel_pressed = false;
    input->select_slot = -1;
    input->cycle_slot = 0;
    input->drop_pressed = false;
    input->open_inventory = false;
    input->backspace_count = 0;
    input->text_count = 0;
}

static const char *asset_root(void)
{
    const char *override = getenv("KILIX_LAND_DESKTOP_ASSETS");
    return override && override[0] != '\0' ? override : ".";
}

static bool world_path(char *path, size_t size)
{
    int written = snprintf(path, size, "%s/assets/world/world.json",
                           asset_root());
    return written >= 0 && (size_t)written < size;
}

static bool items_path(char *path, size_t size)
{
    int written = snprintf(path, size, "%s/assets/world/items.json",
                           asset_root());
    return written >= 0 && (size_t)written < size;
}

/* Startup order per the module contract: validate the item catalog,
 * parse the world manifest, then resolve the world's item and receiver
 * references against the catalog before anything is published. */
static bool load_world(desk_world *world, desk_item_catalog *catalog,
                       char *error, size_t error_size)
{
    char path[1024];
    if (!items_path(path, sizeof path) || !world_path(path, sizeof path)) {
        if (error && error_size > 0u)
            (void)snprintf(error, error_size, "asset path too long");
        return false;
    }
    if (!items_path(path, sizeof path) ||
        !desk_items_load(catalog, path, error, error_size))
        return false;
    if (!world_path(path, sizeof path))
        return false;
    return desk_world_load(world, path, error, error_size) &&
           desk_world_validate(world, error, error_size) &&
           desk_world_validate_items(world, catalog, error, error_size);
}

/* Land creates its preview directories with `mkdir -p` in the Makefile; the
 * desktop Makefile does not, so the render tests create their one level. */
static bool ensure_directory(const char *path)
{
    struct stat status;
    if (!path || path[0] == '\0') return false;
    if (mkdir(path, 0755) == 0) return true;
    if (errno != EEXIST) return false;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static bool make_temp_config(char *directory, size_t size)
{
    const char *base = getenv("TMPDIR");
    int written;
    if (!base || base[0] == '\0') base = "/tmp";
    written = snprintf(directory, size, "%s/kilix-land-desktop.XXXXXX", base);
    if (written < 0 || (size_t)written >= size) return false;
    if (!mkdtemp(directory)) return false;
    return setenv("KILIX_LAND_DESKTOP_CONFIG_HOME", directory, 1) == 0;
}

static bool remove_tree(const char *directory)
{
    DIR *handle = opendir(directory);
    struct dirent *entry;
    bool ok = true;
    if (!handle) return rmdir(directory) == 0;
    while ((entry = readdir(handle)) != NULL) {
        char path[1024];
        int written;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        written = snprintf(path, sizeof path, "%s/%s", directory,
                           entry->d_name);
        if (written < 0 || (size_t)written >= sizeof path ||
            (unlink(path) != 0 && rmdir(path) != 0))
            ok = false;
    }
    closedir(handle);
    return rmdir(directory) == 0 && ok;
}

static desk_cast effective_style(const desk_state *state)
{
    return state->mode == DESK_MODE_WIZARD ?
           (desk_cast)state->wizard_cast_cursor : state->profile.cast;
}

static int effective_outfit(const desk_state *state)
{
    return state->mode == DESK_MODE_WIZARD ?
           state->wizard_outfit_cursor : (int)state->profile.outfit;
}

static bool sync_graphics(desk_graphics *graphics, const desk_world *world,
                          desk_state *state, int *loaded_style)
{
    desk_cast style = effective_style(state);
    if (*loaded_style != (int)style) {
        if (!desk_graphics_load_plates(graphics, asset_root(), world, style))
            return false;
        *loaded_style = (int)style;
        /* A style change always invalidates the recolored cells, even when
         * the mode transition that caused it did not mark them dirty. */
        state->outfit_dirty = true;
    }
    if (state->outfit_dirty) {
        if (!desk_graphics_set_outfit(graphics, style,
                                      effective_outfit(state)))
            return false;
        state->outfit_dirty = false;
    }
    return true;
}

static int audio_test(void)
{
    size_t loaded = 0u;
    if (!desk_audio_assets_selftest(asset_root(), &loaded) ||
        loaded != (size_t)DESK_AUDIO_SOURCE_CUE_COUNT) {
        (void)fprintf(stderr, "FAIL native-audio loaded=%zu/%d\n",
                      loaded, DESK_AUDIO_SOURCE_CUE_COUNT);
        return EXIT_FAILURE;
    }
    (void)printf(
        "PASS native-audio casts=%d source-cues=%zu mixer=offline\n",
        DESK_CAST_COUNT, loaded);
    return EXIT_SUCCESS;
}

static int graphics_test(void)
{
    desk_world world;
    desk_item_catalog catalog;
    desk_graphics graphics;
    ki_td_rgba8 image;
    char error[DESK_ERROR_CAPACITY];
    size_t loaded;
    int plates = 0;
    int room;
    if (!load_world(&world, &catalog, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL graphics world: %s\n", error);
        return EXIT_FAILURE;
    }
    if (!desk_graphics_init(&graphics, asset_root())) {
        (void)fprintf(stderr, "FAIL graphics init under %s\n", asset_root());
        return EXIT_FAILURE;
    }
    loaded = desk_graphics_loaded_count(&graphics);
    if (loaded != (size_t)DESK_GRAPHICS_COUNT ||
        !desk_graphics_cell(&graphics, DESK_GRAPHIC_LEGEND_PLAYER, 0, 0,
                            &image) ||
        !desk_graphics_load_plates(&graphics, asset_root(), &world,
                                   DESK_CAST_LEGEND) ||
        !desk_graphics_set_outfit(&graphics, DESK_CAST_LEGEND, 0) ||
        !desk_graphics_hero_cell(&graphics, DESK_CAST_LEGEND, 0, 0,
                                 &image)) {
        (void)fprintf(stderr, "FAIL graphics loaded=%zu/%d\n",
                      loaded, DESK_GRAPHICS_COUNT);
        desk_graphics_shutdown(&graphics);
        return EXIT_FAILURE;
    }
    for (room = 0; room < world.room_count; ++room)
        if (desk_graphics_plate(&graphics, room, &image)) ++plates;
    desk_graphics_shutdown(&graphics);
    (void)printf(
        "PASS graphics resources=%zu plates=%d/%d outfit=recolored\n",
        loaded, plates, world.room_count);
    return EXIT_SUCCESS;
}

static int world_test(void)
{
    desk_world world;
    desk_item_catalog catalog;
    char path[1024];
    char error[DESK_ERROR_CAPACITY];
    int objects = 0;
    int doors = 0;
    int npcs = 0;
    int walkbehinds = 0;
    int spawns = 0;
    int room;
    if (!world_path(path, sizeof path)) {
        (void)fprintf(stderr, "FAIL world path too long\n");
        return EXIT_FAILURE;
    }
    if (!desk_world_load(&world, path, error, sizeof error) ||
        !desk_world_validate(&world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL world %s: %s\n", path, error);
        return EXIT_FAILURE;
    }
    if (!items_path(path, sizeof path) ||
        !desk_items_load(&catalog, path, error, sizeof error) ||
        !desk_world_validate_items(&world, &catalog, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL world items %s: %s\n", path, error);
        return EXIT_FAILURE;
    }
    for (room = 0; room < world.room_count; ++room) {
        objects += world.rooms[room].object_count;
        doors += world.rooms[room].door_count;
        npcs += world.rooms[room].npc_count;
        walkbehinds += world.rooms[room].walkbehind_count;
        spawns += world.rooms[room].spawn_count;
    }
    (void)printf(
        "PASS world rooms=%d objects=%d doors=%d npcs=%d walkbehinds=%d "
        "spawns=%d start=%s\n",
        world.room_count, objects, doors, npcs, walkbehinds, spawns,
        world.rooms[world.start_room].id);
    return EXIT_SUCCESS;
}

static int profile_test_body(const char *config_dir)
{
    desk_profile saved;
    desk_profile loaded;
    char record_path[1024];
    FILE *record;
    long record_size;
    int byte;
    int written;

    (void)memset(&saved, 0, sizeof saved);
    saved.schema = DESK_PROFILE_SCHEMA;
    saved.cast = DESK_CAST_FANTASY;
    saved.actor = DESK_ACTOR_HERO;
    saved.outfit = 3u;
    (void)snprintf(saved.name, sizeof saved.name, "%s", "ROOK");
    (void)snprintf(saved.last_room, sizeof saved.last_room, "%s", "study");
    saved.last_x = 123.0f;
    saved.last_y = 210.0f;
    saved.first_run_done = true;
    if (!desk_profile_save(&saved)) {
        (void)fprintf(stderr, "FAIL profile save\n");
        return EXIT_FAILURE;
    }
    if (!desk_profile_load(&loaded) ||
        loaded.schema != saved.schema || loaded.cast != saved.cast ||
        loaded.actor != saved.actor || loaded.outfit != saved.outfit ||
        strcmp(loaded.name, saved.name) != 0 ||
        strcmp(loaded.last_room, saved.last_room) != 0 ||
        loaded.last_x != saved.last_x || loaded.last_y != saved.last_y ||
        loaded.first_run_done != saved.first_run_done) {
        (void)fprintf(stderr, "FAIL profile round-trip\n");
        return EXIT_FAILURE;
    }
    /* The record filename mirrors desk.c's DESK_PROFILE_FILENAME. */
    written = snprintf(record_path, sizeof record_path, "%s/profile.state",
                       config_dir);
    if (written < 0 || (size_t)written >= sizeof record_path ||
        (record = fopen(record_path, "r+b")) == NULL) {
        (void)fprintf(stderr, "FAIL profile record open %s\n", record_path);
        return EXIT_FAILURE;
    }
    if (fseek(record, 0L, SEEK_END) != 0 ||
        (record_size = ftell(record)) <= 0L ||
        fseek(record, record_size / 2L, SEEK_SET) != 0 ||
        (byte = fgetc(record)) == EOF ||
        fseek(record, record_size / 2L, SEEK_SET) != 0 ||
        fputc(byte ^ 0xff, record) == EOF) {
        (void)fclose(record);
        (void)fprintf(stderr, "FAIL profile record corruption write\n");
        return EXIT_FAILURE;
    }
    if (fclose(record) != 0) {
        (void)fprintf(stderr, "FAIL profile record close\n");
        return EXIT_FAILURE;
    }
    if (desk_profile_load(&loaded)) {
        (void)fprintf(stderr, "FAIL profile corruption accepted\n");
        return EXIT_FAILURE;
    }
    if (!desk_profile_reset()) {
        (void)fprintf(stderr, "FAIL profile reset\n");
        return EXIT_FAILURE;
    }
    (void)printf(
        "PASS profile schema=%d fields=9 corruption=rejected reset=clean\n",
        DESK_PROFILE_SCHEMA);
    return EXIT_SUCCESS;
}

static int profile_test(void)
{
    char config_dir[1024];
    int result;
    if (!make_temp_config(config_dir, sizeof config_dir)) {
        (void)fprintf(stderr, "FAIL profile temp config\n");
        return EXIT_FAILURE;
    }
    result = profile_test_body(config_dir);
    if (!remove_tree(config_dir) && result == EXIT_SUCCESS) {
        (void)fprintf(stderr, "FAIL profile temp cleanup\n");
        result = EXIT_FAILURE;
    }
    return result;
}

static int selftest_body(void)
{
    desk_world world;
    desk_item_catalog catalog;
    desk_state state;
    desk_profile reloaded;
    desk_audio_event events[4];
    char error[DESK_ERROR_CAPACITY];
    int living;
    int guard;
    int target;
    int event_count;

    if (!load_world(&world, &catalog, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest world: %s\n", error);
        return EXIT_FAILURE;
    }

    /* Walk-behind validation round: an out-of-range id, a duplicate id, a
     * baseline off the logical canvas and a count over the cap must each
     * be rejected, and the restored world must validate clean again. */
    {
        desk_room *scene = &world.rooms[0];
        int saved_count = scene->walkbehind_count;
        desk_walkbehind *probe;
        if (saved_count < 1 ||
            saved_count >= DESK_MAX_WALKBEHINDS_PER_ROOM) {
            (void)fprintf(stderr, "FAIL selftest walkbehind probe slot\n");
            return EXIT_FAILURE;
        }
        probe = &scene->walkbehinds[saved_count];
        probe->id = 0; /* below the 1..15 range */
        probe->baseline = 100.0f;
        scene->walkbehind_count = saved_count + 1;
        if (desk_world_validate(&world, error, sizeof error)) {
            (void)fprintf(stderr,
                          "FAIL selftest bad walkbehind id accepted\n");
            return EXIT_FAILURE;
        }
        probe->id = scene->walkbehinds[0].id; /* duplicate */
        if (desk_world_validate(&world, error, sizeof error)) {
            (void)fprintf(stderr,
                          "FAIL selftest duplicate walkbehind id accepted\n");
            return EXIT_FAILURE;
        }
        probe->id = DESK_MAX_WALKBEHINDS_PER_ROOM; /* free id: valid */
        probe->baseline = (float)DESK_LOGICAL_HEIGHT + 30.0f;
        if (desk_world_validate(&world, error, sizeof error)) {
            (void)fprintf(stderr,
                          "FAIL selftest bad walkbehind baseline accepted\n");
            return EXIT_FAILURE;
        }
        probe->baseline = 100.0f;
        scene->walkbehind_count = DESK_MAX_WALKBEHINDS_PER_ROOM + 1;
        if (desk_world_validate(&world, error, sizeof error)) {
            (void)fprintf(stderr,
                          "FAIL selftest walkbehind count cap ignored\n");
            return EXIT_FAILURE;
        }
        scene->walkbehind_count = saved_count;
        if (!desk_world_validate(&world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest walkbehind restore: %s\n",
                          error);
            return EXIT_FAILURE;
        }
    }

    /* Item-spawn validation round: bad quantity, a point off the walk
     * rect, a duplicate spawn id, and an unresolvable item id must each
     * be rejected, and the restored world must validate clean again. */
    {
        desk_room *scene = &world.rooms[0];
        int saved_count = scene->spawn_count;
        desk_item_spawn *probe;

        if (saved_count < 1 ||
            saved_count >= DESK_MAX_ITEM_SPAWNS_PER_ROOM) {
            (void)fprintf(stderr, "FAIL selftest spawn probe slot\n");
            return EXIT_FAILURE;
        }
        probe = &scene->spawns[saved_count];
        *probe = scene->spawns[0];
        (void)snprintf(probe->id, sizeof probe->id, "%s", "probe-spawn");
        probe->quantity = 0;
        scene->spawn_count = saved_count + 1;
        if (desk_world_validate(&world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest spawn quantity accepted\n");
            return EXIT_FAILURE;
        }
        probe->quantity = 1;
        probe->x = scene->walk.x - 10.0f;
        if (desk_world_validate(&world, error, sizeof error)) {
            (void)fprintf(stderr,
                          "FAIL selftest off-walk spawn accepted\n");
            return EXIT_FAILURE;
        }
        probe->x = scene->spawns[0].x;
        (void)snprintf(probe->id, sizeof probe->id, "%s",
                       scene->spawns[0].id);
        if (desk_world_validate(&world, error, sizeof error)) {
            (void)fprintf(stderr,
                          "FAIL selftest duplicate spawn id accepted\n");
            return EXIT_FAILURE;
        }
        (void)snprintf(probe->id, sizeof probe->id, "%s", "probe-spawn");
        (void)snprintf(probe->item, sizeof probe->item, "%s",
                       "core:test/ghost");
        if (!desk_world_validate(&world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest probe spawn shape: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        if (desk_world_validate_items(&world, &catalog, error,
                                      sizeof error)) {
            (void)fprintf(stderr,
                          "FAIL selftest unknown spawn item accepted\n");
            return EXIT_FAILURE;
        }
        scene->spawn_count = saved_count;
        if (!desk_world_validate(&world, error, sizeof error) ||
            !desk_world_validate_items(&world, &catalog, error,
                                       sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest spawn restore: %s\n",
                          error);
            return EXIT_FAILURE;
        }
    }

    /* Receiver-binding round: an object naming an unknown receiver rule
     * must fail cross-validation. */
    {
        int living_index = desk_world_room_index(&world, "living");
        desk_object *stereo = NULL;
        char saved_receiver[DESK_ID_CAPACITY];
        int object_index;

        if (living_index < 0) {
            (void)fprintf(stderr, "FAIL selftest no living room\n");
            return EXIT_FAILURE;
        }
        for (object_index = 0;
             object_index < world.rooms[living_index].object_count;
             ++object_index)
            if (strcmp(world.rooms[living_index].objects[object_index].id,
                       "stereo") == 0)
                stereo = &world.rooms[living_index].objects[object_index];
        if (!stereo || stereo->receiver[0] == '\0') {
            (void)fprintf(stderr,
                          "FAIL selftest stereo receiver missing\n");
            return EXIT_FAILURE;
        }
        (void)snprintf(saved_receiver, sizeof saved_receiver, "%s",
                       stereo->receiver);
        (void)snprintf(stereo->receiver, sizeof stereo->receiver, "%s",
                       "no-such-rule");
        if (desk_world_validate_items(&world, &catalog, error,
                                      sizeof error)) {
            (void)fprintf(stderr,
                          "FAIL selftest unknown receiver accepted\n");
            return EXIT_FAILURE;
        }
        (void)snprintf(stereo->receiver, sizeof stereo->receiver, "%s",
                       saved_receiver);
        if (!desk_world_validate_items(&world, &catalog, error,
                                       sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest receiver restore: %s\n",
                          error);
            return EXIT_FAILURE;
        }
    }

    desk_init(&state, &world, &catalog);
    if (state.mode != DESK_MODE_WIZARD ||
        state.wizard_step != DESK_WIZARD_CAST ||
        state.wizard_cast_cursor != 0 || state.wizard_name_len != 0 ||
        !desk_validate(&state, &world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest initial wizard: %s\n", error);
        return EXIT_FAILURE;
    }
    desk_update(&state, &world, 0, 1, DESK_TICK_SECONDS);
    desk_update(&state, &world, 0, -1, DESK_TICK_SECONDS);
    event_count = desk_take_audio_events(&state, events);
    if (state.wizard_cast_cursor != 0 || event_count != 2 ||
        events[0] != DESK_AUDIO_UI_MOVE || !state.outfit_dirty) {
        (void)fprintf(stderr, "FAIL selftest cast cursor\n");
        return EXIT_FAILURE;
    }
    state.outfit_dirty = false;
    if (!desk_interact(&state, &world) ||
        state.wizard_step != DESK_WIZARD_ACTOR ||
        !desk_validate(&state, &world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest actor step: %s\n", error);
        return EXIT_FAILURE;
    }
    desk_update(&state, &world, 0, 1, DESK_TICK_SECONDS);
    if (state.wizard_actor_cursor != 1 || desk_interact(&state, &world) ||
        state.wizard_step != DESK_WIZARD_ACTOR || state.toast_ticks == 0) {
        (void)fprintf(stderr, "FAIL selftest ally gate\n");
        return EXIT_FAILURE;
    }
    desk_update(&state, &world, 0, -1, DESK_TICK_SECONDS);
    if (state.wizard_actor_cursor != 0 || !desk_interact(&state, &world) ||
        state.wizard_step != DESK_WIZARD_NAME ||
        !desk_validate(&state, &world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest name step: %s\n", error);
        return EXIT_FAILURE;
    }
    if (desk_interact(&state, &world) ||
        state.wizard_step != DESK_WIZARD_NAME) {
        (void)fprintf(stderr, "FAIL selftest empty name accepted\n");
        return EXIT_FAILURE;
    }
    if (!desk_text_input(&state, (uint32_t)'R') ||
        !desk_text_input(&state, (uint32_t)'O') ||
        !desk_text_input(&state, (uint32_t)'O') ||
        !desk_text_input(&state, (uint32_t)'X') ||
        !desk_text_backspace(&state) ||
        !desk_text_input(&state, (uint32_t)'K') ||
        strcmp(state.wizard_name, "ROOK") != 0 ||
        state.wizard_name_len != 4) {
        (void)fprintf(stderr, "FAIL selftest name text\n");
        return EXIT_FAILURE;
    }
    if (!desk_interact(&state, &world) ||
        state.wizard_step != DESK_WIZARD_OUTFIT) {
        (void)fprintf(stderr, "FAIL selftest outfit step\n");
        return EXIT_FAILURE;
    }
    desk_update(&state, &world, 0, 1, DESK_TICK_SECONDS);
    if (state.wizard_outfit_cursor != 1 || !state.outfit_dirty ||
        !desk_interact(&state, &world) ||
        state.wizard_step != DESK_WIZARD_CONFIRM ||
        state.wizard_confirm_cursor != 0 ||
        !desk_validate(&state, &world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest confirm step: %s\n", error);
        return EXIT_FAILURE;
    }
    if (!desk_interact(&state, &world) || state.mode != DESK_MODE_ROOM ||
        !state.profile.first_run_done ||
        state.profile.cast != DESK_CAST_LEGEND ||
        state.profile.actor != DESK_ACTOR_HERO ||
        state.profile.outfit != 1u ||
        strcmp(state.profile.name, "ROOK") != 0 ||
        state.room != world.start_room ||
        !state.profile_dirty || !state.outfit_dirty ||
        !desk_validate(&state, &world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest wizard commit: %s\n", error);
        return EXIT_FAILURE;
    }
    if (!desk_profile_save(&state.profile) ||
        !desk_profile_load(&reloaded) || !reloaded.first_run_done ||
        strcmp(reloaded.name, "ROOK") != 0) {
        (void)fprintf(stderr, "FAIL selftest profile persist\n");
        return EXIT_FAILURE;
    }
    state.profile_dirty = false;
    state.outfit_dirty = false;
    (void)desk_take_audio_events(&state, events);

    /* Full inventory panel: movement is independent of the selected
     * hotbar index, and Enter drives move, merge, and swap in place. */
    {
        int coffee = desk_items_find(&catalog, "core:drink/coffee");
        int record = desk_items_find(&catalog, "core:media/record");

        if (coffee <= 0 || record <= 0) {
            (void)fprintf(stderr, "FAIL selftest inventory lookup\n");
            return EXIT_FAILURE;
        }
        desk_inventory_init(&state.items.inventory);
        state.items.inventory.slots[0] =
            desk_item_make((uint16_t)coffee, 3u);
        state.items.inventory.slots[2] =
            desk_item_make((uint16_t)coffee, 6u);
        state.items.inventory.slots[3] =
            desk_item_make((uint16_t)coffee, 5u);
        state.items.inventory.slots[4] =
            desk_item_make((uint16_t)record, 1u);
        state.items.inventory.selected = 9;
        state.inventory_cursor = 0;
        if (!desk_toggle_inventory(&state, &world) ||
            state.mode != DESK_MODE_INVENTORY ||
            state.inventory_mark != -1 ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest inventory open: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        desk_select_slot(&state, 8);
        desk_cycle_slot(&state, 1);
        if (state.items.inventory.selected != 9 ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr,
                          "FAIL selftest inventory hotbar keys: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        desk_update(&state, &world, 1, 0, DESK_TICK_SECONDS);
        if (state.inventory_cursor != 1 ||
            state.items.inventory.selected != 9 ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest inventory cursor: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        desk_update(&state, &world, -1, 0, DESK_TICK_SECONDS);
        if (state.inventory_cursor != 0 ||
            !desk_interact(&state, &world) ||
            state.inventory_mark != 0 ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest inventory mark: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        desk_update(&state, &world, 1, 0, DESK_TICK_SECONDS);
        if (!desk_interact(&state, &world) ||
            state.inventory_mark != -1 ||
            !desk_item_is_empty(&state.items.inventory.slots[0]) ||
            state.items.inventory.slots[1].quantity != 3u ||
            state.items.inventory.generation[0] != 1u ||
            state.items.inventory.generation[1] != 1u ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest inventory move: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        desk_update(&state, &world, 1, 0, DESK_TICK_SECONDS);
        desk_update(&state, &world, 1, 0, DESK_TICK_SECONDS);
        if (state.inventory_cursor != 3 ||
            !desk_interact(&state, &world) ||
            state.inventory_mark != 3 ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr,
                          "FAIL selftest inventory merge mark: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        desk_update(&state, &world, -1, 0, DESK_TICK_SECONDS);
        if (!desk_interact(&state, &world) ||
            state.items.inventory.slots[2].quantity != 8u ||
            state.items.inventory.slots[3].quantity != 3u ||
            state.inventory_mark != -1 ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest inventory merge: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        desk_update(&state, &world, 1, 0, DESK_TICK_SECONDS);
        if (!desk_interact(&state, &world) ||
            state.inventory_mark != 3 ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr,
                          "FAIL selftest inventory swap mark: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        desk_update(&state, &world, 1, 0, DESK_TICK_SECONDS);
        if (!desk_interact(&state, &world) ||
            state.items.inventory.slots[3].definition !=
                (uint16_t)record ||
            state.items.inventory.slots[4].definition !=
                (uint16_t)coffee ||
            state.items.inventory.slots[4].quantity != 3u ||
            state.items.inventory.selected != 9 ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest inventory swap: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        desk_cancel(&state, &world);
        if (state.mode != DESK_MODE_ROOM || state.inventory_mark != -1 ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest inventory close: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        desk_inventory_init(&state.items.inventory);
        state.inventory_cursor = 0;
        state.world_dirty = false;
        (void)desk_take_audio_events(&state, events);
    }

    desk_update(&state, &world, 1, 0, DESK_TICK_SECONDS);
    if (!state.player_moving || state.facing != DESK_FACING_RIGHT ||
        !desk_validate(&state, &world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest walk: %s\n", error);
        return EXIT_FAILURE;
    }

    living = desk_world_room_index(&world, "living");
    /* The door trigger is position-based, so place the player inside the
     * start room's door rect instead of walking a path the painted world
     * may have reshaped; the walk itself is exercised above. */
    {
        const desk_room *start_scene = &world.rooms[world.start_room];
        const desk_door *exit_door = NULL;
        int door_index;
        for (door_index = 0; door_index < start_scene->door_count;
             ++door_index)
            if (start_scene->doors[door_index].to_room == living) {
                exit_door = &start_scene->doors[door_index];
                break;
            }
        if (!exit_door) {
            (void)fprintf(stderr,
                          "FAIL selftest no door to living\n");
            return EXIT_FAILURE;
        }
        state.player_x = exit_door->rect.x + exit_door->rect.w * 0.5f;
        state.player_y = exit_door->rect.y + exit_door->rect.h * 0.5f;
    }
    state.door_cooldown_ticks = 0;
    for (guard = 0; guard < 30 && state.room == world.start_room; ++guard)
        desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
    if (living < 0 || state.room != living ||
        strcmp(state.profile.last_room, "living") != 0 ||
        state.door_cooldown_ticks == 0 || !state.profile_dirty ||
        !desk_validate(&state, &world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest door crossing: %s\n", error);
        return EXIT_FAILURE;
    }
    state.profile_dirty = false;
    (void)desk_take_audio_events(&state, events);

    state.player_x = 179.0f;
    state.player_y = 200.0f;
    desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
    if (state.nearest_object != 0 || !desk_interact(&state, &world) ||
        state.pending_launch != DESK_TARGET_GAMES ||
        desk_take_launch_request(&state) != DESK_TARGET_GAMES ||
        desk_take_launch_request(&state) != DESK_TARGET_NONE ||
        !desk_validate(&state, &world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest external launch: %s\n", error);
        return EXIT_FAILURE;
    }

    /* Item flow: the authored record spawn materialized into the living
     * room at first init; pick it up, set it down, retrieve it, exercise
     * hotbar selection, then prove a fresh init neither respawns the
     * claimed spawn nor loses the inventory. */
    {
        static desk_state fresh;
        int record = desk_items_find(&catalog, "core:media/record");
        int before_items = state.items.item_count;

        if (record <= 0 || before_items < 1 ||
            !desk_world_state_is_claimed(&state.items, "starter-record")) {
            (void)fprintf(stderr, "FAIL selftest spawn materialization\n");
            return EXIT_FAILURE;
        }
        state.player_x = 272.0f;
        state.player_y = 224.0f;
        desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        if (state.nearest_world_item < 0 ||
            !desk_interact(&state, &world) ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)record) != 1 ||
            state.items.item_count != before_items - 1 ||
            !state.world_dirty ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest pickup: %s\n", error);
            return EXIT_FAILURE;
        }
        (void)desk_take_audio_events(&state, events);
        state.facing = DESK_FACING_DOWN;
        if (!desk_drop_selected(&state, &world) ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)record) != 0 ||
            state.items.item_count != before_items) {
            (void)fprintf(stderr, "FAIL selftest drop\n");
            return EXIT_FAILURE;
        }
        desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        if (state.nearest_world_item < 0 ||
            !desk_interact(&state, &world) ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)record) != 1 ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest re-pickup: %s\n", error);
            return EXIT_FAILURE;
        }
        desk_select_slot(&state, 3);
        desk_cycle_slot(&state, 1);
        if (state.items.inventory.selected != 4) {
            (void)fprintf(stderr, "FAIL selftest slot selection\n");
            return EXIT_FAILURE;
        }
        desk_cycle_slot(&state, -1);
        desk_select_slot(&state, 0);
        (void)desk_take_audio_events(&state, events);
        if (!desk_world_state_save(&state.items, &catalog)) {
            (void)fprintf(stderr, "FAIL selftest world save\n");
            return EXIT_FAILURE;
        }
        state.world_dirty = false;
        desk_init(&fresh, &world, &catalog);
        if (desk_inventory_total(&fresh.items.inventory,
                                 (uint16_t)record) != 1 ||
            fresh.items.item_count != before_items - 1 ||
            !desk_world_state_is_claimed(&fresh.items, "starter-record")) {
            (void)fprintf(stderr, "FAIL selftest item persistence\n");
            return EXIT_FAILURE;
        }
    }

    /* Animation-timed tool action: inject the toolbox, stand at the
     * shed, and prove the impact lands exactly once on the authored
     * frame, movement stays locked until COMPLETE, and cancel before
     * impact has no world effect. */
    {
        desk_item_plan tool_plan;
        desk_item toolbox_item;
        int toolbox = desk_items_find(&catalog, "core:tool/toolbox");
        int yard = desk_world_room_index(&world, "yard");
        float locked_x;
        int tool_slot = -1;
        int slot_index;
        int tick;

        toolbox_item = desk_item_make((uint16_t)toolbox, 1u);
        if (toolbox <= 0 || yard < 0 ||
            !desk_inventory_plan_add(&state.items.inventory, &catalog,
                                     &toolbox_item, &tool_plan) ||
            !desk_inventory_commit_add(&state.items.inventory, &catalog,
                                       &toolbox_item, &tool_plan)) {
            (void)fprintf(stderr, "FAIL selftest toolbox inject\n");
            return EXIT_FAILURE;
        }
        for (slot_index = 0; slot_index < DESK_INVENTORY_SLOTS;
             ++slot_index)
            if (state.items.inventory.slots[slot_index].definition ==
                    (uint16_t)toolbox &&
                state.items.inventory.slots[slot_index].quantity > 0u)
                tool_slot = slot_index;
        state.room = yard;
        state.player_x = 386.0f;
        state.player_y = 214.0f;
        state.door_cooldown_ticks = DESK_DOOR_COOLDOWN_TICKS;
        desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        desk_select_slot(&state, tool_slot);
        if (tool_slot < 0 || state.nearest_object < 0 ||
            world.rooms[yard].objects[state.nearest_object].target !=
                DESK_TARGET_MAINTENANCE) {
            (void)fprintf(stderr, "FAIL selftest shed targeting\n");
            return EXIT_FAILURE;
        }
        if (!desk_use_item(&state, &world) || !state.action.active ||
            state.action.committed ||
            state.player_animator.clip != DESK_CLIP_USE_TOOL ||
            !state.player_animator.movement_locked ||
            desk_take_launch_request(&state) != DESK_TARGET_NONE) {
            (void)fprintf(stderr, "FAIL selftest tool start\n");
            return EXIT_FAILURE;
        }
        /* A second press and other interactions are refused mid-swing,
         * and held movement input moves nothing. */
        locked_x = state.player_x;
        if (desk_use_item(&state, &world) ||
            desk_interact(&state, &world) ||
            desk_drop_selected(&state, &world) ||
            desk_toggle_inventory(&state, &world)) {
            (void)fprintf(stderr, "FAIL selftest mid-swing refusal\n");
            return EXIT_FAILURE;
        }
        for (tick = 0; tick < 60 && !state.action.committed; ++tick)
            desk_update(&state, &world, 1, 0, DESK_TICK_SECONDS);
        if (!state.action.committed || state.player_x != locked_x ||
            state.pending_launch != DESK_TARGET_MAINTENANCE ||
            strcmp(state.pending_launch_object, "shed") != 0 ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)toolbox) != 1 ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest tool impact: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        if (desk_take_launch_request(&state) != DESK_TARGET_MAINTENANCE) {
            (void)fprintf(stderr, "FAIL selftest tool launch take\n");
            return EXIT_FAILURE;
        }
        for (tick = 0; tick < 60 && state.action.active; ++tick)
            desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        if (state.action.active || state.player_animator.movement_locked ||
            desk_take_launch_request(&state) != DESK_TARGET_NONE) {
            (void)fprintf(stderr, "FAIL selftest tool complete\n");
            return EXIT_FAILURE;
        }
        /* Cancel before impact: no launch, no lock, still in the room. */
        if (!desk_use_item(&state, &world) || !state.action.active) {
            (void)fprintf(stderr, "FAIL selftest tool restart\n");
            return EXIT_FAILURE;
        }
        desk_cancel(&state, &world);
        if (state.action.active || state.mode != DESK_MODE_ROOM ||
            state.player_animator.movement_locked ||
            desk_take_launch_request(&state) != DESK_TARGET_NONE ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest tool cancel: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        /* Walking must begin at the first walk phase now. */
        desk_update(&state, &world, 0, 1, DESK_TICK_SECONDS);
        if (!state.player_moving ||
            state.player_animator.clip != DESK_CLIP_WALK ||
            state.player_animator.frame != 0) {
            (void)fprintf(stderr, "FAIL selftest walk phase reset\n");
            return EXIT_FAILURE;
        }
        state.room = living;
        state.door_cooldown_ticks = DESK_DOOR_COOLDOWN_TICKS;
        (void)desk_take_audio_events(&state, events);
    }

    /* Receiver, drink, and placement: the rest of the vertical slice. */
    {
        static desk_state fresh2;
        const desk_room *living_room = &world.rooms[living];
        const desk_receiver_state *receiver;
        desk_item_plan seed_plan;
        desk_item seed;
        float ghost_x;
        float ghost_y;
        bool ghost_valid;
        int record = desk_items_find(&catalog, "core:media/record");
        int coffee = desk_items_find(&catalog, "core:drink/coffee");
        int plant = desk_items_find(&catalog, "core:decor/houseplant");
        int stereo_index = -1;
        int slot_index;
        int use_slot;
        int items_before;
        int tick;

        for (slot_index = 0; slot_index < living_room->object_count;
             ++slot_index)
            if (strcmp(living_room->objects[slot_index].id, "stereo") == 0)
                stereo_index = slot_index;
        use_slot = -1;
        for (slot_index = 0; slot_index < DESK_INVENTORY_SLOTS;
             ++slot_index)
            if (state.items.inventory.slots[slot_index].definition ==
                    (uint16_t)record &&
                state.items.inventory.slots[slot_index].quantity > 0u)
                use_slot = slot_index;
        state.player_x = 322.0f;
        state.player_y = 200.0f;
        desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        desk_select_slot(&state, use_slot);
        if (stereo_index < 0 || use_slot < 0 ||
            state.nearest_object != stereo_index) {
            (void)fprintf(stderr, "FAIL selftest stereo targeting\n");
            return EXIT_FAILURE;
        }
        if (!desk_use_item(&state, &world) ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)record) != 0 ||
            state.pending_launch != DESK_TARGET_MUSIC ||
            strcmp(state.pending_launch_object, "stereo") != 0) {
            (void)fprintf(stderr, "FAIL selftest record insert\n");
            return EXIT_FAILURE;
        }
        receiver = desk_receiver_lookup(
            &state, living_room, &living_room->objects[stereo_index]);
        if (!receiver ||
            receiver->phase != (uint8_t)DESK_RECEIVER_READY ||
            receiver->item.definition != (uint16_t)record ||
            desk_take_launch_request(&state) != DESK_TARGET_MUSIC ||
            !state.world_dirty ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest receiver ready: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        if (!desk_interact(&state, &world) ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)record) != 1 ||
            receiver->phase != (uint8_t)DESK_RECEIVER_EMPTY) {
            (void)fprintf(stderr, "FAIL selftest record eject\n");
            return EXIT_FAILURE;
        }
        (void)desk_take_audio_events(&state, events);

        seed = desk_item_make((uint16_t)coffee, 2u);
        if (coffee <= 0 ||
            !desk_inventory_plan_add(&state.items.inventory, &catalog,
                                     &seed, &seed_plan) ||
            !desk_inventory_commit_add(&state.items.inventory, &catalog,
                                       &seed, &seed_plan)) {
            (void)fprintf(stderr, "FAIL selftest coffee inject\n");
            return EXIT_FAILURE;
        }
        use_slot = -1;
        for (slot_index = 0; slot_index < DESK_INVENTORY_SLOTS;
             ++slot_index)
            if (state.items.inventory.slots[slot_index].definition ==
                    (uint16_t)coffee &&
                state.items.inventory.slots[slot_index].quantity > 0u)
                use_slot = slot_index;
        desk_select_slot(&state, use_slot);
        if (!desk_use_item(&state, &world) || !state.action.active ||
            state.player_animator.clip != DESK_CLIP_DRINK ||
            !state.player_animator.movement_locked) {
            (void)fprintf(stderr, "FAIL selftest drink start\n");
            return EXIT_FAILURE;
        }
        for (tick = 0; tick < 80 && !state.action.committed; ++tick)
            desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        if (!state.action.committed ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)coffee) != 1 ||
            state.items.effect_count != 1 ||
            state.items.effects[0].definition != (uint16_t)coffee) {
            (void)fprintf(stderr, "FAIL selftest drink swallow\n");
            return EXIT_FAILURE;
        }
        for (tick = 0; tick < 80 && state.action.active; ++tick)
            desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        /* Cancel before the swallow consumes nothing. */
        if (!desk_use_item(&state, &world)) {
            (void)fprintf(stderr, "FAIL selftest drink restart\n");
            return EXIT_FAILURE;
        }
        desk_cancel(&state, &world);
        if (state.action.active ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)coffee) != 1 ||
            state.items.effect_count != 1 ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest drink cancel: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        (void)desk_take_audio_events(&state, events);

        seed = desk_item_make((uint16_t)plant, 2u);
        if (plant <= 0 ||
            !desk_inventory_plan_add(&state.items.inventory, &catalog,
                                     &seed, &seed_plan) ||
            !desk_inventory_commit_add(&state.items.inventory, &catalog,
                                       &seed, &seed_plan)) {
            (void)fprintf(stderr, "FAIL selftest plant inject\n");
            return EXIT_FAILURE;
        }
        use_slot = -1;
        for (slot_index = 0; slot_index < DESK_INVENTORY_SLOTS;
             ++slot_index)
            if (state.items.inventory.slots[slot_index].definition ==
                    (uint16_t)plant &&
                state.items.inventory.slots[slot_index].quantity > 0u)
                use_slot = slot_index;
        state.player_x = 272.0f;
        state.player_y = 210.0f;
        state.facing = DESK_FACING_DOWN;
        desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        desk_select_slot(&state, use_slot);
        items_before = state.items.item_count;
        if (!desk_placement_preview(&state, &world, &ghost_x, &ghost_y,
                                    &ghost_valid) ||
            !ghost_valid) {
            (void)fprintf(stderr, "FAIL selftest placement preview\n");
            return EXIT_FAILURE;
        }
        if (!desk_use_item(&state, &world) ||
            state.items.item_count != items_before + 1 ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)plant) != 1 ||
            !state.items.items[items_before].placed ||
            state.items.items[items_before].x != ghost_x ||
            state.items.items[items_before].y != ghost_y) {
            (void)fprintf(stderr, "FAIL selftest placement commit\n");
            return EXIT_FAILURE;
        }
        /* A blocked spot never decrements the stack (the stereo is the
         * pick here, so no gift preempts the placement path). */
        state.player_x = 322.0f;
        state.player_y = 199.0f;
        state.facing = DESK_FACING_UP;
        desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        if (!desk_placement_preview(&state, &world, &ghost_x, &ghost_y,
                                    &ghost_valid) ||
            ghost_valid ||
            !desk_use_item(&state, &world) ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)plant) != 1 ||
            state.items.item_count != items_before + 1) {
            (void)fprintf(stderr, "FAIL selftest blocked placement\n");
            return EXIT_FAILURE;
        }
        (void)desk_take_audio_events(&state, events);

        /* Reinsert the record, save, and a fresh init must restore the
         * receiver still holding it. */
        use_slot = -1;
        for (slot_index = 0; slot_index < DESK_INVENTORY_SLOTS;
             ++slot_index)
            if (state.items.inventory.slots[slot_index].definition ==
                    (uint16_t)record &&
                state.items.inventory.slots[slot_index].quantity > 0u)
                use_slot = slot_index;
        state.player_x = 322.0f;
        state.player_y = 200.0f;
        state.facing = DESK_FACING_UP;
        desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        desk_select_slot(&state, use_slot);
        if (!desk_use_item(&state, &world) ||
            desk_take_launch_request(&state) != DESK_TARGET_MUSIC ||
            !desk_world_state_save(&state.items, &catalog)) {
            (void)fprintf(stderr, "FAIL selftest receiver save\n");
            return EXIT_FAILURE;
        }
        state.world_dirty = false;
        desk_init(&fresh2, &world, &catalog);
        receiver = desk_receiver_lookup(
            &fresh2, living_room, &living_room->objects[stereo_index]);
        if (!receiver ||
            receiver->phase != (uint8_t)DESK_RECEIVER_READY ||
            receiver->item.definition != (uint16_t)record ||
            desk_inventory_total(&fresh2.items.inventory,
                                 (uint16_t)record) != 0 ||
            fresh2.items.effect_count != 1) {
            (void)fprintf(stderr, "FAIL selftest receiver persistence\n");
            return EXIT_FAILURE;
        }
        (void)desk_take_audio_events(&state, events);
    }

    /* Kitchen receiver machines: trash destroys one discardable unit with
     * no launch, while the kettle transforms two authored tea-leaf inputs
     * across a full run and a save/resume run. */
    {
        const desk_room *kitchen_room;
        const desk_receiver_state *receiver;
        int kitchen = desk_world_room_index(&world, "kitchen");
        int plant = desk_items_find(&catalog, "core:decor/houseplant");
        int leaves = desk_items_find(&catalog,
                                     "core:drink/tea-leaves");
        int tea = desk_items_find(&catalog, "core:drink/fresh-tea");
        int trash_index = -1;
        int kettle_index = -1;
        int use_slot = -1;
        int before_total;
        int saved_remaining;
        int slot_index;
        int tick;

        if (kitchen < 0 || plant <= 0 || leaves <= 0 || tea <= 0) {
            (void)fprintf(stderr, "FAIL selftest kitchen catalog\n");
            return EXIT_FAILURE;
        }
        kitchen_room = &world.rooms[kitchen];
        for (slot_index = 0; slot_index < kitchen_room->object_count;
             ++slot_index) {
            if (strcmp(kitchen_room->objects[slot_index].id,
                       "trash-can") == 0)
                trash_index = slot_index;
            if (strcmp(kitchen_room->objects[slot_index].id,
                       "kettle") == 0)
                kettle_index = slot_index;
        }
        for (slot_index = 0; slot_index < DESK_INVENTORY_SLOTS;
             ++slot_index)
            if (state.items.inventory.slots[slot_index].definition ==
                    (uint16_t)plant &&
                state.items.inventory.slots[slot_index].quantity > 0u)
                use_slot = slot_index;
        state.room = kitchen;
        state.player_x = 71.0f;
        state.player_y = 215.0f;
        state.door_cooldown_ticks = DESK_DOOR_COOLDOWN_TICKS;
        desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        desk_select_slot(&state, use_slot);
        before_total = desk_inventory_total(&state.items.inventory,
                                            (uint16_t)plant);
        if (trash_index < 0 || use_slot < 0 || before_total < 1 ||
            state.nearest_object != trash_index ||
            !desk_use_item(&state, &world) ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)plant) != before_total - 1 ||
            state.pending_launch != DESK_TARGET_NONE) {
            (void)fprintf(stderr, "FAIL selftest trash consume\n");
            return EXIT_FAILURE;
        }
        receiver = desk_receiver_lookup(
            &state, kitchen_room, &kitchen_room->objects[trash_index]);
        if (!receiver ||
            receiver->phase != (uint8_t)DESK_RECEIVER_EMPTY ||
            !desk_item_is_empty(&receiver->item) ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest trash receiver: %s\n",
                          error);
            return EXIT_FAILURE;
        }

        /* An empty kettle is an internal fixture, not a launch request. */
        state.player_x = 407.0f;
        state.player_y = 220.0f;
        desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        if (kettle_index < 0 || state.nearest_object != kettle_index ||
            !desk_interact(&state, &world) ||
            strcmp(state.toast, "The kettle hums patiently.") != 0 ||
            state.pending_launch != DESK_TARGET_NONE) {
            (void)fprintf(stderr, "FAIL selftest empty kettle\n");
            return EXIT_FAILURE;
        }

        /* Pick up the authored two-unit leaf stack. */
        state.player_x = 330.0f;
        state.player_y = 234.0f;
        desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        if (state.nearest_world_item < 0 ||
            !desk_interact(&state, &world) ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)leaves) != 2) {
            (void)fprintf(stderr, "FAIL selftest tea leaves pickup\n");
            return EXIT_FAILURE;
        }

        use_slot = -1;
        for (slot_index = 0; slot_index < DESK_INVENTORY_SLOTS;
             ++slot_index)
            if (state.items.inventory.slots[slot_index].definition ==
                    (uint16_t)leaves &&
                state.items.inventory.slots[slot_index].quantity > 0u)
                use_slot = slot_index;
        state.player_x = 407.0f;
        state.player_y = 220.0f;
        desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        desk_select_slot(&state, use_slot);
        if (use_slot < 0 || state.nearest_object != kettle_index ||
            !desk_use_item(&state, &world) ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)leaves) != 1 ||
            state.pending_launch != DESK_TARGET_NONE) {
            (void)fprintf(stderr, "FAIL selftest kettle insert\n");
            return EXIT_FAILURE;
        }
        receiver = desk_receiver_lookup(
            &state, kitchen_room, &kitchen_room->objects[kettle_index]);
        if (!receiver ||
            receiver->phase != (uint8_t)DESK_RECEIVER_PROCESSING ||
            receiver->remaining_ticks != 1800 ||
            receiver->item.definition != (uint16_t)leaves ||
            receiver->item.quantity != 1u) {
            (void)fprintf(stderr, "FAIL selftest kettle processing\n");
            return EXIT_FAILURE;
        }
        for (tick = 0; tick < 1800; ++tick)
            desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        if (receiver->phase != (uint8_t)DESK_RECEIVER_READY ||
            receiver->remaining_ticks != 0 ||
            receiver->item.definition != (uint16_t)tea ||
            receiver->item.quantity != 1u || receiver->item.serial != 0u ||
            state.pending_launch != DESK_TARGET_NONE) {
            (void)fprintf(stderr, "FAIL selftest kettle output\n");
            return EXIT_FAILURE;
        }
        before_total = desk_inventory_total(&state.items.inventory,
                                            (uint16_t)tea);
        if (!desk_interact(&state, &world) ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)tea) != before_total + 1 ||
            receiver->phase != (uint8_t)DESK_RECEIVER_EMPTY) {
            (void)fprintf(stderr, "FAIL selftest kettle first collect\n");
            return EXIT_FAILURE;
        }

        /* The second input checkpoints in PROCESSING, then a fresh desk
         * state resumes the exact saved countdown and brews the same output. */
        use_slot = -1;
        for (slot_index = 0; slot_index < DESK_INVENTORY_SLOTS;
             ++slot_index)
            if (state.items.inventory.slots[slot_index].definition ==
                    (uint16_t)leaves &&
                state.items.inventory.slots[slot_index].quantity > 0u)
                use_slot = slot_index;
        desk_select_slot(&state, use_slot);
        if (use_slot < 0 || !desk_use_item(&state, &world)) {
            (void)fprintf(stderr, "FAIL selftest kettle second insert\n");
            return EXIT_FAILURE;
        }
        receiver = desk_receiver_lookup(
            &state, kitchen_room, &kitchen_room->objects[kettle_index]);
        if (!receiver ||
            receiver->phase != (uint8_t)DESK_RECEIVER_PROCESSING ||
            receiver->remaining_ticks != 1800) {
            (void)fprintf(stderr,
                          "FAIL selftest kettle second processing\n");
            return EXIT_FAILURE;
        }
        for (tick = 0; tick < 600; ++tick)
            desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        saved_remaining = receiver->remaining_ticks;
        if (saved_remaining != 1200 ||
            !desk_world_state_save(&state.items, &catalog)) {
            (void)fprintf(stderr, "FAIL selftest kettle mid-save\n");
            return EXIT_FAILURE;
        }
        state.world_dirty = false;
        desk_init(&state, &world, &catalog);
        receiver = desk_receiver_lookup(
            &state, kitchen_room, &kitchen_room->objects[kettle_index]);
        if (!receiver ||
            receiver->phase != (uint8_t)DESK_RECEIVER_PROCESSING ||
            receiver->remaining_ticks != saved_remaining ||
            receiver->item.definition != (uint16_t)leaves ||
            receiver->item.quantity != 1u) {
            (void)fprintf(stderr, "FAIL selftest kettle resume\n");
            return EXIT_FAILURE;
        }
        state.room = kitchen;
        state.player_x = 407.0f;
        state.player_y = 220.0f;
        state.door_cooldown_ticks = DESK_DOOR_COOLDOWN_TICKS;
        for (tick = 0; tick < saved_remaining; ++tick)
            desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        if (receiver->phase != (uint8_t)DESK_RECEIVER_READY ||
            receiver->item.definition != (uint16_t)tea ||
            receiver->item.quantity != 1u || receiver->item.serial != 0u ||
            state.nearest_object != kettle_index ||
            state.pending_launch != DESK_TARGET_NONE) {
            (void)fprintf(stderr, "FAIL selftest kettle resumed output\n");
            return EXIT_FAILURE;
        }
        before_total = desk_inventory_total(&state.items.inventory,
                                            (uint16_t)tea);
        if (!desk_interact(&state, &world) ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)tea) != before_total + 1 ||
            receiver->phase != (uint8_t)DESK_RECEIVER_EMPTY ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest kettle collect: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        state.room = living;
        state.door_cooldown_ticks = DESK_DOOR_COOLDOWN_TICKS;
        (void)desk_take_audio_events(&state, events);
    }

    /* Gifts and the accessory slot. */
    {
        static desk_state fresh3;
        desk_item_plan seed_plan;
        desk_item seed;
        int postcard = desk_items_find(&catalog, "core:gift/postcard");
        int coffee = desk_items_find(&catalog, "core:drink/coffee");
        int toolbox = desk_items_find(&catalog, "core:tool/toolbox");
        int pin = desk_items_find(&catalog, "core:gear/lantern-pin");
        int use_slot;
        int slot_index;
        int tick;

        seed = desk_item_make((uint16_t)postcard, 1u);
        if (postcard <= 0 || coffee <= 0 || toolbox <= 0 || pin <= 0 ||
            !desk_inventory_plan_add(&state.items.inventory, &catalog,
                                     &seed, &seed_plan) ||
            !desk_inventory_commit_add(&state.items.inventory, &catalog,
                                       &seed, &seed_plan)) {
            (void)fprintf(stderr, "FAIL selftest postcard inject\n");
            return EXIT_FAILURE;
        }
        state.player_x = 120.0f;
        state.player_y = 210.0f;
        desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        /* A non-giftable item is declined without moving. */
        use_slot = -1;
        for (slot_index = 0; slot_index < DESK_INVENTORY_SLOTS;
             ++slot_index)
            if (state.items.inventory.slots[slot_index].definition ==
                    (uint16_t)toolbox &&
                state.items.inventory.slots[slot_index].quantity > 0u)
                use_slot = slot_index;
        desk_select_slot(&state, use_slot);
        if (use_slot < 0 || state.nearest_npc != 0 ||
            !desk_use_item(&state, &world) || state.action.active ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)toolbox) != 1) {
            (void)fprintf(stderr, "FAIL selftest gift refusal\n");
            return EXIT_FAILURE;
        }
        use_slot = -1;
        for (slot_index = 0; slot_index < DESK_INVENTORY_SLOTS;
             ++slot_index)
            if (state.items.inventory.slots[slot_index].definition ==
                    (uint16_t)postcard &&
                state.items.inventory.slots[slot_index].quantity > 0u)
                use_slot = slot_index;
        desk_select_slot(&state, use_slot);
        if (!desk_use_item(&state, &world) || !state.action.active ||
            state.player_animator.clip != DESK_CLIP_GIVE) {
            (void)fprintf(stderr, "FAIL selftest gift start\n");
            return EXIT_FAILURE;
        }
        for (tick = 0; tick < 80 && !state.action.committed; ++tick)
            desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        if (!state.action.committed ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)postcard) != 0 ||
            state.items.social_count != 1 ||
            state.items.social[0].actor != 1u ||
            state.items.social[0].points != 50 ||
            state.items.social[0].gifts != 1u ||
            strcmp(state.toast, "EMBER BADGER treasures it.") != 0 ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL selftest gift handoff: %s\n",
                          error);
            return EXIT_FAILURE;
        }
        for (tick = 0; tick < 80 && state.action.active; ++tick)
            desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);

        /* Ember Badger dislikes generic giftables. The coffee still
         * leaves the inventory while friendship drops at the handoff. */
        {
            int coffee_before = desk_inventory_total(
                &state.items.inventory, (uint16_t)coffee);

            seed = desk_item_make((uint16_t)coffee, 1u);
            if (!desk_inventory_plan_add(&state.items.inventory, &catalog,
                                         &seed, &seed_plan) ||
                !desk_inventory_commit_add(&state.items.inventory,
                                           &catalog, &seed, &seed_plan)) {
                (void)fprintf(stderr,
                              "FAIL selftest dislike gift inject\n");
                return EXIT_FAILURE;
            }
            use_slot = -1;
            for (slot_index = 0; slot_index < DESK_INVENTORY_SLOTS;
                 ++slot_index)
                if (state.items.inventory.slots[slot_index].definition ==
                        (uint16_t)coffee &&
                    state.items.inventory.slots[slot_index].quantity > 0u)
                    use_slot = slot_index;
            desk_select_slot(&state, use_slot);
            if (use_slot < 0 || !desk_use_item(&state, &world) ||
                !state.action.active ||
                state.player_animator.clip != DESK_CLIP_GIVE) {
                (void)fprintf(stderr,
                              "FAIL selftest dislike gift start\n");
                return EXIT_FAILURE;
            }
            for (tick = 0; tick < 80 && !state.action.committed; ++tick)
                desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
            if (!state.action.committed ||
                desk_inventory_total(&state.items.inventory,
                                     (uint16_t)coffee) != coffee_before ||
                state.items.social[0].points != 40 ||
                state.items.social[0].gifts != 2u ||
                strcmp(state.toast,
                       "EMBER BADGER pretends to like it.") != 0 ||
                !desk_validate(&state, &world, error, sizeof error)) {
                (void)fprintf(stderr,
                              "FAIL selftest dislike gift handoff: %s\n",
                              error);
                return EXIT_FAILURE;
            }
            for (tick = 0; tick < 80 && state.action.active; ++tick)
                desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        }

        /* Equip, then swap: exclusive ownership both ways. Step out of
         * gift reach first — the pin is giftable too. */
        state.player_x = 240.0f;
        state.player_y = 212.0f;
        desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        seed = desk_item_make((uint16_t)pin, 1u);
        seed.serial = desk_world_state_take_serial(&state.items);
        if (!desk_inventory_plan_add(&state.items.inventory, &catalog,
                                     &seed, &seed_plan) ||
            !desk_inventory_commit_add(&state.items.inventory, &catalog,
                                       &seed, &seed_plan)) {
            (void)fprintf(stderr, "FAIL selftest pin inject\n");
            return EXIT_FAILURE;
        }
        use_slot = -1;
        for (slot_index = 0; slot_index < DESK_INVENTORY_SLOTS;
             ++slot_index)
            if (state.items.inventory.slots[slot_index].definition ==
                    (uint16_t)pin &&
                state.items.inventory.slots[slot_index].quantity > 0u)
                use_slot = slot_index;
        desk_select_slot(&state, use_slot);
        if (!desk_use_item(&state, &world) ||
            state.items.equipment[DESK_EQUIP_ACCESSORY].definition !=
                (uint16_t)pin ||
            desk_inventory_total(&state.items.inventory,
                                 (uint16_t)pin) != 0) {
            (void)fprintf(stderr, "FAIL selftest equip\n");
            return EXIT_FAILURE;
        }
        {
            uint32_t worn_serial =
                state.items.equipment[DESK_EQUIP_ACCESSORY].serial;

            seed = desk_item_make((uint16_t)pin, 1u);
            seed.serial = desk_world_state_take_serial(&state.items);
            if (!desk_inventory_plan_add(&state.items.inventory, &catalog,
                                         &seed, &seed_plan) ||
                !desk_inventory_commit_add(&state.items.inventory,
                                           &catalog, &seed, &seed_plan)) {
                (void)fprintf(stderr, "FAIL selftest second pin\n");
                return EXIT_FAILURE;
            }
            use_slot = -1;
            for (slot_index = 0; slot_index < DESK_INVENTORY_SLOTS;
                 ++slot_index)
                if (state.items.inventory.slots[slot_index].definition ==
                        (uint16_t)pin &&
                    state.items.inventory.slots[slot_index].quantity > 0u)
                    use_slot = slot_index;
            desk_select_slot(&state, use_slot);
            if (!desk_use_item(&state, &world) ||
                state.items.equipment[DESK_EQUIP_ACCESSORY].serial ==
                    worn_serial ||
                desk_inventory_total(&state.items.inventory,
                                     (uint16_t)pin) != 1) {
                (void)fprintf(stderr, "FAIL selftest equip swap\n");
                return EXIT_FAILURE;
            }
        }
        if (!desk_world_state_save(&state.items, &catalog)) {
            (void)fprintf(stderr, "FAIL selftest social save\n");
            return EXIT_FAILURE;
        }
        state.world_dirty = false;
        desk_init(&fresh3, &world, &catalog);
        if (fresh3.items.social_count != 1 ||
            fresh3.items.social[0].points != 40 ||
            fresh3.items.equipment[DESK_EQUIP_ACCESSORY].definition !=
                (uint16_t)pin) {
            (void)fprintf(stderr, "FAIL selftest social persistence\n");
            return EXIT_FAILURE;
        }
        (void)desk_take_audio_events(&state, events);
    }

    state.player_x = 120.0f;
    state.player_y = 210.0f;
    desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
    if (state.nearest_npc != 0 || !desk_interact(&state, &world) ||
        state.mode != DESK_MODE_DIALOGUE ||
        state.conversation_npc != (int)DESK_ACTOR_ALLY_1 ||
        desk_dialogue_count(&state) < 2 ||
        desk_dialogue_speaker(&state) != (int)DESK_ACTOR_ALLY_1 ||
        desk_dialogue_text(&state)[0] == '\0' ||
        !desk_validate(&state, &world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest dialogue open: %s\n", error);
        return EXIT_FAILURE;
    }
    if (!desk_interact(&state, &world) ||
        desk_dialogue_visible_chars(&state) !=
            strlen(desk_dialogue_text(&state)) ||
        !desk_interact(&state, &world) || state.dialogue_beat != 1 ||
        !desk_validate(&state, &world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest dialogue advance: %s\n", error);
        return EXIT_FAILURE;
    }
    desk_cancel(&state, &world);
    if (state.mode != DESK_MODE_ROOM || state.conversation_npc != -1 ||
        !desk_validate(&state, &world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest dialogue close: %s\n", error);
        return EXIT_FAILURE;
    }

    desk_cancel(&state, &world);
    if (state.mode != DESK_MODE_PAUSE || state.pause_cursor != 0 ||
        !desk_validate(&state, &world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest pause open: %s\n", error);
        return EXIT_FAILURE;
    }
    /* No desktop.conf in the temp config home, so the Debug entry is
     * present: walk into the submenu and back out, then reach QUIT by
     * label rather than by a hard-coded index. */
    if (desk_pause_item_count(&state) != 5 ||
        strcmp(desk_pause_item(&state, 2), "SETTINGS") != 0 ||
        strcmp(desk_pause_item(&state, 3), "DEBUG") != 0) {
        (void)fprintf(stderr, "FAIL selftest pause debug entry missing\n");
        return EXIT_FAILURE;
    }
    while (state.pause_cursor < desk_pause_item_count(&state) - 1 &&
           strcmp(desk_pause_item(&state, state.pause_cursor),
                  "DEBUG") != 0)
        desk_update(&state, &world, 0, 1, DESK_TICK_SECONDS);
    if (!desk_interact(&state, &world) || !state.pause_debug ||
        desk_pause_item_count(&state) != 2 ||
        strcmp(desk_pause_item(&state, 0), "WALK EDITOR") != 0 ||
        !desk_validate(&state, &world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest debug submenu: %s\n", error);
        return EXIT_FAILURE;
    }
    desk_update(&state, &world, 0, 1, DESK_TICK_SECONDS);
    if (!desk_interact(&state, &world) || state.pause_debug ||
        state.mode != DESK_MODE_PAUSE) {
        (void)fprintf(stderr, "FAIL selftest debug back\n");
        return EXIT_FAILURE;
    }
    desk_cancel(&state, &world);
    desk_cancel(&state, &world);
    if (state.mode != DESK_MODE_PAUSE) {
        (void)fprintf(stderr, "FAIL selftest pause reopen\n");
        return EXIT_FAILURE;
    }
    while (state.pause_cursor < desk_pause_item_count(&state) - 1 &&
           strcmp(desk_pause_item(&state, state.pause_cursor),
                  "QUIT") != 0)
        desk_update(&state, &world, 0, 1, DESK_TICK_SECONDS);
    if (strcmp(desk_pause_item(&state, state.pause_cursor), "QUIT") != 0 ||
        !desk_interact(&state, &world) ||
        state.mode != DESK_MODE_CONFIRM ||
        state.confirm != DESK_CONFIRM_QUIT || state.confirm_cursor != 1 ||
        !desk_validate(&state, &world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest pause quit entry: %s\n", error);
        return EXIT_FAILURE;
    }
    desk_update(&state, &world, 0, 1, DESK_TICK_SECONDS);
    if (state.confirm_cursor != 0 || !desk_interact(&state, &world) ||
        !state.quit_requested || state.mode != DESK_MODE_ROOM ||
        state.confirm != DESK_CONFIRM_NONE ||
        strcmp(state.profile.last_room, "living") != 0 ||
        !state.profile_dirty ||
        !desk_validate(&state, &world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest quit confirm: %s\n", error);
        return EXIT_FAILURE;
    }

    for (target = 1; target < DESK_TARGET_COUNT; ++target) {
        const char *name = desk_target_name((desk_target)target);
        if (!name || name[0] == '\0' ||
            desk_target_from_string(name) != (desk_target)target ||
            desk_target_label((desk_target)target)[0] == '\0') {
            (void)fprintf(stderr, "FAIL selftest target=%d\n", target);
            return EXIT_FAILURE;
        }
    }
    if (desk_target_from_string("no-such-target") != DESK_TARGET_NONE ||
        !desk_target_is_external(DESK_TARGET_TERMINAL) ||
        !desk_target_is_external(DESK_TARGET_MAINTENANCE) ||
        desk_target_is_external(DESK_TARGET_WARDROBE) ||
        desk_target_is_external(DESK_TARGET_KETTLE) ||
        desk_target_is_external(DESK_TARGET_NONE)) {
        (void)fprintf(stderr, "FAIL selftest target classes\n");
        return EXIT_FAILURE;
    }
    (void)printf(
        "PASS selftest rooms=%d wizard=cast-actor-name-outfit-confirm "
        "door=bedroom->living launch=games dialogue=reveal-advance-close "
        "pause=quit-confirm walkbehinds=bad-rejected "
        "items=materialize-pickup-drop-persist tool=impact-timed "
        "receiver=insert-eject-persist drink=swallow-frame "
        "receivers=trash-consume+kettle-brew "
        "place=ghost-committed gift=handoff-frame equip=swap-persist "
        "inventory=panel-move-merge-swap targets=%d\n",
        world.room_count, DESK_TARGET_COUNT - 1);
    return EXIT_SUCCESS;
}

static int selftest(void)
{
    char config_dir[1024];
    int result;
    if (!make_temp_config(config_dir, sizeof config_dir)) {
        (void)fprintf(stderr, "FAIL selftest temp config\n");
        return EXIT_FAILURE;
    }
    result = selftest_body();
    if (!remove_tree(config_dir) && result == EXIT_SUCCESS) {
        (void)fprintf(stderr, "FAIL selftest temp cleanup\n");
        result = EXIT_FAILURE;
    }
    return result;
}

/* Like make_temp_config, but without touching the config-home variable:
 * the JSON tests only need a place to write fixture files. */
static bool make_scratch_directory(char *directory, size_t size)
{
    const char *base = getenv("TMPDIR");
    int written;
    if (!base || base[0] == '\0') base = "/tmp";
    written = snprintf(directory, size, "%s/kilix-land-desktop-json.XXXXXX",
                       base);
    if (written < 0 || (size_t)written >= size) return false;
    return mkdtemp(directory) != NULL;
}

enum {
    JSON_CASE_STRING = 0,
    JSON_CASE_NUMBER = 1,
    JSON_CASE_BOOL = 2,
    JSON_CASE_OBJECT = 3,
    JSON_CASE_ARRAY = 4
};

typedef struct json_case {
    const char *label;
    const char *text;
    int kind;
    bool expect_ok;
    const char *expect_error;  /* exact message when expect_ok is false */
    const char *expect_string; /* JSON_CASE_STRING */
    float expect_number;       /* NUMBER value; OBJECT/ARRAY value sum */
    bool expect_bool;
    size_t expect_end;         /* reader offset after success */
    size_t string_capacity;    /* JSON_CASE_STRING; 0 = full buffer */
} json_case;

/* The reader owns tokens only, so the schema-shaped walks below mirror how
 * rooms.c drives it: known keys claim bits, unknown keys fail at the key
 * offset, values are numbers. */
static const json_case JSON_CASES[] = {
    {.label = "string-plain", .text = "\"abc\"", .kind = JSON_CASE_STRING,
     .expect_ok = true, .expect_string = "abc", .expect_end = 5u},
    {.label = "string-escapes", .text = "\"a\\\"b\\\\c\\/d\\ne\\tf\"",
     .kind = JSON_CASE_STRING, .expect_ok = true,
     .expect_string = "a\"b\\c/d\ne\tf", .expect_end = 18u},
    {.label = "string-leading-ws", .text = "  \"pad\"",
     .kind = JSON_CASE_STRING, .expect_ok = true, .expect_string = "pad",
     .expect_end = 7u},
    {.label = "string-unterminated", .text = "\"abc",
     .kind = JSON_CASE_STRING,
     .expect_error = "t:0: unterminated string"},
    {.label = "string-escape-at-eof", .text = "\"a\\",
     .kind = JSON_CASE_STRING,
     .expect_error = "t:0: unterminated string"},
    {.label = "string-control-char", .text = "\"a\tb\"",
     .kind = JSON_CASE_STRING,
     .expect_error = "t:2: raw control character in string"},
    {.label = "string-bad-escape", .text = "\"a\\qb\"",
     .kind = JSON_CASE_STRING,
     .expect_error = "t:3: unsupported escape sequence"},
    {.label = "string-over-capacity", .text = "\"abcd\"",
     .kind = JSON_CASE_STRING, .string_capacity = 4u,
     .expect_error = "t:0: string longer than capacity 3"},
    {.label = "string-not-a-string", .text = "123",
     .kind = JSON_CASE_STRING, .expect_error = "t:0: expected string"},
    {.label = "number-integer", .text = "42", .kind = JSON_CASE_NUMBER,
     .expect_ok = true, .expect_number = 42.0f, .expect_end = 2u},
    {.label = "number-negative-fraction", .text = "-12.5",
     .kind = JSON_CASE_NUMBER, .expect_ok = true, .expect_number = -12.5f,
     .expect_end = 5u},
    {.label = "number-leading-ws", .text = " 7", .kind = JSON_CASE_NUMBER,
     .expect_ok = true, .expect_number = 7.0f, .expect_end = 2u},
    {.label = "number-bare-minus", .text = "-", .kind = JSON_CASE_NUMBER,
     .expect_error = "t:0: expected number"},
    {.label = "number-open-fraction", .text = "1.",
     .kind = JSON_CASE_NUMBER,
     .expect_error = "t:2: expected digit after decimal point"},
    {.label = "number-leading-dot", .text = ".5", .kind = JSON_CASE_NUMBER,
     .expect_error = "t:0: expected number"},
    {.label = "number-too-long",
     .text = "111111111111111111111111111111111111111111111111111111111111",
     .kind = JSON_CASE_NUMBER, .expect_error = "t:0: number too long"},
    {.label = "number-no-exponent", .text = "1e3", .kind = JSON_CASE_NUMBER,
     .expect_ok = true, .expect_number = 1.0f, .expect_end = 1u},
    {.label = "bool-true", .text = "true", .kind = JSON_CASE_BOOL,
     .expect_ok = true, .expect_bool = true, .expect_end = 4u},
    {.label = "bool-false", .text = "false", .kind = JSON_CASE_BOOL,
     .expect_ok = true, .expect_bool = false, .expect_end = 5u},
    {.label = "bool-leading-ws", .text = " true", .kind = JSON_CASE_BOOL,
     .expect_ok = true, .expect_bool = true, .expect_end = 5u},
    {.label = "bool-prefix-only", .text = "truex", .kind = JSON_CASE_BOOL,
     .expect_ok = true, .expect_bool = true, .expect_end = 4u},
    {.label = "bool-truncated", .text = "tru", .kind = JSON_CASE_BOOL,
     .expect_error = "t:0: expected true or false"},
    {.label = "object-two-keys", .text = "{\"a\":1,\"b\":2}",
     .kind = JSON_CASE_OBJECT, .expect_ok = true, .expect_number = 3.0f,
     .expect_end = 13u},
    {.label = "object-empty", .text = "{}", .kind = JSON_CASE_OBJECT,
     .expect_ok = true, .expect_number = 0.0f, .expect_end = 2u},
    {.label = "object-missing-comma", .text = "{\"a\":1 \"b\":2}",
     .kind = JSON_CASE_OBJECT, .expect_error = "t:7: expected ','"},
    {.label = "object-duplicate-key", .text = "{\"a\":1,\"a\":2}",
     .kind = JSON_CASE_OBJECT, .expect_error = "t:7: duplicate key 'a'"},
    {.label = "object-missing-colon", .text = "{\"a\" 1}",
     .kind = JSON_CASE_OBJECT, .expect_error = "t:5: expected ':'"},
    {.label = "object-unterminated", .text = "{\"a\":1",
     .kind = JSON_CASE_OBJECT,
     .expect_error = "t:6: unterminated object"},
    {.label = "object-unknown-key", .text = "{\"z\":1}",
     .kind = JSON_CASE_OBJECT, .expect_error = "t:1: unknown key 'z'"},
    {.label = "object-bare-comma", .text = "{,}",
     .kind = JSON_CASE_OBJECT, .expect_error = "t:1: expected string"},
    {.label = "array-three", .text = "[1,2,3]", .kind = JSON_CASE_ARRAY,
     .expect_ok = true, .expect_number = 6.0f, .expect_end = 7u},
    {.label = "array-empty", .text = "[]", .kind = JSON_CASE_ARRAY,
     .expect_ok = true, .expect_number = 0.0f, .expect_end = 2u},
    {.label = "array-missing-comma", .text = "[1 2]",
     .kind = JSON_CASE_ARRAY, .expect_error = "t:3: expected ','"},
    {.label = "array-unterminated", .text = "[1", .kind = JSON_CASE_ARRAY,
     .expect_error = "t:2: unterminated array"},
    {.label = "array-trailing-comma", .text = "[1,",
     .kind = JSON_CASE_ARRAY, .expect_error = "t:3: expected number"},
};

static bool json_walk_object(desk_json_reader *reader, float *sum)
{
    static const char *const keys[4] = {"a", "b", "c", "d"};
    bool first = true;
    unsigned seen = 0u;
    char key[8];

    if (!desk_json_expect(reader, '{')) return false;
    for (;;) {
        float value = 0.0f;
        size_t index;
        int step = desk_json_next_key(reader, &first, key, sizeof key);

        if (step < 0) return false;
        if (step == 0) return true;
        for (index = 0u; index < 4u; ++index)
            if (strcmp(key, keys[index]) == 0) break;
        if (index == 4u)
            return desk_json_fail_at(reader, reader->key_offset,
                                     "unknown key '%s'", key);
        if (!desk_json_claim_key(reader, &seen, 1u << index,
                                 keys[index]) ||
            !desk_json_parse_number(reader, &value))
            return false;
        *sum += value;
    }
}

static bool json_walk_array(desk_json_reader *reader, float *sum)
{
    bool first = true;

    if (!desk_json_expect(reader, '[')) return false;
    for (;;) {
        float value = 0.0f;
        int step = desk_json_next_element(reader, &first);

        if (step < 0) return false;
        if (step == 0) return true;
        if (!desk_json_parse_number(reader, &value)) return false;
        *sum += value;
    }
}

static bool json_case_run(const json_case *test)
{
    desk_json_reader reader;
    char error[DESK_ERROR_CAPACITY];
    char out[32];
    float number = 0.0f;
    bool value = false;
    bool ok = false;

    (void)memset(&reader, 0, sizeof reader);
    reader.text = test->text;
    reader.length = strlen(test->text);
    reader.name = "t";
    reader.error = error;
    reader.error_size = sizeof error;
    error[0] = '\0';
    out[0] = '\0';

    switch (test->kind) {
    case JSON_CASE_STRING:
        ok = desk_json_parse_string(&reader, out,
                                    test->string_capacity != 0u ?
                                    test->string_capacity : sizeof out);
        break;
    case JSON_CASE_NUMBER:
        ok = desk_json_parse_number(&reader, &number);
        break;
    case JSON_CASE_BOOL:
        ok = desk_json_parse_bool(&reader, &value);
        break;
    case JSON_CASE_OBJECT:
        ok = json_walk_object(&reader, &number);
        break;
    default:
        ok = json_walk_array(&reader, &number);
        break;
    }
    if (ok != test->expect_ok) {
        (void)fprintf(stderr, "FAIL json %s: ok=%d want %d (error '%s')\n",
                      test->label, ok ? 1 : 0, test->expect_ok ? 1 : 0,
                      error);
        return false;
    }
    if (!test->expect_ok) {
        if (strcmp(error, test->expect_error) != 0) {
            (void)fprintf(stderr, "FAIL json %s: error '%s' want '%s'\n",
                          test->label, error, test->expect_error);
            return false;
        }
        return true;
    }
    if (reader.offset != test->expect_end) {
        (void)fprintf(stderr, "FAIL json %s: end %zu want %zu\n",
                      test->label, reader.offset, test->expect_end);
        return false;
    }
    if (test->kind == JSON_CASE_STRING &&
        strcmp(out, test->expect_string) != 0) {
        (void)fprintf(stderr, "FAIL json %s: string '%s'\n", test->label,
                      out);
        return false;
    }
    if ((test->kind == JSON_CASE_NUMBER || test->kind == JSON_CASE_OBJECT ||
         test->kind == JSON_CASE_ARRAY) &&
        number != test->expect_number) {
        (void)fprintf(stderr, "FAIL json %s: number %g want %g\n",
                      test->label, (double)number,
                      (double)test->expect_number);
        return false;
    }
    if (test->kind == JSON_CASE_BOOL && value != test->expect_bool) {
        (void)fprintf(stderr, "FAIL json %s: bool %d\n", test->label,
                      value ? 1 : 0);
        return false;
    }
    return true;
}

static bool json_write_fixture(const char *directory, const char *name,
                               const char *text, size_t length, char *path,
                               size_t path_size)
{
    FILE *handle;
    int written = snprintf(path, path_size, "%s/%s", directory, name);
    if (written < 0 || (size_t)written >= path_size) return false;
    handle = fopen(path, "wb");
    if (!handle) return false;
    if (fwrite(text, 1u, length, handle) != length) {
        (void)fclose(handle);
        return false;
    }
    return fclose(handle) == 0;
}

static bool json_open_cases(const char *directory)
{
    desk_json_reader reader;
    char error[DESK_ERROR_CAPACITY];
    char path[1024];
    char small[8];
    char buffer[64];
    float sum = 0.0f;

    if (desk_json_open(&reader, NULL, "fallback.json", buffer,
                       sizeof buffer, error, sizeof error) ||
        strcmp(error, "fallback.json:0: no path given") != 0) {
        (void)fprintf(stderr, "FAIL json open null-path: '%s'\n", error);
        return false;
    }
    if (snprintf(path, sizeof path, "%s/absent.json", directory) < 0 ||
        desk_json_open(&reader, path, "absent.json", buffer, sizeof buffer,
                       error, sizeof error) ||
        strcmp(error, "absent.json:0: cannot open file") != 0) {
        (void)fprintf(stderr, "FAIL json open absent: '%s'\n", error);
        return false;
    }
    if (!json_write_fixture(directory, "over.json", "12345678", 8u, path,
                            sizeof path))
        return false;
    if (desk_json_open(&reader, path, "over.json", small, sizeof small,
                       error, sizeof error) ||
        strcmp(error, "over.json:0: file larger than 7 bytes") != 0) {
        (void)fprintf(stderr, "FAIL json open oversize: '%s'\n", error);
        return false;
    }
    if (!json_write_fixture(directory, "ok.json", "{\"a\":4}", 7u, path,
                            sizeof path))
        return false;
    if (!desk_json_open(&reader, path, "ok.json", buffer, sizeof buffer,
                        error, sizeof error) ||
        reader.length != 7u || !json_walk_object(&reader, &sum) ||
        sum != 4.0f) {
        (void)fprintf(stderr, "FAIL json open round-trip: '%s'\n", error);
        return false;
    }
    return true;
}

static int json_test(void)
{
    char directory[1024];
    size_t index;
    size_t count = sizeof JSON_CASES / sizeof JSON_CASES[0];
    bool ok = true;

    for (index = 0u; index < count; ++index)
        if (!json_case_run(&JSON_CASES[index])) ok = false;
    if (!make_scratch_directory(directory, sizeof directory)) {
        (void)fprintf(stderr, "FAIL json scratch directory\n");
        return EXIT_FAILURE;
    }
    if (!json_open_cases(directory)) ok = false;
    if (!remove_tree(directory)) {
        (void)fprintf(stderr, "FAIL json scratch cleanup\n");
        ok = false;
    }
    if (!ok) return EXIT_FAILURE;
    (void)printf("PASS json-reader cases=%zu files=4\n", count);
    return EXIT_SUCCESS;
}

typedef struct catalog_case {
    const char *label;
    const char *text;
    const char *expect_error; /* substring; NULL = must load */
} catalog_case;

/* Every rejection tools/validate_items.py must also produce; --json-test
 * already locks the token-level messages, so these assert the schema
 * reason rather than exact offsets. */
static const catalog_case CATALOG_CASES[] = {
    {"minimal",
     "{\"items\":1,\"definitions\":[{\"id\":\"core:test/thing\","
     "\"name\":\"Thing\",\"description\":\"A thing.\",\"family\":"
     "\"portable\",\"behavior\":\"hold\",\"sprite\":1,\"max_stack\":4}]}",
     NULL},
    {"schema-2",
     "{\"items\":2,\"definitions\":[]}",
     "unsupported schema version"},
    {"missing-definitions",
     "{\"items\":1}",
     "missing key 'definitions'"},
    {"unknown-top-key",
     "{\"items\":1,\"definitions\":[],\"bogus\":1}",
     "unknown key 'bogus'"},
    {"duplicate-id",
     "{\"items\":1,\"definitions\":["
     "{\"id\":\"core:test/thing\",\"name\":\"A\",\"description\":\"a.\","
     "\"family\":\"portable\",\"behavior\":\"hold\",\"sprite\":1,"
     "\"max_stack\":1},"
     "{\"id\":\"core:test/thing\",\"name\":\"B\",\"description\":\"b.\","
     "\"family\":\"portable\",\"behavior\":\"hold\",\"sprite\":2,"
     "\"max_stack\":1}]}",
     "duplicate item id"},
    {"uppercase-id",
     "{\"items\":1,\"definitions\":[{\"id\":\"core:Test\",\"name\":\"T\","
     "\"description\":\"t.\",\"family\":\"portable\",\"behavior\":\"hold\","
     "\"sprite\":1,\"max_stack\":1}]}",
     "invalid item id"},
    {"unqualified-id",
     "{\"items\":1,\"definitions\":[{\"id\":\"record\",\"name\":\"T\","
     "\"description\":\"t.\",\"family\":\"portable\",\"behavior\":\"hold\","
     "\"sprite\":1,\"max_stack\":1}]}",
     "invalid item id"},
    {"dotdot-id",
     "{\"items\":1,\"definitions\":[{\"id\":\"core:a/../b\",\"name\":\"T\","
     "\"description\":\"t.\",\"family\":\"portable\",\"behavior\":\"hold\","
     "\"sprite\":1,\"max_stack\":1}]}",
     "invalid item id"},
    {"reserved-id",
     "{\"items\":1,\"definitions\":[{\"id\":\"core:missing-item\","
     "\"name\":\"T\",\"description\":\"t.\",\"family\":\"portable\","
     "\"behavior\":\"hold\",\"sprite\":1,\"max_stack\":1}]}",
     "reserved item id"},
    {"unknown-family",
     "{\"items\":1,\"definitions\":[{\"id\":\"core:test/thing\","
     "\"name\":\"T\",\"description\":\"t.\",\"family\":\"weapon\","
     "\"behavior\":\"hold\",\"sprite\":1,\"max_stack\":1}]}",
     "unknown family 'weapon'"},
    {"unknown-behavior",
     "{\"items\":1,\"definitions\":[{\"id\":\"core:test/thing\","
     "\"name\":\"T\",\"description\":\"t.\",\"family\":\"portable\","
     "\"behavior\":\"explode\",\"sprite\":1,\"max_stack\":1}]}",
     "unknown behavior 'explode'"},
    {"unknown-tag",
     "{\"items\":1,\"definitions\":[{\"id\":\"core:test/thing\","
     "\"name\":\"T\",\"description\":\"t.\",\"family\":\"portable\","
     "\"behavior\":\"hold\",\"sprite\":1,\"max_stack\":1,"
     "\"tags\":[\"shiny\"]}]}",
     "unknown tag 'shiny'"},
    {"duplicate-tag",
     "{\"items\":1,\"definitions\":[{\"id\":\"core:test/thing\","
     "\"name\":\"T\",\"description\":\"t.\",\"family\":\"portable\","
     "\"behavior\":\"hold\",\"sprite\":1,\"max_stack\":1,"
     "\"tags\":[\"decor\",\"decor\"]}]}",
     "duplicate tag 'decor'"},
    {"sprite-zero",
     "{\"items\":1,\"definitions\":[{\"id\":\"core:test/thing\","
     "\"name\":\"T\",\"description\":\"t.\",\"family\":\"portable\","
     "\"behavior\":\"hold\",\"sprite\":0,\"max_stack\":1}]}",
     "sprite out of range"},
    {"sprite-over",
     "{\"items\":1,\"definitions\":[{\"id\":\"core:test/thing\","
     "\"name\":\"T\",\"description\":\"t.\",\"family\":\"portable\","
     "\"behavior\":\"hold\",\"sprite\":8,\"max_stack\":1}]}",
     "sprite out of range"},
    {"stack-zero",
     "{\"items\":1,\"definitions\":[{\"id\":\"core:test/thing\","
     "\"name\":\"T\",\"description\":\"t.\",\"family\":\"portable\","
     "\"behavior\":\"hold\",\"sprite\":1,\"max_stack\":0}]}",
     "max_stack out of range"},
    {"stack-over",
     "{\"items\":1,\"definitions\":[{\"id\":\"core:test/thing\","
     "\"name\":\"T\",\"description\":\"t.\",\"family\":\"portable\","
     "\"behavior\":\"hold\",\"sprite\":1,\"max_stack\":100}]}",
     "max_stack out of range"},
    {"effect-on-hold",
     "{\"items\":1,\"definitions\":[{\"id\":\"core:test/thing\","
     "\"name\":\"T\",\"description\":\"t.\",\"family\":\"portable\","
     "\"behavior\":\"hold\",\"sprite\":1,\"max_stack\":1,"
     "\"effect_ticks\":60}]}",
     "effect_ticks requires behavior 'drink'"},
    {"unknown-item-key",
     "{\"items\":1,\"definitions\":[{\"id\":\"core:test/thing\","
     "\"name\":\"T\",\"description\":\"t.\",\"family\":\"portable\","
     "\"behavior\":\"hold\",\"sprite\":1,\"max_stack\":1,\"argv\":\"x\"}]}",
     "unknown key 'argv' in item"},
    {"missing-name",
     "{\"items\":1,\"definitions\":[{\"id\":\"core:test/thing\","
     "\"description\":\"t.\",\"family\":\"portable\",\"behavior\":\"hold\","
     "\"sprite\":1,\"max_stack\":1}]}",
     "item missing 'name'"},
    {"receiver-two-accepts",
     "{\"items\":1,\"definitions\":[],\"receivers\":[{\"id\":\"r\","
     "\"accept_any_tag\":[\"media\"],\"accept_family\":\"portable\","
     "\"consume\":false,\"result\":\"activate-fixture\"}]}",
     "more than one accept rule"},
    {"receiver-no-accept",
     "{\"items\":1,\"definitions\":[],\"receivers\":[{\"id\":\"r\","
     "\"consume\":false,\"result\":\"activate-fixture\"}]}",
     "receiver missing accept rule"},
    {"receiver-empty-tags",
     "{\"items\":1,\"definitions\":[],\"receivers\":[{\"id\":\"r\","
     "\"accept_any_tag\":[],\"consume\":false,"
     "\"result\":\"activate-fixture\"}]}",
     "empty accept tag list"},
    {"receiver-unknown-item",
     "{\"items\":1,\"definitions\":[],\"receivers\":[{\"id\":\"r\","
     "\"accept_item\":\"core:test/ghost\",\"consume\":false,"
     "\"result\":\"activate-fixture\"}]}",
     "unknown item 'core:test/ghost' in receiver"},
    {"receiver-output-needs-consume",
     "{\"items\":1,\"definitions\":[{\"id\":\"core:test/thing\","
     "\"name\":\"Thing\",\"description\":\"A thing.\",\"family\":"
     "\"portable\",\"behavior\":\"hold\",\"sprite\":1,"
     "\"max_stack\":1}],\"receivers\":[{\"id\":\"r\","
     "\"accept_any_tag\":[\"media\"],\"consume\":false,"
     "\"result\":\"none\",\"output\":\"core:test/thing\"}]}",
     "output requires 'consume': true"},
    {"receiver-unknown-output",
     "{\"items\":1,\"definitions\":[],\"receivers\":[{\"id\":\"r\","
     "\"accept_any_tag\":[\"media\"],\"consume\":true,"
     "\"result\":\"none\",\"output\":\"core:test/ghost\"}]}",
     "unknown output item 'core:test/ghost' in receiver"},
    {"receiver-launch-shell-result",
     "{\"items\":1,\"definitions\":[],\"receivers\":[{\"id\":\"r\","
     "\"accept_any_tag\":[\"media\"],\"consume\":false,"
     "\"result\":\"launch-shell\"}]}",
     "unknown result 'launch-shell'"},
    {"receiver-bad-result",
     "{\"items\":1,\"definitions\":[],\"receivers\":[{\"id\":\"r\","
     "\"accept_any_tag\":[\"media\"],\"consume\":false,"
     "\"result\":\"spawn-process\"}]}",
     "unknown result 'spawn-process'"},
    {"receiver-bad-id",
     "{\"items\":1,\"definitions\":[],\"receivers\":[{\"id\":\"R!\","
     "\"accept_any_tag\":[\"media\"],\"consume\":false,"
     "\"result\":\"activate-fixture\"}]}",
     "invalid receiver id"},
    {"receiver-duplicate",
     "{\"items\":1,\"definitions\":[],\"receivers\":["
     "{\"id\":\"r\",\"accept_any_tag\":[\"media\"],\"consume\":false,"
     "\"result\":\"activate-fixture\"},"
     "{\"id\":\"r\",\"accept_any_tag\":[\"tool\"],\"consume\":true,"
     "\"result\":\"activate-fixture\"}]}",
     "duplicate receiver id"},
    {"taste-unknown-cast",
     "{\"items\":1,\"definitions\":[],\"tastes\":[{\"cast\":\"myth\","
     "\"actor\":1}]}",
     "unknown taste cast 'myth'"},
    {"taste-actor-zero",
     "{\"items\":1,\"definitions\":[],\"tastes\":[{\"cast\":"
     "\"legend\",\"actor\":0}]}",
     "taste actor out of range"},
    {"taste-duplicate-pair",
     "{\"items\":1,\"definitions\":[],\"tastes\":[{\"cast\":"
     "\"legend\",\"actor\":1},{\"cast\":\"legend\",\"actor\":1}]}",
     "duplicate taste pair"},
    {"taste-unknown-entry",
     "{\"items\":1,\"definitions\":[],\"tastes\":[{\"cast\":"
     "\"legend\",\"actor\":1,\"love\":[\"sparkly\"]}]}",
     "unknown taste entry 'sparkly'"},
    {"taste-unknown-item",
     "{\"items\":1,\"tastes\":[{\"cast\":\"legend\",\"actor\":1,"
     "\"love\":[\"core:test/ghost\"]}],\"definitions\":[]}",
     "unknown taste entry 'core:test/ghost'"},
    {"taste-list-over-cap",
     "{\"items\":1,\"definitions\":[],\"tastes\":[{\"cast\":"
     "\"legend\",\"actor\":1,\"love\":[\"drink\",\"food\",\"media\","
     "\"tool\",\"wearable\",\"placeable\",\"decor\",\"key\","
     "\"giftable\"]}]}",
     "more than 8 taste entries"},
    {"taste-records-over-cap",
     "{\"items\":1,\"definitions\":[],\"tastes\":["
     "{\"cast\":\"legend\",\"actor\":1},{\"cast\":\"legend\",\"actor\":2},"
     "{\"cast\":\"legend\",\"actor\":3},{\"cast\":\"chumrunner\",\"actor\":1},"
     "{\"cast\":\"chumrunner\",\"actor\":2},{\"cast\":\"chumrunner\",\"actor\":3},"
     "{\"cast\":\"fantasy\",\"actor\":1},{\"cast\":\"fantasy\",\"actor\":2},"
     "{\"cast\":\"fantasy\",\"actor\":3},{\"cast\":\"pleb-bound\",\"actor\":1},"
     "{\"cast\":\"pleb-bound\",\"actor\":2},{\"cast\":\"pleb-bound\",\"actor\":3},"
     "{\"cast\":\"legend\",\"actor\":1}]}",
     "more than 12 tastes"},
};

static bool items_catalog_cases(const char *directory)
{
    size_t index;
    size_t count = sizeof CATALOG_CASES / sizeof CATALOG_CASES[0];
    bool ok = true;

    for (index = 0u; index < count; ++index) {
        const catalog_case *test = &CATALOG_CASES[index];
        desk_item_catalog catalog;
        char path[1024];
        char error[DESK_ERROR_CAPACITY];
        bool loaded;

        if (!json_write_fixture(directory, "case.json", test->text,
                                strlen(test->text), path, sizeof path))
            return false;
        loaded = desk_items_load(&catalog, path, error, sizeof error);
        if (test->expect_error == NULL) {
            if (!loaded) {
                (void)fprintf(stderr, "FAIL items %s: %s\n", test->label,
                              error);
                ok = false;
            }
        } else if (loaded || !strstr(error, test->expect_error)) {
            (void)fprintf(stderr, "FAIL items %s: got '%s' want '%s'\n",
                          test->label, loaded ? "(loaded)" : error,
                          test->expect_error);
            ok = false;
        }
    }
    return ok;
}

static bool items_stack_cases(const desk_item_catalog *catalog)
{
    desk_inventory inventory;
    desk_inventory before;
    desk_item_plan plan;
    desk_item item;
    desk_item taken;
    int coffee = desk_items_find(catalog, "core:drink/coffee");
    int record = desk_items_find(catalog, "core:media/record");
    int plant = desk_items_find(catalog, "core:decor/houseplant");

    if (coffee <= 0 || record <= 0 || plant <= 0) {
        (void)fprintf(stderr, "FAIL items lookup\n");
        return false;
    }
    desk_inventory_init(&inventory);

    /* Merge fills partial stacks before opening empty slots. */
    item = desk_item_make((uint16_t)coffee, 5u);
    if (!desk_inventory_plan_add(&inventory, catalog, &item, &plan) ||
        !desk_inventory_commit_add(&inventory, catalog, &item, &plan) ||
        inventory.slots[0].quantity != 5u) {
        (void)fprintf(stderr, "FAIL items first add\n");
        return false;
    }
    item = desk_item_make((uint16_t)coffee, 6u);
    if (!desk_inventory_plan_add(&inventory, catalog, &item, &plan) ||
        plan.entry_count != 2u || plan.entries[0].slot != 0u ||
        plan.entries[0].count != 3u || !plan.entries[1].creates ||
        !desk_inventory_commit_add(&inventory, catalog, &item, &plan) ||
        inventory.slots[0].quantity != 8u ||
        inventory.slots[1].quantity != 3u) {
        (void)fprintf(stderr, "FAIL items merge-then-open\n");
        return false;
    }

    /* A plan that cannot fully fit fails and mutates nothing. */
    {
        int slot;

        for (slot = 2; slot < DESK_INVENTORY_SLOTS; ++slot) {
            desk_item unique = desk_item_make((uint16_t)record, 1u);

            unique.serial = (uint32_t)slot;
            if (!desk_inventory_plan_add(&inventory, catalog, &unique,
                                         &plan) ||
                !desk_inventory_commit_add(&inventory, catalog, &unique,
                                           &plan)) {
                (void)fprintf(stderr, "FAIL items fill slot %d\n", slot);
                return false;
            }
        }
    }
    before = inventory;
    item = desk_item_make((uint16_t)coffee, 14u); /* 5 free, 14 asked */
    if (desk_inventory_plan_add(&inventory, catalog, &item, &plan) ||
        memcmp(&before, &inventory, sizeof before) != 0) {
        (void)fprintf(stderr, "FAIL items overfull add mutated state\n");
        return false;
    }

    /* Serialized instances never stack. */
    if (desk_item_can_stack(catalog, &inventory.slots[2],
                            &inventory.slots[3])) {
        (void)fprintf(stderr, "FAIL items serial stacking\n");
        return false;
    }

    /* Stale generation: plan, mutate the slot, commit must refuse. */
    before = inventory;
    if (!desk_inventory_plan_remove(&inventory, 0, 2u, &plan)) {
        (void)fprintf(stderr, "FAIL items plan remove\n");
        return false;
    }
    inventory.generation[0] =
        (uint16_t)(inventory.generation[0] + 1u); /* someone else wrote */
    if (desk_inventory_commit_remove(&inventory, &plan, &taken)) {
        (void)fprintf(stderr, "FAIL items stale commit accepted\n");
        return false;
    }
    inventory.generation[0] = before.generation[0];
    if (memcmp(&before.slots, &inventory.slots,
               sizeof before.slots) != 0) {
        (void)fprintf(stderr, "FAIL items stale commit mutated slots\n");
        return false;
    }

    /* Partial then full removal. */
    if (!desk_inventory_plan_remove(&inventory, 0, 2u, &plan) ||
        !desk_inventory_commit_remove(&inventory, &plan, &taken) ||
        taken.quantity != 2u || inventory.slots[0].quantity != 6u ||
        !desk_inventory_plan_remove(&inventory, 0, 6u, &plan) ||
        !desk_inventory_commit_remove(&inventory, &plan, &taken) ||
        taken.quantity != 6u ||
        !desk_item_is_empty(&inventory.slots[0])) {
        (void)fprintf(stderr, "FAIL items remove\n");
        return false;
    }

    /* Split one from a stack of many and from a stack of one. */
    if (!desk_item_plan_split_one(&inventory, 1, &plan) ||
        !desk_item_commit_split_one(&inventory, &plan, &taken) ||
        taken.quantity != 1u || inventory.slots[1].quantity != 2u) {
        (void)fprintf(stderr, "FAIL items split many\n");
        return false;
    }
    if (!desk_item_plan_split_one(&inventory, 2, &plan) ||
        !desk_item_commit_split_one(&inventory, &plan, &taken) ||
        taken.quantity != 1u || taken.serial != 2u ||
        !desk_item_is_empty(&inventory.slots[2])) {
        (void)fprintf(stderr, "FAIL items split last\n");
        return false;
    }

    /* Direct rearrangement: move onto empty, merge with a spill remainder,
     * swap incompatible items, preserve selection, and leave a same-slot
     * no-op byte-identical. Every actual two-slot write bumps both
     * generations. */
    {
        desk_inventory moved;
        desk_inventory unchanged;

        desk_inventory_init(&moved);
        moved.selected = 0;
        moved.slots[0] = desk_item_make((uint16_t)coffee, 3u);
        moved.generation[0] = 4u;
        moved.generation[1] = 9u;
        if (!desk_inventory_move(&moved, catalog, 0, 1) ||
            !desk_item_is_empty(&moved.slots[0]) ||
            moved.slots[1].definition != (uint16_t)coffee ||
            moved.slots[1].quantity != 3u || moved.generation[0] != 5u ||
            moved.generation[1] != 10u || moved.selected != 0) {
            (void)fprintf(stderr, "FAIL items move to empty\n");
            return false;
        }

        desk_inventory_init(&moved);
        moved.slots[0] = desk_item_make((uint16_t)coffee, 5u);
        moved.slots[1] = desk_item_make((uint16_t)coffee, 6u);
        moved.generation[0] = 12u;
        moved.generation[1] = 20u;
        if (!desk_inventory_move(&moved, catalog, 0, 1) ||
            moved.slots[0].quantity != 3u ||
            moved.slots[1].quantity != 8u ||
            moved.generation[0] != 13u ||
            moved.generation[1] != 21u) {
            (void)fprintf(stderr, "FAIL items move merge spill\n");
            return false;
        }

        desk_inventory_init(&moved);
        moved.slots[0] = desk_item_make((uint16_t)coffee, 2u);
        moved.slots[1] = desk_item_make((uint16_t)plant, 1u);
        moved.generation[0] = 2u;
        moved.generation[1] = 6u;
        if (!desk_inventory_move(&moved, catalog, 0, 1) ||
            moved.slots[0].definition != (uint16_t)plant ||
            moved.slots[0].quantity != 1u ||
            moved.slots[1].definition != (uint16_t)coffee ||
            moved.slots[1].quantity != 2u ||
            moved.generation[0] != 3u ||
            moved.generation[1] != 7u) {
            (void)fprintf(stderr, "FAIL items move incompatible swap\n");
            return false;
        }

        unchanged = moved;
        if (!desk_inventory_move(&moved, catalog, 1, 1) ||
            memcmp(&moved, &unchanged, sizeof moved) != 0) {
            (void)fprintf(stderr, "FAIL items move same-slot no-op\n");
            return false;
        }
    }

    /* Receiver rule matrix on the shipped catalog. */
    {
        int rule_index = desk_items_find_receiver(catalog, "stereo-record");
        desk_item media = desk_item_make((uint16_t)record, 1u);
        desk_item drink = desk_item_make((uint16_t)coffee, 1u);
        desk_item missing = desk_item_make((uint16_t)DESK_ITEM_DEF_MISSING,
                                           1u);

        if (rule_index < 0 ||
            !desk_receiver_accepts(catalog,
                                   &catalog->receivers[rule_index],
                                   &media) ||
            desk_receiver_accepts(catalog,
                                  &catalog->receivers[rule_index],
                                  &drink) ||
            desk_receiver_accepts(catalog,
                                  &catalog->receivers[rule_index],
                                  &missing)) {
            (void)fprintf(stderr, "FAIL items receiver matrix\n");
            return false;
        }
    }
    return true;
}

static bool items_taste_cases(const desk_item_catalog *catalog)
{
    int postcard = desk_items_find(catalog, "core:gift/postcard");
    int coffee = desk_items_find(catalog, "core:drink/coffee");
    int record = desk_items_find(catalog, "core:media/record");
    int toolbox = desk_items_find(catalog, "core:tool/toolbox");
    int pin = desk_items_find(catalog, "core:gear/lantern-pin");
    desk_item item;

    if (postcard <= 0 || coffee <= 0 || record <= 0 || toolbox <= 0 ||
        pin <= 0) {
        (void)fprintf(stderr, "FAIL items taste lookup\n");
        return false;
    }
    item = desk_item_make((uint16_t)record, 1u);
    /* Hare Courier's exact dislike outranks the record's loved giftable
     * tag; Beacon Keeper's loved media tag outranks disliked giftable. */
    if (desk_item_taste(catalog, DESK_CAST_LEGEND, DESK_ACTOR_ALLY_3,
                        &item) != DESK_TASTE_DISLIKE ||
        desk_item_taste(catalog, DESK_CAST_LEGEND, DESK_ACTOR_ALLY_2,
                        &item) != DESK_TASTE_LOVE) {
        (void)fprintf(stderr, "FAIL items taste id/tag precedence\n");
        return false;
    }
    item = desk_item_make((uint16_t)pin, 1u);
    if (desk_item_taste(catalog, DESK_CAST_LEGEND, DESK_ACTOR_ALLY_2,
                        &item) != DESK_TASTE_LIKE) {
        (void)fprintf(stderr, "FAIL items taste like precedence\n");
        return false;
    }
    item = desk_item_make((uint16_t)postcard, 1u);
    if (desk_item_taste(catalog, DESK_CAST_LEGEND, DESK_ACTOR_ALLY_1,
                        &item) != DESK_TASTE_LOVE) {
        (void)fprintf(stderr, "FAIL items taste postcard love\n");
        return false;
    }
    item = desk_item_make((uint16_t)coffee, 1u);
    if (desk_item_taste(catalog, DESK_CAST_LEGEND, DESK_ACTOR_ALLY_1,
                        &item) != DESK_TASTE_DISLIKE) {
        (void)fprintf(stderr, "FAIL items taste dislike\n");
        return false;
    }
    item = desk_item_make((uint16_t)toolbox, 1u);
    if (desk_item_taste(catalog, DESK_CAST_LEGEND, DESK_ACTOR_ALLY_3,
                        &item) != DESK_TASTE_NEUTRAL) {
        (void)fprintf(stderr, "FAIL items taste neutral default\n");
        return false;
    }
    item = desk_item_make((uint16_t)DESK_ITEM_DEF_MISSING, 1u);
    if (desk_item_taste(catalog, DESK_CAST_LEGEND, DESK_ACTOR_ALLY_1,
                        &item) != DESK_TASTE_NEUTRAL ||
        desk_item_taste(catalog, -1, DESK_ACTOR_ALLY_1, &item) !=
            DESK_TASTE_NEUTRAL) {
        (void)fprintf(stderr, "FAIL items taste missing/unknown neutral\n");
        return false;
    }
    return true;
}

static bool world_states_equal(const desk_world_state *a,
                               const desk_world_state *b)
{
    /* Slot generations are session-local, so compare content only. */
    return memcmp(a->inventory.slots, b->inventory.slots,
                  sizeof a->inventory.slots) == 0 &&
           a->inventory.selected == b->inventory.selected &&
           memcmp(a->equipment, b->equipment, sizeof a->equipment) == 0 &&
           a->next_serial == b->next_serial &&
           a->orphan_count == b->orphan_count &&
           memcmp(a->orphans, b->orphans, sizeof a->orphans) == 0 &&
           a->claimed_count == b->claimed_count &&
           memcmp(a->claimed, b->claimed, sizeof a->claimed) == 0 &&
           a->item_count == b->item_count &&
           memcmp(a->items, b->items, sizeof a->items) == 0 &&
           a->receiver_count == b->receiver_count &&
           memcmp(a->receivers, b->receivers, sizeof a->receivers) == 0 &&
           a->effect_count == b->effect_count &&
           memcmp(a->effects, b->effects, sizeof a->effects) == 0 &&
           a->social_count == b->social_count &&
           memcmp(a->social, b->social, sizeof a->social) == 0;
}

static bool items_state_cases(const char *directory,
                              const desk_item_catalog *catalog)
{
    desk_world_state saved;
    desk_world_state loaded;
    desk_item_plan plan;
    desk_item item;
    bool corrupt = false;
    int coffee = desk_items_find(catalog, "core:drink/coffee");
    int record = desk_items_find(catalog, "core:media/record");
    int plant = desk_items_find(catalog, "core:decor/houseplant");

    /* Absent record: empty state, not corrupt. */
    if (!desk_world_state_load(&loaded, catalog, &corrupt) || corrupt ||
        loaded.item_count != 0 || loaded.claimed_count != 0) {
        (void)fprintf(stderr, "FAIL world-state absent load\n");
        return false;
    }

    desk_world_state_init(&saved);
    item = desk_item_make((uint16_t)coffee, 7u);
    if (!desk_inventory_plan_add(&saved.inventory, catalog, &item, &plan) ||
        !desk_inventory_commit_add(&saved.inventory, catalog, &item,
                                   &plan)) {
        (void)fprintf(stderr, "FAIL world-state seed inventory\n");
        return false;
    }
    saved.inventory.selected = 3;
    saved.equipment[DESK_EQUIP_HANDS] =
        desk_item_make((uint16_t)record, 1u);
    saved.equipment[DESK_EQUIP_HANDS].serial =
        desk_world_state_take_serial(&saved);
    if (!desk_world_state_claim(&saved, "starter-record") ||
        !desk_world_state_claim(&saved, "kitchen-coffee") ||
        desk_world_state_claim(&saved, "starter-record")) {
        (void)fprintf(stderr, "FAIL world-state claims\n");
        return false;
    }
    saved.items[0].item = desk_item_make((uint16_t)plant, 2u);
    saved.items[0].x = 240.0f;
    saved.items[0].y = 200.5f;
    saved.items[0].placed = true;
    saved.items[0].room = -1;
    (void)snprintf(saved.items[0].room_id, sizeof saved.items[0].room_id,
                   "%s", "living");
    saved.item_count = 1;
    (void)snprintf(saved.receivers[0].room_id,
                   sizeof saved.receivers[0].room_id, "%s", "living");
    (void)snprintf(saved.receivers[0].object_id,
                   sizeof saved.receivers[0].object_id, "%s", "stereo");
    saved.receivers[0].phase = (uint8_t)DESK_RECEIVER_PROCESSING;
    saved.receivers[0].remaining_ticks = 120;
    saved.receivers[0].item = desk_item_make((uint16_t)record, 1u);
    saved.receivers[0].item.serial = desk_world_state_take_serial(&saved);
    saved.receiver_count = 1;
    saved.effects[0].definition = (uint16_t)coffee;
    saved.effects[0].remaining_ticks = 600;
    saved.effect_count = 1;
    saved.social[0].cast = 1u;
    saved.social[0].actor = 2u;
    saved.social[0].points = 250;
    saved.social[0].gifts = 3u;
    saved.social[0].flags = 1u;
    saved.social_count = 1;

    if (!desk_world_state_save(&saved, catalog) ||
        !desk_world_state_load(&loaded, catalog, &corrupt) || corrupt ||
        !world_states_equal(&saved, &loaded)) {
        (void)fprintf(stderr, "FAIL world-state round-trip\n");
        return false;
    }

    /* Duplicate serials must never encode. */
    {
        desk_world_state bad = saved;

        bad.receivers[0].item.serial =
            bad.equipment[DESK_EQUIP_HANDS].serial;
        if (desk_world_state_save(&bad, catalog)) {
            (void)fprintf(stderr, "FAIL world-state duplicate serial\n");
            return false;
        }
    }

    /* Corrupt record: flagged, empty state, profile stays valid. */
    {
        char record_path[1024];
        FILE *handle;
        long size;
        int byte;
        int written = snprintf(record_path, sizeof record_path,
                               "%s/world.state", directory);
        desk_profile profile;

        (void)memset(&profile, 0, sizeof profile);
        profile.schema = DESK_PROFILE_SCHEMA;
        profile.cast = DESK_CAST_LEGEND;
        profile.actor = DESK_ACTOR_HERO;
        (void)snprintf(profile.name, sizeof profile.name, "%s", "ROOK");
        (void)snprintf(profile.last_room, sizeof profile.last_room, "%s",
                       "bedroom");
        profile.first_run_done = true;
        if (written < 0 || (size_t)written >= sizeof record_path ||
            !desk_profile_save(&profile) ||
            (handle = fopen(record_path, "r+b")) == NULL) {
            (void)fprintf(stderr, "FAIL world-state corrupt setup\n");
            return false;
        }
        if (fseek(handle, 0L, SEEK_END) != 0 ||
            (size = ftell(handle)) <= 0L ||
            fseek(handle, size / 2L, SEEK_SET) != 0 ||
            (byte = fgetc(handle)) == EOF ||
            fseek(handle, size / 2L, SEEK_SET) != 0 ||
            fputc(byte ^ 0xff, handle) == EOF || fclose(handle) != 0) {
            (void)fprintf(stderr, "FAIL world-state corrupt write\n");
            return false;
        }
        if (!desk_world_state_load(&loaded, catalog, &corrupt) ||
            !corrupt || loaded.item_count != 0 ||
            !desk_profile_load(&profile) ||
            strcmp(profile.name, "ROOK") != 0) {
            (void)fprintf(stderr, "FAIL world-state corruption isolation\n");
            return false;
        }
    }

    /* Missing definition: recover as orphan, then restore. */
    {
        static const char *const REDUCED =
            "{\"items\":1,\"definitions\":[{\"id\":\"core:media/record\","
            "\"name\":\"House Record\",\"description\":\"r.\","
            "\"family\":\"portable\",\"behavior\":\"hold\",\"sprite\":1,"
            "\"max_stack\":1}]}";
        desk_item_catalog reduced;
        desk_world_state recovered;
        char path[1024];
        char error[DESK_ERROR_CAPACITY];

        if (!json_write_fixture(directory, "reduced.json", REDUCED,
                                strlen(REDUCED), path, sizeof path) ||
            !desk_items_load(&reduced, path, error, sizeof error)) {
            (void)fprintf(stderr, "FAIL world-state reduced catalog\n");
            return false;
        }
        if (!desk_world_state_save(&saved, catalog) ||
            !desk_world_state_load(&loaded, &reduced, &corrupt) ||
            corrupt) {
            (void)fprintf(stderr, "FAIL world-state reduced load\n");
            return false;
        }
        if (loaded.inventory.slots[0].definition !=
                (uint16_t)DESK_ITEM_DEF_MISSING ||
            loaded.orphan_count < 1 ||
            strcmp(loaded.orphans[loaded.inventory.slots[0].variant],
                   "core:drink/coffee") != 0 ||
            loaded.inventory.slots[0].quantity != 7u) {
            (void)fprintf(stderr, "FAIL world-state orphan\n");
            return false;
        }
        /* Save under the reduced catalog, reload under the full one: the
         * original id must resolve again with nothing lost. */
        if (!desk_world_state_save(&loaded, &reduced) ||
            !desk_world_state_load(&recovered, catalog, &corrupt) ||
            corrupt ||
            recovered.inventory.slots[0].definition != (uint16_t)coffee ||
            recovered.inventory.slots[0].variant != 0u ||
            recovered.inventory.slots[0].quantity != 7u ||
            /* The effect referenced a definition that was absent when the
             * reduced catalog saved, so it was dropped for good. */
            recovered.effect_count != 0) {
            (void)fprintf(stderr, "FAIL world-state recovery\n");
            return false;
        }
    }
    return true;
}

static int items_test(void)
{
    char directory[1024];
    desk_item_catalog catalog;
    char path[1024];
    char error[DESK_ERROR_CAPACITY];
    bool ok = true;
    int written;

    if (!make_temp_config(directory, sizeof directory)) {
        (void)fprintf(stderr, "FAIL items temp config\n");
        return EXIT_FAILURE;
    }
    written = snprintf(path, sizeof path, "%s/assets/world/items.json",
                       asset_root());
    if (written < 0 || (size_t)written >= sizeof path ||
        !desk_items_load(&catalog, path, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL items catalog %s: %s\n", path, error);
        ok = false;
    }
    if (ok &&
        (catalog.definition_count != 10 || catalog.receiver_count != 3 ||
         catalog.taste_count != 12 ||
         desk_items_find(&catalog, DESK_ITEM_MISSING_ID) !=
             (int)DESK_ITEM_DEF_MISSING ||
         desk_items_find(&catalog, "core:media/record") <= 0 ||
         desk_items_find(&catalog, "core:tool/laptop") <= 0)) {
        (void)fprintf(stderr, "FAIL items shipped catalog shape\n");
        ok = false;
    }
    if (ok && !items_catalog_cases(directory)) ok = false;
    if (ok && !items_taste_cases(&catalog)) ok = false;
    if (ok && !items_stack_cases(&catalog)) ok = false;
    if (ok && !items_state_cases(directory, &catalog)) ok = false;
    if (!remove_tree(directory)) {
        (void)fprintf(stderr, "FAIL items temp cleanup\n");
        ok = false;
    }
    if (!ok) return EXIT_FAILURE;
    (void)printf(
        "PASS items definitions=%d receivers=%d tastes=%d "
        "negative-cases=%zu move=empty+merge-spill+swap+same "
        "state=round-trip+orphan+corruption\n",
        catalog.definition_count, catalog.receiver_count,
        catalog.taste_count,
        sizeof CATALOG_CASES / sizeof CATALOG_CASES[0] - 1u);
    return EXIT_SUCCESS;
}

typedef struct render_fixture {
    desk_world world;
    desk_item_catalog catalog;
    desk_graphics graphics;
    ki_td_soft_renderer renderer;
    int loaded_style;
} render_fixture;

static bool fixture_open(render_fixture *fixture, const char *directory)
{
    char error[DESK_ERROR_CAPACITY];
    char resolved[PATH_MAX];
    (void)memset(fixture, 0, sizeof *fixture);
    fixture->loaded_style = -1;
    if (!directory || !ensure_directory(directory)) return false;
    /* Keep the fixture deterministic and self-contained: the profile store
     * points into DIR (as an absolute path, which the store requires) and
     * any record a previous run left there is removed, so desk_init always
     * opens the first-run wizard and nothing is written outside DIR. */
    if (!realpath(directory, resolved) ||
        setenv("KILIX_LAND_DESKTOP_CONFIG_HOME", resolved, 1) != 0)
        return false;
    if (!desk_profile_reset() || !desk_world_state_reset()) return false;
    if (!load_world(&fixture->world, &fixture->catalog, error,
                    sizeof error)) {
        (void)fprintf(stderr, "FAIL render world: %s\n", error);
        return false;
    }
    if (!desk_graphics_init(&fixture->graphics, asset_root())) return false;
    if (!ki_td_soft_renderer_init(&fixture->renderer, DESK_LOGICAL_WIDTH,
                                  DESK_LOGICAL_HEIGHT)) {
        desk_graphics_shutdown(&fixture->graphics);
        return false;
    }
    return true;
}

static void fixture_close(render_fixture *fixture)
{
    ki_td_soft_renderer_destroy(&fixture->renderer);
    desk_graphics_shutdown(&fixture->graphics);
}

static bool fixture_snapshot(render_fixture *fixture, desk_state *state,
                             const char *directory, const char *name)
{
    char path[1024];
    int written = snprintf(path, sizeof path, "%s/%s.ppm", directory, name);
    if (written < 0 || (size_t)written >= sizeof path) return false;
    if (!sync_graphics(&fixture->graphics, &fixture->world, state,
                       &fixture->loaded_style))
        return false;
    return desk_render(&fixture->renderer, state, &fixture->world,
                       &fixture->graphics) &&
           sr_write_ppm(ki_td_soft_canvas(&fixture->renderer), path);
}

static bool complete_wizard(desk_state *state, const desk_world *world)
{
    return desk_interact(state, world) &&               /* cast -> actor */
           desk_interact(state, world) &&               /* actor -> name */
           desk_text_input(state, (uint32_t)'R') &&
           desk_text_input(state, (uint32_t)'O') &&
           desk_text_input(state, (uint32_t)'O') &&
           desk_text_input(state, (uint32_t)'K') &&
           desk_interact(state, world) &&               /* name -> outfit */
           desk_interact(state, world) &&               /* outfit -> confirm */
           desk_interact(state, world) &&               /* confirm -> room */
           state->mode == DESK_MODE_ROOM;
}

/* --doors-test: prove every door is a WALK, not a teleport.
 *
 * The selftest crosses one door by placing the player inside its rect,
 * which proves the trigger fires but says nothing about whether the
 * painted world lets anybody stand there. These routes start on the spawn
 * a player really arrives at and hold real directions, tick by tick, so a
 * room the obstacle map has sealed off fails here instead of shipping.
 * The probes then walk into scenery and prove the feet box stops at the
 * floor line rather than sliding into the wall behind it. */
typedef struct walk_leg {
    int dx;
    int dy;
    int ticks;   /* upper bound; the route stops the moment a door fires */
} walk_leg;

typedef struct door_route {
    const char *room;    /* room the walk happens in */
    const char *from;    /* room whose door spawned the player here */
    const char *to;      /* destination the route must reach */
    walk_leg legs[3];
} door_route;

static const door_route DOOR_ROUTES[] = {
    /* Out of the bedroom: straight at the doorway beside the wardrobe. */
    {"bedroom", "living", "living", {{1, 0, 40}, {0, 0, 0}, {0, 0, 0}}},
    /* Living room, entering from the yard. The coffee table sits in front
     * of the kitchen archway, so the way through is the floor lane along
     * the back wall: step clear of the table, walk up to the wall, then
     * along it into the arch. */
    {"living", "yard", "kitchen",
     {{1, 0, 16}, {0, -1, 40}, {-1, 0, 40}}},
    {"living", "yard", "yard", {{0, 1, 40}, {0, 0, 0}, {0, 0, 0}}},
    {"living", "yard", "bedroom", {{-1, 0, 140}, {0, 0, 0}, {0, 0, 0}}},
    /* One step down clears housemate 2, who stands in the lane. */
    {"living", "yard", "study", {{0, 1, 1}, {1, 0, 140}, {0, 0, 0}}},
    /* The study's doorway is reached along the bottom of the room. */
    {"study", "living", "living", {{0, 1, 16}, {-1, 0, 40}, {0, 0, 0}}},
    {"kitchen", "living", "living", {{0, 1, 40}, {0, 0, 0}, {0, 0, 0}}},
    {"yard", "living", "living", {{0, -1, 40}, {0, 0, 0}, {0, 0, 0}}}
};

typedef struct solid_probe {
    const char *room;
    const char *what;
    float x;
    float y;
    int dx;
    int dy;
    float floor_y;  /* walking up must never lift the feet above this */
} solid_probe;

static const solid_probe SOLID_PROBES[] = {
    {"living", "stereo cabinet", 340.0f, 250.0f, 0, -1, 194.0f},
    {"bedroom", "bed", 200.0f, 250.0f, 0, -1, 236.0f},
    {"study", "desk wall", 200.0f, 250.0f, 0, -1, 200.0f},
    {"kitchen", "counter", 60.0f, 250.0f, 0, -1, 218.0f},
    {"yard", "shed", 380.0f, 250.0f, 0, -1, 200.0f}
};

static const desk_door *find_door(const desk_room *room, const char *to)
{
    int index;
    for (index = 0; index < room->door_count; ++index)
        if (strcmp(room->doors[index].to_id, to) == 0)
            return &room->doors[index];
    return NULL;
}

static int doors_test_body(void)
{
    desk_world world;
    desk_item_catalog catalog;
    desk_state state;
    char path[1024];
    char error[DESK_ERROR_CAPACITY];
    size_t index;
    int crossings = 0;

    if (!world_path(path, sizeof path) ||
        !desk_world_load(&world, path, error, sizeof error) ||
        !desk_world_validate(&world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL doors world: %s\n", error);
        return EXIT_FAILURE;
    }
    if (!items_path(path, sizeof path) ||
        !desk_items_load(&catalog, path, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL doors items: %s\n", error);
        return EXIT_FAILURE;
    }

    for (index = 0; index < sizeof DOOR_ROUTES / sizeof DOOR_ROUTES[0];
         ++index) {
        const door_route *route = &DOOR_ROUTES[index];
        int here = desk_world_room_index(&world, route->room);
        int from = desk_world_room_index(&world, route->from);
        int want = desk_world_room_index(&world, route->to);
        const desk_door *arrival;
        const desk_door *exit_door;
        size_t leg;
        int idle;

        if (here < 0 || from < 0 || want < 0) {
            (void)fprintf(stderr, "FAIL doors route %zu: unknown room\n",
                          index);
            return EXIT_FAILURE;
        }
        arrival = find_door(&world.rooms[from], route->room);
        exit_door = find_door(&world.rooms[here], route->to);
        if (!arrival || !exit_door) {
            (void)fprintf(stderr,
                          "FAIL doors route %zu: no door %s->%s or %s->%s\n",
                          index, route->from, route->room, route->room,
                          route->to);
            return EXIT_FAILURE;
        }
        desk_init(&state, &world, &catalog);
        if (!complete_wizard(&state, &world)) {
            (void)fprintf(stderr, "FAIL doors wizard\n");
            return EXIT_FAILURE;
        }
        state.room = here;
        state.player_x = arrival->spawn_x;
        state.player_y = arrival->spawn_y;
        state.door_cooldown_ticks = 0;

        /* Arriving is stable: standing still on the spawn past the whole
         * cooldown must never bounce the player back through a door. */
        for (idle = 0; idle < DESK_DOOR_COOLDOWN_TICKS * 2; ++idle)
            desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        if (state.room != here) {
            (void)fprintf(stderr,
                          "FAIL doors %s: the spawn from %s stands inside a "
                          "door and bounces to %s\n", route->room,
                          route->from, world.rooms[state.room].id);
            return EXIT_FAILURE;
        }

        for (leg = 0; leg < sizeof route->legs / sizeof route->legs[0] &&
                      state.room == here; ++leg) {
            int tick;
            for (tick = 0;
                 tick < route->legs[leg].ticks && state.room == here; ++tick)
                desk_update(&state, &world, route->legs[leg].dx,
                            route->legs[leg].dy, DESK_TICK_SECONDS);
        }
        if (state.room != want) {
            (void)fprintf(stderr,
                          "FAIL doors %s->%s: walking from the %s spawn "
                          "(%g,%g) ended in '%s' at (%g,%g)\n", route->room,
                          route->to, route->from, (double)arrival->spawn_x,
                          (double)arrival->spawn_y,
                          world.rooms[state.room].id,
                          (double)state.player_x, (double)state.player_y);
            return EXIT_FAILURE;
        }
        if (state.player_x != exit_door->spawn_x ||
            state.player_y != exit_door->spawn_y ||
            strcmp(state.profile.last_room, route->to) != 0 ||
            state.door_cooldown_ticks <= 0 || !state.profile_dirty ||
            !desk_validate(&state, &world, error, sizeof error)) {
            (void)fprintf(stderr,
                          "FAIL doors %s->%s arrival (%g,%g) want (%g,%g) "
                          "last_room=%s: %s\n", route->room, route->to,
                          (double)state.player_x, (double)state.player_y,
                          (double)exit_door->spawn_x,
                          (double)exit_door->spawn_y,
                          state.profile.last_room, error);
            return EXIT_FAILURE;
        }
        ++crossings;
    }

    for (index = 0; index < sizeof SOLID_PROBES / sizeof SOLID_PROBES[0];
         ++index) {
        const solid_probe *probe = &SOLID_PROBES[index];
        int room_index = desk_world_room_index(&world, probe->room);
        float start_y;
        int tick;

        if (room_index < 0) {
            (void)fprintf(stderr, "FAIL doors probe %zu: unknown room\n",
                          index);
            return EXIT_FAILURE;
        }
        desk_init(&state, &world, &catalog);
        if (!complete_wizard(&state, &world)) {
            (void)fprintf(stderr, "FAIL doors probe wizard\n");
            return EXIT_FAILURE;
        }
        state.room = room_index;
        state.player_x = probe->x;
        state.player_y = probe->y;
        state.door_cooldown_ticks = DESK_DOOR_COOLDOWN_TICKS;
        start_y = state.player_y;
        for (tick = 0; tick < 90; ++tick) {
            desk_update(&state, &world, probe->dx, probe->dy,
                        DESK_TICK_SECONDS);
            if (state.player_y < probe->floor_y) {
                (void)fprintf(stderr,
                              "FAIL doors probe %s/%s: walked to y=%g, "
                              "inside scenery whose floor line is %g\n",
                              probe->room, probe->what,
                              (double)state.player_y,
                              (double)probe->floor_y);
                return EXIT_FAILURE;
            }
        }
        if (state.player_y >= start_y) {
            (void)fprintf(stderr,
                          "FAIL doors probe %s/%s: never moved off %g\n",
                          probe->room, probe->what, (double)start_y);
            return EXIT_FAILURE;
        }
    }

    (void)printf("PASS doors crossings=%d walked=spawn-to-door "
                 "arrivals=stable solid-probes=%zu\n", crossings,
                 sizeof SOLID_PROBES / sizeof SOLID_PROBES[0]);
    return EXIT_SUCCESS;
}

static int doors_test(void)
{
    char config_dir[1024];
    int result;
    if (!make_temp_config(config_dir, sizeof config_dir)) {
        (void)fprintf(stderr, "FAIL doors temp config\n");
        return EXIT_FAILURE;
    }
    result = doors_test_body();
    if (!remove_tree(config_dir) && result == EXIT_SUCCESS) {
        (void)fprintf(stderr, "FAIL doors temp cleanup\n");
        result = EXIT_FAILURE;
    }
    return result;
}

static int wizard_render_test(const char *directory)
{
    render_fixture fixture;
    desk_state state;
    bool success;
    if (!fixture_open(&fixture, directory)) return EXIT_FAILURE;
    desk_init(&state, &fixture.world, &fixture.catalog);
    success = state.mode == DESK_MODE_WIZARD &&
              state.wizard_cast_cursor == 0 &&
              fixture_snapshot(&fixture, &state, directory, "cast");
    if (success) {
        desk_update(&state, &fixture.world, 0, 1, DESK_TICK_SECONDS);
        success = state.wizard_cast_cursor == 1 &&
                  desk_interact(&state, &fixture.world) &&
                  state.wizard_step == DESK_WIZARD_ACTOR &&
                  fixture_snapshot(&fixture, &state, directory, "actor");
    }
    if (success)
        success = desk_interact(&state, &fixture.world) &&
                  state.wizard_step == DESK_WIZARD_NAME &&
                  desk_text_input(&state, (uint32_t)'R') &&
                  desk_text_input(&state, (uint32_t)'O') &&
                  desk_text_input(&state, (uint32_t)'O') &&
                  desk_text_input(&state, (uint32_t)'K') &&
                  fixture_snapshot(&fixture, &state, directory, "name");
    if (success) {
        success = desk_interact(&state, &fixture.world) &&
                  state.wizard_step == DESK_WIZARD_OUTFIT;
        desk_update(&state, &fixture.world, 0, 1, DESK_TICK_SECONDS);
        desk_update(&state, &fixture.world, 0, 1, DESK_TICK_SECONDS);
        success = success && state.wizard_outfit_cursor == 2 &&
                  fixture_snapshot(&fixture, &state, directory, "outfit");
    }
    if (success)
        success = desk_interact(&state, &fixture.world) &&
                  state.wizard_step == DESK_WIZARD_CONFIRM &&
                  fixture_snapshot(&fixture, &state, directory, "confirm");
    fixture_close(&fixture);
    if (!success) return EXIT_FAILURE;
    (void)printf(
        "PASS render scene=wizard steps=5 files=5 size=%dx%d directory=%s\n",
        DESK_LOGICAL_WIDTH, DESK_LOGICAL_HEIGHT, directory);
    return EXIT_SUCCESS;
}

/* Logical x centroid of the mask pixels carrying the region id, or a
 * negative value when the mask (or the id) is absent. */
static float behind_region_center_x(const uint8_t *mask, int id)
{
    uint64_t total = 0u;
    uint64_t count = 0u;
    int y;
    if (!mask || id < 1 || id > DESK_MAX_WALKBEHINDS_PER_ROOM)
        return -1.0f;
    for (y = 0; y < DESK_PLATE_HEIGHT; ++y) {
        const uint8_t *row = mask + (size_t)y * (size_t)DESK_PLATE_WIDTH;
        int x;
        for (x = 0; x < DESK_PLATE_WIDTH; ++x)
            if (row[x] == (uint8_t)id) {
                total += (uint64_t)(unsigned int)x;
                ++count;
            }
    }
    if (count == 0u) return -1.0f;
    return (float)((double)total / (double)count) *
           (float)DESK_LOGICAL_WIDTH / (float)DESK_PLATE_WIDTH;
}

static int room_render_test(const char *directory)
{
    render_fixture fixture;
    desk_state state;
    bool success;
    int behind = 0;
    int room;
    if (!fixture_open(&fixture, directory)) return EXIT_FAILURE;
    desk_init(&state, &fixture.world, &fixture.catalog);
    success = state.mode == DESK_MODE_WIZARD &&
              complete_wizard(&state, &fixture.world) &&
              state.profile.cast == DESK_CAST_LEGEND &&
              state.profile.outfit == 0u;
    for (room = 0; room < fixture.world.room_count && success; ++room) {
        const desk_room *scene = &fixture.world.rooms[room];
        state.room = room;
        state.player_x = scene->walk.x + scene->walk.w * 0.5f;
        state.player_y = scene->walk.y + scene->walk.h * 0.5f;
        state.facing = DESK_FACING_DOWN;
        state.door_cooldown_ticks = DESK_DOOR_COOLDOWN_TICKS;
        desk_update(&state, &fixture.world, 0, 0, DESK_TICK_SECONDS);
        state.toast_ticks = 0;
        success = fixture_snapshot(&fixture, &state, directory, scene->id);
        if (success && scene->walkbehind_count > 0) {
            /* Walk-behind proof: feet just above the first region's
             * baseline, horizontally centered on the region's mask
             * pixels, put the player behind the furniture, so the mask's
             * plate pixels must re-blit over the sprite. No desk_update
             * here: the behind position may lie outside the walk rect and
             * a tick would clamp it back inside. The snapshot above ran
             * sync_graphics, so the legend masks are loaded. */
            const desk_walkbehind *walkbehind = &scene->walkbehinds[0];
            float center_x = behind_region_center_x(
                desk_graphics_behind_mask(&fixture.graphics, room),
                walkbehind->id);
            char name[DESK_ID_CAPACITY + 12];
            int written = snprintf(name, sizeof name, "%s-behind",
                                   scene->id);
            if (center_x >= 0.0f) {
                state.player_x = center_x;
                state.player_y = walkbehind->baseline - 2.0f;
                state.nearest_object = -1;
                state.nearest_npc = -1;
                success = written >= 0 &&
                          (size_t)written < sizeof name &&
                          fixture_snapshot(&fixture, &state, directory,
                                           name);
                if (success) ++behind;
            }
        }
    }
    fixture_close(&fixture);
    if (!success) return EXIT_FAILURE;
    (void)printf(
        "PASS render scene=rooms cast=legend files=%d behind=%d "
        "size=%dx%d directory=%s\n",
        fixture.world.room_count + behind, behind, DESK_LOGICAL_WIDTH,
        DESK_LOGICAL_HEIGHT, directory);
    return EXIT_SUCCESS;
}

static int outfit_render_test(const char *directory)
{
    static const char *const cast_ids[DESK_CAST_COUNT] = {
        "legend", "chumrunner", "fantasy", "pleb-bound"
    };
    static const int swatches[] = { 0, 2, 4 };
    render_fixture fixture;
    bool success = true;
    int files = 0;
    int cast;
    if (!fixture_open(&fixture, directory)) return EXIT_FAILURE;
    for (cast = 0; cast < DESK_CAST_COUNT && success; ++cast) {
        size_t swatch;
        for (swatch = 0u;
             swatch < sizeof swatches / sizeof swatches[0] && success;
             ++swatch) {
            desk_state state;
            char name[64];
            int written;
            desk_init(&state, &fixture.world, &fixture.catalog);
            state.mode = DESK_MODE_WIZARD;
            state.wizard_step = DESK_WIZARD_OUTFIT;
            state.wizard_cast_cursor = cast;
            state.wizard_actor_cursor = 0;
            state.wizard_outfit_cursor = swatches[swatch];
            (void)snprintf(state.wizard_name, sizeof state.wizard_name,
                           "%s", "ROOK");
            state.wizard_name_len = 4;
            state.outfit_dirty = true;
            state.toast_ticks = 0;
            written = snprintf(name, sizeof name, "%s-outfit-%d",
                               cast_ids[cast], swatches[swatch]);
            success = written >= 0 && (size_t)written < sizeof name &&
                      fixture_snapshot(&fixture, &state, directory, name);
            if (success) ++files;
        }
    }
    fixture_close(&fixture);
    if (!success) return EXIT_FAILURE;
    (void)printf(
        "PASS render scene=outfits casts=%d swatches=3 files=%d "
        "size=%dx%d directory=%s\n",
        DESK_CAST_COUNT, files, DESK_LOGICAL_WIDTH, DESK_LOGICAL_HEIGHT,
        directory);
    return EXIT_SUCCESS;
}

static int walk_render_test(const char *directory)
{
    render_fixture fixture;
    desk_state state;
    const desk_door *door = NULL;
    float approach_y = -1.0f;
    bool success;
    int frame;
    if (!fixture_open(&fixture, directory)) return EXIT_FAILURE;
    desk_init(&state, &fixture.world, &fixture.catalog);
    success = state.mode == DESK_MODE_WIZARD &&
              complete_wizard(&state, &fixture.world) &&
              fixture.world.rooms[fixture.world.start_room].door_count > 0;
    if (success) {
        /* The painted world decides which lane reaches the door: probe
         * each 6px approach row (no rendering) until a straight walk-right
         * crosses, then re-run that lane for the rendered frames. */
        float try_y;
        door = &fixture.world.rooms[fixture.world.start_room].doors[0];
        for (try_y = door->rect.y + 3.0f;
             try_y <= door->rect.y + door->rect.h - 3.0f &&
             approach_y < 0.0f; try_y += 6.0f) {
            int tick;
            state.room = fixture.world.start_room;
            state.player_x = door->rect.x - 22.0f;
            state.player_y = try_y;
            state.facing = DESK_FACING_RIGHT;
            state.door_cooldown_ticks = 0;
            for (tick = 0;
                 tick < 40 && state.room == fixture.world.start_room;
                 ++tick)
                desk_update(&state, &fixture.world, 1, 0,
                            DESK_TICK_SECONDS);
            if (state.room != fixture.world.start_room)
                approach_y = try_y;
        }
        success = approach_y >= 0.0f;
    }
    if (success) {
        uint64_t base_tick;
        state.room = fixture.world.start_room;
        state.player_x = door->rect.x - 22.0f;
        state.player_y = approach_y;
        state.facing = DESK_FACING_RIGHT;
        state.door_cooldown_ticks = 0;
        state.toast_ticks = 0;
        base_tick = state.simulation_tick;
        for (frame = 0; frame < 8 && success; ++frame) {
            char name[32];
            uint64_t target_tick = base_tick +
                (uint64_t)(unsigned int)frame * 5u + 1u;
            int written = snprintf(name, sizeof name, "walk-%02d", frame);
            while (state.simulation_tick < target_tick)
                desk_update(&state, &fixture.world, 1, 0,
                            DESK_TICK_SECONDS);
            success = written >= 0 && (size_t)written < sizeof name &&
                      state.player_moving &&
                      state.facing == DESK_FACING_RIGHT &&
                      fixture_snapshot(&fixture, &state, directory, name);
        }
        success = success && state.room != fixture.world.start_room;
    }
    fixture_close(&fixture);
    if (!success) return EXIT_FAILURE;
    (void)printf(
        "PASS render scene=walk cast=legend frames=8 door=crossed "
        "size=%dx%d directory=%s\n",
        DESK_LOGICAL_WIDTH, DESK_LOGICAL_HEIGHT, directory);
    return EXIT_SUCCESS;
}

static int items_render_test(const char *directory)
{
    render_fixture fixture;
    desk_state state;
    bool success;
    int living;
    int record_index = -1;
    int index;

    if (!fixture_open(&fixture, directory)) return EXIT_FAILURE;
    desk_init(&state, &fixture.world, &fixture.catalog);
    living = desk_world_room_index(&fixture.world, "living");
    success = state.mode == DESK_MODE_WIZARD &&
              complete_wizard(&state, &fixture.world) && living >= 0;
    if (success) {
        state.room = living;
        state.door_cooldown_ticks = DESK_DOOR_COOLDOWN_TICKS;
        for (index = 0; index < state.items.item_count; ++index)
            if (state.items.items[index].room == living)
                record_index = index;
        success = record_index >= 0;
    }
    if (success) {
        /* Feet above the item's baseline: the item must draw in front. */
        const desk_world_item *entry = &state.items.items[record_index];

        state.player_x = entry->x;
        state.player_y = entry->y - 6.0f;
        state.facing = DESK_FACING_DOWN;
        desk_update(&state, &fixture.world, 0, 0, DESK_TICK_SECONDS);
        state.toast_ticks = 0;
        success = fixture_snapshot(&fixture, &state, directory,
                                   "item-in-front");
    }
    if (success) {
        /* Feet below the baseline: the item must draw behind. */
        const desk_world_item *entry = &state.items.items[record_index];

        state.player_y = entry->y + 8.0f;
        desk_update(&state, &fixture.world, 0, 0, DESK_TICK_SECONDS);
        state.toast_ticks = 0;
        success = fixture_snapshot(&fixture, &state, directory,
                                   "item-behind");
    }
    if (success) {
        /* Hotbar states: stack with quantity, single item, recovery icon,
         * and a non-zero selected slot. */
        int coffee = desk_items_find(&fixture.catalog,
                                     "core:drink/coffee");
        int record = desk_items_find(&fixture.catalog,
                                     "core:media/record");
        int orphan = desk_world_state_orphan_add(&state.items,
                                                 "core:test/lost-thing");

        success = coffee > 0 && record > 0 && orphan >= 0;
        if (success) {
            state.items.inventory.slots[0] =
                desk_item_make((uint16_t)coffee, 8u);
            state.items.inventory.slots[1] =
                desk_item_make((uint16_t)record, 1u);
            state.items.inventory.slots[2] =
                desk_item_make((uint16_t)DESK_ITEM_DEF_MISSING, 1u);
            state.items.inventory.slots[2].variant = (uint16_t)orphan;
            state.items.inventory.selected = 1;
            desk_update(&state, &fixture.world, 0, 0, DESK_TICK_SECONDS);
            state.toast_ticks = 0;
            success = fixture_snapshot(&fixture, &state, directory,
                                       "hotbar");
        }
    }
    if (success) {
        /* Full panel: populated slots, independent hotbar selection, and
         * an occupied source marked for rearrangement. */
        state.inventory_cursor = 0;
        success = desk_toggle_inventory(&state, &fixture.world) &&
                  state.mode == DESK_MODE_INVENTORY &&
                  desk_interact(&state, &fixture.world) &&
                  state.inventory_mark == 0 &&
                  fixture_snapshot(&fixture, &state, directory,
                                   "inventory-panel");
        if (success)
            success = desk_toggle_inventory(&state, &fixture.world) &&
                      state.mode == DESK_MODE_ROOM;
    }
    if (success) {
        /* The raised-tool frame of the swing, held item drawn at the
         * hand offset over the idle pose. */
        int toolbox = desk_items_find(&fixture.catalog,
                                      "core:tool/toolbox");
        int yard = desk_world_room_index(&fixture.world, "yard");
        int tick;

        success = toolbox > 0 && yard >= 0;
        if (success) {
            state.items.inventory.slots[3] =
                desk_item_make((uint16_t)toolbox, 1u);
            state.items.inventory.selected = 3;
            state.room = yard;
            state.player_x = 386.0f;
            state.player_y = 214.0f;
            state.facing = DESK_FACING_RIGHT;
            state.door_cooldown_ticks = DESK_DOOR_COOLDOWN_TICKS;
            desk_update(&state, &fixture.world, 0, 0, DESK_TICK_SECONDS);
            success = desk_use_item(&state, &fixture.world);
            for (tick = 0; success && tick < 9; ++tick)
                desk_update(&state, &fixture.world, 0, 0,
                            DESK_TICK_SECONDS);
            state.toast_ticks = 0;
            success = success && state.action.active &&
                      fixture_snapshot(&fixture, &state, directory,
                                       "tool-swing");
        }
    }
    fixture_close(&fixture);
    if (!success) return EXIT_FAILURE;
    (void)printf(
        "PASS render scene=items files=5 depth=front+behind "
        "hotbar=stack+missing inventory=panel-marked tool=swing "
        "size=%dx%d directory=%s\n",
        DESK_LOGICAL_WIDTH, DESK_LOGICAL_HEIGHT, directory);
    return EXIT_SUCCESS;
}

static desk_cast audio_cast(const desk_state *state)
{
    return state->mode == DESK_MODE_WIZARD ?
           (desk_cast)state->wizard_cast_cursor : state->profile.cast;
}

static int run_interactive(void)
{
    kittyts_options terminal_options;
    kilix_game_signal_scope signals = {0};
    kilix_game_clock clock;
    kilix_game_clock_options clock_options;
    ki_td_soft_renderer renderer = {0};
    desk_world world;
    static desk_item_catalog catalog;
    desk_graphics graphics;
    desk_audio audio = {0};
    desk_launcher launcher;
    desk_state state;
    desk_key_input current_input;
    desk_key_input pending_input = {0};
    char error[DESK_ERROR_CAPACITY];
    const char *audio_switch;
    bool quit_requested = false;
    bool failed = false;
    bool first_frame = true;
    uint64_t last_presented_step = UINT64_MAX;
    int loaded_style = -1;
    int width;
    int height;

    if (!load_world(&world, &catalog, error, sizeof error)) {
        (void)fprintf(stderr,
            "kilix-land-desktop: %s (set KILIX_LAND_DESKTOP_ASSETS)\n",
            error);
        return EXIT_FAILURE;
    }
    desk_init(&state, &world, &catalog);
    /* Asked once, before the terminal is live: the notice board grows a
     * "change the locks" note only while the login password is still the
     * one the image shipped with. */
    state.default_password = desk_launcher_password_is_default();
    if (!desk_graphics_init(&graphics, asset_root())) {
        (void)fprintf(stderr,
            "kilix-land-desktop: could not load graphics under %s "
            "(set KILIX_LAND_DESKTOP_ASSETS)\n", asset_root());
        return EXIT_FAILURE;
    }
    if (!sync_graphics(&graphics, &world, &state, &loaded_style)) {
        (void)fprintf(stderr,
            "kilix-land-desktop: could not prepare style graphics\n");
        desk_graphics_shutdown(&graphics);
        return EXIT_FAILURE;
    }
    audio_switch = getenv("KILIX_LAND_DESKTOP_AUDIO");
    if (!desk_audio_init(&audio, asset_root(),
                         !(audio_switch &&
                           strcmp(audio_switch, "0") == 0)))
        (void)fprintf(stderr,
            "kilix-land-desktop: native sound assets could not be loaded; "
            "continuing silently\n");
    desk_launcher_init(&launcher);
    kittyts_session_init(&terminal_session);
    kittyts_options_init(&terminal_options);
    terminal_options.framebuffer.min_width = DESK_FRAMEBUFFER_MIN_WIDTH;
    terminal_options.framebuffer.min_height = DESK_FRAMEBUFFER_MIN_HEIGHT;
    terminal_options.framebuffer.max_width = DESK_FRAMEBUFFER_MAX_WIDTH;
    terminal_options.framebuffer.max_height = DESK_FRAMEBUFFER_MAX_HEIGHT;
    if (kittyts_start(&terminal_session, STDIN_FILENO, STDOUT_FILENO,
                      &terminal_options) != 0) {
        (void)fprintf(stderr,
                      "kilix-land-desktop: terminal start failed: %s\n",
                      strerror(errno));
        desk_audio_shutdown(&audio);
        desk_graphics_shutdown(&graphics);
        return EXIT_FAILURE;
    }
    if (!kilix_game_signals_install(&signals)) {
        kittyts_stop(&terminal_session);
        desk_audio_shutdown(&audio);
        desk_graphics_shutdown(&graphics);
        (void)fprintf(stderr, "kilix-land-desktop: signal scope failed\n");
        return EXIT_FAILURE;
    }
    last_movement_key = KITTYKB_KEY_NONE;
    width = kittyts_width(&terminal_session);
    height = kittyts_height(&terminal_session);
    if (!ki_td_soft_renderer_init(&renderer, width, height)) {
        failed = true;
        goto done;
    }
    kilix_game_clock_options_init(&clock_options);
    clock_options.step_ns = KILIX_GAME_NANOSECONDS_PER_SECOND /
                            DESK_SIMULATION_HZ;
    if (!kilix_game_clock_init(&clock, &clock_options)) {
        failed = true;
        goto done;
    }

    while (!quit_requested && !kilix_game_signals_requested(&signals)) {
        kilix_game_frame frame;
        uint32_t step;
        int resized_width;
        int resized_height;
        int64_t now;

        if (!poll_input(&current_input, &quit_requested)) {
            failed = true;
            break;
        }
        if (quit_requested) break;
        merge_pending_input(&pending_input, &current_input);

        now = kilix_game_monotonic_ns();
        frame = kilix_game_clock_advance(&clock, now);
        for (step = 0u; step < frame.steps; ++step) {
            /* The wizard's name step and the laptop's open field are the
             * two places typing means text rather than a shortcut. */
            bool name_entry = desk_text_entry_active(&state);
            if (name_entry) {
                int text_index;
                for (text_index = 0;
                     text_index < pending_input.text_count; ++text_index)
                    (void)desk_text_input(&state,
                                          pending_input.text[text_index]);
                for (text_index = 0;
                     text_index < pending_input.backspace_count;
                     ++text_index)
                    (void)desk_text_backspace(&state);
            }
            /* Enter is the interact intent, Space the use-item intent;
             * menus treat both as confirm and an empty hand falls back
             * inside desk_use_item. */
            if (pending_input.open_inventory)
                (void)desk_toggle_inventory(&state, &world);
            else if (pending_input.cancel_pressed)
                desk_cancel(&state, &world);
            else if (pending_input.enter_pressed)
                (void)desk_interact(&state, &world);
            else if (!name_entry && pending_input.space_pressed)
                (void)desk_use_item(&state, &world);
            if (pending_input.select_slot >= 0)
                desk_select_slot(&state, pending_input.select_slot);
            if (pending_input.cycle_slot != 0)
                desk_cycle_slot(&state, pending_input.cycle_slot);
            if (pending_input.drop_pressed)
                (void)desk_drop_selected(&state, &world);
            /* The room walks on held keys; menus step on key presses. */
            if (state.mode == DESK_MODE_ROOM)
                desk_update(&state, &world, pending_input.move_x,
                            pending_input.move_y, DESK_TICK_SECONDS);
            else
                desk_update(&state, &world, pending_input.menu_x,
                            pending_input.menu_y, DESK_TICK_SECONDS);
            {
                desk_audio_event events[4];
                int event_count = desk_take_audio_events(&state, events);
                int event_index;
                for (event_index = 0; event_index < event_count;
                     ++event_index)
                    desk_audio_play(&audio, audio_cast(&state),
                                    events[event_index]);
            }
            if (!sync_graphics(&graphics, &world, &state, &loaded_style)) {
                failed = true;
                break;
            }
            (void)desk_launcher_service(&launcher, &state, &world);
            {
                /* A persistently failing save (unwritable config dir, full
                 * disk) must not retry at 60 Hz. The two records retry
                 * independently so one failing store cannot starve the
                 * other. */
                static int save_retry_ticks;
                static int world_save_retry_ticks;
                if (save_retry_ticks > 0) {
                    save_retry_ticks--;
                } else if (state.profile_dirty) {
                    if (desk_profile_save(&state.profile))
                        state.profile_dirty = false;
                    else
                        save_retry_ticks = 5 * DESK_SIMULATION_HZ;
                }
                if (world_save_retry_ticks > 0) {
                    world_save_retry_ticks--;
                } else if (state.world_dirty) {
                    if (desk_world_state_save(&state.items, &catalog))
                        state.world_dirty = false;
                    else
                        world_save_retry_ticks = 5 * DESK_SIMULATION_HZ;
                }
            }
            desk_audio_update(&audio, DESK_TICK_SECONDS);
            consume_edge_input(&pending_input);
            if (state.quit_requested) {
                quit_requested = true;
                break;
            }
        }
        if (failed) break;

        if (kittyts_check_resize(&terminal_session, &resized_width,
                                 &resized_height)) {
            if (!ki_td_soft_renderer_resize(&renderer, resized_width,
                                            resized_height)) {
                failed = true;
                break;
            }
            width = resized_width;
            height = resized_height;
            first_frame = true;
        }
        if (first_frame ||
            state.simulation_tick / 2u != last_presented_step / 2u) {
            if (!desk_render(&renderer, &state, &world, &graphics) ||
                !kittyts_present(&terminal_session, renderer.rgba,
                                 width, height)) {
                failed = true;
                break;
            }
            first_frame = false;
            last_presented_step = state.simulation_tick;
        }
        if (frame.steps == 0u)
            (void)kilix_game_sleep_until_ns(now + INT64_C(1000000));
    }

done:
    if (!failed && state.mode == DESK_MODE_ROOM) {
        /* Ordinary walking never syncs the profile position; refresh it so
         * an abrupt exit resumes where the player actually stood. */
        if (state.room >= 0 && state.room < world.room_count) {
            (void)snprintf(state.profile.last_room,
                           sizeof state.profile.last_room, "%s",
                           world.rooms[state.room].id);
            state.profile.last_x = state.player_x;
            state.profile.last_y = state.player_y;
        }
        (void)desk_profile_save(&state.profile);
    }
    if (!failed && state.world_dirty)
        (void)desk_world_state_save(&state.items, &catalog);
    ki_td_soft_renderer_destroy(&renderer);
    kittyts_stop(&terminal_session);
    kilix_game_signals_restore(&signals);
    desk_audio_shutdown(&audio);
    desk_graphics_shutdown(&graphics);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int style_index(const char *name)
{
    static const char *const ids[DESK_CAST_COUNT] = {
        "legend", "chumrunner", "fantasy", "pleb-bound"
    };
    int index;
    for (index = 0; index < DESK_CAST_COUNT; ++index)
        if (strcmp(ids[index], name) == 0) return index;
    return -1;
}

static bool laptop_write_fixture(const char *directory, const char *name,
                                 const char *contents)
{
    char path[1024];
    FILE *handle;
    int written = snprintf(path, sizeof path, "%s/%s", directory, name);
    if (written < 0 || (size_t)written >= sizeof path) return false;
    handle = fopen(path, "w");
    if (!handle) return false;
    if (fputs(contents, handle) == EOF) {
        (void)fclose(handle);
        return false;
    }
    return fclose(handle) == 0;
}

static int laptop_inventory_slot(const desk_state *state, int definition)
{
    int slot;
    for (slot = 0; slot < DESK_INVENTORY_SLOTS; ++slot) {
        const desk_item *item = &state->items.inventory.slots[slot];
        if (!desk_item_is_empty(item) &&
            item->definition == (uint16_t)definition)
            return slot;
    }
    return -1;
}

static int laptop_world_item_index(const desk_state *state, int definition)
{
    int index;
    for (index = 0; index < state->items.item_count; ++index)
        if (state->items.items[index].item.definition ==
            (uint16_t)definition)
            return index;
    return -1;
}

/* Walk the laptop cursor to a row. The screen steps one row per update,
 * exactly as a keypress does. */
static bool laptop_move_to(desk_state *state, const desk_world *world,
                           int row)
{
    int guard;
    for (guard = 0; guard < DESK_LAPTOP_ROWS_MAX * 2; ++guard) {
        if (state->laptop.cursor == row) return true;
        desk_update(state, world, 0, 1, DESK_TICK_SECONDS);
    }
    return false;
}

/* Replace whatever the open field holds. */
static bool laptop_type(desk_state *state, const char *text)
{
    size_t index;
    if (!state->laptop.editing) return false;
    while (state->laptop.edit_length > 0)
        if (!desk_text_backspace(state)) return false;
    for (index = 0u; text[index] != '\0'; ++index)
        if (!desk_text_input(state, (uint32_t)(unsigned char)text[index]))
            return false;
    return true;
}

/* The configuration pages: create a profile, edit its fields, refuse a
 * value the loader would refuse, save it to disk, and delete it again —
 * the whole round trip a person makes without leaving the house. */
static int laptop_configuration_test(desk_state *state,
                                     const desk_world *world)
{
    desk_laptop_profile written;
    char error[DESK_LAPTOP_ERROR_CAPACITY];

    if (!desk_interact(state, world) ||
        state->mode != DESK_MODE_LAPTOP) {
        (void)fprintf(stderr, "FAIL laptop config open\n");
        return EXIT_FAILURE;
    }
    /* HOME -> CONFIGURE PROFILES */
    if (!laptop_move_to(state, world, 2) || !desk_interact(state, world) ||
        state->laptop.page != DESK_LAPTOP_PAGE_PROFILES ||
        desk_laptop_menu_count(state) != 4 ||
        strcmp(desk_laptop_menu_item(state, 2), "NEW PROFILE") != 0) {
        (void)fprintf(stderr, "FAIL laptop config list\n");
        return EXIT_FAILURE;
    }
    /* NEW PROFILE -> EDIT, a one-pane session by default. */
    if (!laptop_move_to(state, world, 2) || !desk_interact(state, world) ||
        state->laptop.page != DESK_LAPTOP_PAGE_EDIT ||
        !state->laptop.creating ||
        state->laptop.working.pane_count != 1 ||
        desk_laptop_menu_count(state) != 7) {
        (void)fprintf(stderr, "FAIL laptop new profile\n");
        return EXIT_FAILURE;
    }
    /* NAME */
    if (!laptop_move_to(state, world, 0) || !desk_interact(state, world) ||
        !state->laptop.editing || !laptop_type(state, "Test Bench") ||
        !desk_interact(state, world) || state->laptop.editing ||
        strcmp(state->laptop.working.name, "Test Bench") != 0) {
        (void)fprintf(stderr, "FAIL laptop name field\n");
        return EXIT_FAILURE;
    }
    /* OPENS toggles to a desktop profile and the provider page picks
     * which one; toggling back restores a session with a pane. */
    if (!laptop_move_to(state, world, 1) || !desk_interact(state, world) ||
        state->laptop.working.desktop[0] == '\0' ||
        state->laptop.working.pane_count != 0 ||
        desk_laptop_menu_count(state) != 6) {
        (void)fprintf(stderr, "FAIL laptop kind toggle\n");
        return EXIT_FAILURE;
    }
    if (!laptop_move_to(state, world, 2) || !desk_interact(state, world) ||
        state->laptop.page != DESK_LAPTOP_PAGE_PROVIDER ||
        !laptop_move_to(state, world, 3) || !desk_interact(state, world) ||
        state->laptop.page != DESK_LAPTOP_PAGE_EDIT ||
        strcmp(state->laptop.working.desktop, "cap") != 0) {
        (void)fprintf(stderr, "FAIL laptop provider page\n");
        return EXIT_FAILURE;
    }
    if (!laptop_move_to(state, world, 1) || !desk_interact(state, world) ||
        state->laptop.working.desktop[0] != '\0' ||
        state->laptop.working.pane_count != 1) {
        (void)fprintf(stderr, "FAIL laptop kind toggle back\n");
        return EXIT_FAILURE;
    }
    /* LAYOUT toggles splits/tabs in place. */
    if (!laptop_move_to(state, world, 2) || !desk_interact(state, world) ||
        !state->laptop.working.tabs) {
        (void)fprintf(stderr, "FAIL laptop layout toggle\n");
        return EXIT_FAILURE;
    }
    /* PANES -> add a second pane, then edit it. */
    if (!laptop_move_to(state, world, 3) || !desk_interact(state, world) ||
        state->laptop.page != DESK_LAPTOP_PAGE_PANES ||
        desk_laptop_menu_count(state) != 3) {
        (void)fprintf(stderr, "FAIL laptop panes page\n");
        return EXIT_FAILURE;
    }
    if (!laptop_move_to(state, world, 1) || !desk_interact(state, world) ||
        state->laptop.page != DESK_LAPTOP_PAGE_PANE ||
        state->laptop.working.pane_count != 2 ||
        state->laptop.pane_index != 1) {
        (void)fprintf(stderr, "FAIL laptop add pane\n");
        return EXIT_FAILURE;
    }
    /* A destination the loader would refuse is refused here too, and the
     * field stays open rather than keeping a value nothing can run. */
    if (!laptop_move_to(state, world, 2) || !desk_interact(state, world) ||
        !laptop_type(state, "host; rm -rf /") ||
        !desk_interact(state, world) || !state->laptop.editing ||
        state->laptop.working.panes[1].ssh[0] != '\0' ||
        desk_laptop_message(state)[0] == '\0') {
        (void)fprintf(stderr, "FAIL laptop ssh refusal\n");
        return EXIT_FAILURE;
    }
    if (!laptop_type(state, "admin@example-host") ||
        !desk_interact(state, world) || state->laptop.editing ||
        strcmp(state->laptop.working.panes[1].ssh,
               "admin@example-host") != 0) {
        (void)fprintf(stderr, "FAIL laptop ssh accept\n");
        return EXIT_FAILURE;
    }
    if (!laptop_move_to(state, world, 3) || !desk_interact(state, world) ||
        !laptop_type(state, "tail -f syslog") ||
        !desk_interact(state, world) ||
        strcmp(state->laptop.working.panes[1].cmd, "tail -f syslog") != 0) {
        (void)fprintf(stderr, "FAIL laptop command field\n");
        return EXIT_FAILURE;
    }
    /* Back out to EDIT and save. */
    desk_cancel(state, world);
    if (state->laptop.page != DESK_LAPTOP_PAGE_PANES) {
        (void)fprintf(stderr, "FAIL laptop pane escape\n");
        return EXIT_FAILURE;
    }
    desk_cancel(state, world);
    if (state->laptop.page != DESK_LAPTOP_PAGE_EDIT ||
        !laptop_move_to(state, world, 4) || !desk_interact(state, world) ||
        state->laptop.creating || state->laptop.working_dirty ||
        state->laptop_menu_count != 3) {
        (void)fprintf(stderr, "FAIL laptop save\n");
        return EXIT_FAILURE;
    }
    /* What landed on disk is what the loader reads back. */
    if (!desk_laptop_load("test-bench", &written, error, sizeof error) ||
        strcmp(written.name, "Test Bench") != 0 || !written.tabs ||
        written.pane_count != 2 ||
        strcmp(written.panes[1].ssh, "admin@example-host") != 0 ||
        strcmp(written.panes[1].cmd, "tail -f syslog") != 0) {
        (void)fprintf(stderr, "FAIL laptop saved profile reload: %s\n",
                      error);
        return EXIT_FAILURE;
    }
    /* DELETE asks first, then removes the file and the row. */
    if (!laptop_move_to(state, world, 5) || !desk_interact(state, world) ||
        state->laptop.page != DESK_LAPTOP_PAGE_DELETE ||
        !laptop_move_to(state, world, 0) || !desk_interact(state, world) ||
        state->laptop.page != DESK_LAPTOP_PAGE_PROFILES ||
        state->laptop_menu_count != 2 ||
        desk_laptop_load("test-bench", &written, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL laptop delete\n");
        return EXIT_FAILURE;
    }
    desk_cancel(state, world);
    if (state->laptop.page != DESK_LAPTOP_PAGE_HOME) {
        (void)fprintf(stderr, "FAIL laptop config back to home\n");
        return EXIT_FAILURE;
    }
    desk_cancel(state, world);
    if (state->mode != DESK_MODE_ROOM) {
        (void)fprintf(stderr, "FAIL laptop config close\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/* The set-up laptop's whole loop: menu on Enter, profile choice raised as
 * one take-and-clear request, PICK UP back into the hand, Space to set it
 * up again, and the placed laptop persisting through a world.state
 * round trip like every other world item. */
static int laptop_test_body(const char *config_dir)
{
    char profiles_dir[1024];
    char taken[DESK_LAPTOP_ID_CAPACITY];
    char error[DESK_ERROR_CAPACITY];
    desk_world world;
    desk_item_catalog catalog;
    desk_state state;
    int study;
    int laptop_def;
    int slot;
    int written;

    if (!desk_laptop_selftest()) {
        (void)fprintf(stderr, "FAIL laptop module selftest\n");
        return EXIT_FAILURE;
    }

    written = snprintf(profiles_dir, sizeof profiles_dir, "%s/profiles",
                       config_dir);
    if (written < 0 || (size_t)written >= sizeof profiles_dir ||
        mkdir(profiles_dir, 0700) != 0 ||
        setenv("KILIX_LAPTOP_PROFILES", profiles_dir, 1) != 0) {
        (void)fprintf(stderr, "FAIL laptop profiles fixture dir\n");
        return EXIT_FAILURE;
    }
    if (!laptop_write_fixture(profiles_dir, "bench.profile",
                              "name=Bench\npane.1.cwd=~\n") ||
        !laptop_write_fixture(profiles_dir, "ops.profile",
                              "name=Ops\nlayout=tabs\n"
                              "pane.1.ssh=admin@example-host\n")) {
        (void)fprintf(stderr, "FAIL laptop profile fixtures\n");
        return EXIT_FAILURE;
    }

    if (!load_world(&world, &catalog, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL laptop world: %s\n", error);
        return EXIT_FAILURE;
    }
    laptop_def = desk_items_find(&catalog, "core:tool/laptop");
    study = desk_world_room_index(&world, "study");
    if (laptop_def <= 0 || study < 0) {
        (void)fprintf(stderr, "FAIL laptop catalog/world lookup\n");
        return EXIT_FAILURE;
    }
    desk_init(&state, &world, &catalog);
    if (state.mode != DESK_MODE_WIZARD ||
        !complete_wizard(&state, &world)) {
        (void)fprintf(stderr, "FAIL laptop wizard\n");
        return EXIT_FAILURE;
    }

    /* Stand just below the authored study spawn and interact. */
    state.room = study;
    state.player_x = 300.0f;
    state.player_y = 244.0f;
    state.facing = DESK_FACING_DOWN;
    state.door_cooldown_ticks = DESK_DOOR_COOLDOWN_TICKS;
    desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
    if (laptop_world_item_index(&state, laptop_def) < 0 ||
        state.nearest_world_item < 0 ||
        state.items.items[state.nearest_world_item].item.definition !=
            (uint16_t)laptop_def) {
        (void)fprintf(stderr, "FAIL laptop spawn not nearest\n");
        return EXIT_FAILURE;
    }
    if (!desk_interact(&state, &world) ||
        state.mode != DESK_MODE_LAPTOP ||
        state.laptop.page != DESK_LAPTOP_PAGE_HOME ||
        state.laptop_menu_count != 2 ||
        desk_laptop_menu_count(&state) != 5 ||
        strcmp(desk_laptop_menu_item(&state, 0), "bench") != 0 ||
        strcmp(desk_laptop_menu_item(&state, 1), "ops") != 0 ||
        strcmp(desk_laptop_menu_item(&state, 2),
               "CONFIGURE PROFILES") != 0 ||
        strcmp(desk_laptop_menu_item(&state, 3), "PICK UP LAPTOP") != 0 ||
        strcmp(desk_laptop_menu_item(&state, 4), "CLOSE THE LID") != 0) {
        (void)fprintf(stderr, "FAIL laptop menu open\n");
        return EXIT_FAILURE;
    }

    /* Cursor to the second profile and choose it: HOME keeps the fast
     * path, one Enter from the list to the session. */
    desk_update(&state, &world, 0, 1, DESK_TICK_SECONDS);
    if (state.laptop.cursor != 1 || !desk_interact(&state, &world) ||
        state.mode != DESK_MODE_ROOM ||
        !desk_take_laptop_request(&state, taken) ||
        strcmp(taken, "ops") != 0 ||
        desk_take_laptop_request(&state, taken)) {
        (void)fprintf(stderr, "FAIL laptop profile choice\n");
        return EXIT_FAILURE;
    }

    /* Escape closes without choosing. */
    if (!desk_interact(&state, &world) ||
        state.mode != DESK_MODE_LAPTOP)
        return EXIT_FAILURE;
    desk_cancel(&state, &world);
    if (state.mode != DESK_MODE_ROOM ||
        desk_take_laptop_request(&state, taken)) {
        (void)fprintf(stderr, "FAIL laptop escape\n");
        return EXIT_FAILURE;
    }

    if (laptop_configuration_test(&state, &world) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    /* PICK UP LAPTOP pockets it through the ordinary pickup path. */
    if (!desk_interact(&state, &world) ||
        state.mode != DESK_MODE_LAPTOP)
        return EXIT_FAILURE;
    desk_update(&state, &world, 0, 1, DESK_TICK_SECONDS);
    desk_update(&state, &world, 0, 1, DESK_TICK_SECONDS);
    desk_update(&state, &world, 0, 1, DESK_TICK_SECONDS);
    if (state.laptop.cursor != 3 || !desk_interact(&state, &world) ||
        state.mode != DESK_MODE_ROOM ||
        laptop_world_item_index(&state, laptop_def) >= 0 ||
        (slot = laptop_inventory_slot(&state, laptop_def)) < 0) {
        (void)fprintf(stderr, "FAIL laptop pick up\n");
        return EXIT_FAILURE;
    }

    /* Space sets it up again in front of the player. */
    desk_select_slot(&state, slot);
    if (!desk_use_item(&state, &world) ||
        laptop_world_item_index(&state, laptop_def) < 0 ||
        laptop_inventory_slot(&state, laptop_def) >= 0) {
        (void)fprintf(stderr, "FAIL laptop set up\n");
        return EXIT_FAILURE;
    }

    /* The placed laptop persists like every other world item. */
    if (!desk_world_state_save(&state.items, &catalog)) {
        (void)fprintf(stderr, "FAIL laptop world save\n");
        return EXIT_FAILURE;
    }
    {
        desk_world_state reloaded;
        bool corrupt = false;
        int index;
        if (!desk_world_state_load(&reloaded, &catalog, &corrupt) ||
            corrupt) {
            (void)fprintf(stderr, "FAIL laptop world reload\n");
            return EXIT_FAILURE;
        }
        index = -1;
        {
            int cursor;
            for (cursor = 0; cursor < reloaded.item_count; ++cursor)
                if (reloaded.items[cursor].item.definition ==
                    (uint16_t)laptop_def)
                    index = cursor;
        }
        /* The codec stores room ids, not runtime indexes; a fresh load is
         * unresolved (-1) until the simulation adopts it. */
        if (index < 0 ||
            strcmp(reloaded.items[index].room_id, "study") != 0) {
            (void)fprintf(stderr, "FAIL laptop persistence\n");
            return EXIT_FAILURE;
        }
    }

    /* The run registry drives the lid: a recorded live session opens it
     * (one frame per DESK_LAPTOP_LID_TICKS after the per-second refresh
     * notices), the open menu marks the running profile, Enter on that
     * row raises exactly one close request, and a cleared registry
     * closes the lid again. */
    {
        char close_id[DESK_LAPTOP_ID_CAPACITY];
        char run_dir[1024];
        char pid_path[1024];
        int tick;
        int settle = DESK_SIMULATION_HZ +
                     DESK_LAPTOP_LID_FRAMES * DESK_LAPTOP_LID_TICKS + 2;
        if (state.laptop_on || state.laptop_lid_frame != 0) {
            (void)fprintf(stderr, "FAIL laptop starts closed\n");
            return EXIT_FAILURE;
        }
        if (!desk_laptop_run_record("ops", (long)getpid())) {
            (void)fprintf(stderr, "FAIL laptop run record\n");
            return EXIT_FAILURE;
        }
        for (tick = 0; tick < settle; ++tick)
            desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        if (!state.laptop_on ||
            state.laptop_lid_frame != DESK_LAPTOP_LID_FRAMES - 1) {
            (void)fprintf(stderr,
                          "FAIL laptop lid opens with a live session\n");
            return EXIT_FAILURE;
        }
        if (!desk_interact(&state, &world) ||
            state.mode != DESK_MODE_LAPTOP ||
            state.laptop_menu_running[0] ||
            !state.laptop_menu_running[1] ||
            strcmp(desk_laptop_row_value(&state, 1),
                   "RUNNING - ENTER CLOSES") != 0 ||
            strcmp(desk_laptop_row_value(&state, 0), "") != 0) {
            (void)fprintf(stderr, "FAIL laptop running row marker\n");
            return EXIT_FAILURE;
        }
        desk_update(&state, &world, 0, 1, DESK_TICK_SECONDS);
        if (state.laptop.cursor != 1 || !desk_interact(&state, &world) ||
            state.mode != DESK_MODE_LAPTOP ||
            !desk_take_laptop_close_request(&state, close_id) ||
            strcmp(close_id, "ops") != 0 ||
            desk_take_laptop_close_request(&state, close_id)) {
            (void)fprintf(stderr, "FAIL laptop close request\n");
            return EXIT_FAILURE;
        }
        desk_cancel(&state, &world);
        if (state.mode != DESK_MODE_ROOM) {
            (void)fprintf(stderr, "FAIL laptop close request escape\n");
            return EXIT_FAILURE;
        }
        if (!desk_laptop_run_directory(run_dir, sizeof run_dir)) {
            (void)fprintf(stderr, "FAIL laptop run directory\n");
            return EXIT_FAILURE;
        }
        written = snprintf(pid_path, sizeof pid_path, "%s/ops.pid",
                           run_dir);
        if (written < 0 || (size_t)written >= sizeof pid_path ||
            unlink(pid_path) != 0) {
            (void)fprintf(stderr, "FAIL laptop registry cleanup\n");
            return EXIT_FAILURE;
        }
        for (tick = 0; tick < settle; ++tick)
            desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        if (state.laptop_on || state.laptop_lid_frame != 0) {
            (void)fprintf(stderr,
                          "FAIL laptop lid closes when the session ends\n");
            return EXIT_FAILURE;
        }
        (void)rmdir(run_dir);
    }

    /* An empty profile directory still opens a usable menu. */
    {
        char empty_dir[1024];
        written = snprintf(empty_dir, sizeof empty_dir, "%s/empty",
                           config_dir);
        if (written < 0 || (size_t)written >= sizeof empty_dir ||
            mkdir(empty_dir, 0700) != 0 ||
            setenv("KILIX_LAPTOP_PROFILES", empty_dir, 1) != 0)
            return EXIT_FAILURE;
        desk_update(&state, &world, 0, 0, DESK_TICK_SECONDS);
        if (!desk_interact(&state, &world) ||
            state.mode != DESK_MODE_LAPTOP ||
            state.laptop_menu_count != 0 ||
            desk_laptop_menu_count(&state) != 3 ||
            strcmp(desk_laptop_menu_item(&state, 0),
                   "CONFIGURE PROFILES") != 0) {
            (void)fprintf(stderr, "FAIL laptop empty menu\n");
            return EXIT_FAILURE;
        }
        desk_cancel(&state, &world);
    }
    (void)unsetenv("KILIX_LAPTOP_PROFILES");

    /* remove_tree only clears one level; empty the fixture subdirectory
     * so the caller's cleanup can rmdir it. */
    {
        char path[1024];
        written = snprintf(path, sizeof path, "%s/bench.profile",
                           profiles_dir);
        if (written > 0 && (size_t)written < sizeof path)
            (void)unlink(path);
        written = snprintf(path, sizeof path, "%s/ops.profile",
                           profiles_dir);
        if (written > 0 && (size_t)written < sizeof path)
            (void)unlink(path);
    }

    (void)printf(
        "PASS laptop profiles=2 menu=open/choose/escape/pickup "
        "config=new/name/kind/provider/layout/panes/refusal/save/delete "
        "setup=placed persistence=world.state "
        "registry=lid-open/marker/close-request/lid-close empty=usable\n");
    return EXIT_SUCCESS;
}

static int laptop_test(void)
{
    char config_dir[1024];
    int result;
    if (!make_temp_config(config_dir, sizeof config_dir)) {
        (void)fprintf(stderr, "FAIL laptop temp config\n");
        return EXIT_FAILURE;
    }
    result = laptop_test_body(config_dir);
    if (!remove_tree(config_dir) && result == EXIT_SUCCESS) {
        (void)fprintf(stderr, "FAIL laptop temp cleanup\n");
        result = EXIT_FAILURE;
    }
    return result;
}

/* Review workflow (IMPLEMENTATION.md section 13): render one frame of an
 * arbitrary room in an arbitrary style to PATH as a P6 PPM. */
/* Panels a screenshot can open, so a UI change can be looked at rather
 * than only asserted about. Each names the fixture whose activation
 * raises it; "board" activates the notice board twice to get past its
 * panel to the board itself. */
typedef struct panel_shot {
    const char *name;
    const char *room;
    const char *object;
    int activations;
} panel_shot;

static const panel_shot PANEL_SHOTS[] = {
    /* Zero activations frames the fixture with its prompt chip up, which
     * is how a wall fixture's placement against the painted plate gets
     * checked per house style. */
    { "fuse-box", "yard", "fuse-box", 0 },
    { "bed", "bedroom", "bed", 1 },
    { "shed", "yard", "shed", 1 },
    { "phone", "living", "phone", 1 },
    { "dev-rig", "study", "dev-rig", 1 },
    { "bookshelf", "study", "bookshelf", 1 },
    { "monitor", "study", "monitor", 1 },
    { "computer", "study", "computer", 1 },
    { "notice-board", "kitchen", "notice-board", 1 },
    { "board", "kitchen", "notice-board", 2 },
    { "power", "bedroom", "bed", 2 },
    { "laptop", "study", NULL, 1 },
    { "laptop-profiles", "study", NULL, 2 },
    { "laptop-edit", "study", NULL, 3 }
};

static const panel_shot *find_panel_shot(const char *name)
{
    size_t index;
    for (index = 0u; index < sizeof PANEL_SHOTS / sizeof PANEL_SHOTS[0];
         ++index)
        if (strcmp(PANEL_SHOTS[index].name, name) == 0)
            return &PANEL_SHOTS[index];
    return NULL;
}

/* Stand where the fixture is reachable: just below it, clamped into the
 * room's walkable rect, which is how a player reaches it too. */
static bool stand_by_object(desk_state *state, const desk_room *room,
                            const char *object_id)
{
    int index;
    for (index = 0; index < room->object_count; ++index) {
        const desk_object *object = &room->objects[index];
        float x;
        float y;
        if (strcmp(object->id, object_id) != 0) continue;
        x = object->rect.x + object->rect.w * 0.5f;
        y = object->rect.y + object->rect.h + 16.0f;
        if (x < room->walk.x) x = room->walk.x;
        if (x > room->walk.x + room->walk.w) x = room->walk.x + room->walk.w;
        if (y < room->walk.y) y = room->walk.y;
        if (y > room->walk.y + room->walk.h) y = room->walk.y + room->walk.h;
        state->player_x = x;
        state->player_y = y;
        state->facing = DESK_FACING_UP;
        return true;
    }
    return false;
}

static int screenshot(const char *path, const char *room_id,
                      const char *style_name, const char *panel_name)
{
    char config_dir[1024];
    render_fixture fixture;
    desk_state state;
    int style = style_name ? style_index(style_name) : 0;
    int room = -1;
    int step;
    bool success;
    if (style < 0) {
        (void)fprintf(stderr, "FAIL screenshot unknown style '%s'\n",
                      style_name);
        return EXIT_FAILURE;
    }
    if (!make_temp_config(config_dir, sizeof config_dir)) {
        (void)fprintf(stderr, "FAIL screenshot temp config\n");
        return EXIT_FAILURE;
    }
    success = fixture_open(&fixture, config_dir);
    if (success) {
        desk_init(&state, &fixture.world, &fixture.catalog);
        for (step = 0; step < style; ++step)
            desk_update(&state, &fixture.world, 0, 1, DESK_TICK_SECONDS);
        success = state.mode == DESK_MODE_WIZARD &&
                  state.wizard_cast_cursor == style &&
                  complete_wizard(&state, &fixture.world);
    }
    if (success && room_id) {
        room = desk_world_room_index(&fixture.world, room_id);
        if (room < 0) {
            (void)fprintf(stderr, "FAIL screenshot unknown room '%s'\n",
                          room_id);
            success = false;
        }
        if (success) state.room = room;
    }
    if (success && panel_name) {
        const panel_shot *shot = find_panel_shot(panel_name);
        if (!shot) {
            (void)fprintf(stderr, "FAIL screenshot unknown panel '%s'\n",
                          panel_name);
            success = false;
        } else {
            room = desk_world_room_index(&fixture.world, shot->room);
            if (room < 0) success = false;
            else state.room = room;
        }
    }
    if (success) {
        const desk_room *scene = &fixture.world.rooms[state.room];
        const panel_shot *shot = panel_name ? find_panel_shot(panel_name)
                                            : NULL;
        state.player_x = scene->walk.x + scene->walk.w * 0.5f;
        state.player_y = scene->walk.y + scene->walk.h * 0.5f;
        state.facing = DESK_FACING_DOWN;
        state.door_cooldown_ticks = DESK_DOOR_COOLDOWN_TICKS;
        if (shot) {
            int activation;
            if (shot->object) {
                if (!stand_by_object(&state, scene, shot->object)) {
                    (void)fprintf(stderr,
                                  "FAIL screenshot no object '%s'\n",
                                  shot->object);
                    success = false;
                }
            } else {
                /* The laptop: the study's authored spawn is the target. */
                state.player_x = 300.0f;
                state.player_y = 244.0f;
                state.facing = DESK_FACING_DOWN;
            }
            desk_update(&state, &fixture.world, 0, 0, DESK_TICK_SECONDS);
            for (activation = 0; success && activation < shot->activations;
                 ++activation) {
                if (!desk_interact(&state, &fixture.world)) {
                    (void)fprintf(stderr,
                                  "FAIL screenshot panel '%s' step %d\n",
                                  shot->name, activation + 1);
                    success = false;
                }
                /* The laptop's deeper pages sit under CONFIGURE
                 * PROFILES, which is the row after the profile list. */
                if (success && !shot->object &&
                    state.mode == DESK_MODE_LAPTOP &&
                    activation + 1 < shot->activations) {
                    while (state.laptop.cursor <
                           state.laptop_menu_count)
                        desk_update(&state, &fixture.world, 0, 1,
                                    DESK_TICK_SECONDS);
                }
            }
        }
        desk_update(&state, &fixture.world, 0, 0, DESK_TICK_SECONDS);
        state.toast_ticks = 0;
        success = sync_graphics(&fixture.graphics, &fixture.world, &state,
                                &fixture.loaded_style) &&
                  desk_render(&fixture.renderer, &state, &fixture.world,
                              &fixture.graphics) &&
                  sr_write_ppm(ki_td_soft_canvas(&fixture.renderer), path);
        if (success)
            (void)printf("PASS screenshot room=%s style=%s panel=%s "
                         "file=%s\n",
                         fixture.world.rooms[state.room].id,
                         style_name ? style_name : "legend",
                         panel_name ? panel_name : "none", path);
    }
    fixture_close(&fixture);
    if (!remove_tree(config_dir) && success) {
        (void)fprintf(stderr, "FAIL screenshot temp cleanup\n");
        success = false;
    }
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void usage(const char *program)
{
    (void)fprintf(stderr,
        "usage: %s [--selftest | --audio-test | --graphics-test | "
        "--json-test | --items-test | "
        "--world-test | --profile-test | --laptop-test | "
        "--wizard-render-test DIR | "
        "--room-render-test DIR | --outfit-render-test DIR | "
        "--walk-render-test DIR | --items-render-test DIR | "
        "--screenshot PATH [--room ID] [--style STYLE] [--panel NAME] | "
        "--version]\n",
        program);
}

int main(int argc, char **argv)
{
    if (argc == 1) return run_interactive();
    if (argc == 2 && strcmp(argv[1], "--selftest") == 0) return selftest();
    if (argc == 2 && strcmp(argv[1], "--audio-test") == 0)
        return audio_test();
    if (argc == 2 && strcmp(argv[1], "--graphics-test") == 0)
        return graphics_test();
    if (argc == 2 && strcmp(argv[1], "--json-test") == 0)
        return json_test();
    if (argc == 2 && strcmp(argv[1], "--items-test") == 0)
        return items_test();
    if (argc == 2 && strcmp(argv[1], "--world-test") == 0)
        return world_test();
    if (argc == 2 && strcmp(argv[1], "--doors-test") == 0)
        return doors_test();
    if (argc == 2 && strcmp(argv[1], "--profile-test") == 0)
        return profile_test();
    if (argc == 2 && strcmp(argv[1], "--laptop-test") == 0)
        return laptop_test();
    if (argc == 3 && strcmp(argv[1], "--wizard-render-test") == 0)
        return wizard_render_test(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--room-render-test") == 0)
        return room_render_test(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--outfit-render-test") == 0)
        return outfit_render_test(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--walk-render-test") == 0)
        return walk_render_test(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--items-render-test") == 0)
        return items_render_test(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "--screenshot") == 0) {
        const char *room = NULL;
        const char *style = NULL;
        const char *panel = NULL;
        int argument = 3;
        while (argument + 1 < argc) {
            if (strcmp(argv[argument], "--room") == 0)
                room = argv[argument + 1];
            else if (strcmp(argv[argument], "--style") == 0)
                style = argv[argument + 1];
            else if (strcmp(argv[argument], "--panel") == 0)
                panel = argv[argument + 1];
            else
                break;
            argument += 2;
        }
        if (argument == argc)
            return screenshot(argv[2], room, style, panel);
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        (void)printf("kilix-land-desktop 0.1.0\n");
        return EXIT_SUCCESS;
    }
    usage(argv[0]);
    return 2;
}
