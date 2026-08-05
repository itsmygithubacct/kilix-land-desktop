#ifndef KILIX_LAND_DESKTOP_H
#define KILIX_LAND_DESKTOP_H

#include "kilix_game_kit.h"
#include "kilix_assets.h"
#include "kilix_top_down.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "source_parity.h"
#include "items.h"
#include "laptop.h"
#include "world_state.h"

#define DESK_LOGICAL_WIDTH 480
#define DESK_LOGICAL_HEIGHT 270
#define DESK_SIMULATION_HZ 60
#define DESK_TICK_SECONDS (1.0f / 60.0f)
#define DESK_FRAMEBUFFER_MIN_WIDTH 800
#define DESK_FRAMEBUFFER_MIN_HEIGHT 450
#define DESK_FRAMEBUFFER_MAX_WIDTH 1920
#define DESK_FRAMEBUFFER_MAX_HEIGHT 1080
#define DESK_PLATE_WIDTH 1280
#define DESK_PLATE_HEIGHT 720

#define DESK_CAST_COUNT 4
#define DESK_ACTOR_COUNT 4
#define DESK_GRAPHICS_COUNT 11
#define DESK_AUDIO_EVENT_COUNT 3
#define DESK_AUDIO_SOURCE_CUE_COUNT 12
#define DESK_HERO_MOTION_VARIANT_COUNT 5
#define DESK_OUTFIT_COUNT 6

#define DESK_NAME_CAPACITY 24
#define DESK_ID_CAPACITY 24
#define DESK_LABEL_CAPACITY 40
#define DESK_PROMPT_CAPACITY 48
#define DESK_TOAST_CAPACITY 96
#define DESK_ERROR_CAPACITY 160

#define DESK_MAX_ROOMS 16
#define DESK_MAX_OBJECTS_PER_ROOM 12
#define DESK_MAX_DOORS_PER_ROOM 4
#define DESK_MAX_OBSTACLES_PER_ROOM 64
#define DESK_MAX_NPCS_PER_ROOM 3
#define DESK_MAX_WALKBEHINDS_PER_ROOM 15
#define DESK_MAX_ITEM_SPAWNS_PER_ROOM 8
#define DESK_NPC_SPAWN_EXCLUSION 24.0f

/* Stardew-model boundaries: solidity tests a small FEET BOX, never the
 * sprite — a 16x8 logical box hugging the ground contact, symmetric on
 * the spine. Bodies may overlap scenery and each other freely above the
 * feet; depth sorting and walk-behind masks own that. NPCs are solid
 * through the same box. */
#define DESK_FEET_BOX_HALF_WIDTH 8.0f
#define DESK_FEET_BOX_HEIGHT 8.0f

#define DESK_INTERACT_RADIUS 72.0f
/* A gift needs an arm's-length handoff, not the full interact radius,
 * so placing or drinking near a housemate never turns into a present. */
#define DESK_GIFT_REACH 40.0f
#define DESK_DOOR_COOLDOWN_TICKS 30
#define DESK_TOAST_TICKS 180
#define DESK_DIALOGUE_REVEAL_TICKS_PER_CHAR 2
#define DESK_PROFILE_SCHEMA 1
#define DESK_STATUS_LINE_COUNT 10
#define DESK_STATUS_LINE_CAPACITY 64
/* A fixture's choice panel never grows past this; the panel is sized from
 * the row count and still has to fit the 480x270 canvas. */
#define DESK_CHOICE_MAX_ROWS 6

typedef enum desk_cast {
    DESK_CAST_LEGEND = 0,
    DESK_CAST_CHUMRUNNER = 1,
    DESK_CAST_FANTASY = 2,
    DESK_CAST_PLEB_BOUND = 3
} desk_cast;

typedef enum desk_actor {
    DESK_ACTOR_HERO = 0,
    DESK_ACTOR_ALLY_1 = 1,
    DESK_ACTOR_ALLY_2 = 2,
    DESK_ACTOR_ALLY_3 = 3
} desk_actor;

typedef enum desk_facing {
    DESK_FACING_DOWN = 0,
    DESK_FACING_LEFT = 1,
    DESK_FACING_RIGHT = 2,
    DESK_FACING_UP = 3
} desk_facing;

