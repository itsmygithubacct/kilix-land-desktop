/* rooms.c — world manifest load + validation.
 *
 * Parses the strict JSON subset used by assets/world/world.json (objects,
 * arrays, strings with the \" \\ \/ \n \t escapes, numbers with an optional
 * fraction, true/false) on top of the shared token reader in json_reader.c.
 * Unknown keys are schema errors. Parse errors are reported as
 * "world.json:<byte-offset>: <what>"; desk_world_validate mirrors
 * tools/validate_world.py check for check.
 */

#include "kilix_land_desktop.h"
#include "json_reader.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DESK_WORLD_FILE_CAPACITY (256u * 1024u)

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static bool vfail(char *error, size_t error_size, const char *format, ...)
{
    va_list args;

    if (error && error_size > 0u) {
        va_start(args, format);
        (void)vsnprintf(error, error_size, format, args);
        va_end(args);
    }
    return false;
}

static bool parse_rect_value(desk_json_reader *p, desk_rect *rect)
{
    static const char *const rect_keys[4] = {"x", "y", "w", "h"};
    bool first = true;
    unsigned seen = 0u;
    size_t start;
    size_t i;
    char key[32];

    desk_json_skip_ws(p);
    start = p->offset;
    if (!desk_json_expect(p, '{'))
        return false;
    for (;;) {
        float *slot = NULL;
        unsigned bit = 0u;
        int step = desk_json_next_key(p, &first, key, sizeof key);

        if (step < 0)
            return false;
        if (step == 0)
            break;
        if (strcmp(key, "x") == 0) {
            slot = &rect->x;
            bit = 1u << 0;
        } else if (strcmp(key, "y") == 0) {
            slot = &rect->y;
            bit = 1u << 1;
        } else if (strcmp(key, "w") == 0) {
            slot = &rect->w;
            bit = 1u << 2;
        } else if (strcmp(key, "h") == 0) {
            slot = &rect->h;
            bit = 1u << 3;
        } else {
            return desk_json_fail_at(p, p->key_offset,
                                     "unknown key '%s' in rect", key);
        }
        if (!desk_json_claim_key(p, &seen, bit, key) ||
            !desk_json_parse_number(p, slot))
            return false;
    }
    for (i = 0u; i < 4u; ++i)
        if ((seen & (1u << i)) == 0u)
            return desk_json_fail_at(p, start, "rect missing '%s'",
                                     rect_keys[i]);
    return true;
}

static bool parse_point(desk_json_reader *p, float *x, float *y)
{
    bool first = true;
    unsigned seen = 0u;
    size_t start;
    char key[32];

    desk_json_skip_ws(p);
    start = p->offset;
    if (!desk_json_expect(p, '{'))
        return false;
    for (;;) {
        int step = desk_json_next_key(p, &first, key, sizeof key);

        if (step < 0)
            return false;
        if (step == 0)
            break;
        if (strcmp(key, "x") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 0, key) ||
                !desk_json_parse_number(p, x))
                return false;
        } else if (strcmp(key, "y") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 1, key) ||
                !desk_json_parse_number(p, y))
                return false;
        } else {
            return desk_json_fail_at(p, p->key_offset,
                                     "unknown key '%s' in point", key);
        }
    }
    if ((seen & (1u << 0)) == 0u)
        return desk_json_fail_at(p, start, "point missing 'x'");
    if ((seen & (1u << 1)) == 0u)
        return desk_json_fail_at(p, start, "point missing 'y'");
    return true;
}

static bool parse_door(desk_json_reader *p, desk_door *door)
{
    bool first = true;
    unsigned seen = 0u;
    size_t start;
    char key[32];

    door->to_room = -1; /* resolved after every room id is known */
    desk_json_skip_ws(p);
    start = p->offset;
    if (!desk_json_expect(p, '{'))
        return false;
    for (;;) {
        int step = desk_json_next_key(p, &first, key, sizeof key);

        if (step < 0)
            return false;
        if (step == 0)
            break;
        if (strcmp(key, "rect") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 0, key) ||
                !parse_rect_value(p, &door->rect))
                return false;
        } else if (strcmp(key, "to") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 1, key) ||
                !desk_json_parse_string(p, door->to_id, sizeof door->to_id))
                return false;
        } else if (strcmp(key, "spawn") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 2, key) ||
                !parse_point(p, &door->spawn_x, &door->spawn_y))
                return false;
        } else {
            return desk_json_fail_at(p, p->key_offset,
                                     "unknown key '%s' in door", key);
        }
    }
    if ((seen & (1u << 0)) == 0u)
        return desk_json_fail_at(p, start, "door missing 'rect'");
    if ((seen & (1u << 1)) == 0u)
        return desk_json_fail_at(p, start, "door missing 'to'");
    if ((seen & (1u << 2)) == 0u)
        return desk_json_fail_at(p, start, "door missing 'spawn'");
    return true;
}

