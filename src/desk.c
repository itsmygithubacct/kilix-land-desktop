#include "kilix_land_desktop.h"
#include "state_store.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Not in the module header; matches kilix-land's dialogue text budget. */
#define DESK_DIALOGUE_TEXT_CAPACITY 192

#define DESK_PROFILE_FILENAME "profile.state"
#define DESK_PROFILE_PAYLOAD_SIZE 96u
#define DESK_PROFILE_MAX_PAYLOAD 256u

typedef struct dialogue_beat {
    int speaker;
    const char *text;
} dialogue_beat;

typedef struct conversation {
    const dialogue_beat *beats;
    int count;
} conversation;

typedef struct cast_profile {
    const char *title;
    const char *subtitle;
    const char *house_name;
    const char *actor_names[DESK_ACTOR_COUNT];
    uint32_t actor_colors[DESK_ACTOR_COUNT];
} cast_profile;

#define CONVERSATION_OF(beats_) \
    {beats_, (int)(sizeof beats_ / sizeof beats_[0])}

/* Titles, subtitles, actor names, and accent colors are copied verbatim from
 * kilix-land's CAST_PROFILES; only the house names are desktop-specific. */
static const cast_profile CAST_PROFILES[DESK_CAST_COUNT] = {
    {
        "LEGEND OF KILIX",
        "KILIX + FRONTIER FRIENDS",
        "HEARTHSIDE COTTAGE",
        {"KILIX", "EMBER BADGER", "BEACON KEEPER", "HARE COURIER"},
        {UINT32_C(0xffa33c), UINT32_C(0xe98243),
         UINT32_C(0x7ed7dc), UINT32_C(0xbadf78)}
    },
    {
        "CHUMRUNNER",
        "ROOK + THE RUNNER CREW",
        "NULL SAFEHOUSE FLAT",
        {"ROOK", "CIPHER", "PATCH", "MARA VELLE"},
        {UINT32_C(0x55d6d0), UINT32_C(0xb69cff),
         UINT32_C(0xf0c36b), UINT32_C(0xff7f6f)}
    },
    {
        "KILIX FANTASY",
        "ARDEN + THE CRYSTAL PARTY",
        "EMBERLIGHT LODGE",
        {"ARDEN", "MIRA", "SOL", "TAMSIN"},
        {UINT32_C(0x55d6d0), UINT32_C(0xb69cff),
         UINT32_C(0x9ee27a), UINT32_C(0xf0c36b)}
    },
    {
        "PLEB BOUND",
        "PIP + THE MAPLE LOOP CREW",
        "MAPLE STREET HOUSE",
        {"PIP", "MO", "DOT", "BERYL"},
        {UINT32_C(0xffc15c), UINT32_C(0x75d8c8),
         UINT32_C(0xff8c87), UINT32_C(0xb69cff)}
    }
};

static const dialogue_beat LEGEND_BADGER_FIRST[] = {
    {DESK_ACTOR_ALLY_1, "Kilix, I oiled every hinge in this cottage except the yard gate. It refused on principle."},
    {DESK_ACTOR_HERO, "A gate with principles. We are already home."},
    {DESK_ACTOR_ALLY_2, "The study computer hums at the same pitch as the west beacon did. I checked twice."},
    {DESK_ACTOR_ALLY_3, "I labeled the kitchen shelves. The emergency biscuit has its own tin now."},
    {DESK_ACTOR_HERO, "Hinges, hum, and biscuit secured. The cottage is officially settled."}
};

static const dialogue_beat LEGEND_BEACON_FIRST[] = {
    {DESK_ACTOR_ALLY_2, "The notice board is the best beacon I have kept. It never blinks and it never lies."},
    {DESK_ACTOR_HERO, "It did once claim we were out of tea."},
    {DESK_ACTOR_ALLY_1, "That was a warning, not a lie. I fixed it with a supply run."},
    {DESK_ACTOR_ALLY_3, "I pinned the route map beside the grocery list. They overlap nicely."},
    {DESK_ACTOR_HERO, "Then the board stands watch, and we keep it honest."}
};

static const dialogue_beat LEGEND_COURIER_FIRST[] = {
    {DESK_ACTOR_ALLY_3, "I timed a run to the mailbox and back. Eleven seconds, door to door."},
    {DESK_ACTOR_HERO, "The postal service has never been so outclassed."},
    {DESK_ACTOR_ALLY_1, "She jumped the yard gate. The gate is still sulking about it."},
    {DESK_ACTOR_ALLY_2, "The mail was one seed catalog and a letter addressed to the house itself."},
    {DESK_ACTOR_HERO, "Then the house reads first. We take turns after."}
};

static const dialogue_beat LEGEND_BADGER_REPEAT[] = {
    {DESK_ACTOR_ALLY_1, "The wardrobe hinge squeaks in a friendly way now. I left it that squeak."},
    {DESK_ACTOR_HERO, "Good. A silent wardrobe cannot be trusted."}
};

static const dialogue_beat LEGEND_BEACON_REPEAT[] = {
    {DESK_ACTOR_ALLY_2, "The stereo and I have an agreement: soft songs before dark."},
    {DESK_ACTOR_HERO, "The finest treaty this cottage has signed."}
};

static const dialogue_beat LEGEND_COURIER_REPEAT[] = {
    {DESK_ACTOR_ALLY_3, "Mailbox status: one catalog, zero mysteries. So far."},
    {DESK_ACTOR_HERO, "Stay ready. Mysteries love a settled house."}
};

static const dialogue_beat CHUM_CIPHER_FIRST[] = {
    {DESK_ACTOR_ALLY_1, "Rook, I swept the flat twice. The only bug is the TV asking for a software update."},
    {DESK_ACTOR_HERO, "Deny it. This safehouse updates on our schedule."},
    {DESK_ACTOR_ALLY_2, "The study rig is clean and fast. I routed its fan noise through the stereo, so it purrs."},
    {DESK_ACTOR_ALLY_3, "The yard gate is locked. For once, that is a feature I approve of."},
    {DESK_ACTOR_HERO, "A quiet flat, a purring rig, a locked gate. Suspiciously ideal. I love it."}
};

static const dialogue_beat CHUM_PATCH_FIRST[] = {
    {DESK_ACTOR_ALLY_2, "I wired the notice board with color codes. Green is chores, red is emergencies."},
    {DESK_ACTOR_HERO, "And the orange note in the middle?"},
    {DESK_ACTOR_ALLY_1, "That one is dinner. Dinner outranks both."},
    {DESK_ACTOR_ALLY_3, "I amended the code. Orange requires immediate response."},
    {DESK_ACTOR_HERO, "Approved. This is the best-run safehouse in the city."}
};

static const dialogue_beat CHUM_MARA_FIRST[] = {
    {DESK_ACTOR_ALLY_3, "I priced everything in this flat out of habit. The wardrobe is worth more than my last job."},
    {DESK_ACTOR_HERO, "It holds my coats, so it is priceless."},
    {DESK_ACTOR_ALLY_1, "She appraised the gate too. Its lock scored higher than the front door's."},
    {DESK_ACTOR_ALLY_2, "Because I upgraded it. Nothing enters that yard uninvited."},
    {DESK_ACTOR_HERO, "Then we are officially the hardest house on a very soft street."}
};

static const dialogue_beat CHUM_CIPHER_REPEAT[] = {
    {DESK_ACTOR_ALLY_1, "Perimeter check: the mailbox contains one flyer for a pizza we already trust."},
    {DESK_ACTOR_HERO, "Keep the flyer. That is actionable intelligence."}
};

static const dialogue_beat CHUM_PATCH_REPEAT[] = {
    {DESK_ACTOR_ALLY_2, "The kitchen appliances are networked now. Do not name them."},
    {DESK_ACTOR_HERO, "Too late. The kettle is Bernard the Second."}
};

static const dialogue_beat CHUM_MARA_REPEAT[] = {
    {DESK_ACTOR_ALLY_3, "The record player is worth keeping. The records are worth guarding."},
    {DESK_ACTOR_HERO, "Post a watch, then. Side two is a masterpiece."}
};

static const dialogue_beat FANTASY_MIRA_FIRST[] = {
    {DESK_ACTOR_ALLY_1, "Arden, the lodge hearth lights itself when someone walks in. I checked for tricks. There are none."},
    {DESK_ACTOR_HERO, "Then the lodge is simply glad to see us. Houses can be like that."},
    {DESK_ACTOR_ALLY_2, "The lantern vine took root by the yard gate. It blooms whenever the gate is bumped."},
    {DESK_ACTOR_ALLY_3, "And the whole archive fits on one bookshelf here. Cozy scholarship at last."},
    {DESK_ACTOR_HERO, "A warm hearth, a watchful vine, a small archive. We defend this comfort."}
};

static const dialogue_beat FANTASY_SOL_FIRST[] = {
    {DESK_ACTOR_ALLY_2, "I taught the kitchen to make berry buns. The kitchen was an eager student."},
    {DESK_ACTOR_HERO, "This lodge grows more strategic by the hour."},
    {DESK_ACTOR_ALLY_1, "The buns emit a faint warmth signature. I logged it for research."},
    {DESK_ACTOR_ALLY_3, "The records indicate morale rises sharply near the oven."},
    {DESK_ACTOR_HERO, "Then the oven is our new rally point. Scholarship demands it."}
};

static const dialogue_beat FANTASY_TAMSIN_FIRST[] = {
    {DESK_ACTOR_ALLY_3, "I catalogued the lodge: five rooms, one locked gate, zero secret passages. So far."},
    {DESK_ACTOR_HERO, "'So far' is my favorite entry in your catalog."},
    {DESK_ACTOR_ALLY_1, "The gate lock predates the fence around it. Curious."},
    {DESK_ACTOR_ALLY_2, "Perhaps it guards the street from us, not the other way around."},
    {DESK_ACTOR_HERO, "Then we respect it. Every good lodge keeps one polite mystery."}
};

static const dialogue_beat FANTASY_MIRA_REPEAT[] = {
    {DESK_ACTOR_ALLY_1, "The star compass rests on the study shelf and points at your chair."},
    {DESK_ACTOR_HERO, "Home, confirmed by science. Excellent."}
};

static const dialogue_beat FANTASY_SOL_REPEAT[] = {
    {DESK_ACTOR_ALLY_2, "I left a berry bun by the computer for your long watch."},
    {DESK_ACTOR_HERO, "The wisest provisioning this party has known."}
};

static const dialogue_beat FANTASY_TAMSIN_REPEAT[] = {
    {DESK_ACTOR_ALLY_3, "Catalog update: still zero secret passages. The floorboards remain suspects."},
    {DESK_ACTOR_HERO, "Question them gently. They hold up everything we love."}
};

static const dialogue_beat PLEB_MO_FIRST[] = {
    {DESK_ACTOR_ALLY_1, "Pip, the living room TV gets one channel we cannot identify. It only shows the ocean."},
    {DESK_ACTOR_HERO, "Do not change it. That is clearly a premium channel."},
    {DESK_ACTOR_ALLY_2, "The couch fits all four of us plus one snack bowl. I measured."},
    {DESK_ACTOR_ALLY_3, "I aligned the antenna with the moon. The ocean got ten percent calmer."},
    {DESK_ACTOR_HERO, "This house tuned itself to us. We stay forever."}
};

static const dialogue_beat PLEB_DOT_FIRST[] = {
    {DESK_ACTOR_ALLY_2, "The neighborhood pigeons inspected our yard gate and filed no complaints."},
    {DESK_ACTOR_HERO, "Finally, a passing grade from the committee."},
    {DESK_ACTOR_ALLY_1, "One pigeon left a sunflower seed on the mailbox. I think it is rent."},
    {DESK_ACTOR_ALLY_3, "I banked the seed. Our assets are growing."},
    {DESK_ACTOR_HERO, "House approved, rent collected. Maple Street suits us."}
};

static const dialogue_beat PLEB_BERYL_FIRST[] = {
    {DESK_ACTOR_ALLY_3, "My moon receiver works better in the kitchen. I suspect the toaster is amplifying it."},
    {DESK_ACTOR_HERO, "Our toaster is gifted. We always suspected."},
    {DESK_ACTOR_ALLY_1, "It also browns bread unevenly, which is a small price for radio astronomy."},
    {DESK_ACTOR_ALLY_2, "The uneven side is the antenna side. That is called engineering."},
    {DESK_ACTOR_HERO, "Toast and telemetry from one appliance. This house is the future."}
};

static const dialogue_beat PLEB_MO_REPEAT[] = {
    {DESK_ACTOR_ALLY_1, "The mystery ocean channel showed a boat today. Big news."},
    {DESK_ACTOR_HERO, "Log it on the notice board. The public deserves to know."}
};

static const dialogue_beat PLEB_DOT_REPEAT[] = {
    {DESK_ACTOR_ALLY_2, "The pigeons nodded at me from the fence this morning."},
    {DESK_ACTOR_HERO, "Nod back. Diplomacy is fragile."}
};

