/* items.c — immutable item catalog plus value-semantics item instances.
 *
 * Parses assets/world/items.json with the shared strict reader
 * (json_reader.c) exactly the way rooms.c parses world.json: unknown keys,
 * duplicate keys, unknown names, and out-of-range numbers are errors with
 * byte offsets, and tools/validate_items.py accepts the same language.
 * After a successful load the catalog is never written again.
 *
 * Ownership rules live here as the only quantity-mutation API: callers
 * build a pure plan, then commit it once. Commits re-verify every slot
 * generation and the expected payload before the first write, so a stale
 * or malformed commit leaves the inventory byte-identical.
 */

#include "items.h"
#include "json_reader.h"

#include <stdio.h>
#include <string.h>

#define DESK_ITEMS_FILE_CAPACITY (64u * 1024u)

/* The compiled tag vocabulary; at most 64 entries so a tag set is one
 * uint64_t. Catalog validation rejects anything else by name. */
static const char *const ITEM_TAG_NAMES[] = {
    "drink",
    "food",
    "media",
    "tool",
    "wearable",
    "placeable",
    "decor",
    "key",
    "giftable",
    "quest",
    "light",
    "discardable",
    "receiver-input",
    "terminal",
};
#define ITEM_TAG_COUNT \
    ((int)(sizeof ITEM_TAG_NAMES / sizeof ITEM_TAG_NAMES[0]))

static const char *const ITEM_FAMILY_NAMES[DESK_ITEM_FAMILY_COUNT] = {
    "portable", "consumable", "tool", "wearable", "placeable", "key"
};

static const char *const ITEM_BEHAVIOR_NAMES[DESK_ITEM_BEHAVIOR_COUNT] = {
    "hold", "drink", "use-tool", "equip", "place", "unlock"
};

static const char *const ITEM_TASTE_CAST_NAMES[] = {
    "legend", "chumrunner", "fantasy", "pleb-bound"
};
#define ITEM_TASTE_CAST_COUNT \
    ((int)(sizeof ITEM_TASTE_CAST_NAMES / sizeof ITEM_TASTE_CAST_NAMES[0]))

int desk_item_tag_index(const char *name)
{
    int index;

    if (!name) return -1;
    for (index = 0; index < ITEM_TAG_COUNT; ++index)
        if (strcmp(ITEM_TAG_NAMES[index], name) == 0) return index;
    return -1;
}

const char *desk_item_tag_name(int index)
{
    if (index < 0 || index >= ITEM_TAG_COUNT) return NULL;
    return ITEM_TAG_NAMES[index];
}

static int family_from_string(const char *name)
{
    int index;

    for (index = 0; index < DESK_ITEM_FAMILY_COUNT; ++index)
        if (strcmp(ITEM_FAMILY_NAMES[index], name) == 0) return index;
    return -1;
}

static int behavior_from_string(const char *name)
{
    int index;

    for (index = 0; index < DESK_ITEM_BEHAVIOR_COUNT; ++index)
        if (strcmp(ITEM_BEHAVIOR_NAMES[index], name) == 0) return index;
    return -1;
}

static int taste_cast_from_string(const char *name)
{
    int index;

    for (index = 0; index < ITEM_TASTE_CAST_COUNT; ++index)
        if (strcmp(ITEM_TASTE_CAST_NAMES[index], name) == 0) return index;
    return -1;
}

static bool id_char_valid(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_';
}

/* "<namespace>:<path>": lowercase namespace of [a-z0-9-_], one colon, a
 * lowercase path of [a-z0-9-_/.] with no "..", no empty segments, and no
 * leading or trailing separator. Unqualified strings never resolve. */
bool desk_item_id_valid(const char *id)
{
    const char *colon;
    const char *cursor;
    size_t length;

    if (!id) return false;
    length = strlen(id);
    if (length < 3u || length >= (size_t)DESK_ITEM_ID_CAPACITY)
        return false;
    colon = strchr(id, ':');
    if (!colon || colon == id || colon[1] == '\0') return false;
    if (strchr(colon + 1, ':')) return false;
    for (cursor = id; cursor < colon; ++cursor)
        if (!id_char_valid(*cursor)) return false;
    if (colon[1] == '/' || colon[1] == '.' || id[length - 1u] == '/' ||
        id[length - 1u] == '.')
        return false;
    for (cursor = colon + 1; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '.') {
            char next = cursor[1];

            if (next == '/' || next == '.') return false;
            continue;
        }
        if (!id_char_valid(*cursor)) return false;
    }
    return true;
}

