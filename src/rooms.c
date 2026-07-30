/* rooms.c — world manifest load + validation.
 *
 * Parses the strict JSON subset used by assets/world/world.json (objects,
 * arrays, strings with the \" \\ \/ \n \t escapes, numbers with an optional
 * fraction, true/false). Unknown keys are schema errors. Parse errors are
 * reported as "world.json:<byte-offset>: <what>"; desk_world_validate
 * mirrors tools/validate_world.py check for check.
 */

#include "kilix_land_desktop.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DESK_WORLD_FILE_CAPACITY (256u * 1024u)

typedef struct world_parser {
    const char *text;
    size_t length;
    size_t offset;
    size_t key_offset; /* start of the most recent object key */
    const char *name;  /* basename used in error prefixes */
    char *error;
    size_t error_size;
} world_parser;

static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static bool fail_at(world_parser *p, size_t offset, const char *format, ...)
{
    char message[112];
    va_list args;

    va_start(args, format);
    (void)vsnprintf(message, sizeof message, format, args);
    va_end(args);
    if (p->error && p->error_size > 0u)
        (void)snprintf(p->error, p->error_size, "%s:%zu: %s", p->name,
                       offset, message);
    return false;
}

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

static void skip_ws(world_parser *p)
{
    while (p->offset < p->length) {
        char c = p->text[p->offset];

        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        p->offset++;
    }
}

static char peek_char(const world_parser *p)
{
    return p->offset < p->length ? p->text[p->offset] : '\0';
}

static bool is_digit_char(char c)
{
    return c >= '0' && c <= '9';
}

static bool expect_char(world_parser *p, char expected)
{
    skip_ws(p);
    if (p->offset >= p->length || p->text[p->offset] != expected)
        return fail_at(p, p->offset, "expected '%c'", expected);
    p->offset++;
    return true;
}

static bool parse_keyword(world_parser *p, const char *word)
{
    size_t word_length = strlen(word);

    if (p->length - p->offset < word_length ||
        strncmp(p->text + p->offset, word, word_length) != 0)
        return false;
    p->offset += word_length;
    return true;
}

static bool parse_string(world_parser *p, char *out, size_t capacity)
{
    size_t start;
    size_t len = 0u;

    skip_ws(p);
    start = p->offset;
    if (p->offset >= p->length || p->text[p->offset] != '"')
        return fail_at(p, p->offset, "expected string");
    p->offset++;
    for (;;) {
        char c;

        if (p->offset >= p->length)
            return fail_at(p, start, "unterminated string");
        c = p->text[p->offset];
        if (c == '"') {
            p->offset++;
            break;
        }
        if ((unsigned char)c < 0x20u)
            return fail_at(p, p->offset, "raw control character in string");
        if (c == '\\') {
            p->offset++;
            if (p->offset >= p->length)
                return fail_at(p, start, "unterminated string");
            switch (p->text[p->offset]) {
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            case '/':
                c = '/';
                break;
            case 'n':
                c = '\n';
                break;
            case 't':
                c = '\t';
                break;
            default:
                return fail_at(p, p->offset, "unsupported escape sequence");
            }
        }
        if (len + 1u >= capacity)
            return fail_at(p, start, "string longer than capacity %zu",
                           capacity - 1u);
        out[len++] = c;
        p->offset++;
    }
    out[len] = '\0';
    return true;
}

static bool parse_number(world_parser *p, float *out)
{
    char scratch[48];
    size_t start;
    size_t span;

    skip_ws(p);
    start = p->offset;
    if (peek_char(p) == '-')
        p->offset++;
    if (!is_digit_char(peek_char(p)))
        return fail_at(p, start, "expected number");
    while (is_digit_char(peek_char(p)))
        p->offset++;
    if (peek_char(p) == '.') {
        p->offset++;
        if (!is_digit_char(peek_char(p)))
            return fail_at(p, p->offset,
                           "expected digit after decimal point");
        while (is_digit_char(peek_char(p)))
            p->offset++;
    }
    span = p->offset - start;
    if (span >= sizeof scratch)
        return fail_at(p, start, "number too long");
    memcpy(scratch, p->text + start, span);
    scratch[span] = '\0';
    *out = strtof(scratch, NULL);
    return true;
}

static bool parse_bool(world_parser *p, bool *out)
{
    size_t start;

    skip_ws(p);
    start = p->offset;
    if (parse_keyword(p, "true")) {
        *out = true;
        return true;
    }
    if (parse_keyword(p, "false")) {
        *out = false;
        return true;
    }
    return fail_at(p, start, "expected true or false");
}