static const dialogue_beat PLEB_BERYL_REPEAT[] = {
    {DESK_ACTOR_ALLY_3, "The moon receiver picked up the gate lock humming at midnight."},
    {DESK_ACTOR_HERO, "Let it hum. Every house needs a hobby."}
};

static const conversation FIRST_CONVERSATIONS[DESK_CAST_COUNT][3] = {
    {
        CONVERSATION_OF(LEGEND_BADGER_FIRST),
        CONVERSATION_OF(LEGEND_BEACON_FIRST),
        CONVERSATION_OF(LEGEND_COURIER_FIRST)
    },
    {
        CONVERSATION_OF(CHUM_CIPHER_FIRST),
        CONVERSATION_OF(CHUM_PATCH_FIRST),
        CONVERSATION_OF(CHUM_MARA_FIRST)
    },
    {
        CONVERSATION_OF(FANTASY_MIRA_FIRST),
        CONVERSATION_OF(FANTASY_SOL_FIRST),
        CONVERSATION_OF(FANTASY_TAMSIN_FIRST)
    },
    {
        CONVERSATION_OF(PLEB_MO_FIRST),
        CONVERSATION_OF(PLEB_DOT_FIRST),
        CONVERSATION_OF(PLEB_BERYL_FIRST)
    }
};

static const conversation REPEAT_CONVERSATIONS[DESK_CAST_COUNT][3] = {
    {
        CONVERSATION_OF(LEGEND_BADGER_REPEAT),
        CONVERSATION_OF(LEGEND_BEACON_REPEAT),
        CONVERSATION_OF(LEGEND_COURIER_REPEAT)
    },
    {
        CONVERSATION_OF(CHUM_CIPHER_REPEAT),
        CONVERSATION_OF(CHUM_PATCH_REPEAT),
        CONVERSATION_OF(CHUM_MARA_REPEAT)
    },
    {
        CONVERSATION_OF(FANTASY_MIRA_REPEAT),
        CONVERSATION_OF(FANTASY_SOL_REPEAT),
        CONVERSATION_OF(FANTASY_TAMSIN_REPEAT)
    },
    {
        CONVERSATION_OF(PLEB_MO_REPEAT),
        CONVERSATION_OF(PLEB_DOT_REPEAT),
        CONVERSATION_OF(PLEB_BERYL_REPEAT)
    }
};

static bool cast_valid(desk_cast cast)
{
    return (int)cast >= 0 && (int)cast < DESK_CAST_COUNT;
}

static void set_toast(desk_state *state, const char *message)
{
    if (!state || !message) return;
    (void)snprintf(state->toast, sizeof state->toast, "%s", message);
    state->toast_ticks = DESK_TOAST_TICKS;
}

static void queue_audio(desk_state *state, desk_audio_event event)
{
    if (!state || state->pending_audio_count < 0 ||
        state->pending_audio_count >=
            (int)(sizeof state->pending_audio /
                  sizeof state->pending_audio[0]))
        return;
    state->pending_audio[state->pending_audio_count] = event;
    state->pending_audio_count++;
}

static const desk_room *current_room(const desk_state *state,
                                     const desk_world *world)
{
    if (!state || !world || world->room_count < 1 ||
        world->room_count > DESK_MAX_ROOMS || state->room < 0 ||
        state->room >= world->room_count)
        return NULL;
    return &world->rooms[state->room];
}

static float point_distance_sq(float ax, float ay, float bx, float by)
{
    float dx = ax - bx;
    float dy = ay - by;
    return dx * dx + dy * dy;
}

static float object_distance_sq(const desk_state *state,
                                const desk_object *object)
{
    return point_distance_sq(state->player_x, state->player_y,
                             object->rect.x + object->rect.w * 0.5f,
                             object->rect.y + object->rect.h * 0.5f);
}

static void update_nearest(desk_state *state, const desk_world *world)
{
    const desk_room *room;
    float limit = DESK_INTERACT_RADIUS * DESK_INTERACT_RADIUS;
    float best;
    int index;
    if (!state) return;
    state->nearest_object = -1;
    state->nearest_npc = -1;
    state->nearest_world_item = -1;
    if (state->mode != DESK_MODE_ROOM) return;
    room = current_room(state, world);
    if (!room) return;
    best = limit;
    for (index = 0; index < state->items.item_count &&
                    index < DESK_MAX_WORLD_ITEMS; ++index) {
        const desk_world_item *entry = &state->items.items[index];
        float distance;
        if (entry->room != state->room) continue;
        distance = point_distance_sq(state->player_x, state->player_y,
                                     entry->x, entry->y);
        if (distance > limit) continue;
        if (state->nearest_world_item < 0 || distance < best) {
            best = distance;
            state->nearest_world_item = index;
        }
    }
    best = limit;
    for (index = 0; index < room->object_count &&
                    index < DESK_MAX_OBJECTS_PER_ROOM; ++index) {
        float distance = object_distance_sq(state, &room->objects[index]);
        if (distance > limit) continue;
        if (state->nearest_object < 0 || distance < best) {
            best = distance;
            state->nearest_object = index;
        }
    }
    /* land's nearest-actor rule: strict compare against the squared radius */
    best = limit;
    for (index = 0; index < room->npc_count &&
                    index < DESK_MAX_NPCS_PER_ROOM; ++index) {
        const desk_npc *npc = &room->npcs[index];
        float distance = point_distance_sq(state->player_x, state->player_y,
                                           npc->x, npc->y);
        if (distance < best) {
            best = distance;
            state->nearest_npc = index;
        }
    }
}

typedef enum desk_pick {
    DESK_PICK_NONE = 0,
    DESK_PICK_ITEM = 1,
    DESK_PICK_NPC = 2,
    DESK_PICK_OBJECT = 3
} desk_pick;

/* One decision for prompt, accent, and interaction: the nearest of the
 * three candidate kinds wins, and on exact ties a pickup beats a talk
 * beats a fixture (the doc's interact priority, radial edition). */
static desk_pick room_pick(const desk_state *state, const desk_room *room)
{
    float item_distance = FLT_MAX;
    float npc_distance = FLT_MAX;
    float object_distance = FLT_MAX;

    if (!state || !room) return DESK_PICK_NONE;
    if (state->nearest_world_item >= 0 &&
        state->nearest_world_item < state->items.item_count) {
        const desk_world_item *entry =
            &state->items.items[state->nearest_world_item];

        item_distance = point_distance_sq(state->player_x, state->player_y,
                                          entry->x, entry->y);
    }
    if (state->nearest_npc >= 0 && state->nearest_npc < room->npc_count) {
        const desk_npc *npc = &room->npcs[state->nearest_npc];

        if (npc->actor >= (int)DESK_ACTOR_ALLY_1 &&
            npc->actor <= (int)DESK_ACTOR_ALLY_3)
            npc_distance = point_distance_sq(state->player_x,
                                             state->player_y, npc->x,
                                             npc->y);
    }
    if (state->nearest_object >= 0 &&
        state->nearest_object < room->object_count)
        object_distance =
            object_distance_sq(state,
                               &room->objects[state->nearest_object]);
    if (item_distance == FLT_MAX && npc_distance == FLT_MAX &&
        object_distance == FLT_MAX)
        return DESK_PICK_NONE;
    if (item_distance <= npc_distance && item_distance <= object_distance)
        return DESK_PICK_ITEM;
    if (npc_distance <= object_distance) return DESK_PICK_NPC;
    return DESK_PICK_OBJECT;
}

static bool npc_selected(const desk_state *state, const desk_room *room)
{
    return room_pick(state, room) == DESK_PICK_NPC;
}

static bool world_item_selected(const desk_state *state,
                                const desk_room *room)
{
    return room_pick(state, room) == DESK_PICK_ITEM;
}

static const conversation *active_conversation(const desk_state *state)
{
    int index;
    uint32_t bit;
    if (!state || !cast_valid(state->profile.cast) ||
        state->conversation_npc < (int)DESK_ACTOR_ALLY_1 ||
        state->conversation_npc > (int)DESK_ACTOR_ALLY_3)
        return NULL;
    index = state->conversation_npc - (int)DESK_ACTOR_ALLY_1;
    bit = UINT32_C(1) << (unsigned int)state->conversation_npc;
    return (state->talked_mask & bit) != 0u ?
           &REPEAT_CONVERSATIONS[state->profile.cast][index] :
           &FIRST_CONVERSATIONS[state->profile.cast][index];
}

static const dialogue_beat *current_beat(const desk_state *state)
{
    const conversation *script = active_conversation(state);
    if (!script || state->dialogue_beat < 0 ||
        state->dialogue_beat >= script->count)
        return NULL;
    return &script->beats[state->dialogue_beat];
}

static void clamp_to_walk(desk_state *state, const desk_room *room)
{
    if (state->player_x < room->walk.x)
        state->player_x = room->walk.x;
    if (state->player_x > room->walk.x + room->walk.w)
        state->player_x = room->walk.x + room->walk.w;
    if (state->player_y < room->walk.y)
        state->player_y = room->walk.y;
    if (state->player_y > room->walk.y + room->walk.h)
        state->player_y = room->walk.y + room->walk.h;
}

/* Movement is axis-exclusive, so pushing back out along the moved axis is a
 * full resolution and leaves the free axis untouched (edge sliding). */
static void resolve_obstacles(desk_state *state, const desk_room *room,
                              int move_x, int move_y)
{
    int index;
    for (index = 0; index < room->obstacle_count &&
                    index < DESK_MAX_OBSTACLES_PER_ROOM; ++index) {
        const desk_rect *rect = &room->obstacles[index];
        if (state->player_x <= rect->x ||
            state->player_x >= rect->x + rect->w ||
            state->player_y <= rect->y ||
            state->player_y >= rect->y + rect->h)
            continue;
        if (move_x < 0)
            state->player_x = rect->x + rect->w;
        else if (move_x > 0)
            state->player_x = rect->x;
        else if (move_y < 0)
            state->player_y = rect->y + rect->h;
        else if (move_y > 0)
            state->player_y = rect->y;
    }
}

static bool point_in_rect(float x, float y, const desk_rect *rect)
{
    return x >= rect->x && x <= rect->x + rect->w && y >= rect->y &&
           y <= rect->y + rect->h;
}

static void check_doors(desk_state *state, const desk_world *world,
                        const desk_room *room)
{
    int index;
    for (index = 0; index < room->door_count &&
                    index < DESK_MAX_DOORS_PER_ROOM; ++index) {
        const desk_door *door = &room->doors[index];
        const desk_room *next;
        if (!point_in_rect(state->player_x, state->player_y, &door->rect))
            continue;
        if (door->to_room < 0 || door->to_room >= world->room_count)
            continue;
        next = &world->rooms[door->to_room];
        state->room = door->to_room;
        state->player_x = door->spawn_x;
        state->player_y = door->spawn_y;
        clamp_to_walk(state, next);
        state->door_cooldown_ticks = DESK_DOOR_COOLDOWN_TICKS;
        set_toast(state, next->name);
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        (void)snprintf(state->profile.last_room,
                       sizeof state->profile.last_room, "%s", next->id);
        state->profile.last_x = state->player_x;
        state->profile.last_y = state->player_y;
        state->profile_dirty = true;
        return;
    }
}

static bool move_cursor(int *cursor, int delta, int count)
{
    int next;
    if (!cursor || delta == 0 || count <= 1) return false;
    next = *cursor + (delta < 0 ? -1 : 1);
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;
    if (next == *cursor) return false;
    *cursor = next;
    return true;
}

static void sync_profile_position(desk_state *state, const desk_world *world)
{
    const desk_room *room = current_room(state, world);
    if (room)
        (void)snprintf(state->profile.last_room,
                       sizeof state->profile.last_room, "%s", room->id);
    state->profile.last_x = state->player_x;
    state->profile.last_y = state->player_y;
    state->profile_dirty = true;
}

static void open_wizard_from_profile(desk_state *state)
{
    state->mode = DESK_MODE_WIZARD;
    state->wizard_step = DESK_WIZARD_CAST;
    state->wizard_cast_cursor = (int)state->profile.cast;
    state->wizard_actor_cursor = (int)state->profile.actor;
    state->wizard_outfit_cursor = (int)state->profile.outfit;
    state->wizard_confirm_cursor = 0;
    (void)snprintf(state->wizard_name, sizeof state->wizard_name, "%s",
                   state->profile.name);
    state->wizard_name_len = (int)strlen(state->wizard_name);
    state->wizard_editing_existing = true;
    state->outfit_dirty = true;
    state->player_moving = false;
    state->nearest_object = -1;
    state->nearest_npc = -1;
    state->nearest_world_item = -1;
}