static bool parse_object_entry(desk_json_reader *p, desk_object *object)
{
    bool first = true;
    unsigned seen = 0u;
    size_t start;
    char key[32];

    desk_json_skip_ws(p);
    start = p->offset;
    if (!desk_json_expect(p, '{'))
        return false;
    for (;;) {
        int step = desk_json_next_key(p, &first, key, sizeof key);

        if (step < 0)
            return false;
        if (step == 0)
            break;
        if (strcmp(key, "id") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 0, key) ||
                !desk_json_parse_string(p, object->id, sizeof object->id))
                return false;
        } else if (strcmp(key, "prompt") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 1, key) ||
                !desk_json_parse_string(p, object->prompt,
                                        sizeof object->prompt))
                return false;
        } else if (strcmp(key, "rect") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 2, key) ||
                !parse_rect_value(p, &object->rect))
                return false;
        } else if (strcmp(key, "target") == 0) {
            char target_name[48];
            size_t value_offset;
            desk_target target;

            if (!desk_json_claim_key(p, &seen, 1u << 3, key))
                return false;
            desk_json_skip_ws(p);
            value_offset = p->offset;
            if (!desk_json_parse_string(p, target_name, sizeof target_name))
                return false;
            target = desk_target_from_string(target_name);
            if (target == DESK_TARGET_NONE)
                return desk_json_fail_at(p, value_offset,
                                         "unknown target '%s'", target_name);
            object->target = target;
        } else if (strcmp(key, "receiver") == 0) {
            size_t value_offset;

            if (!desk_json_claim_key(p, &seen, 1u << 4, key))
                return false;
            desk_json_skip_ws(p);
            value_offset = p->offset;
            if (!desk_json_parse_string(p, object->receiver,
                                        sizeof object->receiver))
                return false;
            if (object->receiver[0] == '\0')
                return desk_json_fail_at(p, value_offset,
                                         "empty receiver id");
        } else {
            return desk_json_fail_at(p, p->key_offset,
                                     "unknown key '%s' in object", key);
        }
    }
    if ((seen & (1u << 0)) == 0u)
        return desk_json_fail_at(p, start, "object missing 'id'");
    if ((seen & (1u << 1)) == 0u)
        return desk_json_fail_at(p, start, "object missing 'prompt'");
    if ((seen & (1u << 2)) == 0u)
        return desk_json_fail_at(p, start, "object missing 'rect'");
    if ((seen & (1u << 3)) == 0u)
        return desk_json_fail_at(p, start, "object missing 'target'");
    return true;
}

static bool parse_npc(desk_json_reader *p, desk_npc *npc)
{
    bool first = true;
    unsigned seen = 0u;
    size_t start;
    char key[32];

    desk_json_skip_ws(p);
    start = p->offset;
    if (!desk_json_expect(p, '{'))
        return false;
    for (;;) {
        int step = desk_json_next_key(p, &first, key, sizeof key);

        if (step < 0)
            return false;
        if (step == 0)
            break;
        if (strcmp(key, "actor") == 0) {
            float value = 0.0f;
            size_t value_offset;
            int actor;

            if (!desk_json_claim_key(p, &seen, 1u << 0, key))
                return false;
            desk_json_skip_ws(p);
            value_offset = p->offset;
            if (!desk_json_parse_number(p, &value))
                return false;
            if (value < -16777216.0f || value > 16777216.0f)
                return desk_json_fail_at(p, value_offset,
                                         "actor out of range");
            actor = (int)value;
            if ((float)actor != value)
                return desk_json_fail_at(p, value_offset,
                                         "actor must be an integer");
            npc->actor = actor;
        } else if (strcmp(key, "x") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 1, key) ||
                !desk_json_parse_number(p, &npc->x))
                return false;
        } else if (strcmp(key, "y") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 2, key) ||
                !desk_json_parse_number(p, &npc->y))
                return false;
        } else {
            return desk_json_fail_at(p, p->key_offset,
                                     "unknown key '%s' in npc", key);
        }
    }
    if ((seen & (1u << 0)) == 0u)
        return desk_json_fail_at(p, start, "npc missing 'actor'");
    if ((seen & (1u << 1)) == 0u)
        return desk_json_fail_at(p, start, "npc missing 'x'");
    if ((seen & (1u << 2)) == 0u)
        return desk_json_fail_at(p, start, "npc missing 'y'");
    return true;
}