typedef enum desk_mode {
    DESK_MODE_WIZARD = 0,
    DESK_MODE_ROOM = 1,
    DESK_MODE_DIALOGUE = 2,
    DESK_MODE_PAUSE = 3,
    DESK_MODE_CONFIRM = 4,
    DESK_MODE_STATUS = 5,
    DESK_MODE_INVENTORY = 6,
    DESK_MODE_LAPTOP = 7,
    /* A fixture that offers several intents (the bed's power options, the
     * shed's maintenance jobs, the phone's two directions). One object =
     * one intent stays the rule; a choice panel is how an object states
     * which intents it owns. */
    DESK_MODE_CHOICE = 8
} desk_mode;
#define DESK_MODE_LAST DESK_MODE_CHOICE

/* The laptop menu lists at most this many profiles plus its two fixed
 * rows (PICK UP LAPTOP, CLOSE), so the tallest menu still fits the
 * 480x270 logical canvas. */
#define DESK_LAPTOP_MENU_PROFILES 8

/* The set-up laptop's lid. Frame 0 is the closed art, the last frame is
 * the open sprite, and the frame between is a procedural tween (the open
 * sprite rising) — compiled frame timing in the item-clip idiom, never
 * authored callbacks. The lid tracks the run registry: open while any
 * profile session is live, closed otherwise. */
#define DESK_LAPTOP_LID_FRAMES 3
#define DESK_LAPTOP_LID_TICKS 6

typedef enum desk_wizard_step {
    DESK_WIZARD_CAST = 0,
    DESK_WIZARD_ACTOR = 1,
    DESK_WIZARD_NAME = 2,
    DESK_WIZARD_OUTFIT = 3,
    DESK_WIZARD_CONFIRM = 4
} desk_wizard_step;

typedef enum desk_confirm {
    DESK_CONFIRM_NONE = 0,
    DESK_CONFIRM_QUIT = 1,
    DESK_CONFIRM_CAST_CHANGE = 2,
    /* Session and machine power, each behind the same YES/NO panel the
     * bed already used for turning in. The argv lives in launcher.c and
     * mirrors kilix-tui-utils' privileged.py, the fleet's one list. */
    DESK_CONFIRM_LOGOUT = 3,
    DESK_CONFIRM_REBOOT = 4,
    DESK_CONFIRM_POWEROFF = 5
} desk_confirm;

/* Which fixture's choice panel is open. The menus themselves are compiled
 * tables in desk.c keyed by the activated object's target, so world data
 * gains no new vocabulary and per-object bindings keep applying to each
 * fixture's default row. */
typedef enum desk_choice_menu {
    DESK_CHOICE_NONE = 0,
    DESK_CHOICE_BED = 1,
    DESK_CHOICE_SHED = 2,
    DESK_CHOICE_PHONE = 3,
    DESK_CHOICE_DEV_RIG = 4,
    DESK_CHOICE_BOOKSHELF = 5,
    DESK_CHOICE_MONITOR = 6,
    DESK_CHOICE_COMPUTER = 7,
    DESK_CHOICE_NOTICE_BOARD = 8
} desk_choice_menu;

typedef enum desk_graphic {
    DESK_GRAPHIC_LEGEND_PLAYER = 0,
    DESK_GRAPHIC_LEGEND_NPCS = 1,
    DESK_GRAPHIC_LEGEND_PORTRAITS = 2,
    DESK_GRAPHIC_CHUM_CHARACTERS = 3,
    DESK_GRAPHIC_CHUM_PORTRAITS = 4,
    DESK_GRAPHIC_FANTASY_CHARACTERS = 5,
    DESK_GRAPHIC_FANTASY_PORTRAITS = 6,
    DESK_GRAPHIC_PLEB_CHARACTERS = 7,
    DESK_GRAPHIC_PLEB_PORTRAITS = 8,
    /* Project-owned item art: one row per house style, one column per
     * catalog sprite, column 0 = missing-item icon. Owned by
     * tools/sync_item_art.py + validate_items.py, not cast parity. */
    DESK_GRAPHIC_ITEMS = 9,
    /* Held-item action frames: per style, three clip rows (use-tool,
     * drink, give) of four frames, composed from desktop-items by
     * tools/sync_action_art.py. */
    DESK_GRAPHIC_ITEM_ACTIONS = 10
} desk_graphic;

typedef enum desk_hero_motion_variant {
    DESK_HERO_MOTION_DOWN_A = 0,
    DESK_HERO_MOTION_DOWN_B = 1,
    DESK_HERO_MOTION_UP_STEP = 2,
    DESK_HERO_MOTION_MIRRORED_SIDE = 3,
    DESK_HERO_MOTION_MIRRORED_WALK = 4
} desk_hero_motion_variant;