static bool position_clear(const desk_room *room, float x, float y)
{
    int index;
    if (x < room->walk.x || x > room->walk.x + room->walk.w ||
        y < room->walk.y || y > room->walk.y + room->walk.h)
        return false;
    for (index = 0; index < room->obstacle_count; ++index) {
        const desk_rect *rect = &room->obstacles[index];
        if (x >= rect->x && x <= rect->x + rect->w &&
            y >= rect->y && y <= rect->y + rect->h)
            return false;
    }
    return true;
}

/* Painted worlds can put walls across the walk-rect center, so a fixed
 * center drop may land inside furniture (or a door, which would teleport
 * instantly).  Prefer the center; otherwise the nearest clear 6px cell
 * center outside every door rect. */
static void room_safe_spawn(const desk_room *room, float *x, float *y)
{
    float center_x = room->walk.x + room->walk.w * 0.5f;
    float center_y = room->walk.y + room->walk.h * 0.5f;
    float best_x = center_x;
    float best_y = center_y;
    float best_distance = -1.0f;
    float grid_y;
    if (position_clear(room, center_x, center_y)) {
        *x = center_x;
        *y = center_y;
        return;
    }
    for (grid_y = room->walk.y + 3.0f;
         grid_y <= room->walk.y + room->walk.h; grid_y += 6.0f) {
        float grid_x;
        for (grid_x = room->walk.x + 3.0f;
             grid_x <= room->walk.x + room->walk.w; grid_x += 6.0f) {
            float dx;
            float dy;
            float distance;
            int door;
            bool in_door = false;
            if (!position_clear(room, grid_x, grid_y)) continue;
            for (door = 0; door < room->door_count; ++door) {
                const desk_rect *rect = &room->doors[door].rect;
                if (grid_x >= rect->x && grid_x <= rect->x + rect->w &&
                    grid_y >= rect->y && grid_y <= rect->y + rect->h) {
                    in_door = true;
                    break;
                }
            }
            if (in_door) continue;
            dx = grid_x - center_x;
            dy = grid_y - center_y;
            distance = dx * dx + dy * dy;
            if (best_distance < 0.0f || distance < best_distance) {
                best_distance = distance;
                best_x = grid_x;
                best_y = grid_y;
            }
        }
    }
    *x = best_x;
    *y = best_y;
}

static void commit_wizard(desk_state *state, const desk_world *world)
{
    const desk_room *room = NULL;
    bool cast_changed = !state->profile.first_run_done ||
                        state->profile.cast !=
                            (desk_cast)state->wizard_cast_cursor;
    state->profile.schema = DESK_PROFILE_SCHEMA;
    state->profile.cast = (desk_cast)state->wizard_cast_cursor;
    state->profile.actor = (desk_actor)state->wizard_actor_cursor;
    state->profile.style = (uint32_t)state->wizard_cast_cursor;
    state->profile.outfit = (uint32_t)state->wizard_outfit_cursor;
    (void)snprintf(state->profile.name, sizeof state->profile.name, "%s",
                   state->wizard_name);
    state->profile.first_run_done = true;
    /* A rename or recolor keeps the player where they stand; only a new
     * character (first run or cast change) moves into the new house. */
    if (cast_changed && world && world->room_count > 0 &&
        world->start_room >= 0 && world->start_room < world->room_count &&
        world->room_count <= DESK_MAX_ROOMS) {
        state->room = world->start_room;
        room = &world->rooms[world->start_room];
        room_safe_spawn(room, &state->player_x, &state->player_y);
    }
    if (world && state->room >= 0 && state->room < world->room_count)
        (void)snprintf(state->profile.last_room,
                       sizeof state->profile.last_room, "%s",
                       world->rooms[state->room].id);
    state->profile.last_x = state->player_x;
    state->profile.last_y = state->player_y;
    state->profile_dirty = true;
    state->outfit_dirty = true;
    state->mode = DESK_MODE_ROOM;
    state->facing = DESK_FACING_DOWN;
    state->player_moving = false;
    state->wizard_editing_existing = false;
    state->confirm = DESK_CONFIRM_NONE;
    state->conversation_npc = -1;
    state->dialogue_beat = 0;
    state->dialogue_age = 0;
    if (cast_changed) state->talked_mask = 0u;
    state->profile.talked_mask = state->talked_mask;
    set_toast(state, desk_cast_house_name(state->profile.cast));
    queue_audio(state, DESK_AUDIO_UI_CONFIRM);
    update_nearest(state, world);
}

/* Resolve saved room ids and materialize each unclaimed authored spawn
 * exactly once: the spawn id joins the claimed table the moment the item
 * exists, so a save that later moves or consumes the item can never
 * resurrect it, while a new release's added spawns still appear. */
static void init_world_items(desk_state *state, const desk_world *world)
{
    bool corrupt = false;
    int index;
    int r;

    if (!state->catalog || !world) return;
    if (desk_world_state_load(&state->items, state->catalog, &corrupt) &&
        corrupt) {
        set_toast(state, "The house misplaced a few belongings.");
        state->world_dirty = true;
    }
    for (index = 0; index < state->items.item_count; ++index)
        state->items.items[index].room =
            desk_world_room_index(world, state->items.items[index].room_id);
    for (r = 0; r < world->room_count; ++r) {
        const desk_room *room = &world->rooms[r];

        for (index = 0; index < room->spawn_count; ++index) {
            const desk_item_spawn *spawn = &room->spawns[index];
            desk_world_item *entry;
            int definition;

            if (desk_world_state_is_claimed(&state->items, spawn->id))
                continue;
            definition = desk_items_find(state->catalog, spawn->item);
            if (definition <= 0 ||
                state->items.item_count >= DESK_MAX_WORLD_ITEMS ||
                !desk_world_state_claim(&state->items, spawn->id))
                continue;
            entry = &state->items.items[state->items.item_count];
            memset(entry, 0, sizeof *entry);
            entry->item = desk_item_make((uint16_t)definition,
                                         (uint16_t)spawn->quantity);
            entry->x = spawn->x;
            entry->y = spawn->y;
            entry->placed = false;
            entry->room = r;
            (void)snprintf(entry->room_id, sizeof entry->room_id, "%s",
                           room->id);
            state->items.item_count++;
            state->world_dirty = true;
        }
    }
}

void desk_init(desk_state *state, const desk_world *world,
               const desk_item_catalog *catalog)
{
    const desk_room *room = NULL;
    int room_index = -1;
    bool from_start = false;
    if (!state) return;
    (void)memset(state, 0, sizeof *state);
    state->nearest_object = -1;
    state->nearest_npc = -1;
    state->nearest_world_item = -1;
    state->conversation_npc = -1;
    state->facing = DESK_FACING_DOWN;
    state->pending_launch = DESK_TARGET_NONE;
    state->catalog = catalog;
    desk_world_state_init(&state->items);
    if (world && world->room_count > 0 &&
        world->room_count <= DESK_MAX_ROOMS && world->start_room >= 0 &&
        world->start_room < world->room_count) {
        state->room = world->start_room;
        room = &world->rooms[world->start_room];
        room_safe_spawn(room, &state->player_x, &state->player_y);
    }
    if (desk_profile_load(&state->profile) && state->profile.first_run_done) {
        state->talked_mask = state->profile.talked_mask;
        room_index = world ?
            desk_world_room_index(world, state->profile.last_room) : -1;
        if (room_index < 0 || !world || room_index >= world->room_count) {
            from_start = true;
            room_index = room ? state->room : -1;
        }
        if (room_index >= 0 && world) {
            state->room = room_index;
            room = &world->rooms[room_index];
            if (!from_start) {
                state->player_x = state->profile.last_x;
                state->player_y = state->profile.last_y;
            } else {
                room_safe_spawn(room, &state->player_x, &state->player_y);
            }
            clamp_to_walk(state, room);
            /* The map may have been repainted since this position was
             * saved; never resume inside freshly blocked space. */
            if (!position_clear(room, state->player_x, state->player_y))
                room_safe_spawn(room, &state->player_x, &state->player_y);
        }
        state->mode = DESK_MODE_ROOM;
        state->outfit_dirty = true;
    } else {
        (void)memset(&state->profile, 0, sizeof state->profile);
        state->profile.schema = DESK_PROFILE_SCHEMA;
        state->profile.cast = DESK_CAST_LEGEND;
        state->profile.actor = DESK_ACTOR_HERO;
        state->profile.outfit = 0u;
        if (room)
            (void)snprintf(state->profile.last_room,
                           sizeof state->profile.last_room, "%s", room->id);
        state->profile.last_x = state->player_x;
        state->profile.last_y = state->player_y;
        state->mode = DESK_MODE_WIZARD;
        state->wizard_step = DESK_WIZARD_CAST;
        state->wizard_cast_cursor = 0;
        state->wizard_actor_cursor = 0;
        state->wizard_outfit_cursor = 0;
        state->wizard_confirm_cursor = 0;
        state->wizard_name_len = 0;
        state->wizard_name[0] = '\0';
        state->wizard_editing_existing = false;
        state->outfit_dirty = true;
    }
    init_world_items(state, world);
    update_nearest(state, world);
}

static int room_world_item_count(const desk_world_state *items, int room)
{
    int count = 0;
    int index;

    for (index = 0; index < items->item_count; ++index)
        if (items->items[index].room == room) count++;
    return count;
}

static void front_of_player(const desk_state *state, float *x, float *y)
{
    switch (state->facing) {
    case DESK_FACING_LEFT:
        *x = state->player_x - 20.0f;
        *y = state->player_y;
        return;
    case DESK_FACING_RIGHT:
        *x = state->player_x + 20.0f;
        *y = state->player_y;
        return;
    case DESK_FACING_UP:
        *x = state->player_x;
        *y = state->player_y - 14.0f;
        return;
    case DESK_FACING_DOWN:
    default:
        *x = state->player_x;
        *y = state->player_y + 14.0f;
        return;
    }
}

/* Shared by drop, placement, and the placement ghost: one rulebook for
 * where an item may stand. Returns false with a player-facing reason. */
static bool placement_spot_clear(const desk_state *state,
                                 const desk_room *room, float x, float y,
                                 const char **reason)
{
    const char *why = NULL;
    int index;

    if (!position_clear(room, x, y))
        why = "No clear spot to set that down.";
    if (!why &&
        (room_world_item_count(&state->items, state->room) >=
             DESK_MAX_WORLD_ITEMS_PER_ROOM ||
         state->items.item_count >= DESK_MAX_WORLD_ITEMS))
        why = "This room holds enough already.";
    for (index = 0; !why && index < room->door_count; ++index)
        if (point_in_rect(x, y, &room->doors[index].rect))
            why = "Not in the doorway.";
    for (index = 0; !why && index < room->object_count; ++index)
        if (point_in_rect(x, y, &room->objects[index].rect))
            why = "That spot is taken.";
    for (index = 0; !why && index < room->npc_count; ++index) {
        float dx = x - room->npcs[index].x;
        float dy = y - room->npcs[index].y;

        if (dx * dx + dy * dy < DESK_NPC_SPAWN_EXCLUSION *
                                DESK_NPC_SPAWN_EXCLUSION)
            why = "Give them a little space.";
    }
    if (reason) *reason = why;
    return why == NULL;
}

/* Creates a world item at a validated spot from an already-removed
 * instance. The caller owns ordering (validate, then remove, then
 * settle). */
static void settle_world_item(desk_state *state, const desk_room *room,
                              const desk_item *item, float x, float y,
                              bool placed)
{
    desk_world_item *entry =
        &state->items.items[state->items.item_count];

    memset(entry, 0, sizeof *entry);
    entry->item = *item;
    entry->x = x;
    entry->y = y;
    entry->placed = placed;
    entry->room = state->room;
    (void)snprintf(entry->room_id, sizeof entry->room_id, "%s", room->id);
    state->items.item_count++;
    state->world_dirty = true;
}

static desk_receiver_state *receiver_state_for(desk_state *state,
                                               const desk_room *room,
                                               const desk_object *object,
                                               bool create)
{
    desk_receiver_state *entry;
    int index;

    for (index = 0; index < state->items.receiver_count; ++index) {
        entry = &state->items.receivers[index];
        if (strcmp(entry->room_id, room->id) == 0 &&
            strcmp(entry->object_id, object->id) == 0)
            return entry;
    }
    if (!create ||
        state->items.receiver_count >= DESK_MAX_RECEIVER_STATES)
        return NULL;
    entry = &state->items.receivers[state->items.receiver_count];
    memset(entry, 0, sizeof *entry);
    (void)snprintf(entry->room_id, sizeof entry->room_id, "%s", room->id);
    (void)snprintf(entry->object_id, sizeof entry->object_id, "%s",
                   object->id);
    entry->phase = (uint8_t)DESK_RECEIVER_EMPTY;
    state->items.receiver_count++;
    return entry;
}