static bool parse_walkbehind(desk_json_reader *p, desk_walkbehind *walkbehind)
{
    bool first = true;
    unsigned seen = 0u;
    size_t start;
    char key[32];

    desk_json_skip_ws(p);
    start = p->offset;
    if (!desk_json_expect(p, '{'))
        return false;
    for (;;) {
        int step = desk_json_next_key(p, &first, key, sizeof key);

        if (step < 0)
            return false;
        if (step == 0)
            break;
        if (strcmp(key, "id") == 0) {
            float value = 0.0f;
            size_t value_offset;
            int id;

            if (!desk_json_claim_key(p, &seen, 1u << 0, key))
                return false;
            desk_json_skip_ws(p);
            value_offset = p->offset;
            if (!desk_json_parse_number(p, &value))
                return false;
            if (value < -16777216.0f || value > 16777216.0f)
                return desk_json_fail_at(p, value_offset,
                                         "walkbehind id out of range");
            id = (int)value;
            if ((float)id != value)
                return desk_json_fail_at(p, value_offset,
                                         "walkbehind id must be an integer");
            walkbehind->id = id;
        } else if (strcmp(key, "baseline") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 1, key) ||
                !desk_json_parse_number(p, &walkbehind->baseline))
                return false;
        } else {
            return desk_json_fail_at(p, p->key_offset,
                                     "unknown key '%s' in walkbehind", key);
        }
    }
    if ((seen & (1u << 0)) == 0u)
        return desk_json_fail_at(p, start, "walkbehind missing 'id'");
    if ((seen & (1u << 1)) == 0u)
        return desk_json_fail_at(p, start, "walkbehind missing 'baseline'");
    return true;
}

static bool parse_item_spawn(desk_json_reader *p, desk_item_spawn *spawn)
{
    bool first = true;
    unsigned seen = 0u;
    size_t start;
    size_t id_offset = 0u;
    size_t item_offset = 0u;
    char key[32];

    memset(spawn, 0, sizeof *spawn);
    desk_json_skip_ws(p);
    start = p->offset;
    if (!desk_json_expect(p, '{'))
        return false;
    for (;;) {
        int step = desk_json_next_key(p, &first, key, sizeof key);

        if (step < 0)
            return false;
        if (step == 0)
            break;
        if (strcmp(key, "id") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 0, key))
                return false;
            desk_json_skip_ws(p);
            id_offset = p->offset;
            if (!desk_json_parse_string(p, spawn->id, sizeof spawn->id))
                return false;
        } else if (strcmp(key, "item") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 1, key))
                return false;
            desk_json_skip_ws(p);
            item_offset = p->offset;
            if (!desk_json_parse_string(p, spawn->item,
                                        sizeof spawn->item))
                return false;
        } else if (strcmp(key, "quantity") == 0) {
            float value = 0.0f;
            size_t value_offset;
            int quantity;

            if (!desk_json_claim_key(p, &seen, 1u << 2, key))
                return false;
            desk_json_skip_ws(p);
            value_offset = p->offset;
            if (!desk_json_parse_number(p, &value))
                return false;
            if (value < 1.0f || value > (float)DESK_ITEM_MAX_STACK_LIMIT)
                return desk_json_fail_at(p, value_offset,
                                         "quantity out of range");
            quantity = (int)value;
            if ((float)quantity != value)
                return desk_json_fail_at(p, value_offset,
                                         "quantity must be an integer");
            spawn->quantity = quantity;
        } else if (strcmp(key, "x") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 3, key) ||
                !desk_json_parse_number(p, &spawn->x))
                return false;
        } else if (strcmp(key, "y") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 4, key) ||
                !desk_json_parse_number(p, &spawn->y))
                return false;
        } else {
            return desk_json_fail_at(p, p->key_offset,
                                     "unknown key '%s' in item spawn", key);
        }
    }
    if ((seen & (1u << 0)) == 0u)
        return desk_json_fail_at(p, start, "item spawn missing 'id'");
    if ((seen & (1u << 1)) == 0u)
        return desk_json_fail_at(p, start, "item spawn missing 'item'");
    if ((seen & (1u << 2)) == 0u)
        return desk_json_fail_at(p, start, "item spawn missing 'quantity'");
    if ((seen & (1u << 3)) == 0u)
        return desk_json_fail_at(p, start, "item spawn missing 'x'");
    if ((seen & (1u << 4)) == 0u)
        return desk_json_fail_at(p, start, "item spawn missing 'y'");
    if (!desk_spawn_id_valid(spawn->id))
        return desk_json_fail_at(p, id_offset, "invalid spawn id '%s'",
                                 spawn->id);
    if (!desk_item_id_valid(spawn->item))
        return desk_json_fail_at(p, item_offset,
                                 "invalid item id '%s' in spawn",
                                 spawn->item);
    return true;
}

static bool parse_obstacle_element(desk_json_reader *p, desk_room *room,
                                   int index)
{
    return parse_rect_value(p, &room->obstacles[index]);
}