/* 1 = key parsed (name + ':' consumed), 0 = object closed, -1 = error. */
static int next_key(world_parser *p, bool *first, char *key, size_t capacity)
{
    skip_ws(p);
    if (p->offset >= p->length) {
        (void)fail_at(p, p->offset, "unterminated object");
        return -1;
    }
    if (p->text[p->offset] == '}') {
        p->offset++;
        return 0;
    }
    if (!*first && !expect_char(p, ','))
        return -1;
    *first = false;
    skip_ws(p);
    p->key_offset = p->offset;
    if (!parse_string(p, key, capacity))
        return -1;
    if (!expect_char(p, ':'))
        return -1;
    return 1;
}

/* 1 = element follows, 0 = array closed, -1 = error. */
static int next_element(world_parser *p, bool *first)
{
    skip_ws(p);
    if (p->offset >= p->length) {
        (void)fail_at(p, p->offset, "unterminated array");
        return -1;
    }
    if (p->text[p->offset] == ']') {
        p->offset++;
        return 0;
    }
    if (!*first && !expect_char(p, ','))
        return -1;
    *first = false;
    return 1;
}

static bool claim_key(world_parser *p, unsigned *seen, unsigned bit,
                      const char *key)
{
    if (*seen & bit)
        return fail_at(p, p->key_offset, "duplicate key '%s'", key);
    *seen |= bit;
    return true;
}

static bool parse_rect_value(world_parser *p, desk_rect *rect)
{
    static const char *const rect_keys[4] = {"x", "y", "w", "h"};
    bool first = true;
    unsigned seen = 0u;
    size_t start;
    size_t i;
    char key[32];

    skip_ws(p);
    start = p->offset;
    if (!expect_char(p, '{'))
        return false;
    for (;;) {
        float *slot = NULL;
        unsigned bit = 0u;
        int step = next_key(p, &first, key, sizeof key);

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
            return fail_at(p, p->key_offset, "unknown key '%s' in rect",
                           key);
        }
        if (!claim_key(p, &seen, bit, key) || !parse_number(p, slot))
            return false;
    }
    for (i = 0u; i < 4u; ++i)
        if ((seen & (1u << i)) == 0u)
            return fail_at(p, start, "rect missing '%s'", rect_keys[i]);
    return true;
}

static bool parse_point(world_parser *p, float *x, float *y)
{
    bool first = true;
    unsigned seen = 0u;
    size_t start;
    char key[32];

    skip_ws(p);
    start = p->offset;
    if (!expect_char(p, '{'))
        return false;
    for (;;) {
        int step = next_key(p, &first, key, sizeof key);

        if (step < 0)
            return false;
        if (step == 0)
            break;
        if (strcmp(key, "x") == 0) {
            if (!claim_key(p, &seen, 1u << 0, key) || !parse_number(p, x))
                return false;
        } else if (strcmp(key, "y") == 0) {
            if (!claim_key(p, &seen, 1u << 1, key) || !parse_number(p, y))
                return false;
        } else {
            return fail_at(p, p->key_offset, "unknown key '%s' in point",
                           key);
        }
    }
    if ((seen & (1u << 0)) == 0u)
        return fail_at(p, start, "point missing 'x'");
    if ((seen & (1u << 1)) == 0u)
        return fail_at(p, start, "point missing 'y'");
    return true;
}

static bool parse_door(world_parser *p, desk_door *door)
{
    bool first = true;
    unsigned seen = 0u;
    size_t start;
    char key[32];

    door->to_room = -1; /* resolved after every room id is known */
    skip_ws(p);
    start = p->offset;
    if (!expect_char(p, '{'))
        return false;
    for (;;) {
        int step = next_key(p, &first, key, sizeof key);

        if (step < 0)
            return false;
        if (step == 0)
            break;
        if (strcmp(key, "rect") == 0) {
            if (!claim_key(p, &seen, 1u << 0, key) ||
                !parse_rect_value(p, &door->rect))
                return false;
        } else if (strcmp(key, "to") == 0) {
            if (!claim_key(p, &seen, 1u << 1, key) ||
                !parse_string(p, door->to_id, sizeof door->to_id))
                return false;
        } else if (strcmp(key, "spawn") == 0) {
            if (!claim_key(p, &seen, 1u << 2, key) ||
                !parse_point(p, &door->spawn_x, &door->spawn_y))
                return false;
        } else {
            return fail_at(p, p->key_offset, "unknown key '%s' in door",
                           key);
        }
    }
    if ((seen & (1u << 0)) == 0u)
        return fail_at(p, start, "door missing 'rect'");
    if ((seen & (1u << 1)) == 0u)
        return fail_at(p, start, "door missing 'to'");
    if ((seen & (1u << 2)) == 0u)
        return fail_at(p, start, "door missing 'spawn'");
    return true;
}