static desk_social_record *social_record_for(desk_state *state,
                                             desk_cast cast, int actor,
                                             bool create)
{
    desk_social_record *record;
    int index;

    if (actor < (int)DESK_ACTOR_ALLY_1 || actor > (int)DESK_ACTOR_ALLY_3)
        return NULL;
    for (index = 0; index < state->items.social_count; ++index) {
        record = &state->items.social[index];
        if (record->cast == (uint8_t)cast &&
            record->actor == (uint8_t)actor)
            return record;
    }
    if (!create ||
        state->items.social_count >= DESK_MAX_SOCIAL_RECORDS)
        return NULL;
    record = &state->items.social[state->items.social_count];
    memset(record, 0, sizeof *record);
    record->cast = (uint8_t)cast;
    record->actor = (uint8_t)actor;
    state->items.social_count++;
    return record;
}

const desk_receiver_state *desk_receiver_lookup(const desk_state *state,
                                                const desk_room *room,
                                                const desk_object *object)
{
    int index;

    if (!state || !room || !object) return NULL;
    for (index = 0; index < state->items.receiver_count; ++index) {
        const desk_receiver_state *entry = &state->items.receivers[index];

        if (strcmp(entry->room_id, room->id) == 0 &&
            strcmp(entry->object_id, object->id) == 0)
            return entry;
    }
    return NULL;
}

void desk_select_slot(desk_state *state, int slot)
{
    if (!state || state->mode != DESK_MODE_ROOM || slot < 0 ||
        slot >= DESK_INVENTORY_SLOTS)
        return;
    if (state->items.inventory.selected == slot) return;
    state->items.inventory.selected = slot;
    queue_audio(state, DESK_AUDIO_UI_MOVE);
}

void desk_cycle_slot(desk_state *state, int delta)
{
    int next;

    if (!state || state->mode != DESK_MODE_ROOM || delta == 0) return;
    next = state->items.inventory.selected + (delta < 0 ? -1 : 1);
    if (next < 0) next = DESK_INVENTORY_SLOTS - 1;
    if (next >= DESK_INVENTORY_SLOTS) next = 0;
    state->items.inventory.selected = next;
    queue_audio(state, DESK_AUDIO_UI_MOVE);
}

bool desk_drop_selected(desk_state *state, const desk_world *world)
{
    const desk_room *room;
    const desk_item_def *def;
    const char *reason;
    desk_item_plan plan;
    desk_item removed;
    char message[64];
    float drop_x;
    float drop_y;
    int slot;

    if (!state || !state->catalog || state->mode != DESK_MODE_ROOM ||
        state->action.active)
        return false;
    room = current_room(state, world);
    if (!room) return false;
    slot = state->items.inventory.selected;
    if (slot < 0 || slot >= DESK_INVENTORY_SLOTS ||
        desk_item_is_empty(&state->items.inventory.slots[slot]))
        return false;
    def = desk_items_def(state->catalog,
                         state->items.inventory.slots[slot].definition);
    if (!def) return false;
    front_of_player(state, &drop_x, &drop_y);
    if (!placement_spot_clear(state, room, drop_x, drop_y, &reason)) {
        set_toast(state, reason);
        return false;
    }
    if (!desk_inventory_plan_remove(
            &state->items.inventory, slot,
            state->items.inventory.slots[slot].quantity, &plan) ||
        !desk_inventory_commit_remove(&state->items.inventory, &plan,
                                      &removed))
        return false;
    settle_world_item(state, room, &removed, drop_x, drop_y, false);
    (void)snprintf(message, sizeof message, "Set down %.30s", def->name);
    set_toast(state, message);
    queue_audio(state, DESK_AUDIO_UI_CONFIRM);
    update_nearest(state, world);
    return true;
}

static void update_wizard_cursors(desk_state *state, int move_x, int move_y)
{
    int delta = move_y != 0 ? move_y : move_x;
    switch (state->wizard_step) {
    case DESK_WIZARD_CAST:
        if (move_cursor(&state->wizard_cast_cursor, delta,
                        DESK_CAST_COUNT)) {
            state->outfit_dirty = true;
            queue_audio(state, DESK_AUDIO_UI_MOVE);
        }
        break;
    case DESK_WIZARD_ACTOR:
        if (move_cursor(&state->wizard_actor_cursor, delta,
                        DESK_ACTOR_COUNT))
            queue_audio(state, DESK_AUDIO_UI_MOVE);
        break;
    case DESK_WIZARD_NAME:
        break;
    case DESK_WIZARD_OUTFIT:
        if (move_cursor(&state->wizard_outfit_cursor, delta,
                        DESK_OUTFIT_COUNT)) {
            state->outfit_dirty = true;
            queue_audio(state, DESK_AUDIO_UI_MOVE);
        }
        break;
    case DESK_WIZARD_CONFIRM:
        if (move_cursor(&state->wizard_confirm_cursor, delta, 2))
            queue_audio(state, DESK_AUDIO_UI_MOVE);
        break;
    }
}

/* Logical clip descriptors shared by every cast: frame counts, uniform
 * per-frame tick durations, loop flags, and the authored APPLY frame for
 * one-shot action clips. Which atlas cell a frame shows stays with
 * render.c per cast; gameplay only ever sees these numbers. */
typedef struct desk_clip_descriptor {
    int frames;
    int frame_ticks;
    bool looping;
    int apply_frame; /* -1 = no gameplay event */
} desk_clip_descriptor;

static const desk_clip_descriptor CLIP_TABLE[DESK_CLIP_COUNT] = {
    {4, 11, true, -1}, /* idle */
    {4, 7, true, -1},  /* walk */
    {4, 8, false, 2},  /* use-tool: impact on the strike frame */
    {4, 9, false, 2},  /* drink: swallow near the cup-up frame */
    {4, 8, false, 2},  /* give: transfer at the handoff frame */
};

static void action_reset(desk_state *state)
{
    memset(&state->action, 0, sizeof state->action);
    state->player_animator.movement_locked = false;
    if (state->player_animator.clip != DESK_CLIP_IDLE &&
        state->player_animator.clip != DESK_CLIP_WALK) {
        state->player_animator.clip = DESK_CLIP_IDLE;
        state->player_animator.frame = 0;
        state->player_animator.frame_ticks = 0;
    }
}

/* The exactly-once world mutation, fired by the clip's APPLY frame. Every
 * assumption the plan recorded is re-verified; a stale plan does nothing
 * and the clip simply finishes as presentation. */
static void action_apply(desk_state *state, const desk_world *world)
{
    desk_action_state *action = &state->action;
    const desk_action_plan *plan = &action->plan;
    const desk_room *room = current_room(state, world);
    const desk_item *held;
    const desk_item_def *def;

    if (!action->active || action->committed || !room ||
        (int)plan->room != state->room || !state->catalog)
        return;
    if (plan->inventory_slot >= (uint16_t)DESK_INVENTORY_SLOTS ||
        state->items.inventory.generation[plan->inventory_slot] !=
            plan->inventory_generation)
        return;
    held = &state->items.inventory.slots[plan->inventory_slot];
    def = desk_items_def(state->catalog, held->definition);
    if (desk_item_is_empty(held) || !def) return;
    switch ((desk_action_kind)plan->kind) {
    case DESK_ACTION_USE_TOOL: {
        const desk_object *object;

        if (def->behavior != (uint16_t)DESK_BEHAVIOR_USE_TOOL) return;
        if (plan->object_index >= (uint16_t)room->object_count) return;
        object = &room->objects[plan->object_index];
        if (object->target != plan->launch ||
            strcmp(object->id, plan->object_id) != 0)
            return;
        if (object_distance_sq(state, object) >
            DESK_INTERACT_RADIUS * DESK_INTERACT_RADIUS)
            return;
        /* The impact queues the fixture's own compiled target through
         * the ordinary take-and-clear host path; the tool is not
         * consumed. */
        state->pending_launch = object->target;
        (void)snprintf(state->pending_launch_object,
                       sizeof state->pending_launch_object, "%s",
                       object->id);
        action->committed = true;
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        return;
    }
    case DESK_ACTION_DRINK: {
        desk_item_plan split_plan;
        desk_item one;
        uint16_t definition = held->definition;

        if (def->behavior != (uint16_t)DESK_BEHAVIOR_DRINK) return;
        /* Removal and effect are one commit: the swallow frame either
         * does both or neither. */
        if (!desk_item_plan_split_one(&state->items.inventory,
                                      plan->inventory_slot, &split_plan) ||
            !desk_item_commit_split_one(&state->items.inventory,
                                        &split_plan, &one))
            return;
        if (def->parameter_a > 0) {
            int index;
            int found = -1;

            for (index = 0; index < state->items.effect_count; ++index)
                if (state->items.effects[index].definition == definition)
                    found = index;
            if (found >= 0) {
                state->items.effects[found].remaining_ticks =
                    def->parameter_a;
            } else if (state->items.effect_count <
                       DESK_MAX_ACTIVE_EFFECTS) {
                state->items.effects[state->items.effect_count]
                    .definition = definition;
                state->items.effects[state->items.effect_count]
                    .remaining_ticks = def->parameter_a;
                state->items.effect_count++;
            }
        }
        state->world_dirty = true;
        action->committed = true;
        set_toast(state, "Warm. Focused. Ready.");
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        return;
    }
    case DESK_ACTION_GIVE: {
        const desk_npc *npc;
        desk_social_record *social;
        desk_item_plan split_plan;
        desk_item one;
        char message[64];
        int giftable = desk_item_tag_index("giftable");

        if (plan->npc_index >= (uint16_t)room->npc_count) return;
        npc = &room->npcs[plan->npc_index];
        if (npc->actor < (int)DESK_ACTOR_ALLY_1 ||
            npc->actor > (int)DESK_ACTOR_ALLY_3)
            return;
        if (point_distance_sq(state->player_x, state->player_y, npc->x,
                              npc->y) > DESK_GIFT_REACH * DESK_GIFT_REACH)
            return;
        if (giftable < 0 ||
            (def->tags &
             ((desk_item_tags)1u << (unsigned int)giftable)) == 0u)
            return;
        /* The transfer and the friendship change land together at the
         * handoff frame; the reaction toast reads the same result. */
        if (!desk_item_plan_split_one(&state->items.inventory,
                                      plan->inventory_slot, &split_plan) ||
            !desk_item_commit_split_one(&state->items.inventory,
                                        &split_plan, &one))
            return;
        social = social_record_for(state, state->profile.cast, npc->actor,
                                   true);
        if (social) {
            if (social->points <= 1000000 - 25) social->points += 25;
            if (social->gifts < UINT16_MAX) social->gifts++;
        }
        state->world_dirty = true;
        action->committed = true;
        (void)snprintf(message, sizeof message, "%.20s is delighted.",
                       desk_actor_name(state->profile.cast, npc->actor));
        set_toast(state, message);
        queue_audio(state, DESK_AUDIO_DIALOGUE);
        return;
    }
    case DESK_ACTION_NONE:
    default:
        return;
    }
}

static void action_complete(desk_state *state)
{
    memset(&state->action, 0, sizeof state->action);
    state->player_animator.movement_locked = false;
}

static void advance_player_animator(desk_state *state,
                                    const desk_world *world)
{
    desk_animator *animator = &state->player_animator;
    const desk_clip_descriptor *clip;
    desk_clip_id desired;

    if (state->action.active) {
        /* One-shot action clips run to completion once started. */
        desired = animator->clip;
    } else {
        desired = state->player_moving ? DESK_CLIP_WALK : DESK_CLIP_IDLE;
    }
    if (animator->clip != desired) {
        animator->clip = desired;
        animator->frame = 0;
        animator->frame_ticks = 0;
        return;
    }
    clip = &CLIP_TABLE[animator->clip];
    animator->frame_ticks++;
    if (animator->frame_ticks < clip->frame_ticks) return;
    animator->frame_ticks = 0;
    animator->frame++;
    if (animator->frame >= clip->frames) {
        if (clip->looping) {
            animator->frame = 0;
            return;
        }
        animator->frame = clip->frames - 1;
        if (state->action.active) action_complete(state);
        animator->clip = DESK_CLIP_IDLE;
        animator->frame = 0;
        animator->frame_ticks = 0;
        return;
    }
    if (state->action.active && animator->frame == clip->apply_frame)
        action_apply(state, world);
}

