#ifndef KILIX_LAND_DESKTOP_ITEMS_H
#define KILIX_LAND_DESKTOP_ITEMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Immutable item catalog and mutable item instances.
 *
 * The catalog is loaded once from assets/world/items.json, validated
 * strictly (tools/validate_items.py mirrors every check), and never
 * mutated afterwards. Instances are small values owned by exactly one
 * container: an inventory slot, an equipment slot, a room's world-item
 * list, or a fixture receiver. All quantity changes go through the
 * plan/commit pairs below so a failed or stale operation leaves every
 * container byte-identical, and data can never select code: behaviors are
 * a compiled enum, never callback names, commands, or launch targets. */

#define DESK_MAX_ITEM_DEFS 64
#define DESK_ITEM_ID_CAPACITY 48
#define DESK_ITEM_NAME_CAPACITY 40
#define DESK_ITEM_DESCRIPTION_CAPACITY 96
#define DESK_INVENTORY_SLOTS 12
#define DESK_MAX_WORLD_ITEMS_PER_ROOM 16
#define DESK_MAX_RECEIVER_RULES 16
#define DESK_RECEIVER_ID_CAPACITY 24
#define DESK_ITEM_MAX_STACK_LIMIT 99
#define DESK_ITEM_SPRITE_COLUMNS 8
/* Ten hours of 60 Hz simulation; bounds every authored tick count. */
#define DESK_ITEM_MAX_TICKS 2160000

/* Sentinel for "no item / no definition" in packed indexes. */
#define DESK_ITEM_NONE UINT16_MAX
/* Catalog index 0 is always the compiled core:missing-item definition
 * saved instances resolve to when their definition disappeared; the
 * original id string lives in the world-state orphan table, indexed by
 * the instance's variant field. */
#define DESK_ITEM_DEF_MISSING 0u
#define DESK_ITEM_MISSING_ID "core:missing-item"

typedef uint64_t desk_item_tags;

typedef enum desk_item_family {
    DESK_ITEM_PORTABLE = 0,
    DESK_ITEM_CONSUMABLE = 1,
    DESK_ITEM_TOOL = 2,
    DESK_ITEM_WEARABLE = 3,
    DESK_ITEM_PLACEABLE = 4,
    DESK_ITEM_KEY = 5
} desk_item_family;
#define DESK_ITEM_FAMILY_COUNT 6

/* Compiled behavior ids. Data selects one by name; the implementation is
 * always in the binary. */
typedef enum desk_item_behavior {
    DESK_BEHAVIOR_HOLD = 0,
    DESK_BEHAVIOR_DRINK = 1,
    DESK_BEHAVIOR_USE_TOOL = 2,
    DESK_BEHAVIOR_EQUIP = 3,
    DESK_BEHAVIOR_PLACE = 4,
    DESK_BEHAVIOR_UNLOCK = 5
} desk_item_behavior;
#define DESK_ITEM_BEHAVIOR_COUNT 6

typedef struct desk_item_def {
    char id[DESK_ITEM_ID_CAPACITY];
    char name[DESK_ITEM_NAME_CAPACITY];
    char description[DESK_ITEM_DESCRIPTION_CAPACITY];
    desk_item_family family;
    uint16_t behavior; /* desk_item_behavior */
    uint16_t sprite;   /* desktop-items atlas column; 0 = missing icon */
    uint16_t max_stack;
    desk_item_tags tags;
    int32_t parameter_a; /* behavior-specific (drink: effect ticks) */
    int32_t parameter_b; /* reserved for later behaviors */
} desk_item_def;

/* One stack or unique item. No owning pointers: safe to move by value.
 * Empty has exactly one canonical form: all fields zero with
 * definition == DESK_ITEM_NONE is never used; emptiness is quantity == 0
 * with every other field zero (desk_item_clear writes it). */
typedef struct desk_item {
    uint16_t definition; /* catalog index, resolved after validation */
    uint16_t quantity;   /* 1..max_stack when occupied */
    uint16_t variant;    /* recovery: orphan index; otherwise 0 for now */
    uint16_t durability; /* tools only; 0 for everything else */
    uint32_t serial;     /* nonzero only when unique identity matters */
} desk_item;

typedef enum desk_receiver_match {
    DESK_RECEIVER_MATCH_ANY_TAG = 0,
    DESK_RECEIVER_MATCH_ALL_TAGS = 1,
    DESK_RECEIVER_MATCH_ITEM = 2,
    DESK_RECEIVER_MATCH_FAMILY = 3
} desk_receiver_match;

typedef enum desk_receiver_result {
    DESK_RECEIVER_RESULT_ACTIVATE_FIXTURE = 0,
    DESK_RECEIVER_RESULT_NONE = 1
} desk_receiver_result;