typedef enum desk_audio_event {
    DESK_AUDIO_UI_MOVE = 0,
    DESK_AUDIO_UI_CONFIRM = 1,
    DESK_AUDIO_DIALOGUE = 2
} desk_audio_event;

/* Launch targets. world.json objects reference these by name via
 * desk_target_from_string(); the argv for every external target lives only in
 * launcher.c. Internal targets are handled inside desk.c and never spawn. */
typedef enum desk_target {
    DESK_TARGET_NONE = 0,
    /* external (serviced by launcher.c) */
    DESK_TARGET_TERMINAL = 1,
    DESK_TARGET_CODING_AGENTS = 2,
    DESK_TARGET_FILES = 3,
    DESK_TARGET_MANUALS = 4,
    DESK_TARGET_MODELS = 5,
    DESK_TARGET_GAMES = 6,
    DESK_TARGET_MUSIC = 7,
    DESK_TARGET_VOICE = 8,
    DESK_TARGET_TRASH = 9,
    DESK_TARGET_MAILBOX = 10,
    DESK_TARGET_MAINTENANCE = 11,
    /* internal (handled by desk.c) */
    DESK_TARGET_WARDROBE = 12,
    DESK_TARGET_BED = 13,
    DESK_TARGET_STATUS_BOARD = 14,
    DESK_TARGET_GATE_LOCKED = 15,
    /* external, debug menu only (never placed in world.json) */
    DESK_TARGET_WALK_EDITOR = 16,
    /* internal (handled by desk.c) */
    DESK_TARGET_KETTLE = 17,
    /* external (serviced by launcher.c). Appended rather than filed with the
     * other external targets because these values are compiled into saved
     * state; renumbering them would repoint existing worlds. */
    DESK_TARGET_BROWSER = 18,
    /* Parity fixtures (0.1.8). Same append-only rule: new rows go at the
     * end, existing numbers never move. */
    DESK_TARGET_SETTINGS = 19,
    DESK_TARGET_UPDATE = 20,
    DESK_TARGET_CATALOG = 21,
    DESK_TARGET_DICTATION = 22,
    DESK_TARGET_VOICE_HELP = 23,
    DESK_TARGET_PTY = 24,
    DESK_TARGET_TMUX = 25,
    DESK_TARGET_MUX = 26,
    DESK_TARGET_TEMPS = 27,
    DESK_TARGET_PASSWORD = 28,
    DESK_TARGET_MANUAL = 29,
    DESK_TARGET_RECOVERY = 30,
    DESK_TARGET_WEB = 31,
    /* Power actions run detached rather than in a tab: the machine is on
     * its way down, and a failure has to survive the tab it would close. */
    DESK_TARGET_POWER_LOGOUT = 32,
    DESK_TARGET_POWER_REBOOT = 33,
    DESK_TARGET_POWER_POWEROFF = 34
} desk_target;
#define DESK_TARGET_COUNT 35

typedef struct desk_rect {
    float x;
    float y;
    float w;
    float h;
} desk_rect;

typedef struct desk_object {
    char id[DESK_ID_CAPACITY];
    char prompt[DESK_PROMPT_CAPACITY];
    desk_rect rect;
    desk_target target;
    /* Optional item-receiver rule id from items.json; "" = plain fixture.
     * Resolved against the catalog by desk_world_validate_items. */
    char receiver[DESK_ID_CAPACITY];
} desk_object;

/* An authored world item spawn: stable content identity separate from the
 * item id. Each spawn materializes into a world item exactly once (its id
 * is then recorded in world.state's claimed table), so later releases can
 * add spawns without resurrecting old pickups. */
typedef struct desk_item_spawn {
    char id[DESK_ID_CAPACITY];
    char item[DESK_ITEM_ID_CAPACITY];
    int quantity;
    float x;
    float y;
} desk_item_spawn;

typedef struct desk_door {
    desk_rect rect;
    char to_id[DESK_ID_CAPACITY];
    int to_room; /* resolved by desk_world_validate */
    float spawn_x;
    float spawn_y;
} desk_door;

typedef struct desk_npc {
    int actor; /* 1..3: ally index of the active cast */
    float x;
    float y;
} desk_npc;

/* Walk-behind region (the AGS model): a per-style plate-sized mask image
 * rooms/<style>/<plate>-behind.png marks each plate pixel with a region id
 * (0 = none, 1..15 = region). The ids and their baselines are shared across
 * styles and declared here. An entity whose feet y is strictly less than a
 * region's baseline is behind it: after that entity draws, the plate pixels
 * inside its bbox whose mask value maps to such a region re-blit over the
 * sprite (per-pixel silhouette). Rooms rendering the procedural fallback
 * (no plate) skip walk-behinds entirely. */