void desk_update(desk_state *state, const desk_world *world, int move_x,
                 int move_y, float seconds)
{
    const desk_room *room;
    int delta;
    if (!state || !world || !isfinite(seconds) || seconds <= 0.0f ||
        seconds > 0.25f)
        return;
    state->player_moving = false;
    room = current_room(state, world);
    if (state->mode == DESK_MODE_ROOM && room &&
        state->player_animator.movement_locked) {
        /* An action clip owns the body until COMPLETE. */
    } else if (state->mode == DESK_MODE_ROOM && room) {
        if (move_x < 0) {
            state->facing = DESK_FACING_LEFT;
            state->player_x -= DESK_PARITY_LEGEND_PLAYER_SPEED * seconds;
            state->player_moving = true;
        } else if (move_x > 0) {
            state->facing = DESK_FACING_RIGHT;
            state->player_x += DESK_PARITY_LEGEND_PLAYER_SPEED * seconds;
            state->player_moving = true;
        } else if (move_y < 0) {
            state->facing = DESK_FACING_UP;
            state->player_y -= DESK_PARITY_LEGEND_PLAYER_SPEED * seconds;
            state->player_moving = true;
        } else if (move_y > 0) {
            state->facing = DESK_FACING_DOWN;
            state->player_y += DESK_PARITY_LEGEND_PLAYER_SPEED * seconds;
            state->player_moving = true;
        }
        clamp_to_walk(state, room);
        resolve_obstacles(state, room, move_x, move_y);
        clamp_to_walk(state, room);
        if (state->door_cooldown_ticks == 0)
            check_doors(state, world, room);
    } else if (state->mode == DESK_MODE_WIZARD) {
        update_wizard_cursors(state, move_x, move_y);
    } else if (state->mode == DESK_MODE_PAUSE) {
        delta = move_y != 0 ? move_y : move_x;
        if (move_cursor(&state->pause_cursor, delta,
                        desk_pause_item_count(state)))
            queue_audio(state, DESK_AUDIO_UI_MOVE);
    } else if (state->mode == DESK_MODE_CONFIRM) {
        delta = move_y != 0 ? move_y : move_x;
        if (move_cursor(&state->confirm_cursor, delta, 2))
            queue_audio(state, DESK_AUDIO_UI_MOVE);
    }
    state->simulation_tick += 1u;
    if (state->toast_ticks > 0) state->toast_ticks--;
    if (state->door_cooldown_ticks > 0) state->door_cooldown_ticks--;
    if (state->mode == DESK_MODE_DIALOGUE) state->dialogue_age++;
    if (state->mode == DESK_MODE_ROOM) {
        bool processing = false;
        int index = 0;

        /* Effects and receiver timers count fixed simulation ticks and
         * pause with the process; there is deliberately no wall-clock
         * or offline progression. */
        while (index < state->items.effect_count) {
            state->items.effects[index].remaining_ticks--;
            if (state->items.effects[index].remaining_ticks <= 0) {
                int shift;

                for (shift = index;
                     shift < state->items.effect_count - 1; ++shift)
                    state->items.effects[shift] =
                        state->items.effects[shift + 1];
                state->items.effect_count--;
                set_toast(state, "The focus fades.");
                state->world_dirty = true;
            } else {
                ++index;
            }
        }
        for (index = 0; index < state->items.receiver_count; ++index) {
            desk_receiver_state *receiver =
                &state->items.receivers[index];

            if (receiver->phase != (uint8_t)DESK_RECEIVER_PROCESSING)
                continue;
            receiver->remaining_ticks--;
            if (receiver->remaining_ticks <= 0) {
                receiver->remaining_ticks = 0;
                receiver->phase = (uint8_t)DESK_RECEIVER_READY;
                state->world_dirty = true;
            } else {
                processing = true;
            }
        }
        /* Modest timer checkpoint: a crash replays at most a minute. */
        if ((state->simulation_tick % 3600u) == 0u &&
            (state->items.effect_count > 0 || processing))
            state->world_dirty = true;
    }
    if (state->mode == DESK_MODE_ROOM || state->mode == DESK_MODE_DIALOGUE ||
        state->mode == DESK_MODE_WIZARD)
        advance_player_animator(state, world);
    update_nearest(state, world);
}

bool desk_use_item(desk_state *state, const desk_world *world)
{
    const desk_room *room;
    const desk_item *held;
    const desk_item_def *def;

    if (!state || !world) return false;
    if (state->mode != DESK_MODE_ROOM)
        return desk_interact(state, world);
    room = current_room(state, world);
    if (!room || !state->catalog) return false;
    if (state->action.active) return false;
    held = &state->items.inventory.slots[state->items.inventory.selected];
    /* Compatibility: an empty hand keeps Space as a second interact
     * button until the hotbar is second nature. */
    if (desk_item_is_empty(held)) return desk_interact(state, world);
    def = desk_items_def(state->catalog, held->definition);
    if (!def) return false;
    if (held->definition == DESK_ITEM_DEF_MISSING) {
        set_toast(state, "Whatever this was, it stays a mystery.");
        return true;
    }
    /* A housemate in front takes priority — but only at handoff reach:
     * giftable items start the give clip, anything else is politely
     * declined without moving. */
    if (npc_selected(state, room) &&
        point_distance_sq(state->player_x, state->player_y,
                          room->npcs[state->nearest_npc].x,
                          room->npcs[state->nearest_npc].y) <=
            DESK_GIFT_REACH * DESK_GIFT_REACH) {
        int giftable = desk_item_tag_index("giftable");

        if (giftable < 0 ||
            (def->tags &
             ((desk_item_tags)1u << (unsigned int)giftable)) == 0u) {
            set_toast(state, "They'd rather not.");
            return true;
        }
        state->action_nonce++;
        memset(&state->action, 0, sizeof state->action);
        state->action.active = true;
        state->action.plan.nonce = state->action_nonce;
        state->action.plan.kind = (uint16_t)DESK_ACTION_GIVE;
        state->action.plan.room = (uint16_t)state->room;
        state->action.plan.inventory_slot =
            (uint16_t)state->items.inventory.selected;
        state->action.plan.inventory_generation =
            state->items.inventory
                .generation[state->items.inventory.selected];
        state->action.plan.object_index = (uint16_t)UINT16_MAX;
        state->action.plan.npc_index = (uint16_t)state->nearest_npc;
        state->player_animator.clip = DESK_CLIP_GIVE;
        state->player_animator.frame = 0;
        state->player_animator.frame_ticks = 0;
        state->player_animator.movement_locked = true;
        state->player_moving = false;
        queue_audio(state, DESK_AUDIO_UI_MOVE);
        return true;
    }
    /* Offer to a fixture receiver before any behavior: the fixture's
     * data-defined rule decides, and an instant accept both moves the
     * item and queues the fixture's own compiled target once. */
    if (state->nearest_object >= 0 &&
        state->nearest_object < room->object_count) {
        const desk_object *object = &room->objects[state->nearest_object];
        int rule_index = object->receiver[0] != '\0' ?
            desk_items_find_receiver(state->catalog, object->receiver) :
            -1;

        if (rule_index >= 0) {
            const desk_receiver_rule *rule =
                &state->catalog->receivers[rule_index];

            if (desk_receiver_accepts(state->catalog, rule, held)) {
                desk_receiver_state *receiver =
                    receiver_state_for(state, room, object, true);
                desk_item_plan plan;
                desk_item one;

                if (!receiver) return false;
                if (receiver->phase != (uint8_t)DESK_RECEIVER_EMPTY) {
                    set_toast(state, "It's already holding something.");
                    return true;
                }
                if (!desk_item_plan_split_one(
                        &state->items.inventory,
                        state->items.inventory.selected, &plan) ||
                    !desk_item_commit_split_one(&state->items.inventory,
                                                &plan, &one))
                    return false;
                if (rule->consume) {
                    /* Consumed input is destroyed once, here. */
                    desk_item_clear(&receiver->item);
                } else {
                    receiver->item = one;
                }
                if (rule->processing_ticks > 0) {
                    receiver->phase = (uint8_t)DESK_RECEIVER_PROCESSING;
                    receiver->remaining_ticks = rule->processing_ticks;
                } else {
                    receiver->phase = rule->consume ?
                        (uint8_t)DESK_RECEIVER_EMPTY :
                        (uint8_t)DESK_RECEIVER_READY;
                    receiver->remaining_ticks = 0;
                    if (rule->result ==
                        DESK_RECEIVER_RESULT_ACTIVATE_FIXTURE) {
                        state->pending_launch = object->target;
                        (void)snprintf(state->pending_launch_object,
                                       sizeof state->pending_launch_object,
                                       "%s", object->id);
                    }
                }
                state->world_dirty = true;
                set_toast(state, "In it goes.");
                queue_audio(state, DESK_AUDIO_UI_CONFIRM);
                return true;
            }
        }
    }
    switch ((desk_item_behavior)def->behavior) {
    case DESK_BEHAVIOR_USE_TOOL: {
        const desk_object *object = NULL;

        /* Tool-to-target dispatch: the tool requests a maintain impact
         * and only the maintenance fixture accepts it today. */
        if (state->nearest_object >= 0 &&
            state->nearest_object < room->object_count &&
            room->objects[state->nearest_object].target ==
                DESK_TARGET_MAINTENANCE)
            object = &room->objects[state->nearest_object];
        if (!object) {
            set_toast(state, "Nothing here needs the toolbox.");
            return true;
        }
        state->action_nonce++;
        memset(&state->action, 0, sizeof state->action);
        state->action.active = true;
        state->action.plan.nonce = state->action_nonce;
        state->action.plan.kind = (uint16_t)DESK_ACTION_USE_TOOL;
        state->action.plan.room = (uint16_t)state->room;
        state->action.plan.inventory_slot =
            (uint16_t)state->items.inventory.selected;
        state->action.plan.inventory_generation =
            state->items.inventory
                .generation[state->items.inventory.selected];
        state->action.plan.object_index =
            (uint16_t)state->nearest_object;
        state->action.plan.npc_index = (uint16_t)UINT16_MAX;
        state->action.plan.launch = object->target;
        (void)snprintf(state->action.plan.object_id,
                       sizeof state->action.plan.object_id, "%s",
                       object->id);
        state->player_animator.clip = DESK_CLIP_USE_TOOL;
        state->player_animator.frame = 0;
        state->player_animator.frame_ticks = 0;
        state->player_animator.movement_locked = true;
        state->player_moving = false;
        queue_audio(state, DESK_AUDIO_UI_MOVE);
        return true;
    }
    case DESK_BEHAVIOR_DRINK:
        state->action_nonce++;
        memset(&state->action, 0, sizeof state->action);
        state->action.active = true;
        state->action.plan.nonce = state->action_nonce;
        state->action.plan.kind = (uint16_t)DESK_ACTION_DRINK;
        state->action.plan.room = (uint16_t)state->room;
        state->action.plan.inventory_slot =
            (uint16_t)state->items.inventory.selected;
        state->action.plan.inventory_generation =
            state->items.inventory
                .generation[state->items.inventory.selected];
        state->action.plan.object_index = (uint16_t)UINT16_MAX;
        state->action.plan.npc_index = (uint16_t)UINT16_MAX;
        state->player_animator.clip = DESK_CLIP_DRINK;
        state->player_animator.frame = 0;
        state->player_animator.frame_ticks = 0;
        state->player_animator.movement_locked = true;
        state->player_moving = false;
        queue_audio(state, DESK_AUDIO_UI_MOVE);
        return true;
    case DESK_BEHAVIOR_PLACE: {
        const char *reason;
        desk_item_plan plan;
        desk_item one;
        float place_x;
        float place_y;

        /* Placement is an instant plan/commit: the render-only ghost
         * previews this exact spot, and a failed placement never
         * consumes anything. */
        front_of_player(state, &place_x, &place_y);
        if (!placement_spot_clear(state, room, place_x, place_y,
                                  &reason)) {
            set_toast(state, reason);
            return true;
        }
        if (!desk_item_plan_split_one(&state->items.inventory,
                                      state->items.inventory.selected,
                                      &plan) ||
            !desk_item_commit_split_one(&state->items.inventory, &plan,
                                        &one))
            return false;
        settle_world_item(state, room, &one, place_x, place_y, true);
        set_toast(state, "There. Perfect spot.");
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        update_nearest(state, world);
        return true;
    }
    case DESK_BEHAVIOR_EQUIP: {
        desk_item_plan take_plan;
        desk_item taken;
        desk_item previous;
        char message[64];

        /* Exclusive ownership swap: the accessory slot takes the item
         * and any previous accessory drops into the freed slot, so both
         * transitions happen exactly once. */
        previous = state->items.equipment[DESK_EQUIP_ACCESSORY];
        if (!desk_inventory_plan_remove(&state->items.inventory,
                                        state->items.inventory.selected,
                                        held->quantity, &take_plan) ||
            !desk_inventory_commit_remove(&state->items.inventory,
                                          &take_plan, &taken))
            return false;
        state->items.equipment[DESK_EQUIP_ACCESSORY] = taken;
        if (!desk_item_is_empty(&previous)) {
            desk_item_plan back_plan;

            if (desk_inventory_plan_add(&state->items.inventory,
                                        state->catalog, &previous,
                                        &back_plan))
                (void)desk_inventory_commit_add(&state->items.inventory,
                                                state->catalog, &previous,
                                                &back_plan);
        }
        state->world_dirty = true;
        (void)snprintf(message, sizeof message, "Wearing the %.24s.",
                       def->name);
        set_toast(state, message);
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        return true;
    }
    case DESK_BEHAVIOR_HOLD:
    case DESK_BEHAVIOR_UNLOCK:
    default:
        set_toast(state, "Not the moment for that.");
        return true;
    }
}

