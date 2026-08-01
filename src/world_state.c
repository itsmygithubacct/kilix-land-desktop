/* world_state.c — the durable world record (world.state).
 *
 * Same storage discipline as the profile: CRC-protected atomic
 * kilix-state record, strict staged decode, publish only after the whole
 * record validates. Unlike the profile, a corrupt record is not fatal:
 * the caller gets a fresh empty world plus a corruption flag, and the
 * player keeps their identity.
 *
 * Every reference is a stable string id. Runtime indexes never enter the
 * file. */

#include "world_state.h"
#include "state_store.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define DESK_WORLD_FILENAME "world.state"
#define DESK_SOCIAL_POINT_LIMIT 1000000

void desk_world_state_init(desk_world_state *state)
{
    int index;

    if (!state) return;
    memset(state, 0, sizeof *state);
    desk_inventory_init(&state->inventory);
    state->next_serial = 1u;
    for (index = 0; index < DESK_MAX_WORLD_ITEMS; ++index)
        state->items[index].room = -1;
}

uint32_t desk_world_state_take_serial(desk_world_state *state)
{
    uint32_t serial;

    if (!state) return 0u;
    if (state->next_serial == 0u) state->next_serial = 1u;
    serial = state->next_serial;
    state->next_serial++;
    return serial;
}

int desk_world_state_orphan_add(desk_world_state *state, const char *id)
{
    int index;

    if (!state || !id || id[0] == '\0' ||
        strlen(id) >= (size_t)DESK_ITEM_ID_CAPACITY)
        return -1;
    for (index = 0; index < state->orphan_count; ++index)
        if (strcmp(state->orphans[index], id) == 0) return index;
    if (state->orphan_count >= DESK_MAX_ITEM_ORPHANS) return -1;
    (void)snprintf(state->orphans[state->orphan_count],
                   DESK_ITEM_ID_CAPACITY, "%s", id);
    return state->orphan_count++;
}