static bool parse_object_entry(world_parser *p, desk_object *object)
{
    bool first = true;
    unsigned seen = 0u;
    size_t start;
    char key[32];

    skip_ws(p);
    start = p->offset;
    if (!expect_char(p, '{'))
        return false;
    for (;;) {
        int step = next_key(p, &first, key, sizeof key);

        if (step < 0)
            return false;
        if (step == 0)
            break;
        if (strcmp(key, "id") == 0) {
            if (!claim_key(p, &seen, 1u << 0, key) ||
                !parse_string(p, object->id, sizeof object->id))
                return false;
        } else if (strcmp(key, "prompt") == 0) {
            if (!claim_key(p, &seen, 1u << 1, key) ||
                !parse_string(p, object->prompt, sizeof object->prompt))
                return false;
        } else if (strcmp(key, "rect") == 0) {
            if (!claim_key(p, &seen, 1u << 2, key) ||
                !parse_rect_value(p, &object->rect))
                return false;
        } else if (strcmp(key, "target") == 0) {
            char target_name[48];
            size_t value_offset;
            desk_target target;

            if (!claim_key(p, &seen, 1u << 3, key))
                return false;
            skip_ws(p);
            value_offset = p->offset;
            if (!parse_string(p, target_name, sizeof target_name))
                return false;
            target = desk_target_from_string(target_name);
            if (target == DESK_TARGET_NONE)
                return fail_at(p, value_offset, "unknown target '%s'",
                               target_name);
            object->target = target;
        } else {
            return fail_at(p, p->key_offset, "unknown key '%s' in object",
                           key);
        }
    }
    if ((seen & (1u << 0)) == 0u)
        return fail_at(p, start, "object missing 'id'");
    if ((seen & (1u << 1)) == 0u)
        return fail_at(p, start, "object missing 'prompt'");
    if ((seen & (1u << 2)) == 0u)
        return fail_at(p, start, "object missing 'rect'");
    if ((seen & (1u << 3)) == 0u)
        return fail_at(p, start, "object missing 'target'");
    return true;
}

static bool parse_npc(world_parser *p, desk_npc *npc)
{
    bool first = true;
    unsigned seen = 0u;
    size_t start;
    char key[32];

    skip_ws(p);
    start = p->offset;
    if (!expect_char(p, '{'))
        return false;
    for (;;) {
        int step = next_key(p, &first, key, sizeof key);

        if (step < 0)
            return false;
        if (step == 0)
            break;
        if (strcmp(key, "actor") == 0) {
            float value = 0.0f;
            size_t value_offset;
            int actor;

            if (!claim_key(p, &seen, 1u << 0, key))
                return false;
            skip_ws(p);
            value_offset = p->offset;
            if (!parse_number(p, &value))
                return false;
            if (value < -16777216.0f || value > 16777216.0f)
                return fail_at(p, value_offset, "actor out of range");
            actor = (int)value;
            if ((float)actor != value)
                return fail_at(p, value_offset, "actor must be an integer");
            npc->actor = actor;
        } else if (strcmp(key, "x") == 0) {
            if (!claim_key(p, &seen, 1u << 1, key) ||
                !parse_number(p, &npc->x))
                return false;
        } else if (strcmp(key, "y") == 0) {
            if (!claim_key(p, &seen, 1u << 2, key) ||
                !parse_number(p, &npc->y))
                return false;
        } else {
            return fail_at(p, p->key_offset, "unknown key '%s' in npc",
                           key);
        }
    }
    if ((seen & (1u << 0)) == 0u)
        return fail_at(p, start, "npc missing 'actor'");
    if ((seen & (1u << 1)) == 0u)
        return fail_at(p, start, "npc missing 'x'");
    if ((seen & (1u << 2)) == 0u)
        return fail_at(p, start, "npc missing 'y'");
    return true;
}

