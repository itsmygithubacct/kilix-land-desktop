#ifndef KILIX_LAND_DESKTOP_WORLD_STATE_H
#define KILIX_LAND_DESKTOP_WORLD_STATE_H

#include "items.h"

#include <stdbool.h>
#include <stdint.h>

/* Durable world record: inventory, equipment, claimed authored spawns,
 * loose/placed world items, fixture receiver contents, temporary effects,
 * and social state. It lives in world.state beside profile.state (same
 * kilix-state protection, separate record) so corrupt inventory bytes can
 * never send the player back through the character wizard.
 *
 * The codec writes stable string ids only — item ids, room ids, fixture
 * ids, spawn ids — never runtime indexes, so catalog and manifest
 * reordering are harmless. An item id that no longer resolves loads as
 * the compiled core:missing-item definition with its original id kept in
 * the orphan table (index = the instance's variant field), and converts
 * back the next time the definition exists. */

#define DESK_WORLD_STATE_SCHEMA 1
#define DESK_WORLD_MAX_PAYLOAD 16384u
#define DESK_WORLD_ROOM_ID_CAPACITY 24
#define DESK_SPAWN_ID_CAPACITY 24
#define DESK_MAX_WORLD_ITEMS 64
#define DESK_MAX_ITEM_ORPHANS 16
#define DESK_MAX_CLAIMED_SPAWNS 64
#define DESK_MAX_RECEIVER_STATES 16
#define DESK_MAX_ACTIVE_EFFECTS 8
#define DESK_MAX_SOCIAL_RECORDS 16
#define DESK_EQUIPMENT_SLOTS 2
#define DESK_EQUIP_HANDS 0
#define DESK_EQUIP_ACCESSORY 1

/* A loose or placed item standing in a room. room is resolved against the
 * loaded world manifest by the simulation (-1 while unresolved); the
 * codec only ever sees room_id, so items in a room a later release
 * removed stay preserved and invisible instead of being dropped. */
typedef struct desk_world_item {
    desk_item item;
    float x;
    float y;
    bool placed; /* deliberately placed decoration vs loose pickup */
    int room;    /* runtime index into desk_world, -1 = unresolved */
    char room_id[DESK_WORLD_ROOM_ID_CAPACITY];
} desk_world_item;

typedef enum desk_receiver_phase {
    DESK_RECEIVER_EMPTY = 0,
    DESK_RECEIVER_PROCESSING = 1,
    DESK_RECEIVER_READY = 2
} desk_receiver_phase;

typedef struct desk_receiver_state {
    char room_id[DESK_WORLD_ROOM_ID_CAPACITY];
    char object_id[DESK_WORLD_ROOM_ID_CAPACITY];
    uint8_t phase; /* desk_receiver_phase */
    int32_t remaining_ticks;
    desk_item item; /* owned input/output; empty exactly when EMPTY */
} desk_receiver_state;

typedef struct desk_active_effect {
    uint16_t definition; /* resolved catalog index */
    int32_t remaining_ticks;
} desk_active_effect;

typedef struct desk_social_record {
    uint8_t cast;
    uint8_t actor; /* 1..3 */
    int32_t points;
    uint16_t gifts;
    uint16_t flags;
} desk_social_record;

typedef struct desk_world_state {
    desk_inventory inventory;
    desk_item equipment[DESK_EQUIPMENT_SLOTS];
    uint32_t next_serial; /* starts at 1; 0 means "no unique identity" */
    char orphans[DESK_MAX_ITEM_ORPHANS][DESK_ITEM_ID_CAPACITY];
    int orphan_count;
    char claimed[DESK_MAX_CLAIMED_SPAWNS][DESK_SPAWN_ID_CAPACITY];
    int claimed_count;
    desk_world_item items[DESK_MAX_WORLD_ITEMS];
    int item_count;
    desk_receiver_state receivers[DESK_MAX_RECEIVER_STATES];
    int receiver_count;
    desk_active_effect effects[DESK_MAX_ACTIVE_EFFECTS];
    int effect_count;
    desk_social_record social[DESK_MAX_SOCIAL_RECORDS];
    int social_count;
} desk_world_state;

void desk_world_state_init(desk_world_state *state);
/* Loads world.state. A missing record initializes an empty state and
 * succeeds; a corrupt or invalid record initializes an empty state,
 * sets *corrupt, and still succeeds (the caller shows one warning toast).
 * Returns false only when the store itself cannot open. */
bool desk_world_state_load(desk_world_state *state,
                           const desk_item_catalog *catalog, bool *corrupt);
bool desk_world_state_save(const desk_world_state *state,
                           const desk_item_catalog *catalog);
bool desk_world_state_reset(void);
uint32_t desk_world_state_take_serial(desk_world_state *state);
int desk_world_state_orphan_add(desk_world_state *state, const char *id);
bool desk_world_state_is_claimed(const desk_world_state *state,
                                 const char *spawn_id);
bool desk_world_state_claim(desk_world_state *state, const char *spawn_id);

#endif