static bool parse_door_element(desk_json_reader *p, desk_room *room,
                               int index)
{
    return parse_door(p, &room->doors[index]);
}

static bool parse_object_element(desk_json_reader *p, desk_room *room,
                                 int index)
{
    return parse_object_entry(p, &room->objects[index]);
}

static bool parse_npc_element(desk_json_reader *p, desk_room *room, int index)
{
    return parse_npc(p, &room->npcs[index]);
}

static bool parse_walkbehind_element(desk_json_reader *p, desk_room *room,
                                     int index)
{
    return parse_walkbehind(p, &room->walkbehinds[index]);
}

static bool parse_item_spawn_element(desk_json_reader *p, desk_room *room,
                                     int index)
{
    return parse_item_spawn(p, &room->spawns[index]);
}

static bool parse_array(desk_json_reader *p, desk_room *room, int *count,
                        int capacity, const char *what,
                        bool (*parse_element)(desk_json_reader *,
                                              desk_room *, int))
{
    bool first = true;

    if (!desk_json_expect(p, '['))
        return false;
    for (;;) {
        int step = desk_json_next_element(p, &first);

        if (step < 0)
            return false;
        if (step == 0)
            return true;
        if (*count >= capacity)
            return desk_json_fail_at(p, p->offset, "more than %d %s",
                                     capacity, what);
        if (!parse_element(p, room, *count))
            return false;
        (*count)++;
    }
}

static bool parse_room(desk_json_reader *p, desk_room *room)
{
    static const struct {
        unsigned bit;
        const char *key;
    } required[] = {
        {1u << 0, "id"},
        {1u << 1, "name"},
        {1u << 2, "plate"},
        {1u << 4, "walk"},
    };
    bool first = true;
    unsigned seen = 0u;
    size_t start;
    size_t i;
    char key[32];

    desk_json_skip_ws(p);
    start = p->offset;
    if (!desk_json_expect(p, '{'))
        return false;
    for (;;) {
        int step = desk_json_next_key(p, &first, key, sizeof key);

        if (step < 0)
            return false;
        if (step == 0)
            break;
        if (strcmp(key, "id") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 0, key) ||
                !desk_json_parse_string(p, room->id, sizeof room->id))
                return false;
        } else if (strcmp(key, "name") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 1, key) ||
                !desk_json_parse_string(p, room->name, sizeof room->name))
                return false;
        } else if (strcmp(key, "plate") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 2, key) ||
                !desk_json_parse_string(p, room->plate, sizeof room->plate))
                return false;
        } else if (strcmp(key, "outdoor") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 3, key) ||
                !desk_json_parse_bool(p, &room->outdoor))
                return false;
        } else if (strcmp(key, "walk") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 4, key) ||
                !parse_rect_value(p, &room->walk))
                return false;
        } else if (strcmp(key, "obstacles") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 5, key) ||
                !parse_array(p, room, &room->obstacle_count,
                             DESK_MAX_OBSTACLES_PER_ROOM, "obstacles",
                             parse_obstacle_element))
                return false;
        } else if (strcmp(key, "doors") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 6, key) ||
                !parse_array(p, room, &room->door_count,
                             DESK_MAX_DOORS_PER_ROOM, "doors",
                             parse_door_element))
                return false;
        } else if (strcmp(key, "objects") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 7, key) ||
                !parse_array(p, room, &room->object_count,
                             DESK_MAX_OBJECTS_PER_ROOM, "objects",
                             parse_object_element))
                return false;
        } else if (strcmp(key, "npcs") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 8, key) ||
                !parse_array(p, room, &room->npc_count,
                             DESK_MAX_NPCS_PER_ROOM, "npcs",
                             parse_npc_element))
                return false;
        } else if (strcmp(key, "walkbehinds") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 9, key) ||
                !parse_array(p, room, &room->walkbehind_count,
                             DESK_MAX_WALKBEHINDS_PER_ROOM, "walkbehinds",
                             parse_walkbehind_element))
                return false;
        } else if (strcmp(key, "item_spawns") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 10, key) ||
                !parse_array(p, room, &room->spawn_count,
                             DESK_MAX_ITEM_SPAWNS_PER_ROOM, "item spawns",
                             parse_item_spawn_element))
                return false;
        } else {
            return desk_json_fail_at(p, p->key_offset,
                                     "unknown key '%s' in room", key);
        }
    }
    for (i = 0u; i < sizeof required / sizeof required[0]; ++i)
        if ((seen & required[i].bit) == 0u)
            return desk_json_fail_at(p, start, "room missing '%s'",
                                     required[i].key);
    return true;
}