typedef struct desk_walkbehind {
    int id;         /* 1..15, unique within the room */
    float baseline; /* logical y where the furniture meets the floor */
} desk_walkbehind;

typedef struct desk_room {
    char id[DESK_ID_CAPACITY];
    char name[DESK_LABEL_CAPACITY];
    char plate[DESK_LABEL_CAPACITY]; /* basename under rooms/<style>/ */
    bool outdoor;
    desk_rect walk;
    desk_rect obstacles[DESK_MAX_OBSTACLES_PER_ROOM];
    int obstacle_count;
    desk_door doors[DESK_MAX_DOORS_PER_ROOM];
    int door_count;
    desk_object objects[DESK_MAX_OBJECTS_PER_ROOM];
    int object_count;
    desk_npc npcs[DESK_MAX_NPCS_PER_ROOM];
    int npc_count;
    desk_walkbehind walkbehinds[DESK_MAX_WALKBEHINDS_PER_ROOM];
    int walkbehind_count;
    desk_item_spawn spawns[DESK_MAX_ITEM_SPAWNS_PER_ROOM];
    int spawn_count;
} desk_room;

typedef struct desk_world {
    desk_room rooms[DESK_MAX_ROOMS];
    int room_count;
    int start_room; /* first-login spawn room (the bedroom) */
} desk_world;

typedef struct desk_profile {
    uint32_t schema;
    desk_cast cast;
    desk_actor actor;
    uint32_t style; /* == cast in v1; kept separate for later mix-and-match */
    uint32_t outfit;
    uint32_t talked_mask;
    char name[DESK_NAME_CAPACITY];
    char last_room[DESK_ID_CAPACITY];
    float last_x;
    float last_y;
    bool first_run_done;
} desk_profile;

/* Local animation clips. Item data may select a logical clip id; frame
 * timing and typed events are compiled, never authored callbacks. */
typedef enum desk_clip_id {
    DESK_CLIP_IDLE = 0,
    DESK_CLIP_WALK = 1,
    DESK_CLIP_USE_TOOL = 2,
    DESK_CLIP_DRINK = 3,
    DESK_CLIP_GIVE = 4
} desk_clip_id;
#define DESK_CLIP_COUNT 5

/* The laptop screen. Opening a set-up laptop hands the whole canvas to
 * these pages — the machine is the screen, not a panel floating over the
 * room — and closing the lid gives the house back. HOME keeps the fast
 * path (Enter on a profile opens that session); everything else lives
 * behind CONFIGURE PROFILES so authoring never sits in front of use. */
typedef enum desk_laptop_page {
    DESK_LAPTOP_PAGE_HOME = 0,
    DESK_LAPTOP_PAGE_PROFILES = 1, /* configure: pick a profile or add one */
    DESK_LAPTOP_PAGE_EDIT = 2,     /* one profile's fields */
    DESK_LAPTOP_PAGE_PANES = 3,    /* its pane list */
    DESK_LAPTOP_PAGE_PANE = 4,     /* one pane's fields */
    DESK_LAPTOP_PAGE_PROVIDER = 5, /* which desktop a desktop profile opens */
    DESK_LAPTOP_PAGE_DELETE = 6    /* delete confirmation */
} desk_laptop_page;
#define DESK_LAPTOP_PAGE_LAST DESK_LAPTOP_PAGE_DELETE
/* Rows on the widest laptop page (EDIT: name, kind, layout/provider,
 * panes, save, delete, back). */
#define DESK_LAPTOP_ROWS_MAX (DESK_LAPTOP_MENU_PROFILES + 4)

typedef struct desk_laptop_ui {
    desk_laptop_page page;
    int cursor;      /* row cursor on the open page */
    int home_cursor; /* remembered across a trip through the pages */
    int pane_index;  /* pane the PANE page edits */
    /* Field editing is in-place: the row keeps its position and grows a
     * caret. Committing writes the field, never the file. */
    bool editing;
    int edit_row;
    int edit_length;
    char edit_buffer[DESK_LAPTOP_VALUE_CAPACITY];
    desk_laptop_profile working; /* the profile under edit */
    bool working_loaded;
    bool working_dirty;  /* edited since the last save */
    bool creating;       /* a new profile with no file behind it yet */
    char message[DESK_LAPTOP_ERROR_CAPACITY];
} desk_laptop_ui;

typedef struct desk_animator {
    desk_clip_id clip;
    int frame;
    int frame_ticks;
    bool movement_locked;
} desk_animator;