static bool parse_walkbehind(world_parser *p, desk_walkbehind *walkbehind)
{
    bool first = true;
    unsigned seen = 0u;
    size_t start;
    char key[32];

    skip_ws(p);
    start = p->offset;
    if (!expect_char(p, '{'))
        return false;
    for (;;) {
        int step = next_key(p, &first, key, sizeof key);

        if (step < 0)
            return false;
        if (step == 0)
            break;
        if (strcmp(key, "id") == 0) {
            float value = 0.0f;
            size_t value_offset;
            int id;

            if (!claim_key(p, &seen, 1u << 0, key))
                return false;
            skip_ws(p);
            value_offset = p->offset;
            if (!parse_number(p, &value))
                return false;
            if (value < -16777216.0f || value > 16777216.0f)
                return fail_at(p, value_offset,
                               "walkbehind id out of range");
            id = (int)value;
            if ((float)id != value)
                return fail_at(p, value_offset,
                               "walkbehind id must be an integer");
            walkbehind->id = id;
        } else if (strcmp(key, "baseline") == 0) {
            if (!claim_key(p, &seen, 1u << 1, key) ||
                !parse_number(p, &walkbehind->baseline))
                return false;
        } else {
            return fail_at(p, p->key_offset,
                           "unknown key '%s' in walkbehind", key);
        }
    }
    if ((seen & (1u << 0)) == 0u)
        return fail_at(p, start, "walkbehind missing 'id'");
    if ((seen & (1u << 1)) == 0u)
        return fail_at(p, start, "walkbehind missing 'baseline'");
    return true;
}

static bool parse_obstacle_element(world_parser *p, desk_room *room,
                                   int index)
{
    return parse_rect_value(p, &room->obstacles[index]);
}

static bool parse_door_element(world_parser *p, desk_room *room, int index)
{
    return parse_door(p, &room->doors[index]);
}

static bool parse_object_element(world_parser *p, desk_room *room, int index)
{
    return parse_object_entry(p, &room->objects[index]);
}

static bool parse_npc_element(world_parser *p, desk_room *room, int index)
{
    return parse_npc(p, &room->npcs[index]);
}

static bool parse_walkbehind_element(world_parser *p, desk_room *room,
                                     int index)
{
    return parse_walkbehind(p, &room->walkbehinds[index]);
}

static bool parse_array(world_parser *p, desk_room *room, int *count,
                        int capacity, const char *what,
                        bool (*parse_element)(world_parser *, desk_room *,
                                              int))
{
    bool first = true;

    if (!expect_char(p, '['))
        return false;
    for (;;) {
        int step = next_element(p, &first);

        if (step < 0)
            return false;
        if (step == 0)
            return true;
        if (*count >= capacity)
            return fail_at(p, p->offset, "more than %d %s", capacity, what);
        if (!parse_element(p, room, *count))
            return false;
        (*count)++;
    }
}

static bool parse_room(world_parser *p, desk_room *room)
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

    skip_ws(p);
    start = p->offset;
    if (!expect_char(p, '{'))
        return false;
    for (;;) {
        int step = next_key(p, &first, key, sizeof key);

        if (step < 0)
            return false;
        if (step == 0)
            break;
        if (strcmp(key, "id") == 0) {
            if (!claim_key(p, &seen, 1u << 0, key) ||
                !parse_string(p, room->id, sizeof room->id))
                return false;
        } else if (strcmp(key, "name") == 0) {
            if (!claim_key(p, &seen, 1u << 1, key) ||
                !parse_string(p, room->name, sizeof room->name))
                return false;
        } else if (strcmp(key, "plate") == 0) {
            if (!claim_key(p, &seen, 1u << 2, key) ||
                !parse_string(p, room->plate, sizeof room->plate))
                return false;
        } else if (strcmp(key, "outdoor") == 0) {
            if (!claim_key(p, &seen, 1u << 3, key) ||
                !parse_bool(p, &room->outdoor))
                return false;
        } else if (strcmp(key, "walk") == 0) {
            if (!claim_key(p, &seen, 1u << 4, key) ||
                !parse_rect_value(p, &room->walk))
                return false;
        } else if (strcmp(key, "obstacles") == 0) {
            if (!claim_key(p, &seen, 1u << 5, key) ||
                !parse_array(p, room, &room->obstacle_count,
                             DESK_MAX_OBSTACLES_PER_ROOM, "obstacles",
                             parse_obstacle_element))
                return false;
        } else if (strcmp(key, "doors") == 0) {
            if (!claim_key(p, &seen, 1u << 6, key) ||
                !parse_array(p, room, &room->door_count,
                             DESK_MAX_DOORS_PER_ROOM, "doors",
                             parse_door_element))
                return false;
        } else if (strcmp(key, "objects") == 0) {
            if (!claim_key(p, &seen, 1u << 7, key) ||
                !parse_array(p, room, &room->object_count,
                             DESK_MAX_OBJECTS_PER_ROOM, "objects",
                             parse_object_element))
                return false;
        } else if (strcmp(key, "npcs") == 0) {
            if (!claim_key(p, &seen, 1u << 8, key) ||
                !parse_array(p, room, &room->npc_count,
                             DESK_MAX_NPCS_PER_ROOM, "npcs",
                             parse_npc_element))
                return false;
        } else if (strcmp(key, "walkbehinds") == 0) {
            if (!claim_key(p, &seen, 1u << 9, key) ||
                !parse_array(p, room, &room->walkbehind_count,
                             DESK_MAX_WALKBEHINDS_PER_ROOM, "walkbehinds",
                             parse_walkbehind_element))
                return false;
        } else {
            return fail_at(p, p->key_offset, "unknown key '%s' in room",
                           key);
        }
    }
    for (i = 0u; i < sizeof required / sizeof required[0]; ++i)
        if ((seen & required[i].bit) == 0u)
            return fail_at(p, start, "room missing '%s'", required[i].key);
    return true;
}