static bool parse_rooms(desk_json_reader *p, desk_world *world)
{
    bool first = true;

    if (!desk_json_expect(p, '['))
        return false;
    for (;;) {
        int step = desk_json_next_element(p, &first);

        if (step < 0)
            return false;
        if (step == 0)
            return true;
        if (world->room_count >= DESK_MAX_ROOMS)
            return desk_json_fail_at(p, p->offset, "more than %d rooms",
                                     DESK_MAX_ROOMS);
        if (!parse_room(p, &world->rooms[world->room_count]))
            return false;
        world->room_count++;
    }
}

bool desk_world_load(desk_world *world, const char *path, char *error,
                     size_t error_size)
{
    /* Bounded read: world.json is a few KiB; 256 KiB is a hard ceiling. */
    static char file_text[DESK_WORLD_FILE_CAPACITY + 1u];
    desk_json_reader parser_state;
    bool first = true;
    unsigned seen = 0u;
    char key[32];
    char start_id[DESK_ID_CAPACITY] = {0};
    size_t start_offset = 0u;
    int start_index;
    int r;

    if (error && error_size > 0u)
        error[0] = '\0';
    if (!world)
        return false;
    memset(world, 0, sizeof *world);
    world->start_room = -1;

    if (!desk_json_open(&parser_state, path, "world.json", file_text,
                        sizeof file_text, error, error_size))
        return false;

    if (!desk_json_expect(&parser_state, '{'))
        return false;
    for (;;) {
        int step = desk_json_next_key(&parser_state, &first, key,
                                      sizeof key);

        if (step < 0)
            return false;
        if (step == 0)
            break;
        if (strcmp(key, "world") == 0) {
            float schema = 0.0f;
            size_t value_offset;

            if (!desk_json_claim_key(&parser_state, &seen, 1u << 0, key))
                return false;
            desk_json_skip_ws(&parser_state);
            value_offset = parser_state.offset;
            if (!desk_json_parse_number(&parser_state, &schema))
                return false;
            /* Schema 1 = no item spawns or receivers; schema 2 adds the
             * optional item fields. Both parse identically because the
             * new keys are optional. */
            if (schema != 1.0f && schema != 2.0f)
                return desk_json_fail_at(
                    &parser_state, value_offset,
                    "unsupported schema version (want world: 1 or 2)");
        } else if (strcmp(key, "start") == 0) {
            if (!desk_json_claim_key(&parser_state, &seen, 1u << 1, key))
                return false;
            desk_json_skip_ws(&parser_state);
            start_offset = parser_state.offset;
            if (!desk_json_parse_string(&parser_state, start_id,
                                        sizeof start_id))
                return false;
        } else if (strcmp(key, "rooms") == 0) {
            if (!desk_json_claim_key(&parser_state, &seen, 1u << 2, key))
                return false;
            if (!parse_rooms(&parser_state, world))
                return false;
        } else {
            return desk_json_fail_at(&parser_state, parser_state.key_offset,
                                     "unknown key '%s' at top level", key);
        }
    }
    desk_json_skip_ws(&parser_state);
    if (parser_state.offset != parser_state.length)
        return desk_json_fail_at(&parser_state, parser_state.offset,
                                 "trailing data after JSON document");
    if ((seen & (1u << 0)) == 0u)
        return desk_json_fail_at(&parser_state, 0u, "missing key 'world'");
    if ((seen & (1u << 1)) == 0u)
        return desk_json_fail_at(&parser_state, 0u, "missing key 'start'");
    if ((seen & (1u << 2)) == 0u)
        return desk_json_fail_at(&parser_state, 0u, "missing key 'rooms'");

    /* Doors may reference rooms declared later; resolve once all ids exist.
     * Unknown destinations stay -1 for desk_world_validate to report. */
    for (r = 0; r < world->room_count; ++r) {
        desk_room *room = &world->rooms[r];
        int d;

        for (d = 0; d < room->door_count; ++d)
            room->doors[d].to_room =
                desk_world_room_index(world, room->doors[d].to_id);
    }
    start_index = desk_world_room_index(world, start_id);
    if (start_index < 0)
        return desk_json_fail_at(&parser_state, start_offset,
                                 "start room '%s' does not exist", start_id);
    world->start_room = start_index;
    return true;
}

static bool check_rect(const desk_rect *rect, const char *label, char *error,
                       size_t error_size)
{
    if (rect->w <= 0.0f || rect->h <= 0.0f)
        return vfail(error, error_size, "%s: non-positive size", label);
    if (rect->x < 0.0f || rect->y < 0.0f)
        return vfail(error, error_size, "%s: negative origin", label);
    if (rect->x + rect->w > (float)DESK_LOGICAL_WIDTH)
        return vfail(error, error_size, "%s: exceeds logical width %d",
                     label, DESK_LOGICAL_WIDTH);
    if (rect->y + rect->h > (float)DESK_LOGICAL_HEIGHT)
        return vfail(error, error_size, "%s: exceeds logical height %d",
                     label, DESK_LOGICAL_HEIGHT);
    return true;
}