static bool receiver_id_valid(const char *id)
{
    const char *cursor;

    if (!id || id[0] == '\0') return false;
    for (cursor = id; *cursor != '\0'; ++cursor)
        if (!((*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '-'))
            return false;
    return true;
}

int desk_items_find(const desk_item_catalog *catalog, const char *id)
{
    int index;

    if (!catalog || !id || id[0] == '\0') return -1;
    for (index = 0; index < catalog->definition_count &&
                    index < DESK_MAX_ITEM_DEFS; ++index)
        if (strcmp(catalog->definitions[index].id, id) == 0) return index;
    return -1;
}

const desk_item_def *desk_items_def(const desk_item_catalog *catalog,
                                    uint16_t definition)
{
    if (!catalog || definition >= (uint16_t)catalog->definition_count ||
        definition >= (uint16_t)DESK_MAX_ITEM_DEFS)
        return NULL;
    return &catalog->definitions[definition];
}

int desk_items_find_receiver(const desk_item_catalog *catalog,
                             const char *id)
{
    int index;

    if (!catalog || !id || id[0] == '\0') return -1;
    for (index = 0; index < catalog->receiver_count &&
                    index < DESK_MAX_RECEIVER_RULES; ++index)
        if (strcmp(catalog->receivers[index].id, id) == 0) return index;
    return -1;
}

bool desk_receiver_accepts(const desk_item_catalog *catalog,
                           const desk_receiver_rule *rule,
                           const desk_item *item)
{
    const desk_item_def *def;

    if (!catalog || !rule || !item || desk_item_is_empty(item)) return false;
    /* A recovery item has unknown semantics; it can be carried and
     * discarded, never inserted. */
    if (item->definition == DESK_ITEM_DEF_MISSING) return false;
    def = desk_items_def(catalog, item->definition);
    if (!def) return false;
    switch (rule->match) {
    case DESK_RECEIVER_MATCH_ANY_TAG:
        return (def->tags & rule->tags) != 0u;
    case DESK_RECEIVER_MATCH_ALL_TAGS:
        return (def->tags & rule->tags) == rule->tags;
    case DESK_RECEIVER_MATCH_ITEM:
        return item->definition == rule->item;
    case DESK_RECEIVER_MATCH_FAMILY:
        return def->family == rule->family;
    }
    return false;
}

static bool taste_matches_item(const desk_item_taste_match *match,
                               uint16_t definition)
{
    int index;

    for (index = 0; index < (int)match->item_count; ++index)
        if (match->items[index] == definition) return true;
    return false;
}

desk_taste desk_item_taste(const desk_item_catalog *catalog, int cast,
                           int actor, const desk_item *item)
{
    const desk_item_taste_rule *rule = NULL;
    const desk_item_def *def;
    int index;

    if (!catalog || desk_item_is_empty(item) ||
        item->definition == (uint16_t)DESK_ITEM_DEF_MISSING)
        return DESK_TASTE_NEUTRAL;
    def = desk_items_def(catalog, item->definition);
    if (!def) return DESK_TASTE_NEUTRAL;
    for (index = 0; index < catalog->taste_count; ++index)
        if ((int)catalog->tastes[index].cast == cast &&
            (int)catalog->tastes[index].actor == actor) {
            rule = &catalog->tastes[index];
            break;
        }
    if (!rule) return DESK_TASTE_NEUTRAL;

    /* Exact item matches outrank every tag match. Within either
     * specificity the warmer reaction wins. */
    if (taste_matches_item(&rule->love, item->definition))
        return DESK_TASTE_LOVE;
    if (taste_matches_item(&rule->like, item->definition))
        return DESK_TASTE_LIKE;
    if (taste_matches_item(&rule->dislike, item->definition))
        return DESK_TASTE_DISLIKE;
    if ((def->tags & rule->love.tags) != 0u) return DESK_TASTE_LOVE;
    if ((def->tags & rule->like.tags) != 0u) return DESK_TASTE_LIKE;
    if ((def->tags & rule->dislike.tags) != 0u)
        return DESK_TASTE_DISLIKE;
    return DESK_TASTE_NEUTRAL;
}

/* ---- catalog parsing ---------------------------------------------------- */

/* Receiver accept_item/output references may appear before their
 * definition, so the ids are staged as strings and resolved after the
 * whole document parses. */
typedef struct receiver_staging {
    char item_id[DESK_ITEM_ID_CAPACITY];
    size_t offset; /* of the accept_item value, for the error message */
    char output_id[DESK_ITEM_ID_CAPACITY];
    size_t output_offset;
    bool output_seen;
} receiver_staging;

typedef struct taste_list_staging {
    char item_ids[DESK_MAX_TASTE_ENTRIES][DESK_ITEM_ID_CAPACITY];
    size_t offsets[DESK_MAX_TASTE_ENTRIES];
    uint8_t item_count;
    uint8_t entry_count;
} taste_list_staging;

typedef struct taste_staging {
    taste_list_staging love;
    taste_list_staging like;
    taste_list_staging dislike;
} taste_staging;

static void install_missing_definition(desk_item_catalog *catalog)
{
    desk_item_def *def = &catalog->definitions[DESK_ITEM_DEF_MISSING];

    memset(def, 0, sizeof *def);
    (void)snprintf(def->id, sizeof def->id, "%s", DESK_ITEM_MISSING_ID);
    (void)snprintf(def->name, sizeof def->name, "%s", "Missing Item");
    (void)snprintf(def->description, sizeof def->description, "%s",
                   "Something this desktop no longer recognizes.");
    def->family = DESK_ITEM_PORTABLE;
    def->behavior = (uint16_t)DESK_BEHAVIOR_HOLD;
    def->sprite = 0u;
    /* A recovered stack keeps its saved quantity, so the recovery
     * definition accepts the full stack range. */
    def->max_stack = DESK_ITEM_MAX_STACK_LIMIT;
    def->tags = (desk_item_tags)1u
                << (unsigned int)desk_item_tag_index("discardable");
    catalog->definition_count = 1;
}

static bool parse_bounded_int(desk_json_reader *p, long minimum,
                              long maximum, int32_t *out, const char *what)
{
    float value = 0.0f;
    size_t value_offset;
    int32_t integer;

    desk_json_skip_ws(p);
    value_offset = p->offset;
    if (!desk_json_parse_number(p, &value))
        return false;
    if (value < (float)minimum || value > (float)maximum)
        return desk_json_fail_at(p, value_offset, "%s out of range", what);
    integer = (int32_t)value;
    if ((float)integer != value)
        return desk_json_fail_at(p, value_offset, "%s must be an integer",
                                 what);
    *out = integer;
    return true;
}

static bool parse_tag_array(desk_json_reader *p, desk_item_tags *tags)
{
    bool first = true;

    *tags = 0u;
    if (!desk_json_expect(p, '['))
        return false;
    for (;;) {
        char name[24];
        size_t value_offset;
        int tag;
        int step = desk_json_next_element(p, &first);

        if (step < 0)
            return false;
        if (step == 0)
            return true;
        desk_json_skip_ws(p);
        value_offset = p->offset;
        if (!desk_json_parse_string(p, name, sizeof name))
            return false;
        tag = desk_item_tag_index(name);
        if (tag < 0)
            return desk_json_fail_at(p, value_offset, "unknown tag '%s'",
                                     name);
        if ((*tags & ((desk_item_tags)1u << (unsigned int)tag)) != 0u)
            return desk_json_fail_at(p, value_offset, "duplicate tag '%s'",
                                     name);
        *tags |= (desk_item_tags)1u << (unsigned int)tag;
    }
}

static bool parse_taste_array(desk_json_reader *p,
                              desk_item_taste_match *match,
                              taste_list_staging *staging,
                              const char *tier)
{
    bool first = true;

    if (!desk_json_expect(p, '['))
        return false;
    for (;;) {
        char name[DESK_ITEM_ID_CAPACITY];
        size_t value_offset;
        int index;
        int step = desk_json_next_element(p, &first);

        if (step < 0)
            return false;
        if (step == 0)
            return true;
        desk_json_skip_ws(p);
        value_offset = p->offset;
        if (staging->entry_count >= (uint8_t)DESK_MAX_TASTE_ENTRIES)
            return desk_json_fail_at(
                p, value_offset, "more than %d taste entries in %s",
                DESK_MAX_TASTE_ENTRIES, tier);
        if (!desk_json_parse_string(p, name, sizeof name))
            return false;
        if (strchr(name, ':')) {
            for (index = 0; index < (int)staging->item_count; ++index)
                if (strcmp(staging->item_ids[index], name) == 0)
                    return desk_json_fail_at(
                        p, value_offset,
                        "duplicate taste entry '%s' in %s", name, tier);
            (void)snprintf(staging->item_ids[staging->item_count],
                           sizeof staging->item_ids[staging->item_count],
                           "%s", name);
            staging->offsets[staging->item_count] = value_offset;
            staging->item_count++;
        } else {
            int tag = desk_item_tag_index(name);
            desk_item_tags bit;

            if (tag < 0)
                return desk_json_fail_at(p, value_offset,
                                         "unknown taste entry '%s'", name);
            bit = (desk_item_tags)1u << (unsigned int)tag;
            if ((match->tags & bit) != 0u)
                return desk_json_fail_at(
                    p, value_offset,
                    "duplicate taste entry '%s' in %s", name, tier);
            match->tags |= bit;
        }
        staging->entry_count++;
    }
}

static bool parse_taste(desk_json_reader *p, desk_item_taste_rule *rule,
                        taste_staging *staging)
{
    bool first = true;
    unsigned seen = 0u;
    size_t start;
    char key[32];

    memset(rule, 0, sizeof *rule);
    memset(staging, 0, sizeof *staging);
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
        if (strcmp(key, "cast") == 0) {
            char name[24];
            size_t value_offset;
            int cast;

            if (!desk_json_claim_key(p, &seen, 1u << 0, key))
                return false;
            desk_json_skip_ws(p);
            value_offset = p->offset;
            if (!desk_json_parse_string(p, name, sizeof name))
                return false;
            cast = taste_cast_from_string(name);
            if (cast < 0)
                return desk_json_fail_at(p, value_offset,
                                         "unknown taste cast '%s'", name);
            rule->cast = (uint8_t)cast;
        } else if (strcmp(key, "actor") == 0) {
            int32_t actor = 0;

            if (!desk_json_claim_key(p, &seen, 1u << 1, key) ||
                !parse_bounded_int(p, 1, 3, &actor, "taste actor"))
                return false;
            rule->actor = (uint8_t)actor;
        } else if (strcmp(key, "love") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 2, key) ||
                !parse_taste_array(p, &rule->love, &staging->love,
                                   "love"))
                return false;
        } else if (strcmp(key, "like") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 3, key) ||
                !parse_taste_array(p, &rule->like, &staging->like,
                                   "like"))
                return false;
        } else if (strcmp(key, "dislike") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 4, key) ||
                !parse_taste_array(p, &rule->dislike, &staging->dislike,
                                   "dislike"))
                return false;
        } else {
            return desk_json_fail_at(p, p->key_offset,
                                     "unknown key '%s' in taste", key);
        }
    }
    if ((seen & (1u << 0)) == 0u)
        return desk_json_fail_at(p, start, "taste missing 'cast'");
    if ((seen & (1u << 1)) == 0u)
        return desk_json_fail_at(p, start, "taste missing 'actor'");
    return true;
}