bool desk_spawn_id_valid(const char *id)
{
    const char *cursor;

    if (!id || id[0] == '\0') return false;
    if (strlen(id) >= (size_t)DESK_SPAWN_ID_CAPACITY) return false;
    for (cursor = id; *cursor != '\0'; ++cursor)
        if (!((*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '-'))
            return false;
    return true;
}

bool desk_world_state_is_claimed(const desk_world_state *state,
                                 const char *spawn_id)
{
    int index;

    if (!state || !spawn_id) return false;
    for (index = 0; index < state->claimed_count; ++index)
        if (strcmp(state->claimed[index], spawn_id) == 0) return true;
    return false;
}

bool desk_world_state_claim(desk_world_state *state, const char *spawn_id)
{
    if (!state || !desk_spawn_id_valid(spawn_id)) return false;
    if (desk_world_state_is_claimed(state, spawn_id)) return false;
    if (state->claimed_count >= DESK_MAX_CLAIMED_SPAWNS) return false;
    (void)snprintf(state->claimed[state->claimed_count],
                   DESK_SPAWN_ID_CAPACITY, "%s", spawn_id);
    state->claimed_count++;
    return true;
}

/* ---- validation --------------------------------------------------------- */

static bool string_field_ok(const char *field, size_t capacity)
{
    return memchr(field, '\0', capacity) != NULL && field[0] != '\0';
}

typedef struct serial_audit {
    uint32_t seen[DESK_INVENTORY_SLOTS + DESK_EQUIPMENT_SLOTS +
                  DESK_MAX_WORLD_ITEMS + DESK_MAX_RECEIVER_STATES];
    int count;
} serial_audit;

static bool audit_serial(serial_audit *audit, const desk_item *item,
                         uint32_t next_serial)
{
    int index;

    if (desk_item_is_empty(item) || item->serial == 0u) return true;
    if (item->serial >= next_serial) return false;
    for (index = 0; index < audit->count; ++index)
        if (audit->seen[index] == item->serial) return false;
    audit->seen[audit->count++] = item->serial;
    return true;
}

static bool instance_shape_ok(const desk_world_state *state,
                              const desk_item_catalog *catalog,
                              const desk_item *item)
{
    if (desk_item_is_empty(item))
        return item->definition == 0u && item->variant == 0u &&
               item->durability == 0u && item->serial == 0u;
    if (!desk_item_valid(catalog, item)) return false;
    if (item->definition == DESK_ITEM_DEF_MISSING)
        return item->variant < (uint16_t)state->orphan_count;
    return item->variant == 0u;
}

static bool desk_world_state_check(const desk_world_state *state,
                                   const desk_item_catalog *catalog)
{
    serial_audit audit;
    int index;

    audit.count = 0;
    if (!state || !catalog) return false;
    if (state->next_serial == 0u) return false;
    if (state->inventory.selected < 0 ||
        state->inventory.selected >= DESK_INVENTORY_SLOTS)
        return false;
    if (state->orphan_count < 0 ||
        state->orphan_count > DESK_MAX_ITEM_ORPHANS ||
        state->claimed_count < 0 ||
        state->claimed_count > DESK_MAX_CLAIMED_SPAWNS ||
        state->item_count < 0 || state->item_count > DESK_MAX_WORLD_ITEMS ||
        state->receiver_count < 0 ||
        state->receiver_count > DESK_MAX_RECEIVER_STATES ||
        state->effect_count < 0 ||
        state->effect_count > DESK_MAX_ACTIVE_EFFECTS ||
        state->social_count < 0 ||
        state->social_count > DESK_MAX_SOCIAL_RECORDS)
        return false;
    for (index = 0; index < state->orphan_count; ++index) {
        int duplicate;

        if (!string_field_ok(state->orphans[index], DESK_ITEM_ID_CAPACITY) ||
            !desk_item_id_valid(state->orphans[index]))
            return false;
        for (duplicate = 0; duplicate < index; ++duplicate)
            if (strcmp(state->orphans[duplicate],
                       state->orphans[index]) == 0)
                return false;
    }
    for (index = 0; index < DESK_INVENTORY_SLOTS; ++index)
        if (!instance_shape_ok(state, catalog,
                               &state->inventory.slots[index]) ||
            !audit_serial(&audit, &state->inventory.slots[index],
                          state->next_serial))
            return false;
    for (index = 0; index < DESK_EQUIPMENT_SLOTS; ++index)
        if (!instance_shape_ok(state, catalog, &state->equipment[index]) ||
            !audit_serial(&audit, &state->equipment[index],
                          state->next_serial))
            return false;
    for (index = 0; index < state->claimed_count; ++index) {
        int duplicate;

        if (!desk_spawn_id_valid(state->claimed[index])) return false;
        for (duplicate = 0; duplicate < index; ++duplicate)
            if (strcmp(state->claimed[duplicate],
                       state->claimed[index]) == 0)
                return false;
    }
    for (index = 0; index < state->item_count; ++index) {
        const desk_world_item *entry = &state->items[index];

        if (desk_item_is_empty(&entry->item) ||
            !instance_shape_ok(state, catalog, &entry->item) ||
            !audit_serial(&audit, &entry->item, state->next_serial))
            return false;
        if (!isfinite(entry->x) || !isfinite(entry->y)) return false;
        if (!string_field_ok(entry->room_id, DESK_WORLD_ROOM_ID_CAPACITY))
            return false;
    }
    for (index = 0; index < state->receiver_count; ++index) {
        const desk_receiver_state *entry = &state->receivers[index];
        int duplicate;

        if (!string_field_ok(entry->room_id, DESK_WORLD_ROOM_ID_CAPACITY) ||
            !string_field_ok(entry->object_id,
                             DESK_WORLD_ROOM_ID_CAPACITY))
            return false;
        for (duplicate = 0; duplicate < index; ++duplicate)
            if (strcmp(state->receivers[duplicate].room_id,
                       entry->room_id) == 0 &&
                strcmp(state->receivers[duplicate].object_id,
                       entry->object_id) == 0)
                return false;
        if (!instance_shape_ok(state, catalog, &entry->item) ||
            !audit_serial(&audit, &entry->item, state->next_serial))
            return false;
        switch (entry->phase) {
        case DESK_RECEIVER_EMPTY:
            if (!desk_item_is_empty(&entry->item) ||
                entry->remaining_ticks != 0)
                return false;
            break;
        case DESK_RECEIVER_PROCESSING:
            if (desk_item_is_empty(&entry->item) ||
                entry->remaining_ticks <= 0 ||
                entry->remaining_ticks > DESK_ITEM_MAX_TICKS)
                return false;
            break;
        case DESK_RECEIVER_READY:
            if (desk_item_is_empty(&entry->item) ||
                entry->remaining_ticks != 0)
                return false;
            break;
        default:
            return false;
        }
    }
    for (index = 0; index < state->effect_count; ++index) {
        const desk_active_effect *effect = &state->effects[index];

        if (effect->definition == DESK_ITEM_DEF_MISSING ||
            !desk_items_def(catalog, effect->definition))
            return false;
        if (effect->remaining_ticks <= 0 ||
            effect->remaining_ticks > DESK_ITEM_MAX_TICKS)
            return false;
    }
    for (index = 0; index < state->social_count; ++index) {
        const desk_social_record *record = &state->social[index];
        int duplicate;

        if (record->cast >= 4u || record->actor < 1u || record->actor > 3u)
            return false;
        if (record->points > DESK_SOCIAL_POINT_LIMIT ||
            record->points < -DESK_SOCIAL_POINT_LIMIT)
            return false;
        for (duplicate = 0; duplicate < index; ++duplicate)
            if (state->social[duplicate].cast == record->cast &&
                state->social[duplicate].actor == record->actor)
                return false;
    }
    return true;
}

/* ---- encode ------------------------------------------------------------- */

static bool write_item_record(kilixstate_writer *writer,
                              const desk_world_state *state,
                              const desk_item_catalog *catalog,
                              const desk_item *item)
{
    char id[DESK_ITEM_ID_CAPACITY];

    memset(id, 0, sizeof id);
    if (!desk_item_is_empty(item)) {
        const char *source;

        if (item->definition == DESK_ITEM_DEF_MISSING) {
            if (item->variant >= (uint16_t)state->orphan_count)
                return false;
            source = state->orphans[item->variant];
        } else {
            const desk_item_def *def =
                desk_items_def(catalog, item->definition);

            if (!def) return false;
            source = def->id;
        }
        (void)snprintf(id, sizeof id, "%s", source);
    }
    return kilixstate_write_bytes(writer, id, sizeof id) &&
           kilixstate_write_u16(writer, desk_item_is_empty(item) ?
                                        0u : item->quantity) &&
           kilixstate_write_u16(writer, 0u) && /* variant: reserved */
           kilixstate_write_u16(writer, desk_item_is_empty(item) ?
                                        0u : item->durability) &&
           kilixstate_write_u32(writer, desk_item_is_empty(item) ?
                                        0u : item->serial);
}

bool desk_world_state_save(const desk_world_state *state,
                           const desk_item_catalog *catalog)
{
    kilixstate_store store;
    kilixstate_writer writer;
    uint8_t payload[DESK_WORLD_MAX_PAYLOAD];
    uint8_t counts[4];
    bool ok;
    int index;

    if (!desk_world_state_check(state, catalog)) return false;
    kilixstate_writer_init(&writer, payload, sizeof payload);
    ok = kilixstate_write_u32(&writer,
                              (uint32_t)DESK_WORLD_STATE_SCHEMA) &&
         kilixstate_write_u32(&writer, state->next_serial) &&
         kilixstate_write_u8(&writer, (uint8_t)state->inventory.selected);
    counts[0] = 0u;
    for (index = 0; index < DESK_INVENTORY_SLOTS; ++index)
        if (!desk_item_is_empty(&state->inventory.slots[index]))
            counts[0]++;
    ok = ok && kilixstate_write_u8(&writer, counts[0]);
    for (index = 0; ok && index < DESK_INVENTORY_SLOTS; ++index) {
        if (desk_item_is_empty(&state->inventory.slots[index])) continue;
        ok = kilixstate_write_u8(&writer, (uint8_t)index) &&
             write_item_record(&writer, state, catalog,
                               &state->inventory.slots[index]);
    }
    counts[1] = 0u;
    for (index = 0; index < DESK_EQUIPMENT_SLOTS; ++index)
        if (!desk_item_is_empty(&state->equipment[index])) counts[1]++;
    ok = ok && kilixstate_write_u8(&writer, counts[1]);
    for (index = 0; ok && index < DESK_EQUIPMENT_SLOTS; ++index) {
        if (desk_item_is_empty(&state->equipment[index])) continue;
        ok = kilixstate_write_u8(&writer, (uint8_t)index) &&
             write_item_record(&writer, state, catalog,
                               &state->equipment[index]);
    }
    ok = ok && kilixstate_write_u8(&writer, (uint8_t)state->orphan_count);
    for (index = 0; ok && index < state->orphan_count; ++index)
        ok = kilixstate_write_bytes(&writer, state->orphans[index],
                                    DESK_ITEM_ID_CAPACITY);
    ok = ok && kilixstate_write_u8(&writer, (uint8_t)state->claimed_count);
    for (index = 0; ok && index < state->claimed_count; ++index)
        ok = kilixstate_write_bytes(&writer, state->claimed[index],
                                    DESK_SPAWN_ID_CAPACITY);
    ok = ok && kilixstate_write_u8(&writer, (uint8_t)state->item_count);
    for (index = 0; ok && index < state->item_count; ++index) {
        const desk_world_item *entry = &state->items[index];
        uint32_t x_bits;
        uint32_t y_bits;

        memcpy(&x_bits, &entry->x, sizeof x_bits);
        memcpy(&y_bits, &entry->y, sizeof y_bits);
        ok = kilixstate_write_bytes(&writer, entry->room_id,
                                    DESK_WORLD_ROOM_ID_CAPACITY) &&
             write_item_record(&writer, state, catalog, &entry->item) &&
             kilixstate_write_u32(&writer, x_bits) &&
             kilixstate_write_u32(&writer, y_bits) &&
             kilixstate_write_bool(&writer, entry->placed);
    }
    ok = ok && kilixstate_write_u8(&writer, (uint8_t)state->receiver_count);
    for (index = 0; ok && index < state->receiver_count; ++index) {
        const desk_receiver_state *entry = &state->receivers[index];

        ok = kilixstate_write_bytes(&writer, entry->room_id,
                                    DESK_WORLD_ROOM_ID_CAPACITY) &&
             kilixstate_write_bytes(&writer, entry->object_id,
                                    DESK_WORLD_ROOM_ID_CAPACITY) &&
             kilixstate_write_u8(&writer, entry->phase) &&
             kilixstate_write_u32(&writer,
                                  (uint32_t)entry->remaining_ticks) &&
             write_item_record(&writer, state, catalog, &entry->item);
    }
    ok = ok && kilixstate_write_u8(&writer, (uint8_t)state->effect_count);
    for (index = 0; ok && index < state->effect_count; ++index) {
        const desk_item_def *def =
            desk_items_def(catalog, state->effects[index].definition);

        ok = def != NULL &&
             kilixstate_write_bytes(&writer, def->id,
                                    DESK_ITEM_ID_CAPACITY) &&
             kilixstate_write_u32(
                 &writer, (uint32_t)state->effects[index].remaining_ticks);
    }
    ok = ok && kilixstate_write_u8(&writer, (uint8_t)state->social_count);
    for (index = 0; ok && index < state->social_count; ++index) {
        const desk_social_record *record = &state->social[index];

        ok = kilixstate_write_u8(&writer, record->cast) &&
             kilixstate_write_u8(&writer, record->actor) &&
             kilixstate_write_u32(&writer, (uint32_t)record->points) &&
             kilixstate_write_u16(&writer, record->gifts) &&
             kilixstate_write_u16(&writer, record->flags);
    }
    if (!ok) return false;
    if (!desk_state_store_open(&store, DESK_WORLD_FILENAME,
                               DESK_WORLD_MAX_PAYLOAD))
        return false;
    ok = kilixstate_save(&store, payload,
                         kilixstate_writer_size(&writer)) == KILIXSTATE_OK;
    kilixstate_store_close(&store);
    return ok;
}

/* ---- decode ------------------------------------------------------------- */

typedef struct world_decode_context {
    desk_world_state *state;
    const desk_item_catalog *catalog;
} world_decode_context;

static bool read_string_field(kilixstate_reader *reader, char *field,
                              size_t capacity)
{
    if (!kilixstate_read_bytes(reader, field, capacity)) return false;
    return memchr(field, '\0', capacity) != NULL;
}

/* Reads one 58-byte item record. Unknown ids become the compiled missing
 * item with their original id parked in the orphan table. */
static bool read_item_record(kilixstate_reader *reader,
                             world_decode_context *context, desk_item *item)
{
    char id[DESK_ITEM_ID_CAPACITY];
    uint16_t quantity = 0u;
    uint16_t variant = 0u;
    uint16_t durability = 0u;
    uint32_t serial = 0u;

    desk_item_clear(item);
    if (!read_string_field(reader, id, sizeof id) ||
        !kilixstate_read_u16(reader, &quantity) ||
        !kilixstate_read_u16(reader, &variant) ||
        !kilixstate_read_u16(reader, &durability) ||
        !kilixstate_read_u32(reader, &serial))
        return false;
    if (id[0] == '\0')
        return quantity == 0u && variant == 0u && durability == 0u &&
               serial == 0u;
    if (quantity == 0u || variant != 0u) return false;
    if (!desk_item_id_valid(id) &&
        strcmp(id, DESK_ITEM_MISSING_ID) != 0)
        return false;
    item->quantity = quantity;
    item->durability = durability;
    item->serial = serial;
    {
        int found = desk_items_find(context->catalog, id);

        if (found >= 0 && found != (int)DESK_ITEM_DEF_MISSING) {
            item->definition = (uint16_t)found;
        } else {
            int orphan =
                desk_world_state_orphan_add(context->state, id);

            if (orphan < 0) return false;
            item->definition = DESK_ITEM_DEF_MISSING;
            item->variant = (uint16_t)orphan;
        }
    }
    return true;
}

static bool decode_world_v1(kilixstate_reader *reader, void *opaque)
{
    world_decode_context *context = opaque;
    desk_world_state *state = context->state;
    uint32_t next_serial = 0u;
    uint8_t selected = 0u;
    uint8_t count = 0u;
    uint8_t previous;
    int index;

    if (!kilixstate_read_u32(reader, &next_serial) ||
        !kilixstate_read_u8(reader, &selected) ||
        selected >= (uint8_t)DESK_INVENTORY_SLOTS)
        return false;
    state->next_serial = next_serial;
    state->inventory.selected = selected;

    if (!kilixstate_read_u8(reader, &count) ||
        count > (uint8_t)DESK_INVENTORY_SLOTS)
        return false;
    previous = UINT8_MAX;
    for (index = 0; index < (int)count; ++index) {
        uint8_t slot = 0u;

        if (!kilixstate_read_u8(reader, &slot) ||
            slot >= (uint8_t)DESK_INVENTORY_SLOTS ||
            (previous != UINT8_MAX && slot <= previous))
            return false;
        previous = slot;
        if (!read_item_record(reader, context,
                              &state->inventory.slots[slot]) ||
            desk_item_is_empty(&state->inventory.slots[slot]))
            return false;
    }

    if (!kilixstate_read_u8(reader, &count) ||
        count > (uint8_t)DESK_EQUIPMENT_SLOTS)
        return false;
    previous = UINT8_MAX;
    for (index = 0; index < (int)count; ++index) {
        uint8_t slot = 0u;

        if (!kilixstate_read_u8(reader, &slot) ||
            slot >= (uint8_t)DESK_EQUIPMENT_SLOTS ||
            (previous != UINT8_MAX && slot <= previous))
            return false;
        previous = slot;
        if (!read_item_record(reader, context, &state->equipment[slot]) ||
            desk_item_is_empty(&state->equipment[slot]))
            return false;
    }

    /* The orphan table decodes after the items that reference it were
     * already re-parked via desk_world_state_orphan_add, so saved entries
     * only need to merge: read them, then verify every recorded orphan is
     * still known (a table the items never referenced is fine). */
    if (!kilixstate_read_u8(reader, &count) ||
        count > (uint8_t)DESK_MAX_ITEM_ORPHANS)
        return false;
    for (index = 0; index < (int)count; ++index) {
        char id[DESK_ITEM_ID_CAPACITY];

        if (!read_string_field(reader, id, sizeof id) || id[0] == '\0')
            return false;
        /* Recovered ids resolve now and simply stop being orphans. */
        if (desk_items_find(context->catalog, id) < 0 &&
            desk_world_state_orphan_add(state, id) < 0)
            return false;
    }

    if (!kilixstate_read_u8(reader, &count) ||
        count > (uint8_t)DESK_MAX_CLAIMED_SPAWNS)
        return false;
    for (index = 0; index < (int)count; ++index) {
        char id[DESK_SPAWN_ID_CAPACITY];

        if (!read_string_field(reader, id, sizeof id) ||
            !desk_world_state_claim(state, id))
            return false;
    }

    if (!kilixstate_read_u8(reader, &count) ||
        count > (uint8_t)DESK_MAX_WORLD_ITEMS)
        return false;
    for (index = 0; index < (int)count; ++index) {
        desk_world_item *entry = &state->items[state->item_count];
        uint32_t x_bits = 0u;
        uint32_t y_bits = 0u;

        if (!read_string_field(reader, entry->room_id,
                               DESK_WORLD_ROOM_ID_CAPACITY) ||
            entry->room_id[0] == '\0' ||
            !read_item_record(reader, context, &entry->item) ||
            desk_item_is_empty(&entry->item) ||
            !kilixstate_read_u32(reader, &x_bits) ||
            !kilixstate_read_u32(reader, &y_bits) ||
            !kilixstate_read_bool(reader, &entry->placed))
            return false;
        memcpy(&entry->x, &x_bits, sizeof entry->x);
        memcpy(&entry->y, &y_bits, sizeof entry->y);
        entry->room = -1;
        state->item_count++;
    }

    if (!kilixstate_read_u8(reader, &count) ||
        count > (uint8_t)DESK_MAX_RECEIVER_STATES)
        return false;
    for (index = 0; index < (int)count; ++index) {
        desk_receiver_state *entry =
            &state->receivers[state->receiver_count];
        uint32_t remaining = 0u;

        if (!read_string_field(reader, entry->room_id,
                               DESK_WORLD_ROOM_ID_CAPACITY) ||
            entry->room_id[0] == '\0' ||
            !read_string_field(reader, entry->object_id,
                               DESK_WORLD_ROOM_ID_CAPACITY) ||
            entry->object_id[0] == '\0' ||
            !kilixstate_read_u8(reader, &entry->phase) ||
            !kilixstate_read_u32(reader, &remaining) ||
            remaining > (uint32_t)DESK_ITEM_MAX_TICKS ||
            !read_item_record(reader, context, &entry->item))
            return false;
        entry->remaining_ticks = (int32_t)remaining;
        state->receiver_count++;
    }

    if (!kilixstate_read_u8(reader, &count) ||
        count > (uint8_t)DESK_MAX_ACTIVE_EFFECTS)
        return false;
    for (index = 0; index < (int)count; ++index) {
        char id[DESK_ITEM_ID_CAPACITY];
        uint32_t remaining = 0u;
        int found;

        if (!read_string_field(reader, id, sizeof id) || id[0] == '\0' ||
            !kilixstate_read_u32(reader, &remaining) || remaining == 0u ||
            remaining > (uint32_t)DESK_ITEM_MAX_TICKS)
            return false;
        /* A vanished definition simply drops its effect. */
        found = desk_items_find(context->catalog, id);
        if (found > 0) {
            state->effects[state->effect_count].definition =
                (uint16_t)found;
            state->effects[state->effect_count].remaining_ticks =
                (int32_t)remaining;
            state->effect_count++;
        }
    }

    if (!kilixstate_read_u8(reader, &count) ||
        count > (uint8_t)DESK_MAX_SOCIAL_RECORDS)
        return false;
    for (index = 0; index < (int)count; ++index) {
        desk_social_record *record = &state->social[state->social_count];
        uint32_t points = 0u;

        if (!kilixstate_read_u8(reader, &record->cast) ||
            !kilixstate_read_u8(reader, &record->actor) ||
            !kilixstate_read_u32(reader, &points) ||
            !kilixstate_read_u16(reader, &record->gifts) ||
            !kilixstate_read_u16(reader, &record->flags))
            return false;
        record->points = (int32_t)points;
        state->social_count++;
    }
    return kilixstate_reader_require_finished(reader);
}

bool desk_world_state_load(desk_world_state *state,
                           const desk_item_catalog *catalog, bool *corrupt)
{
    static const kilixstate_migration WORLD_MIGRATIONS[] = {
        {(uint32_t)DESK_WORLD_STATE_SCHEMA, 0u, false, decode_world_v1}
    };
    kilixstate_store store;
    uint8_t payload[DESK_WORLD_MAX_PAYLOAD];
    size_t payload_size = 0u;
    desk_world_state staged;
    world_decode_context context;
    kilixstate_result result;

    if (corrupt) *corrupt = false;
    if (!state || !catalog) return false;
    desk_world_state_init(state);
    if (!desk_state_store_open(&store, DESK_WORLD_FILENAME,
                               DESK_WORLD_MAX_PAYLOAD))
        return false;
    result = kilixstate_load(&store, payload, sizeof payload,
                             &payload_size);
    kilixstate_store_close(&store);
    if (result == KILIXSTATE_NOT_FOUND) return true;
    if (result != KILIXSTATE_OK) {
        if (corrupt) *corrupt = true;
        return true;
    }
    desk_world_state_init(&staged);
    context.state = &staged;
    context.catalog = catalog;
    if (kilixstate_migrate(payload, payload_size, WORLD_MIGRATIONS,
                           sizeof WORLD_MIGRATIONS /
                               sizeof WORLD_MIGRATIONS[0],
                           &context, NULL) != KILIXSTATE_CODEC_OK ||
        !desk_world_state_check(&staged, catalog)) {
        if (corrupt) *corrupt = true;
        return true;
    }
    *state = staged;
    return true;
}

bool desk_world_state_reset(void)
{
    kilixstate_store store;
    kilixstate_result result;

    if (!desk_state_store_open(&store, DESK_WORLD_FILENAME,
                               DESK_WORLD_MAX_PAYLOAD))
        return false;
    result = kilixstate_remove(&store);
    kilixstate_store_close(&store);
    return result == KILIXSTATE_OK || result == KILIXSTATE_NOT_FOUND;
}