typedef enum desk_action_kind {
    DESK_ACTION_NONE = 0,
    DESK_ACTION_USE_TOOL = 1,
    DESK_ACTION_DRINK = 2,
    DESK_ACTION_GIVE = 3
} desk_action_kind;

/* A pure plan produced by the use-item query and committed exactly once
 * at the clip's authored APPLY frame. Generations let a stale commit
 * refuse instead of mutating changed state. */
typedef struct desk_action_plan {
    uint32_t nonce;
    uint16_t kind; /* desk_action_kind */
    uint16_t room;
    uint16_t inventory_slot;
    uint16_t inventory_generation;
    uint16_t object_index; /* fixture target, or UINT16_MAX */
    uint16_t npc_index;    /* gift target, or UINT16_MAX */
    desk_target launch;    /* fixture's compiled target (USE_TOOL) */
    char object_id[DESK_ID_CAPACITY];
} desk_action_plan;

typedef struct desk_action_state {
    bool active;
    bool committed;
    desk_action_plan plan;
} desk_action_state;

typedef struct desk_state {
    desk_profile profile;
    int room;
    float player_x;
    float player_y;
    desk_facing facing;
    bool player_moving;
    uint64_t simulation_tick;
    desk_mode mode;
    desk_wizard_step wizard_step;
    int wizard_cast_cursor;
    int wizard_actor_cursor;
    int wizard_outfit_cursor;
    int wizard_confirm_cursor;
    int wizard_name_len;
    char wizard_name[DESK_NAME_CAPACITY];
    bool wizard_editing_existing;
    bool outfit_dirty; /* graphics must re-run desk_graphics_set_outfit */
    int nearest_object;     /* -1 = none */
    int nearest_npc;        /* -1 = none */
    int nearest_world_item; /* -1 = none; index into items.items */
    int conversation_npc;
    int dialogue_beat;
    int dialogue_age;
    uint32_t talked_mask;
    int door_cooldown_ticks;
    desk_confirm confirm;
    int confirm_cursor;
    int pause_cursor;
    int inventory_cursor;
    int inventory_mark; /* -1 = none */
    /* The set-up laptop's session menu (DESK_MODE_LAPTOP): profile ids
     * scanned when the menu opened, plus PICK UP LAPTOP and CLOSE rows. */
    int laptop_menu_count; /* profiles listed, 0..DESK_LAPTOP_MENU_PROFILES */
    int laptop_menu_item; /* world-item index the open menu belongs to */
    char laptop_menu_ids[DESK_LAPTOP_MENU_PROFILES]
                        [DESK_LAPTOP_ID_CAPACITY];
    /* Which listed profiles have a live session (the run registry),
     * refreshed once a second while the screen is open so Enter can
     * close a running session instead of opening a duplicate. */
    bool laptop_menu_running[DESK_LAPTOP_MENU_PROFILES];
    bool laptop_pending; /* take-and-clear, like pending_launch */
    char pending_laptop[DESK_LAPTOP_ID_CAPACITY];
    /* Close-a-session request, the same take-and-clear shape. */
    bool laptop_close_pending;
    char pending_laptop_close[DESK_LAPTOP_ID_CAPACITY];
    /* The laptop's power state and lid: laptop_on mirrors the run
     * registry (any live session) once a second; the lid steps toward
     * open/closed one frame per DESK_LAPTOP_LID_TICKS. */
    bool laptop_on;
    int laptop_lid_frame; /* 0 closed .. DESK_LAPTOP_LID_FRAMES-1 open */
    int laptop_lid_ticks;
    desk_laptop_ui laptop;
    /* The open fixture choice panel, its visible rows (conditional rows
     * are filtered out when the menu opens) and its cursor. */
    desk_choice_menu choice;
    int choice_cursor;
    int choice_count;
    uint8_t choice_rows[DESK_CHOICE_MAX_ROWS];
    char choice_object[DESK_ID_CAPACITY]; /* object the panel belongs to */
    /* Whether the login password is still the shipped default, resolved
     * once at start-up by launcher.c (the only place allowed to run a
     * program) and read here to decide whether the notice board grows
     * its "change the locks" note. */
    bool default_password;
    bool debug_menu;  /* desktop.conf debug_menu flag, read at pause open */
    bool pause_debug; /* inside the Debug submenu */
    char status_lines[DESK_STATUS_LINE_COUNT][DESK_STATUS_LINE_CAPACITY];
    int status_line_count;
    char toast[DESK_TOAST_CAPACITY];
    int toast_ticks;
    desk_target pending_launch; /* take-and-clear */
    char pending_launch_object[DESK_ID_CAPACITY]; /* object behind it */
    desk_audio_event pending_audio[4];
    int pending_audio_count;
    bool quit_requested;
    bool profile_dirty; /* main persists via desk_profile_save */
    /* Item world: the immutable catalog (owned by main, set at init) and
     * the durable world record. Every committed item mutation sets
     * world_dirty; main persists via desk_world_state_save with the same
     * retry backoff as the profile. */
    const desk_item_catalog *catalog;
    desk_world_state items;
    bool world_dirty;
    desk_animator player_animator;
    desk_action_state action;
    uint32_t action_nonce;
} desk_state;