static bool parse_definition(desk_json_reader *p, desk_item_def *def)
{
    bool first = true;
    unsigned seen = 0u;
    size_t start;
    size_t id_offset = 0u;
    size_t effect_offset = 0u;
    char key[32];

    memset(def, 0, sizeof *def);
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
            if (!desk_json_parse_string(p, def->id, sizeof def->id))
                return false;
        } else if (strcmp(key, "name") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 1, key) ||
                !desk_json_parse_string(p, def->name, sizeof def->name))
                return false;
        } else if (strcmp(key, "description") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 2, key) ||
                !desk_json_parse_string(p, def->description,
                                        sizeof def->description))
                return false;
        } else if (strcmp(key, "family") == 0) {
            char name[24];
            size_t value_offset;
            int family;

            if (!desk_json_claim_key(p, &seen, 1u << 3, key))
                return false;
            desk_json_skip_ws(p);
            value_offset = p->offset;
            if (!desk_json_parse_string(p, name, sizeof name))
                return false;
            family = family_from_string(name);
            if (family < 0)
                return desk_json_fail_at(p, value_offset,
                                         "unknown family '%s'", name);
            def->family = (desk_item_family)family;
        } else if (strcmp(key, "behavior") == 0) {
            char name[24];
            size_t value_offset;
            int behavior;

            if (!desk_json_claim_key(p, &seen, 1u << 4, key))
                return false;
            desk_json_skip_ws(p);
            value_offset = p->offset;
            if (!desk_json_parse_string(p, name, sizeof name))
                return false;
            behavior = behavior_from_string(name);
            if (behavior < 0)
                return desk_json_fail_at(p, value_offset,
                                         "unknown behavior '%s'", name);
            def->behavior = (uint16_t)behavior;
        } else if (strcmp(key, "sprite") == 0) {
            int32_t value = 0;

            if (!desk_json_claim_key(p, &seen, 1u << 5, key) ||
                !parse_bounded_int(p, 1, DESK_ITEM_SPRITE_COLUMNS - 1,
                                   &value, "sprite"))
                return false;
            def->sprite = (uint16_t)value;
        } else if (strcmp(key, "max_stack") == 0) {
            int32_t value = 0;

            if (!desk_json_claim_key(p, &seen, 1u << 6, key) ||
                !parse_bounded_int(p, 1, DESK_ITEM_MAX_STACK_LIMIT, &value,
                                   "max_stack"))
                return false;
            def->max_stack = (uint16_t)value;
        } else if (strcmp(key, "tags") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 7, key) ||
                !parse_tag_array(p, &def->tags))
                return false;
        } else if (strcmp(key, "effect_ticks") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 8, key))
                return false;
            desk_json_skip_ws(p);
            effect_offset = p->offset;
            if (!parse_bounded_int(p, 1, DESK_ITEM_MAX_TICKS,
                                   &def->parameter_a, "effect_ticks"))
                return false;
        } else {
            return desk_json_fail_at(p, p->key_offset,
                                     "unknown key '%s' in item", key);
        }
    }
    if ((seen & (1u << 0)) == 0u)
        return desk_json_fail_at(p, start, "item missing 'id'");
    if ((seen & (1u << 1)) == 0u)
        return desk_json_fail_at(p, start, "item missing 'name'");
    if ((seen & (1u << 2)) == 0u)
        return desk_json_fail_at(p, start, "item missing 'description'");
    if ((seen & (1u << 3)) == 0u)
        return desk_json_fail_at(p, start, "item missing 'family'");
    if ((seen & (1u << 4)) == 0u)
        return desk_json_fail_at(p, start, "item missing 'behavior'");
    if ((seen & (1u << 5)) == 0u)
        return desk_json_fail_at(p, start, "item missing 'sprite'");
    if ((seen & (1u << 6)) == 0u)
        return desk_json_fail_at(p, start, "item missing 'max_stack'");
    if (!desk_item_id_valid(def->id))
        return desk_json_fail_at(p, id_offset, "invalid item id '%s'",
                                 def->id);
    if (strcmp(def->id, DESK_ITEM_MISSING_ID) == 0)
        return desk_json_fail_at(p, id_offset, "reserved item id '%s'",
                                 def->id);
    if (def->name[0] == '\0')
        return desk_json_fail_at(p, start, "item name is empty");
    if (def->description[0] == '\0')
        return desk_json_fail_at(p, start, "item description is empty");
    if ((seen & (1u << 8)) != 0u &&
        def->behavior != (uint16_t)DESK_BEHAVIOR_DRINK)
        return desk_json_fail_at(p, effect_offset,
                                 "effect_ticks requires behavior 'drink'");
    return true;
}