static bool point_in_rect(float x, float y, const desk_rect *rect)
{
    return x >= rect->x && x <= rect->x + rect->w && y >= rect->y &&
           y <= rect->y + rect->h;
}

bool desk_world_validate(const desk_world *world, char *error,
                         size_t error_size)
{
    bool reachable[DESK_MAX_ROOMS] = {false};
    char label[80];
    int r;

    if (!world)
        return vfail(error, error_size, "world: null world");
    if (world->room_count <= 0)
        return vfail(error, error_size, "world: no rooms");
    if (world->room_count > DESK_MAX_ROOMS)
        return vfail(error, error_size, "world: more than %d rooms",
                     DESK_MAX_ROOMS);
    if (world->start_room < 0 || world->start_room >= world->room_count)
        return vfail(error, error_size,
                     "world: start room index %d out of range",
                     world->start_room);

    for (r = 0; r < world->room_count; ++r) {
        const desk_room *room = &world->rooms[r];
        int duplicate;

        if (room->id[0] == '\0')
            return vfail(error, error_size, "room[%d].id: missing or empty",
                         r);
        for (duplicate = 0; duplicate < r; ++duplicate)
            if (strcmp(room->id, world->rooms[duplicate].id) == 0)
                return vfail(error, error_size,
                             "world: duplicate room id '%s'", room->id);
    }

    for (r = 0; r < world->room_count; ++r) {
        const desk_room *room = &world->rooms[r];
        int i;

        if (room->name[0] == '\0')
            return vfail(error, error_size, "%s.name: missing or empty",
                         room->id);
        if (room->plate[0] == '\0')
            return vfail(error, error_size, "%s.plate: missing or empty",
                         room->id);
        (void)snprintf(label, sizeof label, "%s.walk", room->id);
        if (!check_rect(&room->walk, label, error, error_size))
            return false;

        if (room->obstacle_count < 0 ||
            room->obstacle_count > DESK_MAX_OBSTACLES_PER_ROOM)
            return vfail(error, error_size, "%s: more than %d obstacles",
                         room->id, DESK_MAX_OBSTACLES_PER_ROOM);
        for (i = 0; i < room->obstacle_count; ++i) {
            (void)snprintf(label, sizeof label, "%s.obstacles[%d]",
                           room->id, i);
            if (!check_rect(&room->obstacles[i], label, error, error_size))
                return false;
        }

        if (room->door_count < 0 ||
            room->door_count > DESK_MAX_DOORS_PER_ROOM)
            return vfail(error, error_size, "%s: more than %d doors",
                         room->id, DESK_MAX_DOORS_PER_ROOM);
        for (i = 0; i < room->door_count; ++i) {
            const desk_door *door = &room->doors[i];
            const desk_room *destination;
            int destination_index;
            int j;

            (void)snprintf(label, sizeof label, "%s.doors[%d].rect",
                           room->id, i);
            if (!check_rect(&door->rect, label, error, error_size))
                return false;
            destination_index = desk_world_room_index(world, door->to_id);
            if (destination_index < 0)
                return vfail(error, error_size,
                             "%s.doors[%d]: unknown destination '%s'",
                             room->id, i, door->to_id);
            if (destination_index == r)
                return vfail(error, error_size,
                             "%s.doors[%d]: door to its own room", room->id,
                             i);
            destination = &world->rooms[destination_index];
            if (!point_in_rect(door->spawn_x, door->spawn_y,
                               &destination->walk))
                return vfail(error, error_size,
                             "%s.doors[%d]: spawn (%g,%g) outside '%s' walk rect",
                             room->id, i, (double)door->spawn_x,
                             (double)door->spawn_y, door->to_id);
            for (j = 0; j < destination->obstacle_count; ++j)
                if (point_in_rect(door->spawn_x, door->spawn_y,
                                  &destination->obstacles[j]))
                    return vfail(error, error_size,
                                 "%s.doors[%d]: spawn inside '%s' obstacle [%d]",
                                 room->id, i, door->to_id, j);
            /* Door triggers fire on position alone, so a spawn inside any of
             * the destination's door rects would teleport an idle player. */
            for (j = 0; j < destination->door_count; ++j)
                if (point_in_rect(door->spawn_x, door->spawn_y,
                                  &destination->doors[j].rect))
                    return vfail(error, error_size,
                                 "%s.doors[%d]: spawn inside '%s' door [%d]",
                                 room->id, i, door->to_id, j);
            reachable[destination_index] = true;
            if (door->to_room != destination_index) {
                /* Header contract: validate resolves to_room. The world the
                 * program validates lives in non-const storage, so writing
                 * through the const-stripped pointer is defined. */
                desk_door *resolved = (desk_door *)&room->doors[i];

                resolved->to_room = destination_index;
            }
        }

        if (room->object_count < 0 ||
            room->object_count > DESK_MAX_OBJECTS_PER_ROOM)
            return vfail(error, error_size, "%s: more than %d objects",
                         room->id, DESK_MAX_OBJECTS_PER_ROOM);
        for (i = 0; i < room->object_count; ++i) {
            const desk_object *object = &room->objects[i];
            int j;

            if (object->id[0] == '\0')
                return vfail(error, error_size,
                             "%s.objects[%d].id: missing or empty",
                             room->id, i);
            for (j = 0; j < i; ++j)
                if (strcmp(object->id, room->objects[j].id) == 0)
                    return vfail(error, error_size,
                                 "%s: duplicate object id '%s'", room->id,
                                 object->id);
            if (object->prompt[0] == '\0')
                return vfail(error, error_size,
                             "%s.objects[%d].prompt: missing or empty",
                             room->id, i);
            (void)snprintf(label, sizeof label, "%s.objects[%d].rect",
                           room->id, i);
            if (!check_rect(&object->rect, label, error, error_size))
                return false;
            if (object->target <= DESK_TARGET_NONE ||
                (int)object->target >= DESK_TARGET_COUNT)
                return vfail(error, error_size,
                             "%s.objects[%d]: unknown target", room->id, i);
        }

        if (room->npc_count < 0 || room->npc_count > DESK_MAX_NPCS_PER_ROOM)
            return vfail(error, error_size, "%s: more than %d npcs",
                         room->id, DESK_MAX_NPCS_PER_ROOM);
        for (i = 0; i < room->npc_count; ++i) {
            const desk_npc *npc = &room->npcs[i];
            int j;

            if (npc->actor < 1 || npc->actor > 3)
                return vfail(error, error_size,
                             "%s.npcs[%d]: actor must be 1..3", room->id, i);
            for (j = 0; j < i; ++j)
                if (room->npcs[j].actor == npc->actor)
                    return vfail(error, error_size,
                                 "%s: duplicate npc actor %d", room->id,
                                 npc->actor);
            if (npc->x < 0.0f || npc->x > (float)DESK_LOGICAL_WIDTH ||
                npc->y < 0.0f || npc->y > (float)DESK_LOGICAL_HEIGHT)
                return vfail(error, error_size,
                             "%s.npcs[%d]: position off the logical canvas",
                             room->id, i);
        }

        if (room->walkbehind_count < 0 ||
            room->walkbehind_count > DESK_MAX_WALKBEHINDS_PER_ROOM)
            return vfail(error, error_size, "%s: more than %d walkbehinds",
                         room->id, DESK_MAX_WALKBEHINDS_PER_ROOM);
        for (i = 0; i < room->walkbehind_count; ++i) {
            const desk_walkbehind *walkbehind = &room->walkbehinds[i];
            int j;

            if (walkbehind->id < 1 ||
                walkbehind->id > DESK_MAX_WALKBEHINDS_PER_ROOM)
                return vfail(error, error_size,
                             "%s.walkbehinds[%d]: id must be 1..%d",
                             room->id, i, DESK_MAX_WALKBEHINDS_PER_ROOM);
            for (j = 0; j < i; ++j)
                if (room->walkbehinds[j].id == walkbehind->id)
                    return vfail(error, error_size,
                                 "%s: duplicate walkbehind id %d",
                                 room->id, walkbehind->id);
            if (walkbehind->baseline < 0.0f ||
                walkbehind->baseline > (float)DESK_LOGICAL_HEIGHT)
                return vfail(error, error_size,
                             "%s.walkbehinds[%d]: baseline %g off the "
                             "logical canvas", room->id, i,
                             (double)walkbehind->baseline);
        }

        if (room->spawn_count < 0 ||
            room->spawn_count > DESK_MAX_ITEM_SPAWNS_PER_ROOM)
            return vfail(error, error_size, "%s: more than %d item spawns",
                         room->id, DESK_MAX_ITEM_SPAWNS_PER_ROOM);
        for (i = 0; i < room->spawn_count; ++i) {
            const desk_item_spawn *spawn = &room->spawns[i];
            int j;
            int other;

            if (!desk_spawn_id_valid(spawn->id))
                return vfail(error, error_size,
                             "%s.item_spawns[%d]: invalid spawn id",
                             room->id, i);
            if (!desk_item_id_valid(spawn->item))
                return vfail(error, error_size,
                             "%s.item_spawns[%d]: invalid item id",
                             room->id, i);
            if (spawn->quantity < 1 ||
                spawn->quantity > DESK_ITEM_MAX_STACK_LIMIT)
                return vfail(error, error_size,
                             "%s.item_spawns[%d]: quantity out of range",
                             room->id, i);
            if (!point_in_rect(spawn->x, spawn->y, &room->walk))
                return vfail(error, error_size,
                             "%s.item_spawns[%d]: point outside walk rect",
                             room->id, i);
            for (j = 0; j < room->obstacle_count; ++j)
                if (point_in_rect(spawn->x, spawn->y,
                                  &room->obstacles[j]))
                    return vfail(error, error_size,
                                 "%s.item_spawns[%d]: point inside "
                                 "obstacle [%d]", room->id, i, j);
            for (j = 0; j < room->door_count; ++j)
                if (point_in_rect(spawn->x, spawn->y,
                                  &room->doors[j].rect))
                    return vfail(error, error_size,
                                 "%s.item_spawns[%d]: point inside door "
                                 "[%d]", room->id, i, j);
            for (j = 0; j < room->object_count; ++j)
                if (point_in_rect(spawn->x, spawn->y,
                                  &room->objects[j].rect))
                    return vfail(error, error_size,
                                 "%s.item_spawns[%d]: point inside object "
                                 "'%s'", room->id, i, room->objects[j].id);
            for (j = 0; j < room->npc_count; ++j) {
                float dx = spawn->x - room->npcs[j].x;
                float dy = spawn->y - room->npcs[j].y;

                if (dx * dx + dy * dy < DESK_NPC_SPAWN_EXCLUSION *
                                        DESK_NPC_SPAWN_EXCLUSION)
                    return vfail(error, error_size,
                                 "%s.item_spawns[%d]: too close to npc "
                                 "actor %d", room->id, i,
                                 room->npcs[j].actor);
            }
            /* Spawn ids are stable content identity for the claimed
             * table, so they must be unique across the whole world. */
            for (other = 0; other <= r; ++other) {
                const desk_room *scan = &world->rooms[other];
                int limit = other == r ? i : scan->spawn_count;

                for (j = 0; j < limit; ++j)
                    if (strcmp(scan->spawns[j].id, spawn->id) == 0)
                        return vfail(error, error_size,
                                     "world: duplicate spawn id '%s'",
                                     spawn->id);
            }
        }
    }

    {
        char unreachable_list[96];
        size_t used = 0u;
        bool any = false;

        unreachable_list[0] = '\0';
        for (r = 0; r < world->room_count; ++r) {
            int written;

            if (r == world->start_room || reachable[r])
                continue;
            written = snprintf(unreachable_list + used,
                               sizeof unreachable_list - used, "%s%s",
                               any ? ", " : "", world->rooms[r].id);
            if (written > 0 &&
                (size_t)written < sizeof unreachable_list - used)
                used += (size_t)written;
            any = true;
        }
        if (any)
            return vfail(error, error_size,
                         "world: rooms unreachable by any door: %s",
                         unreachable_list);
    }
    return true;
}