bool desk_placement_preview(const desk_state *state,
                            const desk_world *world, float *x, float *y,
                            bool *valid)
{
    const desk_room *room;
    const desk_item *held;
    const desk_item_def *def;

    if (!state || !world || !x || !y || !valid) return false;
    if (state->mode != DESK_MODE_ROOM || state->action.active ||
        !state->catalog)
        return false;
    room = current_room(state, world);
    if (!room) return false;
    held = &state->items.inventory.slots[state->items.inventory.selected];
    if (desk_item_is_empty(held)) return false;
    def = desk_items_def(state->catalog, held->definition);
    if (!def || def->behavior != (uint16_t)DESK_BEHAVIOR_PLACE)
        return false;
    front_of_player(state, x, y);
    *valid = placement_spot_clear(state, room, *x, *y, NULL);
    return true;
}

/* One trimmed line from a /proc-style fact file; no subprocesses. */
static bool read_fact_line(const char *path, char *line, size_t size)
{
    FILE *handle = fopen(path, "r");
    size_t length;
    if (!handle) return false;
    if (!fgets(line, (int)size, handle)) {
        (void)fclose(handle);
        return false;
    }
    (void)fclose(handle);
    length = strcspn(line, "\n");
    line[length] = '\0';
    return line[0] != '\0';
}

static void open_status_board(desk_state *state, const desk_world *world)
{
    char value[DESK_STATUS_LINE_CAPACITY];
    double uptime = 0.0;
    int count = 0;
    (void)snprintf(state->status_lines[count++], DESK_STATUS_LINE_CAPACITY,
                   "%s of %s", state->profile.name,
                   desk_cast_house_name(state->profile.cast));
    (void)snprintf(state->status_lines[count++], DESK_STATUS_LINE_CAPACITY,
                   "kilix-land-desktop 0.1.0 // %d rooms",
                   world->room_count);
    if (read_fact_line("/proc/sys/kernel/hostname", value, sizeof value))
        (void)snprintf(state->status_lines[count++],
                       DESK_STATUS_LINE_CAPACITY, "host %.56s", value);
    if (read_fact_line("/proc/uptime", value, sizeof value) &&
        sscanf(value, "%lf", &uptime) == 1 && uptime >= 0.0) {
        long minutes = (long)(uptime / 60.0);
        (void)snprintf(state->status_lines[count++],
                       DESK_STATUS_LINE_CAPACITY, "up %ldd %ldh %ldm",
                       minutes / (24L * 60L), (minutes / 60L) % 24L,
                       minutes % 60L);
    }
    if (read_fact_line("/proc/loadavg", value, sizeof value)) {
        value[strcspn(value, " ")] = '\0';
        (void)snprintf(state->status_lines[count++],
                       DESK_STATUS_LINE_CAPACITY, "load %.56s", value);
    }
    state->status_line_count = count;
    state->mode = DESK_MODE_STATUS;
    state->player_moving = false;
}

/* Order-preserving removal so world-item draw order and save order stay
 * deterministic across pickups. */
static void remove_world_item(desk_world_state *items, int index)
{
    int i;

    if (!items || index < 0 || index >= items->item_count) return;
    for (i = index; i < items->item_count - 1; ++i)
        items->items[i] = items->items[i + 1];
    items->item_count--;
    memset(&items->items[items->item_count], 0,
           sizeof items->items[items->item_count]);
    items->items[items->item_count].room = -1;
}

static bool interact_pickup(desk_state *state)
{
    desk_world_item *entry;
    const desk_item_def *def;
    desk_item_plan plan;
    char message[64];

    if (!state->catalog || state->nearest_world_item < 0 ||
        state->nearest_world_item >= state->items.item_count)
        return false;
    entry = &state->items.items[state->nearest_world_item];
    def = desk_items_def(state->catalog, entry->item.definition);
    if (!def) return false;
    if (!desk_inventory_plan_add(&state->items.inventory, state->catalog,
                                 &entry->item, &plan)) {
        /* A full inventory refuses the pickup and moves nothing. */
        set_toast(state, "Hands full. Make room first.");
        return true;
    }
    if (!desk_inventory_commit_add(&state->items.inventory, state->catalog,
                                   &entry->item, &plan))
        return false;
    if (entry->item.quantity > 1u)
        (void)snprintf(message, sizeof message, "Picked up %s x%u",
                       def->name, (unsigned)entry->item.quantity);
    else
        (void)snprintf(message, sizeof message, "Picked up %s", def->name);
    remove_world_item(&state->items, state->nearest_world_item);
    state->nearest_world_item = -1;
    state->world_dirty = true;
    set_toast(state, message);
    queue_audio(state, DESK_AUDIO_UI_CONFIRM);
    return true;
}

/* Collect a READY fixture receiver back into the inventory; a full
 * inventory refuses without touching the receiver's contents. */
static bool interact_collect(desk_state *state, const desk_room *room,
                             const desk_object *object)
{
    desk_receiver_state *receiver =
        receiver_state_for(state, room, object, false);
    const desk_item_def *def;
    desk_item_plan plan;
    char message[64];

    if (!receiver) return false;
    if (receiver->phase == (uint8_t)DESK_RECEIVER_PROCESSING) {
        set_toast(state, "Still working on it.");
        return true;
    }
    if (receiver->phase != (uint8_t)DESK_RECEIVER_READY) return false;
    def = desk_items_def(state->catalog, receiver->item.definition);
    if (!def) return false;
    if (!desk_inventory_plan_add(&state->items.inventory, state->catalog,
                                 &receiver->item, &plan)) {
        set_toast(state, "Hands full. Make room first.");
        return true;
    }
    if (!desk_inventory_commit_add(&state->items.inventory,
                                   state->catalog, &receiver->item, &plan))
        return false;
    desk_item_clear(&receiver->item);
    receiver->phase = (uint8_t)DESK_RECEIVER_EMPTY;
    receiver->remaining_ticks = 0;
    state->world_dirty = true;
    (void)snprintf(message, sizeof message, "Took back the %.28s",
                   def->name);
    set_toast(state, message);
    queue_audio(state, DESK_AUDIO_UI_CONFIRM);
    return true;
}

static bool interact_room(desk_state *state, const desk_world *world,
                          const desk_room *room)
{
    const desk_object *object;
    if (!room) return false;
    /* The body is busy until the action clip completes or cancels. */
    if (state->action.active) return false;
    if (world_item_selected(state, room))
        return interact_pickup(state);
    if (npc_selected(state, room)) {
        state->mode = DESK_MODE_DIALOGUE;
        state->conversation_npc = room->npcs[state->nearest_npc].actor;
        state->dialogue_beat = 0;
        state->dialogue_age = 0;
        state->player_moving = false;
        state->nearest_npc = -1;
        state->nearest_object = -1;
        queue_audio(state, DESK_AUDIO_DIALOGUE);
        return true;
    }
    if (state->nearest_object < 0 ||
        state->nearest_object >= room->object_count)
        return false;
    object = &room->objects[state->nearest_object];
    /* A ready receiver collects before the fixture activates. */
    if (object->receiver[0] != '\0' &&
        interact_collect(state, room, object))
        return true;
    switch (object->target) {
    case DESK_TARGET_WARDROBE:
        open_wizard_from_profile(state);
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        return true;
    case DESK_TARGET_BED:
        state->mode = DESK_MODE_CONFIRM;
        state->confirm = DESK_CONFIRM_QUIT;
        state->confirm_cursor = 1;
        state->player_moving = false;
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        return true;
    case DESK_TARGET_STATUS_BOARD:
        open_status_board(state, world);
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        return true;
    case DESK_TARGET_GATE_LOCKED:
        set_toast(state, "The street is quiet today.");
        return true;
    case DESK_TARGET_NONE:
        return false;
    default:
        state->pending_launch = object->target;
        (void)snprintf(state->pending_launch_object,
                       sizeof state->pending_launch_object, "%s",
                       object->id);
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        return true;
    }
}

static bool interact_dialogue(desk_state *state)
{
    const conversation *script;
    const char *text;
    size_t length;
    text = desk_dialogue_text(state);
    length = strlen(text);
    if (desk_dialogue_visible_chars(state) < length) {
        state->dialogue_age =
            (int)(length * (size_t)DESK_DIALOGUE_REVEAL_TICKS_PER_CHAR);
        return true;
    }
    script = active_conversation(state);
    if (!script) {
        state->mode = DESK_MODE_ROOM;
        state->conversation_npc = -1;
        state->dialogue_beat = 0;
        state->dialogue_age = 0;
        return false;
    }
    state->dialogue_beat++;
    state->dialogue_age = 0;
    if (state->dialogue_beat >= script->count) {
        uint32_t bit = UINT32_C(1) <<
                       (unsigned int)state->conversation_npc;
        desk_social_record *social =
            social_record_for(state, state->profile.cast,
                              state->conversation_npc, true);

        if (social && social->points <= 1000000 - 2) {
            social->points += 2;
            state->world_dirty = true;
        }
        state->talked_mask |= bit;
        state->profile.talked_mask = state->talked_mask;
        state->profile_dirty = true;
        state->mode = DESK_MODE_ROOM;
        state->conversation_npc = -1;
        state->dialogue_beat = 0;
        set_toast(state, "The house settles into an easy quiet.");
    } else {
        queue_audio(state, DESK_AUDIO_DIALOGUE);
    }
    return true;
}

static bool interact_wizard(desk_state *state, const desk_world *world)
{
    switch (state->wizard_step) {
    case DESK_WIZARD_CAST:
        state->wizard_step = DESK_WIZARD_ACTOR;
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        return true;
    case DESK_WIZARD_ACTOR:
        if (state->wizard_actor_cursor != (int)DESK_ACTOR_HERO) {
            set_toast(state, "Allies move in as housemates");
            return false;
        }
        state->wizard_step = DESK_WIZARD_NAME;
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        return true;
    case DESK_WIZARD_NAME:
        if (state->wizard_name_len < 1) {
            set_toast(state, "Every hero signs the lease. Type a name.");
            return false;
        }
        state->wizard_step = DESK_WIZARD_OUTFIT;
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        return true;
    case DESK_WIZARD_OUTFIT:
        state->wizard_step = DESK_WIZARD_CONFIRM;
        state->wizard_confirm_cursor = 0;
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        return true;
    case DESK_WIZARD_CONFIRM:
        if (state->wizard_confirm_cursor != 0) {
            state->wizard_step = DESK_WIZARD_OUTFIT;
            queue_audio(state, DESK_AUDIO_UI_MOVE);
            return true;
        }
        if (state->wizard_editing_existing &&
            state->profile.first_run_done &&
            (desk_cast)state->wizard_cast_cursor != state->profile.cast) {
            state->mode = DESK_MODE_CONFIRM;
            state->confirm = DESK_CONFIRM_CAST_CHANGE;
            state->confirm_cursor = 1;
            queue_audio(state, DESK_AUDIO_UI_CONFIRM);
            return true;
        }
        commit_wizard(state, world);
        return true;
    }
    return false;
}

/* The Debug entry is governed by desktop.conf in the config home; absent
 * file or key means enabled — it is a debug DESKTOP, after all. */
static bool debug_menu_enabled(void)
{
    char path[512];
    char line[128];
    const char *override_dir = getenv("KILIX_LAND_DESKTOP_CONFIG_HOME");
    const char *home = getenv("HOME");
    FILE *handle;
    int written;
    bool enabled = true;
    if (override_dir && override_dir[0] == '/')
        written = snprintf(path, sizeof path, "%s/desktop.conf",
                           override_dir);
    else if (home && home[0] == '/')
        written = snprintf(path, sizeof path,
                           "%s/.local/gpu_terminal/kilix-land-desktop/"
                           "desktop.conf", home);
    else
        return true;
    if (written < 0 || (size_t)written >= sizeof path) return true;
    handle = fopen(path, "r");
    if (!handle) return true;
    while (fgets(line, (int)sizeof line, handle)) {
        char *cursor = line;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (strncmp(cursor, "debug_menu", 10u) != 0) continue;
        cursor += 10u;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '=')
            ++cursor;
        enabled = !(strncmp(cursor, "off", 3u) == 0 || *cursor == '0');
        break;
    }
    (void)fclose(handle);
    return enabled;
}

int desk_pause_item_count(const desk_state *state)
{
    if (!state) return 0;
    if (state->pause_debug) return 2;
    return state->debug_menu ? 4 : 3;
}

const char *desk_pause_item(const desk_state *state, int index)
{
    static const char *const base[3] = { "RESUME", "CHARACTER", "QUIT" };
    static const char *const with_debug[4] = {
        "RESUME", "CHARACTER", "DEBUG", "QUIT"
    };
    static const char *const debug_items[2] = { "WALK EDITOR", "BACK" };
    if (!state || index < 0 || index >= desk_pause_item_count(state))
        return "";
    if (state->pause_debug) return debug_items[index];
    return state->debug_menu ? with_debug[index] : base[index];
}