static bool parse_receiver(desk_json_reader *p, desk_receiver_rule *rule,
                           receiver_staging *staging)
{
    bool first = true;
    bool accept_seen = false;
    unsigned seen = 0u;
    size_t start;
    size_t id_offset = 0u;
    char key[32];

    memset(rule, 0, sizeof *rule);
    memset(staging, 0, sizeof *staging);
    rule->item = (uint16_t)DESK_ITEM_NONE;
    rule->output = (uint16_t)DESK_ITEM_NONE;
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
            if (!desk_json_parse_string(p, rule->id, sizeof rule->id))
                return false;
        } else if (strcmp(key, "accept_any_tag") == 0 ||
                   strcmp(key, "accept_all_tags") == 0) {
            bool all = key[7] == 'l';

            if (!desk_json_claim_key(p, &seen, 1u << 1, key))
                return false;
            if (accept_seen)
                return desk_json_fail_at(p, p->key_offset,
                                         "more than one accept rule");
            accept_seen = true;
            rule->match = all ? DESK_RECEIVER_MATCH_ALL_TAGS :
                                DESK_RECEIVER_MATCH_ANY_TAG;
            if (!parse_tag_array(p, &rule->tags))
                return false;
            if (rule->tags == 0u)
                return desk_json_fail_at(p, p->key_offset,
                                         "empty accept tag list");
        } else if (strcmp(key, "accept_item") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 2, key))
                return false;
            if (accept_seen)
                return desk_json_fail_at(p, p->key_offset,
                                         "more than one accept rule");
            accept_seen = true;
            rule->match = DESK_RECEIVER_MATCH_ITEM;
            desk_json_skip_ws(p);
            staging->offset = p->offset;
            if (!desk_json_parse_string(p, staging->item_id,
                                        sizeof staging->item_id))
                return false;
        } else if (strcmp(key, "accept_family") == 0) {
            char name[24];
            size_t value_offset;
            int family;

            if (!desk_json_claim_key(p, &seen, 1u << 3, key))
                return false;
            if (accept_seen)
                return desk_json_fail_at(p, p->key_offset,
                                         "more than one accept rule");
            accept_seen = true;
            rule->match = DESK_RECEIVER_MATCH_FAMILY;
            desk_json_skip_ws(p);
            value_offset = p->offset;
            if (!desk_json_parse_string(p, name, sizeof name))
                return false;
            family = family_from_string(name);
            if (family < 0)
                return desk_json_fail_at(p, value_offset,
                                         "unknown family '%s'", name);
            rule->family = (desk_item_family)family;
        } else if (strcmp(key, "consume") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 4, key) ||
                !desk_json_parse_bool(p, &rule->consume))
                return false;
        } else if (strcmp(key, "processing_ticks") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 5, key) ||
                !parse_bounded_int(p, 0, DESK_ITEM_MAX_TICKS,
                                   &rule->processing_ticks,
                                   "processing_ticks"))
                return false;
        } else if (strcmp(key, "result") == 0) {
            char name[32];
            size_t value_offset;

            if (!desk_json_claim_key(p, &seen, 1u << 6, key))
                return false;
            desk_json_skip_ws(p);
            value_offset = p->offset;
            if (!desk_json_parse_string(p, name, sizeof name))
                return false;
            if (strcmp(name, "activate-fixture") == 0) {
                rule->result = DESK_RECEIVER_RESULT_ACTIVATE_FIXTURE;
            } else if (strcmp(name, "none") == 0) {
                rule->result = DESK_RECEIVER_RESULT_NONE;
            } else {
                return desk_json_fail_at(p, value_offset,
                                         "unknown result '%s'", name);
            }
        } else if (strcmp(key, "output") == 0) {
            if (!desk_json_claim_key(p, &seen, 1u << 7, key))
                return false;
            desk_json_skip_ws(p);
            staging->output_offset = p->offset;
            staging->output_seen = true;
            if (!desk_json_parse_string(p, staging->output_id,
                                        sizeof staging->output_id))
                return false;
        } else {
            return desk_json_fail_at(p, p->key_offset,
                                     "unknown key '%s' in receiver", key);
        }
    }
    if ((seen & (1u << 0)) == 0u)
        return desk_json_fail_at(p, start, "receiver missing 'id'");
    if (!accept_seen)
        return desk_json_fail_at(p, start, "receiver missing accept rule");
    if ((seen & (1u << 4)) == 0u)
        return desk_json_fail_at(p, start, "receiver missing 'consume'");
    if ((seen & (1u << 6)) == 0u)
        return desk_json_fail_at(p, start, "receiver missing 'result'");
    if (!receiver_id_valid(rule->id))
        return desk_json_fail_at(p, id_offset, "invalid receiver id '%s'",
                                 rule->id);
    if (staging->output_seen && !rule->consume)
        return desk_json_fail_at(p, staging->output_offset,
                                 "output requires 'consume': true");
    return true;
}