int desk_world_room_index(const desk_world *world, const char *id)
{
    int index;

    if (!world || !id || id[0] == '\0')
        return -1;
    for (index = 0; index < world->room_count && index < DESK_MAX_ROOMS;
         ++index)
        if (strcmp(world->rooms[index].id, id) == 0)
            return index;
    return -1;
}

bool desk_world_validate_items(const desk_world *world,
                               const desk_item_catalog *catalog,
                               char *error, size_t error_size)
{
    int r;

    if (!world || !catalog)
        return vfail(error, error_size, "world-items: null input");
    for (r = 0; r < world->room_count; ++r) {
        const desk_room *room = &world->rooms[r];
        int i;

        for (i = 0; i < room->object_count; ++i) {
            const desk_object *object = &room->objects[i];

            if (object->receiver[0] != '\0' &&
                desk_items_find_receiver(catalog, object->receiver) < 0)
                return vfail(error, error_size,
                             "%s.%s: unknown receiver rule '%s'",
                             room->id, object->id, object->receiver);
        }
        for (i = 0; i < room->spawn_count; ++i) {
            const desk_item_spawn *spawn = &room->spawns[i];
            int definition = desk_items_find(catalog, spawn->item);
            const desk_item_def *def;

            if (definition <= 0)
                return vfail(error, error_size,
                             "%s.item_spawns[%d]: unknown item '%s'",
                             room->id, i, spawn->item);
            def = desk_items_def(catalog, (uint16_t)definition);
            if (!def || spawn->quantity > (int)def->max_stack)
                return vfail(error, error_size,
                             "%s.item_spawns[%d]: quantity %d exceeds "
                             "'%s' max stack", room->id, i,
                             spawn->quantity, spawn->item);
        }
    }
    return true;
}