static bool interact_pause(desk_state *state, const desk_world *world)
{
    const char *item = desk_pause_item(state, state->pause_cursor);
    if (strcmp(item, "RESUME") == 0) {
        state->mode = DESK_MODE_ROOM;
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        update_nearest(state, world);
        return true;
    }
    if (strcmp(item, "CHARACTER") == 0) {
        open_wizard_from_profile(state);
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        return true;
    }
    if (strcmp(item, "DEBUG") == 0) {
        state->pause_debug = true;
        state->pause_cursor = 0;
        queue_audio(state, DESK_AUDIO_UI_MOVE);
        return true;
    }
    if (strcmp(item, "WALK EDITOR") == 0) {
        state->pending_launch = DESK_TARGET_WALK_EDITOR;
        (void)memset(state->pending_launch_object, 0,
                     sizeof state->pending_launch_object);
        state->pause_debug = false;
        state->mode = DESK_MODE_ROOM;
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        update_nearest(state, world);
        return true;
    }
    if (strcmp(item, "BACK") == 0) {
        state->pause_debug = false;
        state->pause_cursor = 0;
        queue_audio(state, DESK_AUDIO_UI_MOVE);
        return true;
    }
    state->mode = DESK_MODE_CONFIRM;
    state->confirm = DESK_CONFIRM_QUIT;
    state->confirm_cursor = 1;
    queue_audio(state, DESK_AUDIO_UI_CONFIRM);
    return true;
}

static bool interact_confirm(desk_state *state, const desk_world *world)
{
    if (state->confirm == DESK_CONFIRM_QUIT) {
        if (state->confirm_cursor == 0) {
            sync_profile_position(state, world);
            state->quit_requested = true;
        }
        state->confirm = DESK_CONFIRM_NONE;
        state->mode = DESK_MODE_ROOM;
        queue_audio(state, DESK_AUDIO_UI_CONFIRM);
        update_nearest(state, world);
        return true;
    }
    if (state->confirm == DESK_CONFIRM_CAST_CHANGE) {
        state->confirm = DESK_CONFIRM_NONE;
        if (state->confirm_cursor == 0) {
            commit_wizard(state, world);
        } else {
            state->mode = DESK_MODE_WIZARD;
            queue_audio(state, DESK_AUDIO_UI_MOVE);
        }
        return true;
    }
    return false;
}

bool desk_interact(desk_state *state, const desk_world *world)
{
    if (!state || !world) return false;
    switch (state->mode) {
    case DESK_MODE_ROOM:
        return interact_room(state, world, current_room(state, world));
    case DESK_MODE_DIALOGUE:
        return interact_dialogue(state);
    case DESK_MODE_WIZARD:
        return interact_wizard(state, world);
    case DESK_MODE_PAUSE:
        return interact_pause(state, world);
    case DESK_MODE_CONFIRM:
        return interact_confirm(state, world);
    case DESK_MODE_STATUS:
        state->mode = DESK_MODE_ROOM;
        update_nearest(state, world);
        queue_audio(state, DESK_AUDIO_UI_MOVE);
        return true;
    }
    return false;
}

void desk_cancel(desk_state *state, const desk_world *world)
{
    if (!state) return;
    switch (state->mode) {
    case DESK_MODE_DIALOGUE:
        state->mode = DESK_MODE_ROOM;
        state->conversation_npc = -1;
        state->dialogue_beat = 0;
        state->dialogue_age = 0;
        set_toast(state, "Conversation paused. Come back whenever.");
        update_nearest(state, world);
        return;
    case DESK_MODE_ROOM:
        if (state->action.active) {
            /* Escape during a swing: an uncommitted plan is discarded
             * with no world effect; a committed one already queued its
             * launch, so only the tail presentation is shortened. */
            if (state->action.committed) action_complete(state);
            else action_reset(state);
            queue_audio(state, DESK_AUDIO_UI_MOVE);
            return;
        }
        state->mode = DESK_MODE_PAUSE;
        state->pause_cursor = 0;
        state->pause_debug = false;
        state->debug_menu = debug_menu_enabled();
        state->player_moving = false;
        state->nearest_object = -1;
        state->nearest_npc = -1;
        state->nearest_world_item = -1;
        queue_audio(state, DESK_AUDIO_UI_MOVE);
        return;
    case DESK_MODE_PAUSE:
        if (state->pause_debug) {
            state->pause_debug = false;
            state->pause_cursor = 0;
            queue_audio(state, DESK_AUDIO_UI_MOVE);
            return;
        }
        state->mode = DESK_MODE_ROOM;
        queue_audio(state, DESK_AUDIO_UI_MOVE);
        update_nearest(state, world);
        return;
    case DESK_MODE_STATUS:
        state->mode = DESK_MODE_ROOM;
        queue_audio(state, DESK_AUDIO_UI_MOVE);
        update_nearest(state, world);
        return;
    case DESK_MODE_CONFIRM:
        if (state->confirm == DESK_CONFIRM_CAST_CHANGE) {
            state->confirm = DESK_CONFIRM_NONE;
            state->mode = DESK_MODE_WIZARD;
        } else {
            state->confirm = DESK_CONFIRM_NONE;
            state->mode = DESK_MODE_ROOM;
            update_nearest(state, world);
        }
        queue_audio(state, DESK_AUDIO_UI_MOVE);
        return;
    case DESK_MODE_WIZARD:
        switch (state->wizard_step) {
        case DESK_WIZARD_CAST:
            if (state->wizard_editing_existing) {
                state->mode = DESK_MODE_ROOM;
                /* Cursor browsing recolored the live graphics; force a
                 * re-apply from the untouched profile. */
                state->outfit_dirty = true;
                queue_audio(state, DESK_AUDIO_UI_MOVE);
                update_nearest(state, world);
            }
            return;
        case DESK_WIZARD_ACTOR:
            state->wizard_step = DESK_WIZARD_CAST;
            break;
        case DESK_WIZARD_NAME:
            state->wizard_step = DESK_WIZARD_ACTOR;
            break;
        case DESK_WIZARD_OUTFIT:
            state->wizard_step = DESK_WIZARD_NAME;
            break;
        case DESK_WIZARD_CONFIRM:
            state->wizard_step = DESK_WIZARD_OUTFIT;
            break;
        }
        queue_audio(state, DESK_AUDIO_UI_MOVE);
        return;
    }
}

bool desk_text_input(desk_state *state, uint32_t codepoint)
{
    if (!state || state->mode != DESK_MODE_WIZARD ||
        state->wizard_step != DESK_WIZARD_NAME)
        return false;
    if (codepoint < 32u || codepoint > 126u) return false;
    if (state->wizard_name_len < 0 ||
        state->wizard_name_len >= DESK_NAME_CAPACITY - 1)
        return false;
    state->wizard_name[state->wizard_name_len] = (char)codepoint;
    state->wizard_name_len++;
    state->wizard_name[state->wizard_name_len] = '\0';
    return true;
}

bool desk_text_backspace(desk_state *state)
{
    if (!state || state->mode != DESK_MODE_WIZARD ||
        state->wizard_step != DESK_WIZARD_NAME ||
        state->wizard_name_len <= 0)
        return false;
    state->wizard_name_len--;
    state->wizard_name[state->wizard_name_len] = '\0';
    return true;
}

desk_target desk_take_launch_request(desk_state *state)
{
    desk_target target;
    if (!state) return DESK_TARGET_NONE;
    target = state->pending_launch;
    state->pending_launch = DESK_TARGET_NONE;
    /* The caller copies pending_launch_object before taking; the clear here
     * keeps the pair take-and-clear as one operation. */
    (void)memset(state->pending_launch_object, 0,
                 sizeof state->pending_launch_object);
    return target;
}

int desk_take_audio_events(desk_state *state, desk_audio_event events[4])
{
    int count;
    int index;
    if (!state || !events) return 0;
    count = state->pending_audio_count;
    if (count < 0) count = 0;
    if (count > 4) count = 4;
    for (index = 0; index < count; ++index)
        events[index] = state->pending_audio[index];
    state->pending_audio_count = 0;
    return count;
}

static bool validate_fail(char *error, size_t error_size, const char *message)
{
    if (error && error_size > 0u)
        (void)snprintf(error, error_size, "%s", message);
    return false;
}

bool desk_validate(const desk_state *state, const desk_world *world,
                   char *error, size_t error_size)
{
    const desk_room *room;
    if (!state || !world)
        return validate_fail(error, error_size, "null state or world");
    if (world->room_count < 1 || world->room_count > DESK_MAX_ROOMS)
        return validate_fail(error, error_size, "world room count invalid");
    if (state->room < 0 || state->room >= world->room_count)
        return validate_fail(error, error_size, "room index out of range");
    room = &world->rooms[state->room];
    if (state->player_x < room->walk.x ||
        state->player_x > room->walk.x + room->walk.w ||
        state->player_y < room->walk.y ||
        state->player_y > room->walk.y + room->walk.h)
        return validate_fail(error, error_size, "player outside walk rect");
    if ((int)state->mode < (int)DESK_MODE_WIZARD ||
        (int)state->mode > (int)DESK_MODE_STATUS)
        return validate_fail(error, error_size, "mode out of range");
    if (state->status_line_count < 0 ||
        state->status_line_count > DESK_STATUS_LINE_COUNT)
        return validate_fail(error, error_size,
                             "status line count out of range");
    if (state->mode == DESK_MODE_STATUS) {
        int line;
        if (state->status_line_count == 0)
            return validate_fail(error, error_size,
                                 "status mode without status lines");
        for (line = 0; line < state->status_line_count; ++line)
            if (!memchr(state->status_lines[line], '\0',
                        DESK_STATUS_LINE_CAPACITY))
                return validate_fail(error, error_size,
                                     "status line not NUL-terminated");
    }
    if ((int)state->wizard_step < (int)DESK_WIZARD_CAST ||
        (int)state->wizard_step > (int)DESK_WIZARD_CONFIRM)
        return validate_fail(error, error_size, "wizard step out of range");
    if ((int)state->facing < (int)DESK_FACING_DOWN ||
        (int)state->facing > (int)DESK_FACING_UP)
        return validate_fail(error, error_size, "facing out of range");
    if (state->wizard_cast_cursor < 0 ||
        state->wizard_cast_cursor >= DESK_CAST_COUNT)
        return validate_fail(error, error_size, "cast cursor out of range");
    if (state->wizard_actor_cursor < 0 ||
        state->wizard_actor_cursor >= DESK_ACTOR_COUNT)
        return validate_fail(error, error_size, "actor cursor out of range");
    if (state->wizard_outfit_cursor < 0 ||
        state->wizard_outfit_cursor >= DESK_OUTFIT_COUNT)
        return validate_fail(error, error_size, "outfit cursor out of range");
    if (state->wizard_confirm_cursor < 0 ||
        state->wizard_confirm_cursor > 1)
        return validate_fail(error, error_size,
                             "wizard confirm cursor out of range");
    if (state->pause_cursor < 0 || state->pause_cursor > 3)
        return validate_fail(error, error_size, "pause cursor out of range");
    if (state->confirm_cursor < 0 || state->confirm_cursor > 1)
        return validate_fail(error, error_size, "confirm cursor out of range");
    if ((int)state->confirm < (int)DESK_CONFIRM_NONE ||
        (int)state->confirm > (int)DESK_CONFIRM_CAST_CHANGE)
        return validate_fail(error, error_size, "confirm kind out of range");
    if (state->mode == DESK_MODE_CONFIRM &&
        state->confirm == DESK_CONFIRM_NONE)
        return validate_fail(error, error_size,
                             "confirm mode without a confirm kind");
    if (state->wizard_name_len < 0 ||
        state->wizard_name_len > DESK_NAME_CAPACITY - 1 ||
        state->wizard_name[state->wizard_name_len] != '\0' ||
        (int)strlen(state->wizard_name) != state->wizard_name_len)
        return validate_fail(error, error_size, "wizard name malformed");
    if (!memchr(state->profile.name, '\0', sizeof state->profile.name))
        return validate_fail(error, error_size,
                             "profile name not NUL-terminated");
    if (!memchr(state->profile.last_room, '\0',
                sizeof state->profile.last_room))
        return validate_fail(error, error_size,
                             "profile room not NUL-terminated");
    if (!cast_valid(state->profile.cast) ||
        (int)state->profile.actor < 0 ||
        (int)state->profile.actor >= DESK_ACTOR_COUNT ||
        state->profile.outfit >= (uint32_t)DESK_OUTFIT_COUNT)
        return validate_fail(error, error_size, "profile fields out of range");
    if (state->pending_audio_count < 0 ||
        state->pending_audio_count >
            (int)(sizeof state->pending_audio /
                  sizeof state->pending_audio[0]))
        return validate_fail(error, error_size,
                             "pending audio count out of range");
    if ((int)state->pending_launch < (int)DESK_TARGET_NONE ||
        (int)state->pending_launch >= DESK_TARGET_COUNT)
        return validate_fail(error, error_size,
                             "pending launch out of range");
    if (state->nearest_object < -1 ||
        state->nearest_object >= room->object_count)
        return validate_fail(error, error_size, "nearest object out of range");
    if (state->nearest_npc < -1 || state->nearest_npc >= room->npc_count)
        return validate_fail(error, error_size, "nearest npc out of range");
    if (state->items.item_count < 0 ||
        state->items.item_count > DESK_MAX_WORLD_ITEMS)
        return validate_fail(error, error_size,
                             "world item count out of range");
    if (state->nearest_world_item < -1 ||
        state->nearest_world_item >= state->items.item_count)
        return validate_fail(error, error_size,
                             "nearest world item out of range");
    if (state->items.inventory.selected < 0 ||
        state->items.inventory.selected >= DESK_INVENTORY_SLOTS)
        return validate_fail(error, error_size,
                             "selected slot out of range");
    if ((int)state->player_animator.clip < 0 ||
        (int)state->player_animator.clip >= DESK_CLIP_COUNT ||
        state->player_animator.frame < 0 ||
        state->player_animator.frame >= 8 ||
        state->player_animator.frame_ticks < 0)
        return validate_fail(error, error_size, "animator out of range");
    if (state->action.committed && !state->action.active)
        return validate_fail(error, error_size,
                             "committed action without an active one");
    if (state->action.active &&
        !state->player_animator.movement_locked)
        return validate_fail(error, error_size,
                             "active action without a movement lock");
    if (state->door_cooldown_ticks < 0 ||
        state->door_cooldown_ticks > DESK_DOOR_COOLDOWN_TICKS)
        return validate_fail(error, error_size,
                             "door cooldown out of range");
    if (state->toast_ticks < 0 || state->toast_ticks > DESK_TOAST_TICKS)
        return validate_fail(error, error_size, "toast ticks out of range");
    if (state->dialogue_age < 0)
        return validate_fail(error, error_size, "dialogue age negative");
    if (state->mode == DESK_MODE_DIALOGUE) {
        if (state->conversation_npc < (int)DESK_ACTOR_ALLY_1 ||
            state->conversation_npc > (int)DESK_ACTOR_ALLY_3)
            return validate_fail(error, error_size,
                                 "dialogue without a conversation npc");
        if (state->dialogue_beat < 0 ||
            state->dialogue_beat >= desk_dialogue_count(state))
            return validate_fail(error, error_size,
                                 "dialogue beat out of range");
    }
    return true;
}