static bool resolve_taste_array(desk_json_reader *p,
                                const desk_item_catalog *catalog,
                                desk_item_taste_match *match,
                                const taste_list_staging *staging)
{
    int index;

    for (index = 0; index < (int)staging->item_count; ++index) {
        int found = desk_items_find(catalog, staging->item_ids[index]);

        if (found <= 0)
            return desk_json_fail_at(p, staging->offsets[index],
                                     "unknown taste entry '%s'",
                                     staging->item_ids[index]);
        match->items[match->item_count] = (uint16_t)found;
        match->item_count++;
    }
    return true;
}

bool desk_items_load(desk_item_catalog *catalog, const char *path,
                     char *error, size_t error_size)
{
    /* Bounded read: items.json is a few KiB; 64 KiB is a hard ceiling. */
    static char file_text[DESK_ITEMS_FILE_CAPACITY + 1u];
    receiver_staging receiver_stages[DESK_MAX_RECEIVER_RULES];
    taste_staging taste_stages[DESK_MAX_TASTE_RULES];
    desk_json_reader parser_state;
    bool first = true;
    unsigned seen = 0u;
    char key[32];
    int index;

    if (error && error_size > 0u)
        error[0] = '\0';
    if (!catalog)
        return false;
    memset(catalog, 0, sizeof *catalog);
    memset(receiver_stages, 0, sizeof receiver_stages);
    memset(taste_stages, 0, sizeof taste_stages);
    install_missing_definition(catalog);

    if (!desk_json_open(&parser_state, path, "items.json", file_text,
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
        if (strcmp(key, "items") == 0) {
            int32_t schema = 0;
            size_t value_offset;

            if (!desk_json_claim_key(&parser_state, &seen, 1u << 0, key))
                return false;
            desk_json_skip_ws(&parser_state);
            value_offset = parser_state.offset;
            if (!parse_bounded_int(&parser_state, 0, 1000000, &schema,
                                   "schema"))
                return false;
            if (schema != 1)
                return desk_json_fail_at(
                    &parser_state, value_offset,
                    "unsupported schema version (want items: 1)");
        } else if (strcmp(key, "definitions") == 0) {
            bool array_first = true;

            if (!desk_json_claim_key(&parser_state, &seen, 1u << 1, key))
                return false;
            if (!desk_json_expect(&parser_state, '['))
                return false;
            for (;;) {
                desk_item_def *def;
                size_t entry_offset;
                int duplicate;
                int step2 = desk_json_next_element(&parser_state,
                                                   &array_first);

                if (step2 < 0)
                    return false;
                if (step2 == 0)
                    break;
                if (catalog->definition_count >= DESK_MAX_ITEM_DEFS)
                    return desk_json_fail_at(&parser_state,
                                             parser_state.offset,
                                             "more than %d items",
                                             DESK_MAX_ITEM_DEFS - 1);
                def = &catalog->definitions[catalog->definition_count];
                desk_json_skip_ws(&parser_state);
                entry_offset = parser_state.offset;
                if (!parse_definition(&parser_state, def))
                    return false;
                for (duplicate = 0;
                     duplicate < catalog->definition_count; ++duplicate)
                    if (strcmp(catalog->definitions[duplicate].id,
                               def->id) == 0)
                        return desk_json_fail_at(&parser_state,
                                                 entry_offset,
                                                 "duplicate item id '%s'",
                                                 def->id);
                catalog->definition_count++;
            }
        } else if (strcmp(key, "receivers") == 0) {
            bool array_first = true;

            if (!desk_json_claim_key(&parser_state, &seen, 1u << 2, key))
                return false;
            if (!desk_json_expect(&parser_state, '['))
                return false;
            for (;;) {
                desk_receiver_rule *rule;
                size_t entry_offset;
                int duplicate;
                int step2 = desk_json_next_element(&parser_state,
                                                   &array_first);

                if (step2 < 0)
                    return false;
                if (step2 == 0)
                    break;
                if (catalog->receiver_count >= DESK_MAX_RECEIVER_RULES)
                    return desk_json_fail_at(&parser_state,
                                             parser_state.offset,
                                             "more than %d receivers",
                                             DESK_MAX_RECEIVER_RULES);
                rule = &catalog->receivers[catalog->receiver_count];
                desk_json_skip_ws(&parser_state);
                entry_offset = parser_state.offset;
                if (!parse_receiver(&parser_state, rule,
                                    &receiver_stages[
                                        catalog->receiver_count]))
                    return false;
                for (duplicate = 0; duplicate < catalog->receiver_count;
                     ++duplicate)
                    if (strcmp(catalog->receivers[duplicate].id,
                               rule->id) == 0)
                        return desk_json_fail_at(
                            &parser_state, entry_offset,
                            "duplicate receiver id '%s'", rule->id);
                catalog->receiver_count++;
            }
        } else if (strcmp(key, "tastes") == 0) {
            bool array_first = true;

            if (!desk_json_claim_key(&parser_state, &seen, 1u << 3, key))
                return false;
            if (!desk_json_expect(&parser_state, '['))
                return false;
            for (;;) {
                desk_item_taste_rule *rule;
                size_t entry_offset;
                int duplicate;
                int step2 = desk_json_next_element(&parser_state,
                                                   &array_first);

                if (step2 < 0)
                    return false;
                if (step2 == 0)
                    break;
                if (catalog->taste_count >= DESK_MAX_TASTE_RULES)
                    return desk_json_fail_at(&parser_state,
                                             parser_state.offset,
                                             "more than %d tastes",
                                             DESK_MAX_TASTE_RULES);
                rule = &catalog->tastes[catalog->taste_count];
                desk_json_skip_ws(&parser_state);
                entry_offset = parser_state.offset;
                if (!parse_taste(&parser_state, rule,
                                 &taste_stages[catalog->taste_count]))
                    return false;
                for (duplicate = 0; duplicate < catalog->taste_count;
                     ++duplicate)
                    if (catalog->tastes[duplicate].cast == rule->cast &&
                        catalog->tastes[duplicate].actor == rule->actor)
                        return desk_json_fail_at(
                            &parser_state, entry_offset,
                            "duplicate taste pair '%s' actor %u",
                            ITEM_TASTE_CAST_NAMES[rule->cast],
                            (unsigned int)rule->actor);
                catalog->taste_count++;
            }
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
        return desk_json_fail_at(&parser_state, 0u, "missing key 'items'");
    if ((seen & (1u << 1)) == 0u)
        return desk_json_fail_at(&parser_state, 0u,
                                 "missing key 'definitions'");

    /* Cross-file references resolve only after the whole document parsed:
     * accept_item and output may name a definition declared later. */
    for (index = 0; index < catalog->receiver_count; ++index) {
        desk_receiver_rule *rule = &catalog->receivers[index];

        if (rule->match == DESK_RECEIVER_MATCH_ITEM) {
            int found = desk_items_find(
                catalog, receiver_stages[index].item_id);

            if (found <= 0)
                return desk_json_fail_at(&parser_state,
                                         receiver_stages[index].offset,
                                         "unknown item '%s' in receiver",
                                         receiver_stages[index].item_id);
            rule->item = (uint16_t)found;
        }
        if (receiver_stages[index].output_seen) {
            int found = desk_items_find(
                catalog, receiver_stages[index].output_id);

            if (found <= 0)
                return desk_json_fail_at(
                    &parser_state, receiver_stages[index].output_offset,
                    "unknown output item '%s' in receiver",
                    receiver_stages[index].output_id);
            rule->output = (uint16_t)found;
        }
    }
    for (index = 0; index < catalog->taste_count; ++index) {
        desk_item_taste_rule *rule = &catalog->tastes[index];

        if (!resolve_taste_array(&parser_state, catalog, &rule->love,
                                 &taste_stages[index].love) ||
            !resolve_taste_array(&parser_state, catalog, &rule->like,
                                 &taste_stages[index].like) ||
            !resolve_taste_array(&parser_state, catalog, &rule->dislike,
                                 &taste_stages[index].dislike))
            return false;
    }
    return true;
}

/* ---- instances ---------------------------------------------------------- */

desk_item desk_item_make(uint16_t definition, uint16_t quantity)
{
    desk_item item;

    memset(&item, 0, sizeof item);
    item.definition = definition;
    item.quantity = quantity;
    return item;
}

bool desk_item_is_empty(const desk_item *item)
{
    return !item || item->quantity == 0u;
}

void desk_item_clear(desk_item *item)
{
    if (item) memset(item, 0, sizeof *item);
}

bool desk_item_valid(const desk_item_catalog *catalog,
                     const desk_item *item)
{
    const desk_item_def *def;

    if (!catalog || !item) return false;
    if (item->quantity == 0u)
        return item->definition == 0u && item->variant == 0u &&
               item->durability == 0u && item->serial == 0u;
    def = desk_items_def(catalog, item->definition);
    if (!def) return false;
    return item->quantity <= def->max_stack;
}

bool desk_item_can_stack(const desk_item_catalog *catalog,
                         const desk_item *a, const desk_item *b)
{
    const desk_item_def *def;

    if (!catalog || desk_item_is_empty(a) || desk_item_is_empty(b))
        return false;
    if (a->definition != b->definition || a->variant != b->variant ||
        a->durability != b->durability)
        return false;
    if (a->serial != 0u || b->serial != 0u) return false;
    def = desk_items_def(catalog, a->definition);
    return def && def->max_stack > 1u;
}

/* ---- inventory ---------------------------------------------------------- */

void desk_inventory_init(desk_inventory *inventory)
{
    if (!inventory) return;
    memset(inventory, 0, sizeof *inventory);
    inventory->selected = 0;
}

int desk_inventory_total(const desk_inventory *inventory,
                         uint16_t definition)
{
    int total = 0;
    int slot;

    if (!inventory) return 0;
    for (slot = 0; slot < DESK_INVENTORY_SLOTS; ++slot)
        if (!desk_item_is_empty(&inventory->slots[slot]) &&
            inventory->slots[slot].definition == definition)
            total += (int)inventory->slots[slot].quantity;
    return total;
}

static void plan_reset(desk_item_plan *plan)
{
    if (plan) memset(plan, 0, sizeof *plan);
}

bool desk_inventory_plan_add(const desk_inventory *inventory,
                             const desk_item_catalog *catalog,
                             const desk_item *item, desk_item_plan *plan)
{
    const desk_item_def *def;
    uint16_t remaining;
    int slot;

    plan_reset(plan);
    if (!inventory || !catalog || !plan || desk_item_is_empty(item) ||
        !desk_item_valid(catalog, item))
        return false;
    def = desk_items_def(catalog, item->definition);
    if (!def) return false;
    remaining = item->quantity;
    /* Merge into compatible stacks first, then open empty slots; the whole
     * quantity must fit or the plan fails (no silent partial add). */
    for (slot = 0; slot < DESK_INVENTORY_SLOTS && remaining > 0u; ++slot) {
        const desk_item *existing = &inventory->slots[slot];
        uint16_t space;
        uint16_t take;

        if (desk_item_is_empty(existing) ||
            !desk_item_can_stack(catalog, existing, item))
            continue;
        space = (uint16_t)(def->max_stack - existing->quantity);
        if (space == 0u) continue;
        take = remaining < space ? remaining : space;
        plan->entries[plan->entry_count].slot = (uint8_t)slot;
        plan->entries[plan->entry_count].count = take;
        plan->entries[plan->entry_count].generation =
            inventory->generation[slot];
        plan->entries[plan->entry_count].creates = false;
        plan->entry_count++;
        remaining = (uint16_t)(remaining - take);
    }
    for (slot = 0; slot < DESK_INVENTORY_SLOTS && remaining > 0u; ++slot) {
        uint16_t take;

        if (!desk_item_is_empty(&inventory->slots[slot])) continue;
        take = remaining < def->max_stack ? remaining : def->max_stack;
        plan->entries[plan->entry_count].slot = (uint8_t)slot;
        plan->entries[plan->entry_count].count = take;
        plan->entries[plan->entry_count].generation =
            inventory->generation[slot];
        plan->entries[plan->entry_count].creates = true;
        plan->entry_count++;
        remaining = (uint16_t)(remaining - take);
    }
    if (remaining > 0u) {
        plan_reset(plan);
        return false;
    }
    plan->kind = (uint8_t)DESK_ITEM_PLAN_ADD;
    plan->total = item->quantity;
    plan->item = *item;
    return true;
}

bool desk_inventory_commit_add(desk_inventory *inventory,
                               const desk_item_catalog *catalog,
                               const desk_item *item,
                               const desk_item_plan *plan)
{
    const desk_item_def *def;
    uint32_t moved = 0u;
    int entry;

    if (!inventory || !catalog || !plan || desk_item_is_empty(item) ||
        plan->kind != (uint8_t)DESK_ITEM_PLAN_ADD ||
        plan->entry_count == 0u ||
        plan->entry_count > (uint8_t)DESK_INVENTORY_SLOTS)
        return false;
    if (memcmp(item, &plan->item, sizeof *item) != 0 ||
        plan->total != item->quantity)
        return false;
    def = desk_items_def(catalog, item->definition);
    if (!def) return false;
    /* Verify everything before the first write. */
    for (entry = 0; entry < (int)plan->entry_count; ++entry) {
        const desk_item_plan_entry *step = &plan->entries[entry];
        const desk_item *existing;
        int check;

        if (step->slot >= (uint8_t)DESK_INVENTORY_SLOTS ||
            step->count == 0u)
            return false;
        for (check = 0; check < entry; ++check)
            if (plan->entries[check].slot == step->slot) return false;
        if (inventory->generation[step->slot] != step->generation)
            return false;
        existing = &inventory->slots[step->slot];
        if (step->creates) {
            if (!desk_item_is_empty(existing) ||
                step->count > def->max_stack)
                return false;
        } else {
            if (desk_item_is_empty(existing) ||
                !desk_item_can_stack(catalog, existing, item) ||
                (uint32_t)existing->quantity + step->count >
                    def->max_stack)
                return false;
        }
        moved += step->count;
    }
    if (moved != plan->total) return false;
    for (entry = 0; entry < (int)plan->entry_count; ++entry) {
        const desk_item_plan_entry *step = &plan->entries[entry];
        desk_item *slot_item = &inventory->slots[step->slot];

        if (step->creates) {
            *slot_item = *item;
            slot_item->quantity = step->count;
        } else {
            slot_item->quantity =
                (uint16_t)(slot_item->quantity + step->count);
        }
        inventory->generation[step->slot] =
            (uint16_t)(inventory->generation[step->slot] + 1u);
    }
    return true;
}

bool desk_inventory_move(desk_inventory *inventory,
                         const desk_item_catalog *catalog, int from,
                         int to)
{
    desk_item *source;
    desk_item *destination;

    if (!inventory || !catalog || from < 0 ||
        from >= DESK_INVENTORY_SLOTS || to < 0 ||
        to >= DESK_INVENTORY_SLOTS)
        return false;
    source = &inventory->slots[from];
    destination = &inventory->slots[to];
    if (!desk_item_valid(catalog, source) ||
        !desk_item_valid(catalog, destination))
        return false;
    if (from == to || desk_item_is_empty(source)) return true;
    if (desk_item_is_empty(destination)) {
        *destination = *source;
        desk_item_clear(source);
    } else if (desk_item_can_stack(catalog, source, destination)) {
        const desk_item_def *def =
            desk_items_def(catalog, source->definition);
        uint16_t space;
        uint16_t moved;

        if (!def || destination->quantity > def->max_stack) return false;
        space = (uint16_t)(def->max_stack - destination->quantity);
        if (space == 0u) return true;
        moved = source->quantity < space ? source->quantity : space;
        destination->quantity =
            (uint16_t)(destination->quantity + moved);
        source->quantity = (uint16_t)(source->quantity - moved);
        if (source->quantity == 0u) desk_item_clear(source);
    } else {
        desk_item swap = *destination;

        *destination = *source;
        *source = swap;
    }
    inventory->generation[from] =
        (uint16_t)(inventory->generation[from] + 1u);
    inventory->generation[to] =
        (uint16_t)(inventory->generation[to] + 1u);
    return true;
}

static bool plan_take(const desk_inventory *inventory, int slot,
                      uint16_t count, desk_item_plan_kind kind,
                      desk_item_plan *plan)
{
    const desk_item *existing;

    plan_reset(plan);
    if (!inventory || !plan || slot < 0 || slot >= DESK_INVENTORY_SLOTS ||
        count == 0u)
        return false;
    existing = &inventory->slots[slot];
    if (desk_item_is_empty(existing) || count > existing->quantity)
        return false;
    plan->kind = (uint8_t)kind;
    plan->entry_count = 1u;
    plan->total = count;
    plan->item = *existing;
    plan->entries[0].slot = (uint8_t)slot;
    plan->entries[0].count = count;
    plan->entries[0].generation = inventory->generation[slot];
    plan->entries[0].creates = false;
    return true;
}

static bool commit_take(desk_inventory *inventory,
                        const desk_item_plan *plan,
                        desk_item_plan_kind kind, desk_item *taken)
{
    const desk_item_plan_entry *step;
    desk_item *slot_item;

    if (taken) desk_item_clear(taken);
    if (!inventory || !plan || !taken ||
        plan->kind != (uint8_t)kind || plan->entry_count != 1u)
        return false;
    step = &plan->entries[0];
    if (step->slot >= (uint8_t)DESK_INVENTORY_SLOTS || step->count == 0u ||
        step->count != plan->total || step->creates)
        return false;
    if (inventory->generation[step->slot] != step->generation)
        return false;
    slot_item = &inventory->slots[step->slot];
    if (memcmp(slot_item, &plan->item, sizeof *slot_item) != 0 ||
        step->count > slot_item->quantity)
        return false;
    *taken = *slot_item;
    taken->quantity = step->count;
    slot_item->quantity = (uint16_t)(slot_item->quantity - step->count);
    if (slot_item->quantity == 0u)
        desk_item_clear(slot_item);
    inventory->generation[step->slot] =
        (uint16_t)(inventory->generation[step->slot] + 1u);
    return true;
}

bool desk_inventory_plan_remove(const desk_inventory *inventory, int slot,
                                uint16_t count, desk_item_plan *plan)
{
    return plan_take(inventory, slot, count, DESK_ITEM_PLAN_REMOVE, plan);
}

bool desk_inventory_commit_remove(desk_inventory *inventory,
                                  const desk_item_plan *plan,
                                  desk_item *removed)
{
    return commit_take(inventory, plan, DESK_ITEM_PLAN_REMOVE, removed);
}

bool desk_item_plan_split_one(const desk_inventory *inventory, int slot,
                              desk_item_plan *plan)
{
    return plan_take(inventory, slot, 1u, DESK_ITEM_PLAN_SPLIT, plan);
}

bool desk_item_commit_split_one(desk_inventory *inventory,
                                const desk_item_plan *plan, desk_item *one)
{
    return commit_take(inventory, plan, DESK_ITEM_PLAN_SPLIT, one);
}
