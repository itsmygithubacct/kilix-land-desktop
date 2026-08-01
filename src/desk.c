#include "kilix_land_desktop.h"
#include "state_store.h"

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
    if (state->mode != DESK_MODE_ROOM) return;
    room = current_room(state, world);
    if (!room) return;
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

static bool npc_selected(const desk_state *state, const desk_room *room)
{
    const desk_npc *npc;
    if (!state || !room || state->nearest_npc < 0 ||
        state->nearest_npc >= room->npc_count)
        return false;
    npc = &room->npcs[state->nearest_npc];
    if (npc->actor < (int)DESK_ACTOR_ALLY_1 ||
        npc->actor > (int)DESK_ACTOR_ALLY_3)
        return false;
    if (state->nearest_object < 0 ||
        state->nearest_object >= room->object_count)
        return true;
    return point_distance_sq(state->player_x, state->player_y,
                             npc->x, npc->y) <=
           object_distance_sq(state,
                              &room->objects[state->nearest_object]);
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

void desk_init(desk_state *state, const desk_world *world)
{
    const desk_room *room = NULL;
    int room_index = -1;
    bool from_start = false;
    if (!state) return;
    (void)memset(state, 0, sizeof *state);
    state->nearest_object = -1;
    state->nearest_npc = -1;
    state->conversation_npc = -1;
    state->facing = DESK_FACING_DOWN;
    state->pending_launch = DESK_TARGET_NONE;
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
    update_nearest(state, world);
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
    if (state->mode == DESK_MODE_ROOM && room) {
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
    update_nearest(state, world);
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

static bool interact_room(desk_state *state, const desk_world *world,
                          const desk_room *room)
{
    const desk_object *object;
    if (!room) return false;
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
        state->mode = DESK_MODE_PAUSE;
        state->pause_cursor = 0;
        state->pause_debug = false;
        state->debug_menu = debug_menu_enabled();
        state->player_moving = false;
        state->nearest_object = -1;
        state->nearest_npc = -1;
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