static bool parse_rooms(world_parser *p, desk_world *world)
{
    bool first = true;

    if (!expect_char(p, '['))
        return false;
    for (;;) {
        int step = next_element(p, &first);

        if (step < 0)
            return false;
        if (step == 0)
            return true;
        if (world->room_count >= DESK_MAX_ROOMS)
            return fail_at(p, p->offset, "more than %d rooms",
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
    world_parser parser_state;
    FILE *stream;
    size_t bytes;
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

    memset(&parser_state, 0, sizeof parser_state);
    parser_state.name = path ? path_basename(path) : "world.json";
    parser_state.error = error;
    parser_state.error_size = error_size;
    if (!path)
        return fail_at(&parser_state, 0u, "no path given");

    stream = fopen(path, "rb");
    if (!stream)
        return fail_at(&parser_state, 0u, "cannot open file");
    bytes = fread(file_text, 1u, sizeof file_text, stream);
    if (ferror(stream) != 0) {
        (void)fclose(stream);
        return fail_at(&parser_state, 0u, "read failed");
    }
    (void)fclose(stream);
    if (bytes > (size_t)DESK_WORLD_FILE_CAPACITY)
        return fail_at(&parser_state, 0u, "file larger than %u bytes",
                       (unsigned)DESK_WORLD_FILE_CAPACITY);
    parser_state.text = file_text;
    parser_state.length = bytes;

    if (!expect_char(&parser_state, '{'))
        return false;
    for (;;) {
        int step = next_key(&parser_state, &first, key, sizeof key);

        if (step < 0)
            return false;
        if (step == 0)
            break;
        if (strcmp(key, "world") == 0) {
            float schema = 0.0f;
            size_t value_offset;

            if (!claim_key(&parser_state, &seen, 1u << 0, key))
                return false;
            skip_ws(&parser_state);
            value_offset = parser_state.offset;
            if (!parse_number(&parser_state, &schema))
                return false;
            if (schema != 1.0f)
                return fail_at(&parser_state, value_offset,
                               "unsupported schema version (want world: 1)");
        } else if (strcmp(key, "start") == 0) {
            if (!claim_key(&parser_state, &seen, 1u << 1, key))
                return false;
            skip_ws(&parser_state);
            start_offset = parser_state.offset;
            if (!parse_string(&parser_state, start_id, sizeof start_id))
                return false;
        } else if (strcmp(key, "rooms") == 0) {
            if (!claim_key(&parser_state, &seen, 1u << 2, key))
                return false;
            if (!parse_rooms(&parser_state, world))
                return false;
        } else {
            return fail_at(&parser_state, parser_state.key_offset,
                           "unknown key '%s' at top level", key);
        }
    }
    skip_ws(&parser_state);
    if (parser_state.offset != parser_state.length)
        return fail_at(&parser_state, parser_state.offset,
                       "trailing data after JSON document");
    if ((seen & (1u << 0)) == 0u)
        return fail_at(&parser_state, 0u, "missing key 'world'");
    if ((seen & (1u << 1)) == 0u)
        return fail_at(&parser_state, 0u, "missing key 'start'");
    if ((seen & (1u << 2)) == 0u)
        return fail_at(&parser_state, 0u, "missing key 'rooms'");

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
        return fail_at(&parser_state, start_offset,
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