/* Opaque-pixel measurements of one sprite cell, captured when the cell is
 * loaded or rebuilt so drawing never rescans pixels. spine_x is the mean x
 * of the opaque pixels in the top 2/5 of the bounds (head/torso), the
 * anchor render.c centers on; bottom is the foot row. */
typedef struct desk_sprite_metrics {
    bool valid; /* cell has at least one opaque pixel */
    int left;
    int right;
    int top;
    int bottom;
    double spine_x;
} desk_sprite_metrics;

typedef struct desk_graphics {
    kilix_asset_cache cache;
    kilix_asset_manifest manifest;
    const kilix_asset_image *images[DESK_GRAPHICS_COUNT];
    kilix_asset_atlas atlases[DESK_GRAPHICS_COUNT];
    /* room plates for the active style; entries parallel desk_world rooms */
    uint8_t *plate_pixels[DESK_MAX_ROOMS];
    ki_td_rgba8 plates[DESK_MAX_ROOMS];
    bool plate_loaded[DESK_MAX_ROOMS];
    /* plate-sized walk-behind masks (one byte per plate pixel, 0 = none,
     * 1..15 = region id); NULL when the room+style has no mask */
    uint8_t *behind_masks[DESK_MAX_ROOMS];
    /* outfit-recolored hero cells for the active (cast, outfit) */
    desk_cast outfit_cast;
    int outfit_index;
    uint8_t *outfit_pixels;
    ki_td_rgba8 outfit_cells[16 * 8]; /* covers every imported hero sheet */
    desk_sprite_metrics outfit_metrics[16 * 8];
    int outfit_columns;
    int outfit_rows;
    bool outfit_ready;
    /* synthesized motion cells (built from the recolored hero cells) */
    uint8_t *legend_opposite_step_pixels;
    ki_td_rgba8 legend_opposite_step_cells[4];
    desk_sprite_metrics legend_opposite_step_metrics[4];
    uint8_t *hero_motion_pixels;
    ki_td_rgba8 hero_motion_cells[DESK_CAST_COUNT - 1]
                                 [DESK_HERO_MOTION_VARIANT_COUNT];
    desk_sprite_metrics hero_motion_metrics[DESK_CAST_COUNT - 1]
                                           [DESK_HERO_MOTION_VARIANT_COUNT];
    desk_sprite_metrics item_metrics[DESK_CAST_COUNT]
                                    [DESK_ITEM_SPRITE_COLUMNS];
    /* Optional closed-lid art for the laptop item: items/laptop-lid.png,
     * one 32x32 cell per style row, generated by tools/laptop_lid_art.py
     * and loaded like a room plate — present-and-valid or absent, never
     * fatal. Absent (or a transparent style cell) degrades to drawing
     * the open sprite, so review-pending art costs nothing. */
    uint8_t *laptop_lid_pixels;
    ki_td_rgba8 laptop_lid_cells[DESK_CAST_COUNT];
    desk_sprite_metrics laptop_lid_metrics[DESK_CAST_COUNT];
    bool laptop_lid_ready;
    bool cache_ready;
} desk_graphics;

typedef struct desk_audio {
    kilix_game_audio runtime;
} desk_audio;

typedef struct desk_launcher {
    bool external_enabled;
    char status[DESK_TOAST_CAPACITY];
} desk_launcher;

/* rooms.c — world manifest */
bool desk_world_load(desk_world *world, const char *path, char *error,
                     size_t error_size);
bool desk_world_validate(const desk_world *world, char *error,
                         size_t error_size);
/* Cross-file resolution: every spawn's item id and every object's
 * receiver rule id must exist in the loaded catalog, and spawn
 * quantities must fit the definition's stack. Run after both files
 * validated on their own. */
bool desk_world_validate_items(const desk_world *world,
                               const desk_item_catalog *catalog,
                               char *error, size_t error_size);
