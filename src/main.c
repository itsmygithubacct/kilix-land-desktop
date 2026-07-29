#include "kilix_land_desktop.h"

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
    int backspace_count;
    int text_count;
    uint32_t text[DESK_TEXT_QUEUE_CAPACITY];
} desk_key_input;

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

static bool load_world(desk_world *world, char *error, size_t error_size)
{
    char path[1024];
    if (!world_path(path, sizeof path)) {
        if (error && error_size > 0u)
            (void)snprintf(error, error_size, "world path too long");
        return false;
    }
    return desk_world_load(world, path, error, error_size) &&
           desk_world_validate(world, error, error_size);
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
    desk_graphics graphics;
    ki_td_rgba8 image;
    char error[DESK_ERROR_CAPACITY];
    size_t loaded;
    int plates = 0;
    int room;
    if (!load_world(&world, error, sizeof error)) {
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
    char path[1024];
    char error[DESK_ERROR_CAPACITY];
    int objects = 0;
    int doors = 0;
    int npcs = 0;
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
    for (room = 0; room < world.room_count; ++room) {
        objects += world.rooms[room].object_count;
        doors += world.rooms[room].door_count;
        npcs += world.rooms[room].npc_count;
    }
    (void)printf(
        "PASS world rooms=%d objects=%d doors=%d npcs=%d start=%s\n",
        world.room_count, objects, doors, npcs,
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
    desk_state state;
    desk_profile reloaded;
    desk_audio_event events[4];
    char error[DESK_ERROR_CAPACITY];
    int living;
    int guard;
    int target;
    int event_count;

    if (!load_world(&world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest world: %s\n", error);
        return EXIT_FAILURE;
    }
    desk_init(&state, &world);
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

    desk_update(&state, &world, 1, 0, DESK_TICK_SECONDS);
    if (!state.player_moving || state.facing != DESK_FACING_RIGHT ||
        !desk_validate(&state, &world, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL selftest walk: %s\n", error);
        return EXIT_FAILURE;
    }

    living = desk_world_room_index(&world, "living");
    state.player_x = 450.0f;
    state.player_y = 216.0f;
    state.door_cooldown_ticks = 0;
    for (guard = 0; guard < 30 && state.room == world.start_room; ++guard)
        desk_update(&state, &world, 1, 0, DESK_TICK_SECONDS);
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
    desk_update(&state, &world, 0, 1, DESK_TICK_SECONDS);
    desk_update(&state, &world, 0, 1, DESK_TICK_SECONDS);
    if (state.pause_cursor != 2 || !desk_interact(&state, &world) ||
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
        desk_target_is_external(DESK_TARGET_NONE)) {
        (void)fprintf(stderr, "FAIL selftest target classes\n");
        return EXIT_FAILURE;
    }
    (void)printf(
        "PASS selftest rooms=%d wizard=cast-actor-name-outfit-confirm "
        "door=bedroom->living launch=games dialogue=reveal-advance-close "
        "pause=quit-confirm targets=%d\n",
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

typedef struct render_fixture {
    desk_world world;
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
    if (!desk_profile_reset()) return false;
    if (!load_world(&fixture->world, error, sizeof error)) {
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

static int wizard_render_test(const char *directory)
{
    render_fixture fixture;
    desk_state state;
    bool success;
    if (!fixture_open(&fixture, directory)) return EXIT_FAILURE;
    desk_init(&state, &fixture.world);
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

static int room_render_test(const char *directory)
{
    render_fixture fixture;
    desk_state state;
    bool success;
    int room;
    if (!fixture_open(&fixture, directory)) return EXIT_FAILURE;
    desk_init(&state, &fixture.world);
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
    }
    fixture_close(&fixture);
    if (!success) return EXIT_FAILURE;
    (void)printf(
        "PASS render scene=rooms cast=legend files=%d size=%dx%d "
        "directory=%s\n",
        fixture.world.room_count, DESK_LOGICAL_WIDTH, DESK_LOGICAL_HEIGHT,
        directory);
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
            desk_init(&state, &fixture.world);
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
    const desk_door *door;
    bool success;
    int frame;
    if (!fixture_open(&fixture, directory)) return EXIT_FAILURE;
    desk_init(&state, &fixture.world);
    success = state.mode == DESK_MODE_WIZARD &&
              complete_wizard(&state, &fixture.world) &&
              fixture.world.rooms[fixture.world.start_room].door_count > 0;
    if (success) {
        door = &fixture.world.rooms[fixture.world.start_room].doors[0];
        state.player_x = door->rect.x - 22.0f;
        state.player_y = door->rect.y + door->rect.h * 0.5f;
        state.facing = DESK_FACING_RIGHT;
        state.door_cooldown_ticks = 0;
        state.toast_ticks = 0;
        for (frame = 0; frame < 8 && success; ++frame) {
            char name[32];
            uint64_t target_tick = (uint64_t)(unsigned int)frame * 5u + 1u;
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

    if (!load_world(&world, error, sizeof error)) {
        (void)fprintf(stderr,
            "kilix-land-desktop: %s (set KILIX_LAND_DESKTOP_ASSETS)\n",
            error);
        return EXIT_FAILURE;
    }
    desk_init(&state, &world);
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
            bool name_entry = state.mode == DESK_MODE_WIZARD &&
                              state.wizard_step == DESK_WIZARD_NAME;
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
            if (pending_input.cancel_pressed)
                desk_cancel(&state, &world);
            else if (pending_input.enter_pressed ||
                     (!name_entry && pending_input.space_pressed))
                (void)desk_interact(&state, &world);
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
            (void)desk_launcher_service(&launcher, &state);
            {
                /* A persistently failing save (unwritable config dir, full
                 * disk) must not retry at 60 Hz. */
                static int save_retry_ticks;
                if (save_retry_ticks > 0) {
                    save_retry_ticks--;
                } else if (state.profile_dirty) {
                    if (desk_profile_save(&state.profile))
                        state.profile_dirty = false;
                    else
                        save_retry_ticks = 5 * DESK_SIMULATION_HZ;
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

/* Review workflow (IMPLEMENTATION.md section 13): render one frame of an
 * arbitrary room in an arbitrary style to PATH as a P6 PPM. */
static int screenshot(const char *path, const char *room_id,
                      const char *style_name)
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
        desk_init(&state, &fixture.world);
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
    if (success) {
        const desk_room *scene = &fixture.world.rooms[state.room];
        state.player_x = scene->walk.x + scene->walk.w * 0.5f;
        state.player_y = scene->walk.y + scene->walk.h * 0.5f;
        state.facing = DESK_FACING_DOWN;
        state.door_cooldown_ticks = DESK_DOOR_COOLDOWN_TICKS;
        desk_update(&state, &fixture.world, 0, 0, DESK_TICK_SECONDS);
        state.toast_ticks = 0;
        success = sync_graphics(&fixture.graphics, &fixture.world, &state,
                                &fixture.loaded_style) &&
                  desk_render(&fixture.renderer, &state, &fixture.world,
                              &fixture.graphics) &&
                  sr_write_ppm(ki_td_soft_canvas(&fixture.renderer), path);
        if (success)
            (void)printf("PASS screenshot room=%s style=%s file=%s\n",
                         fixture.world.rooms[state.room].id,
                         style_name ? style_name : "legend", path);
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
        "--world-test | --profile-test | --wizard-render-test DIR | "
        "--room-render-test DIR | --outfit-render-test DIR | "
        "--walk-render-test DIR | "
        "--screenshot PATH [--room ID] [--style STYLE] | --version]\n",
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
    if (argc == 2 && strcmp(argv[1], "--world-test") == 0)
        return world_test();
    if (argc == 2 && strcmp(argv[1], "--profile-test") == 0)
        return profile_test();
    if (argc == 3 && strcmp(argv[1], "--wizard-render-test") == 0)
        return wizard_render_test(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--room-render-test") == 0)
        return room_render_test(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--outfit-render-test") == 0)
        return outfit_render_test(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--walk-render-test") == 0)
        return walk_render_test(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "--screenshot") == 0) {
        const char *room = NULL;
        const char *style = NULL;
        int argument = 3;
        while (argument + 1 < argc) {
            if (strcmp(argv[argument], "--room") == 0)
                room = argv[argument + 1];
            else if (strcmp(argv[argument], "--style") == 0)
                style = argv[argument + 1];
            else
                break;
            argument += 2;
        }
        if (argument == argc) return screenshot(argv[2], room, style);
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        (void)printf("kilix-land-desktop 0.1.0\n");
        return EXIT_SUCCESS;
    }
    usage(argv[0]);
    return 2;
}