/* A data-defined accept/output rule for one fixture receiver. The result
 * never names a target or command: activate-fixture invokes the owning
 * fixture's already-validated desk_target through the normal launch path. */
typedef struct desk_receiver_rule {
    char id[DESK_RECEIVER_ID_CAPACITY];
    desk_receiver_match match;
    desk_item_tags tags;     /* ANY_TAG / ALL_TAGS */
    uint16_t item;           /* MATCH_ITEM: resolved definition index */
    uint16_t output;         /* resolved definition index; NONE if absent */
    desk_item_family family; /* MATCH_FAMILY */
    bool consume;
    int32_t processing_ticks; /* 0 = instant */
    desk_receiver_result result;
} desk_receiver_rule;

typedef struct desk_item_catalog {
    desk_item_def definitions[DESK_MAX_ITEM_DEFS];
    int definition_count; /* includes definitions[0] = missing item */
    desk_receiver_rule receivers[DESK_MAX_RECEIVER_RULES];
    int receiver_count;
} desk_item_catalog;

typedef struct desk_inventory {
    desk_item slots[DESK_INVENTORY_SLOTS];
    /* Bumped on every write to the slot; plans record the generations
     * they saw so a stale commit fails instead of mutating changed
     * state. */
    uint16_t generation[DESK_INVENTORY_SLOTS];
    int selected;
} desk_inventory;

typedef struct desk_item_plan_entry {
    uint8_t slot;
    uint16_t count;
    uint16_t generation; /* expected slot generation at commit */
    bool creates;        /* true: deposit into an empty slot */
} desk_item_plan_entry;

typedef enum desk_item_plan_kind {
    DESK_ITEM_PLAN_NONE = 0,
    DESK_ITEM_PLAN_ADD = 1,
    DESK_ITEM_PLAN_REMOVE = 2,
    DESK_ITEM_PLAN_SPLIT = 3
} desk_item_plan_kind;

/* A pure description of one inventory mutation. Building a plan never
 * mutates; committing verifies every recorded generation and the payload
 * before the first write, then applies the whole plan or nothing. */
typedef struct desk_item_plan {
    uint8_t kind; /* desk_item_plan_kind */
    uint8_t entry_count;
    uint16_t total; /* quantity moved across all entries */
    desk_item item; /* ADD: incoming payload; REMOVE/SPLIT: expected source */
    desk_item_plan_entry entries[DESK_INVENTORY_SLOTS];
} desk_item_plan;

/* catalog */
bool desk_items_load(desk_item_catalog *catalog, const char *path,
                     char *error, size_t error_size);
int desk_items_find(const desk_item_catalog *catalog, const char *id);
const desk_item_def *desk_items_def(const desk_item_catalog *catalog,
                                    uint16_t definition);
bool desk_item_id_valid(const char *id);
int desk_item_tag_index(const char *name); /* -1 = unknown */
const char *desk_item_tag_name(int index); /* NULL past the table */
int desk_items_find_receiver(const desk_item_catalog *catalog,
                             const char *id);
bool desk_receiver_accepts(const desk_item_catalog *catalog,
                           const desk_receiver_rule *rule,
                           const desk_item *item);

/* instances */
desk_item desk_item_make(uint16_t definition, uint16_t quantity);
bool desk_item_is_empty(const desk_item *item);
void desk_item_clear(desk_item *item);
bool desk_item_valid(const desk_item_catalog *catalog,
                     const desk_item *item);
bool desk_item_can_stack(const desk_item_catalog *catalog,
                         const desk_item *a, const desk_item *b);

/* inventory */
void desk_inventory_init(desk_inventory *inventory);
int desk_inventory_total(const desk_inventory *inventory,
                         uint16_t definition);
bool desk_inventory_plan_add(const desk_inventory *inventory,
                             const desk_item_catalog *catalog,
                             const desk_item *item, desk_item_plan *plan);
bool desk_inventory_commit_add(desk_inventory *inventory,
                               const desk_item_catalog *catalog,
                               const desk_item *item,
                               const desk_item_plan *plan);
bool desk_inventory_plan_remove(const desk_inventory *inventory, int slot,
                                uint16_t count, desk_item_plan *plan);
bool desk_inventory_commit_remove(desk_inventory *inventory,
                                  const desk_item_plan *plan,
                                  desk_item *removed);
bool desk_item_plan_split_one(const desk_inventory *inventory, int slot,
                              desk_item_plan *plan);
bool desk_item_commit_split_one(desk_inventory *inventory,
                                const desk_item_plan *plan, desk_item *one);

#endif