int desk_world_room_index(const desk_world *world, const char *id);

/* desk.c — simulation, wizard, profile */
void desk_init(desk_state *state, const desk_world *world,
               const desk_item_catalog *catalog);
void desk_update(desk_state *state, const desk_world *world, int move_x,
                 int move_y, float seconds);
bool desk_interact(desk_state *state, const desk_world *world);
/* The Space intent: act with the selected item (tool, consumable, gift,
 * placement). Falls back to desk_interact while the slot is empty. */
bool desk_use_item(desk_state *state, const desk_world *world);
void desk_cancel(desk_state *state, const desk_world *world);
/* The full inventory panel toggles only from room mode and never while an
 * action owns the player. Hotbar selection also remains live in the panel;
 * the explicit drop action remains room-only. */
bool desk_toggle_inventory(desk_state *state, const desk_world *world);
void desk_select_slot(desk_state *state, int slot);
void desk_cycle_slot(desk_state *state, int delta);
bool desk_drop_selected(desk_state *state, const desk_world *world);
/* Render-side reads: the receiver state a fixture currently owns (NULL
 * when none) and the placement ghost for the selected placeable item.
 * Both are pure queries. */
const desk_receiver_state *desk_receiver_lookup(const desk_state *state,
                                                const desk_room *room,
                                                const desk_object *object);
bool desk_placement_preview(const desk_state *state,
                            const desk_world *world, float *x, float *y,
                            bool *valid);
bool desk_text_input(desk_state *state, uint32_t codepoint);
bool desk_text_backspace(desk_state *state);
desk_target desk_take_launch_request(desk_state *state);
/* The set-up laptop's session request: true copies the chosen profile id
 * and clears the request, exactly one take per choice. */
bool desk_take_laptop_request(desk_state *state,
                              char id[DESK_LAPTOP_ID_CAPACITY]);
/* Close-a-session request (Enter on a running profile), the same
 * take-and-clear contract. launcher.c services it: `kilix laptop close`
 * when the host verb exists, the run registry's SIGTERM otherwise. */
bool desk_take_laptop_close_request(desk_state *state,
                                    char id[DESK_LAPTOP_ID_CAPACITY]);
int desk_take_audio_events(desk_state *state,
                           desk_audio_event events[4]);
bool desk_validate(const desk_state *state, const desk_world *world,
                   char *error, size_t error_size);

const char *desk_cast_name(desk_cast cast);
const char *desk_cast_subtitle(desk_cast cast);
const char *desk_cast_house_name(desk_cast cast);
const char *desk_actor_name(desk_cast cast, int actor);
uint32_t desk_actor_color(desk_cast cast, int actor);
const char *desk_dialogue_text(const desk_state *state);
int desk_dialogue_speaker(const desk_state *state);
int desk_dialogue_count(const desk_state *state);
size_t desk_dialogue_visible_chars(const desk_state *state);
const char *desk_interact_prompt(const desk_state *state,
                                 const desk_world *world);
/* Anchor point of the entity behind the current prompt, so the render
 * can tag the target instead of covering the screen. False = no target. */
bool desk_interact_anchor(const desk_state *state, const desk_world *world,
                          float *x, float *y);
int desk_interact_npc(const desk_state *state, const desk_world *world);
int desk_pause_item_count(const desk_state *state);
const char *desk_pause_item(const desk_state *state, int index);
int desk_laptop_menu_count(const desk_state *state);
const char *desk_laptop_menu_item(const desk_state *state, int index);
/* Render-side reads for the laptop screen: the page's title, its rows,
 * the value column beside a row ("" when the row has none), the footer
 * hint, and the last message the pages produced. */
const char *desk_laptop_page_title(const desk_state *state);
const char *desk_laptop_row_value(const desk_state *state, int index);
const char *desk_laptop_hint(const desk_state *state);
const char *desk_laptop_message(const desk_state *state);
/* True while a laptop row is being typed into, so the host routes text
 * and backspaces here the way it does for the wizard's name step. */
bool desk_text_entry_active(const desk_state *state);
/* Fixture choice panels. */
int desk_choice_count(const desk_state *state);
const char *desk_choice_item(const desk_state *state, int index);
const char *desk_choice_title(const desk_state *state);

bool desk_profile_load(desk_profile *profile);
bool desk_profile_save(const desk_profile *profile);
bool desk_profile_reset(void); /* --profile-test helper */