const char *desk_cast_name(desk_cast cast)
{
    return cast_valid(cast) ? CAST_PROFILES[cast].title : "";
}

const char *desk_cast_subtitle(desk_cast cast)
{
    return cast_valid(cast) ? CAST_PROFILES[cast].subtitle : "";
}

const char *desk_cast_house_name(desk_cast cast)
{
    return cast_valid(cast) ? CAST_PROFILES[cast].house_name : "";
}

const char *desk_actor_name(desk_cast cast, int actor)
{
    if (!cast_valid(cast) || actor < 0 || actor >= DESK_ACTOR_COUNT)
        return "";
    return CAST_PROFILES[cast].actor_names[actor];
}

uint32_t desk_actor_color(desk_cast cast, int actor)
{
    if (!cast_valid(cast) || actor < 0 || actor >= DESK_ACTOR_COUNT)
        return UINT32_C(0xffffff);
    return CAST_PROFILES[cast].actor_colors[actor];
}

const char *desk_dialogue_text(const desk_state *state)
{
    const dialogue_beat *beat = current_beat(state);
    return beat ? beat->text : "";
}

int desk_dialogue_speaker(const desk_state *state)
{
    const dialogue_beat *beat = current_beat(state);
    return beat ? beat->speaker : (int)DESK_ACTOR_HERO;
}

int desk_dialogue_count(const desk_state *state)
{
    const conversation *script = active_conversation(state);
    return script ? script->count : 0;
}

size_t desk_dialogue_visible_chars(const desk_state *state)
{
    const char *text;
    size_t length;
    size_t visible;
    if (!state || state->mode != DESK_MODE_DIALOGUE) return 0u;
    text = desk_dialogue_text(state);
    length = strlen(text);
    visible = state->dialogue_age > 0 ?
              (size_t)state->dialogue_age /
                  (size_t)DESK_DIALOGUE_REVEAL_TICKS_PER_CHAR : 0u;
    return visible < length ? visible : length;
}

/* Actor id behind the current talk prompt, or -1 when the prompt (if any)
 * targets an object — the render accent must match this exact decision. */
int desk_interact_npc(const desk_state *state, const desk_world *world)
{
    const desk_room *room;
    if (!state || !world || state->mode != DESK_MODE_ROOM) return -1;
    room = current_room(state, world);
    if (!room || !npc_selected(state, room)) return -1;
    return room->npcs[state->nearest_npc].actor;
}

const char *desk_interact_prompt(const desk_state *state,
                                 const desk_world *world)
{
    static char prompt[DESK_PROMPT_CAPACITY];
    const desk_room *room;
    if (!state || !world || state->mode != DESK_MODE_ROOM) return NULL;
    room = current_room(state, world);
    if (!room) return NULL;
    if (world_item_selected(state, room)) {
        const desk_world_item *entry =
            &state->items.items[state->nearest_world_item];
        const desk_item_def *def =
            desk_items_def(state->catalog, entry->item.definition);

        if (!def) return NULL;
        if (entry->item.quantity > 1u)
            (void)snprintf(prompt, sizeof prompt, "Pick up %.30s x%u",
                           def->name, (unsigned)entry->item.quantity);
        else
            (void)snprintf(prompt, sizeof prompt, "Pick up %.36s",
                           def->name);
        return prompt;
    }
    if (npc_selected(state, room)) {
        (void)snprintf(prompt, sizeof prompt, "Talk to %s",
                       desk_actor_name(state->profile.cast,
                                       room->npcs[state->nearest_npc].actor));
        return prompt;
    }
    if (state->nearest_object >= 0 &&
        state->nearest_object < room->object_count)
        return room->objects[state->nearest_object].prompt;
    return NULL;
}

_Static_assert(DESK_PROFILE_PAYLOAD_SIZE >=
                   4u + 1u + 1u + 1u + 4u + 4u +
                   (unsigned int)DESK_NAME_CAPACITY +
                   (unsigned int)DESK_ID_CAPACITY + 4u + 4u + 1u,
               "profile payload too small for the v1 record");

static bool profile_store_open(kilixstate_store *store)
{
    return desk_state_store_open(store, DESK_PROFILE_FILENAME,
                                 DESK_PROFILE_MAX_PAYLOAD);
}

static bool profile_decode_v1(kilixstate_reader *reader, void *context)
{
    desk_profile *profile = context;
    uint8_t cast = 0u;
    uint8_t actor = 0u;
    uint8_t style = 0u;
    uint32_t outfit = 0u;
    uint32_t talked_mask = 0u;
    uint32_t x_bits = 0u;
    uint32_t y_bits = 0u;
    char name[DESK_NAME_CAPACITY];
    char last_room[DESK_ID_CAPACITY];
    bool first_run_done = false;
    float last_x;
    float last_y;
    if (!profile || !kilixstate_read_u8(reader, &cast) ||
        !kilixstate_read_u8(reader, &actor) ||
        !kilixstate_read_u8(reader, &style) ||
        !kilixstate_read_u32(reader, &outfit) ||
        !kilixstate_read_u32(reader, &talked_mask) ||
        !kilixstate_read_bytes(reader, name, sizeof name) ||
        !kilixstate_read_bytes(reader, last_room, sizeof last_room) ||
        !kilixstate_read_u32(reader, &x_bits) ||
        !kilixstate_read_u32(reader, &y_bits) ||
        !kilixstate_read_bool(reader, &first_run_done))
        return false;
    if (cast >= DESK_CAST_COUNT || actor >= DESK_ACTOR_COUNT ||
        style >= DESK_CAST_COUNT ||
        outfit >= (uint32_t)DESK_OUTFIT_COUNT ||
        !memchr(name, '\0', sizeof name) ||
        !memchr(last_room, '\0', sizeof last_room))
        return false;
    (void)memcpy(&last_x, &x_bits, sizeof last_x);
    (void)memcpy(&last_y, &y_bits, sizeof last_y);
    if (!isfinite(last_x) || !isfinite(last_y)) return false;
    profile->schema = DESK_PROFILE_SCHEMA;
    profile->cast = (desk_cast)cast;
    profile->actor = (desk_actor)actor;
    profile->style = style;
    profile->outfit = outfit;
    profile->talked_mask = talked_mask;
    (void)memcpy(profile->name, name, sizeof profile->name);
    (void)memcpy(profile->last_room, last_room, sizeof profile->last_room);
    profile->last_x = last_x;
    profile->last_y = last_y;
    profile->first_run_done = first_run_done;
    return true;
}

bool desk_profile_load(desk_profile *profile)
{
    static const kilixstate_migration PROFILE_MIGRATIONS[] = {
        {(uint32_t)DESK_PROFILE_SCHEMA, DESK_PROFILE_PAYLOAD_SIZE, true,
         profile_decode_v1}
    };
    kilixstate_store store;
    uint8_t payload[DESK_PROFILE_MAX_PAYLOAD];
    size_t payload_size = 0u;
    desk_profile decoded;
    kilixstate_result result;
    if (!profile) return false;
    (void)memset(profile, 0, sizeof *profile);
    if (!profile_store_open(&store)) return false;
    result = kilixstate_load(&store, payload, sizeof payload, &payload_size);
    kilixstate_store_close(&store);
    if (result != KILIXSTATE_OK) return false;
    (void)memset(&decoded, 0, sizeof decoded);
    if (kilixstate_migrate(payload, payload_size, PROFILE_MIGRATIONS,
                           sizeof PROFILE_MIGRATIONS /
                               sizeof PROFILE_MIGRATIONS[0],
                           &decoded, NULL) != KILIXSTATE_CODEC_OK)
        return false;
    *profile = decoded;
    return true;
}

bool desk_profile_save(const desk_profile *profile)
{
    kilixstate_store store;
    kilixstate_writer writer;
    uint8_t payload[DESK_PROFILE_PAYLOAD_SIZE];
    char name[DESK_NAME_CAPACITY];
    char last_room[DESK_ID_CAPACITY];
    uint32_t x_bits;
    uint32_t y_bits;
    bool ok;
    if (!profile || !cast_valid(profile->cast) ||
        (int)profile->actor < 0 ||
        (int)profile->actor >= DESK_ACTOR_COUNT ||
        profile->style >= (uint32_t)DESK_CAST_COUNT ||
        profile->outfit >= (uint32_t)DESK_OUTFIT_COUNT ||
        !memchr(profile->name, '\0', sizeof profile->name) ||
        !memchr(profile->last_room, '\0', sizeof profile->last_room) ||
        !isfinite(profile->last_x) || !isfinite(profile->last_y))
        return false;
    (void)memset(name, 0, sizeof name);
    (void)snprintf(name, sizeof name, "%s", profile->name);
    (void)memset(last_room, 0, sizeof last_room);
    (void)snprintf(last_room, sizeof last_room, "%s", profile->last_room);
    (void)memcpy(&x_bits, &profile->last_x, sizeof x_bits);
    (void)memcpy(&y_bits, &profile->last_y, sizeof y_bits);
    kilixstate_writer_init(&writer, payload, sizeof payload);
    ok = kilixstate_write_u32(&writer, (uint32_t)DESK_PROFILE_SCHEMA) &&
         kilixstate_write_u8(&writer, (uint8_t)profile->cast) &&
         kilixstate_write_u8(&writer, (uint8_t)profile->actor) &&
         kilixstate_write_u8(&writer, (uint8_t)profile->style) &&
         kilixstate_write_u32(&writer, profile->outfit) &&
         kilixstate_write_u32(&writer, profile->talked_mask) &&
         kilixstate_write_bytes(&writer, name, sizeof name) &&
         kilixstate_write_bytes(&writer, last_room, sizeof last_room) &&
         kilixstate_write_u32(&writer, x_bits) &&
         kilixstate_write_u32(&writer, y_bits) &&
         kilixstate_write_bool(&writer, profile->first_run_done) &&
         kilixstate_write_zeroes(&writer,
                                 kilixstate_writer_remaining(&writer));
    if (!ok || kilixstate_writer_size(&writer) != sizeof payload)
        return false;
    if (!profile_store_open(&store)) return false;
    ok = kilixstate_save(&store, payload, sizeof payload) == KILIXSTATE_OK;
    kilixstate_store_close(&store);
    return ok;
}

bool desk_profile_reset(void)
{
    kilixstate_store store;
    kilixstate_result result;
    if (!profile_store_open(&store)) return false;
    result = kilixstate_remove(&store);
    kilixstate_store_close(&store);
    return result == KILIXSTATE_OK || result == KILIXSTATE_NOT_FOUND;
}