/* launcher.c — target registry and request servicing */
void desk_launcher_init(desk_launcher *launcher);
/* True only when the login password can be confirmed to be the shipped
 * default. Runs the Plebian-OS helper once; every uncertainty answers
 * false. launcher.c owns it because desk.c never runs a program. */
bool desk_launcher_password_is_default(void);
bool desk_launcher_service(desk_launcher *launcher, desk_state *state,
                           const desk_world *world);
desk_target desk_target_from_string(const char *name);
const char *desk_target_name(desk_target target);
const char *desk_target_label(desk_target target);
bool desk_target_is_external(desk_target target);

/* graphics.c — atlases, plates, outfit recolor, motion synthesis */
bool desk_graphics_init(desk_graphics *graphics, const char *asset_root);
void desk_graphics_shutdown(desk_graphics *graphics);
size_t desk_graphics_loaded_count(const desk_graphics *graphics);
bool desk_graphics_image(const desk_graphics *graphics, desk_graphic graphic,
                         ki_td_rgba8 *image);
bool desk_graphics_cell(const desk_graphics *graphics, desk_graphic graphic,
                        int column, int row, ki_td_rgba8 *image);
bool desk_graphics_load_plates(desk_graphics *graphics, const char *asset_root,
                               const desk_world *world, desk_cast style);
bool desk_graphics_plate(const desk_graphics *graphics, int room,
                         ki_td_rgba8 *image);
const uint8_t *desk_graphics_behind_mask(const desk_graphics *graphics,
                                         int room);
bool desk_graphics_set_outfit(desk_graphics *graphics, desk_cast cast,
                              int outfit);
bool desk_graphics_hero_cell(const desk_graphics *graphics, desk_cast cast,
                             int column, int row, ki_td_rgba8 *image);
bool desk_graphics_hero_motion_cell(const desk_graphics *graphics,
                                    desk_cast cast,
                                    desk_hero_motion_variant variant,
                                    ki_td_rgba8 *image);
bool desk_graphics_legend_opposite_step(const desk_graphics *graphics,
                                        desk_facing facing,
                                        ki_td_rgba8 *image);
/* Cached measurements for the cells served by the getters above; each
 * returns false when the cell is unavailable or has no opaque pixels. */
bool desk_graphics_hero_cell_metrics(const desk_graphics *graphics,
                                     desk_cast cast, int column, int row,
                                     desk_sprite_metrics *metrics);
bool desk_graphics_hero_motion_metrics(const desk_graphics *graphics,
                                       desk_cast cast,
                                       desk_hero_motion_variant variant,
                                       desk_sprite_metrics *metrics);
bool desk_graphics_legend_opposite_step_metrics(
    const desk_graphics *graphics, desk_facing facing,
    desk_sprite_metrics *metrics);
/* One item-art path: the same style-row cell feeds hotbar icons, world
 * draws, held art, ghosts, and receiver bubbles. */
bool desk_graphics_item_cell(const desk_graphics *graphics,
                             desk_cast style, int column,
                             ki_td_rgba8 *image);
bool desk_graphics_item_cell_metrics(const desk_graphics *graphics,
                                     desk_cast style, int column,
                                     desk_sprite_metrics *metrics);
/* The laptop's closed-lid cell for a style; false when the optional
 * sheet is absent or that style's cell is transparent — callers fall
 * back to the open sprite. */
bool desk_graphics_laptop_lid_cell(const desk_graphics *graphics,
                                   desk_cast style, ki_td_rgba8 *image);
bool desk_graphics_laptop_lid_metrics(const desk_graphics *graphics,
                                      desk_cast style,
                                      desk_sprite_metrics *metrics);
/* One frame of a held-item action clip; clip_row is 0 use-tool,
 * 1 drink, 2 give, matching sync_action_art.py's row order. */
bool desk_graphics_action_cell(const desk_graphics *graphics,
                               desk_cast style, int clip_row, int frame,
                               ki_td_rgba8 *image);
uint32_t desk_outfit_color(desk_cast cast, int outfit);
const char *desk_outfit_name(desk_cast cast, int outfit);

/* render.c */
bool desk_render(ki_td_soft_renderer *renderer, const desk_state *state,
                 const desk_world *world, const desk_graphics *graphics);

/* audio.c */
bool desk_audio_init(desk_audio *audio, const char *asset_root, bool live);
void desk_audio_play(desk_audio *audio, desk_cast cast,
                     desk_audio_event event);
void desk_audio_update(desk_audio *audio, float seconds);
void desk_audio_shutdown(desk_audio *audio);
bool desk_audio_assets_selftest(const char *asset_root, size_t *loaded_cues);

#endif
