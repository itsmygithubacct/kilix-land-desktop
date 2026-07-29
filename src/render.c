#include "kilix_land_desktop.h"
#include "kilix_ui.h"

#include <stdio.h>
#include <string.h>

#define COLOR_INK UINT32_C(0xf8ead0)
#define COLOR_MUTED UINT32_C(0xb8b2a6)
#define COLOR_PANEL UINT32_C(0x0b1020)
#define COLOR_PANEL_LIGHT UINT32_C(0x18233a)
#define COLOR_GOLD UINT32_C(0xf0c36b)
#define LEGEND_PLAYER_RENDER_SIZE 64
#define STANDARD_PLAYER_RENDER_SIZE 92
#define WIZARD_STEP_TOTAL 5

typedef struct room_palette {
    uint32_t wall;
    uint32_t wall_trim;
    uint32_t sky;
    uint32_t floor;
    uint32_t floor_dark;
    uint32_t floor_edge;
    uint32_t door;
    uint32_t door_edge;
    uint32_t object;
    uint32_t object_highlight;
} room_palette;

/* Deterministic fallback palettes: legend hearth, chumrunner neon-on-dark,
 * fantasy verdant, pleb-bound suburban pastel. Render fixtures hash these. */
static const room_palette ROOM_PALETTES[DESK_CAST_COUNT] = {
    {UINT32_C(0x503524), UINT32_C(0x6a4630), UINT32_C(0x8fb4d8),
     UINT32_C(0x7a5a38), UINT32_C(0x63482c), UINT32_C(0x8a6a44),
     UINT32_C(0x8a6a42), UINT32_C(0xd8a05c), UINT32_C(0x3a2818),
     UINT32_C(0xffca7a)},
    {UINT32_C(0x10162a), UINT32_C(0x1b2440), UINT32_C(0x070b1c),
     UINT32_C(0x1b2438), UINT32_C(0x141b2c), UINT32_C(0x24304e),
     UINT32_C(0x223052), UINT32_C(0x35d4e8), UINT32_C(0x0b101f),
     UINT32_C(0x55d6d0)},
    {UINT32_C(0x27422e), UINT32_C(0x33543a), UINT32_C(0x9fd4e8),
     UINT32_C(0x3d5a34), UINT32_C(0x304a2a), UINT32_C(0x4c6c3e),
     UINT32_C(0x55743c), UINT32_C(0x9ee27a), UINT32_C(0x1d3222),
     UINT32_C(0xcfe89a)},
    {UINT32_C(0xe3d4bd), UINT32_C(0xcdbaa0), UINT32_C(0xa8d8e8),
     UINT32_C(0xc9a887), UINT32_C(0xb2926f), UINT32_C(0xd9bd9d),
     UINT32_C(0xd6c5a4), UINT32_C(0xffc15c), UINT32_C(0xa88a68),
     UINT32_C(0xffe0a0)}
};

static int cast_index(desk_cast cast)
{
    int index = (int)cast;
    return index >= 0 && index < DESK_CAST_COUNT ? index : 0;
}

static desk_cast visible_cast(const desk_state *state)
{
    return (desk_cast)cast_index(state->profile.cast);
}

static desk_cast wizard_cast(const desk_state *state)
{
    int cursor = state->wizard_cast_cursor;
    if (cursor < 0 || cursor >= DESK_CAST_COUNT) return DESK_CAST_LEGEND;
    return (desk_cast)cursor;
}

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static size_t clamp_cursor(int cursor, size_t count)
{
    if (cursor <= 0 || count == 0u) return 0u;
    if ((size_t)cursor >= count) return count - 1u;
    return (size_t)cursor;
}

static int text_scale(const ki_td_view *view)
{
    return view->scale >= 1.85f ? 2 : 1;
}

static void text_at(sr_canvas *canvas, const ki_td_view *view,
                    float x, float y, const char *text, uint32_t color)
{
    sr_text_shadow(canvas, (float)ki_td_screen_x(view, x),
                   (float)ki_td_screen_y(view, y), text, color, 1.0f,
                   text_scale(view));
}

static void small_text(sr_canvas *canvas, const ki_td_view *view,
                       float x, float y, const char *text, uint32_t color)
{
    sr_text_shadow(canvas, (float)ki_td_screen_x(view, x),
                   (float)ki_td_screen_y(view, y), text, color, 1.0f, 1);
}

static void center_text(sr_canvas *canvas, const ki_td_view *view,
                        float center_x, float y, const char *text,
                        uint32_t color, int scale)
{
    int width = sr_text_width(text, scale);
    sr_text_shadow(canvas,
                   (float)ki_td_screen_x(view, center_x) -
                       (float)width * 0.5f,
                   (float)ki_td_screen_y(view, y), text, color, 1.0f, scale);
}

static desk_graphic character_graphic(desk_cast cast)
{
    if (cast == DESK_CAST_CHUMRUNNER) return DESK_GRAPHIC_CHUM_CHARACTERS;
    if (cast == DESK_CAST_FANTASY) return DESK_GRAPHIC_FANTASY_CHARACTERS;
    return DESK_GRAPHIC_PLEB_CHARACTERS;
}

static desk_graphic portrait_graphic(desk_cast cast)
{
    if (cast == DESK_CAST_CHUMRUNNER) return DESK_GRAPHIC_CHUM_PORTRAITS;
    if (cast == DESK_CAST_FANTASY) return DESK_GRAPHIC_FANTASY_PORTRAITS;
    return DESK_GRAPHIC_PLEB_PORTRAITS;
}

static int clip_frame(uint64_t tick, const int *durations,
                      int duration_count)
{
    int total = 0;
    int elapsed;
    int index;
    for (index = 0; index < duration_count; ++index)
        total += durations[index];
    elapsed = (int)(((tick % UINT64_C(2160)) * UINT64_C(1000) /
                     (uint64_t)DESK_SIMULATION_HZ) % (uint64_t)total);
    for (index = 0; index < duration_count - 1; ++index) {
        if (elapsed < durations[index]) return index;
        elapsed -= durations[index];
    }
    return duration_count - 1;
}

static int player_idle_frame(const desk_state *state)
{
    static const int idle_durations[] = {180, 180, 180, 180};
    return clip_frame(state->simulation_tick, idle_durations, 4);
}

static int standard_walk_phase(const desk_state *state)
{
    static const int durations[] = {120, 120, 120, 120};
    return clip_frame(state->simulation_tick, durations, 4);
}

static void draw_shadow(ki_td_soft_renderer *renderer, const ki_td_view *view,
                        float x, float y, float radius)
{
    ki_td_soft_fill_ellipse(renderer, view, x, y, radius, radius * 0.29f,
                            UINT32_C(0x02040a), 0.52f);
}

static bool sprite_anchor(const ki_td_rgba8 *image, float *anchor_x,
                          float *anchor_y)
{
    uint64_t head_x_total = 0u;
    uint64_t head_pixels = 0u;
    int top;
    int bottom;
    int y;
    if (!ki_td_rgba8_is_valid(image) || !anchor_x || !anchor_y)
        return false;
    top = image->height;
    bottom = -1;
    for (y = 0; y < image->height; ++y) {
        const uint8_t *row =
            image->pixels + (size_t)y * image->stride;
        int x;
        for (x = 0; x < image->width; ++x) {
            if (row[(size_t)x * 4u + 3u] < UINT8_C(8)) continue;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    }
    if (bottom < top) return false;
    {
        int head_bottom = top + (bottom - top + 1) * 2 / 5;
        for (y = top; y <= head_bottom; ++y) {
            const uint8_t *row =
                image->pixels + (size_t)y * image->stride;
            int x;
            for (x = 0; x < image->width; ++x) {
                if (row[(size_t)x * 4u + 3u] < UINT8_C(8)) continue;
                head_x_total += (uint64_t)(unsigned int)x;
                ++head_pixels;
            }
        }
    }
    *anchor_x = head_pixels > 0u ?
        (float)((double)head_x_total / (double)head_pixels) :
        (float)image->width * 0.5f;
    *anchor_y = (float)bottom;
    return true;
}

static void draw_foot_anchored(ki_td_soft_renderer *renderer,
                               const ki_td_view *view,
                               const ki_td_rgba8 *image,
                               float foot_x, float foot_y,
                               int width, int height, float alpha)
{
    float anchor_x;
    float anchor_y;
    float scale_x;
    float scale_y;
    if (!sprite_anchor(image, &anchor_x, &anchor_y)) return;
    scale_x = (float)width / (float)image->width;
    scale_y = (float)height / (float)image->height;
    ki_td_soft_rgba_resized(renderer, view,
                            foot_x - anchor_x * scale_x,
                            foot_y - anchor_y * scale_y,
                            image, width, height, alpha);
}

static bool legend_player_cell(const desk_state *state,
                               const desk_graphics *graphics,
                               ki_td_rgba8 *cell)
{
    int column;
    int phase;
    if (!state->player_moving) {
        column = player_idle_frame(state);
        return desk_graphics_hero_cell(graphics, DESK_CAST_LEGEND, column,
                                       (int)state->facing * 2, cell);
    }
    phase = standard_walk_phase(state);
    if (phase == 3)
        return desk_graphics_legend_opposite_step(graphics, state->facing,
                                                  cell);
    column = phase == 1 ? 4 : 0;
    return desk_graphics_hero_cell(graphics, DESK_CAST_LEGEND, column,
                                   (int)state->facing * 2, cell);
}

static bool standard_player_cell(const desk_state *state,
                                 const desk_graphics *graphics,
                                 desk_cast cast, ki_td_rgba8 *cell)
{
    static const int side_columns[] = {4, 5, 4, 5};
    int phase = standard_walk_phase(state);
    int column = 0;
    bool mirrored = false;
    desk_hero_motion_variant variant;
    bool authored_side_faces_right =
        cast == DESK_CAST_FANTASY || cast == DESK_CAST_PLEB_BOUND;
    if (state->facing == DESK_FACING_DOWN) {
        if (!state->player_moving || (phase & 1) == 0)
            column = 0;
        else
            return desk_graphics_hero_motion_cell(
                graphics, cast,
                phase == 1 ? DESK_HERO_MOTION_DOWN_A :
                             DESK_HERO_MOTION_DOWN_B,
                cell);
    } else if (state->facing == DESK_FACING_UP) {
        column = 3;
        if (state->player_moving && (phase & 1) != 0)
            return desk_graphics_hero_motion_cell(
                graphics, cast, DESK_HERO_MOTION_UP_STEP, cell);
    } else if (state->facing == DESK_FACING_LEFT) {
        column = state->player_moving ? side_columns[phase] : 4;
        mirrored = authored_side_faces_right;
    } else {
        column = state->player_moving ? side_columns[phase] : 4;
        mirrored = !authored_side_faces_right;
    }
    if (mirrored) {
        variant = column == 4 ? DESK_HERO_MOTION_MIRRORED_SIDE :
                                DESK_HERO_MOTION_MIRRORED_WALK;
        return desk_graphics_hero_motion_cell(graphics, cast, variant, cell);
    }
    return desk_graphics_hero_cell(graphics, cast, column, 0, cell);
}

static void draw_player(ki_td_soft_renderer *renderer, const ki_td_view *view,
                        const desk_state *state,
                        const desk_graphics *graphics)
{
    desk_cast cast = visible_cast(state);
    ki_td_rgba8 cell;
    draw_shadow(renderer, view, state->player_x, state->player_y, 18.0f);
    if (cast == DESK_CAST_LEGEND) {
        if (legend_player_cell(state, graphics, &cell))
            draw_foot_anchored(renderer, view, &cell,
                               state->player_x, state->player_y,
                               LEGEND_PLAYER_RENDER_SIZE,
                               LEGEND_PLAYER_RENDER_SIZE, 1.0f);
    } else if (standard_player_cell(state, graphics, cast, &cell)) {
        draw_foot_anchored(renderer, view, &cell,
                           state->player_x, state->player_y,
                           STANDARD_PLAYER_RENDER_SIZE,
                           STANDARD_PLAYER_RENDER_SIZE, 1.0f);
    }
}

static void draw_npc(ki_td_soft_renderer *renderer, const ki_td_view *view,
                     const desk_state *state, const desk_graphics *graphics,
                     const desk_npc *npc)
{
    desk_cast cast = visible_cast(state);
    float bob = (state->simulation_tick / 32u +
                 (uint64_t)(unsigned int)npc->actor) % 2u == 0u ?
                0.0f : -1.0f;
    ki_td_rgba8 cell;
    if (npc->actor < DESK_ACTOR_ALLY_1 || npc->actor > DESK_ACTOR_ALLY_3)
        return;
    draw_shadow(renderer, view, npc->x, npc->y - 1.0f, 21.0f);
    if (cast == DESK_CAST_LEGEND) {
        if (desk_graphics_cell(graphics, DESK_GRAPHIC_LEGEND_NPCS,
                               npc->actor - DESK_ACTOR_ALLY_1, 0, &cell))
            ki_td_soft_rgba_pixel_art(renderer, view, npc->x - 32.0f,
                                      npc->y - 60.0f + bob, &cell, 1.0f);
    } else if (desk_graphics_cell(graphics, character_graphic(cast), 0,
                                  npc->actor, &cell)) {
        ki_td_soft_rgba_resized(renderer, view, npc->x - 46.0f,
                                npc->y - 87.0f + bob, &cell,
                                STANDARD_PLAYER_RENDER_SIZE,
                                STANDARD_PLAYER_RENDER_SIZE, 1.0f);
    }
}

static int room_npc_count(const desk_room *room)
{
    return clamp_int(room->npc_count, 0, DESK_MAX_NPCS_PER_ROOM);
}

static void draw_actors(ki_td_soft_renderer *renderer, const ki_td_view *view,
                        const desk_state *state,
                        const desk_graphics *graphics, const desk_room *room)
{
    int order[DESK_MAX_NPCS_PER_ROOM + 1];
    float depth[DESK_MAX_NPCS_PER_ROOM + 1];
    int npc_count = room_npc_count(room);
    int count = 0;
    int left;
    int npc;
    for (npc = 0; npc < npc_count; ++npc) {
        order[count] = npc;
        depth[count] = room->npcs[npc].y;
        ++count;
    }
    order[count] = -1;
    depth[count] = state->player_y;
    ++count;
    for (left = 0; left < count - 1; ++left) {
        int right;
        for (right = left + 1; right < count; ++right) {
            if (depth[right] < depth[left]) {
                int swap_order = order[left];
                float swap_depth = depth[left];
                order[left] = order[right];
                depth[left] = depth[right];
                order[right] = swap_order;
                depth[right] = swap_depth;
            }
        }
    }
    for (left = 0; left < count; ++left) {
        if (order[left] < 0)
            draw_player(renderer, view, state, graphics);
        else
            draw_npc(renderer, view, state, graphics,
                     &room->npcs[order[left]]);
    }
}

static void draw_npc_tags(ki_td_soft_renderer *renderer,
                          const ki_td_view *view, sr_canvas *canvas,
                          const desk_state *state, const desk_room *room)
{
    desk_cast cast = visible_cast(state);
    int npc_count = room_npc_count(room);
    int npc;
    for (npc = 0; npc < npc_count; ++npc) {
        const desk_npc *entry = &room->npcs[npc];
        const char *name;
        uint32_t accent;
        float label_width;
        float label_y;
        if (entry->actor < DESK_ACTOR_ALLY_1 ||
            entry->actor > DESK_ACTOR_ALLY_3)
            continue;
        float label_x;
        name = desk_actor_name(cast, entry->actor);
        accent = desk_actor_color(cast, entry->actor);
        label_width = 16.0f + (float)strlen(name) * 6.0f;
        if (label_width < 50.0f) label_width = 50.0f;
        if (label_width > 106.0f) label_width = 106.0f;
        label_x = entry->x - label_width * 0.5f;
        if (label_x < 2.0f) label_x = 2.0f;
        if (label_x + label_width > (float)DESK_LOGICAL_WIDTH - 2.0f)
            label_x = (float)DESK_LOGICAL_WIDTH - 2.0f - label_width;
        label_y = entry->y + 2.0f;
        ki_td_soft_fill_rect(renderer, view, label_x, label_y,
                             label_width, 13.0f,
                             UINT32_C(0x070b13), 0.78f);
        ki_td_soft_fill_rect(renderer, view, label_x, label_y,
                             2.0f, 13.0f, accent, 1.0f);
        text_at(canvas, view, label_x + 7.0f,
                label_y - 1.0f, name,
                state->nearest_npc == npc ? accent : UINT32_C(0xf6e7cb));
    }
}

static void draw_room_scene(ki_td_soft_renderer *renderer,
                            const ki_td_view *view, sr_canvas *canvas,
                            const desk_state *state,
                            const desk_graphics *graphics,
                            const desk_room *room, int room_index)
{
    const room_palette *palette =
        &ROOM_PALETTES[cast_index(state->profile.cast)];
    ki_td_rgba8 plate;
    int door;
    int object;
    if (desk_graphics_plate(graphics, room_index, &plate)) {
        /* Baked art: objects live inside the plate, no slabs on top. */
        ki_td_soft_rgba_resized(renderer, view, 0.0f, 0.0f, &plate,
                                DESK_LOGICAL_WIDTH, DESK_LOGICAL_HEIGHT,
                                1.0f);
        return;
    }
    ki_td_soft_fill_rect(renderer, view, 0.0f, 0.0f,
                         (float)DESK_LOGICAL_WIDTH, room->walk.y,
                         room->outdoor ? palette->sky : palette->wall, 1.0f);
    if (!room->outdoor)
        ki_td_soft_fill_rect(renderer, view, 0.0f, room->walk.y - 24.0f,
                             (float)DESK_LOGICAL_WIDTH, 2.0f,
                             palette->wall_trim, 0.85f);
    ki_td_soft_fill_rect(renderer, view, 0.0f, room->walk.y,
                         (float)DESK_LOGICAL_WIDTH,
                         (float)DESK_LOGICAL_HEIGHT - room->walk.y,
                         palette->floor_dark, 1.0f);
    ki_td_soft_fill_rect(renderer, view, room->walk.x, room->walk.y,
                         room->walk.w, room->walk.h, palette->floor, 1.0f);
    ki_td_soft_fill_rect(renderer, view, 0.0f, room->walk.y,
                         (float)DESK_LOGICAL_WIDTH, 1.0f,
                         palette->floor_edge, 0.9f);
    for (door = 0; door < clamp_int(room->door_count, 0,
                                    DESK_MAX_DOORS_PER_ROOM); ++door) {
        const desk_rect *rect = &room->doors[door].rect;
        ki_td_soft_fill_rect(renderer, view, rect->x - 2.0f, rect->y - 2.0f,
                             rect->w + 4.0f, rect->h + 4.0f,
                             palette->door_edge, 0.9f);
        ki_td_soft_fill_rect(renderer, view, rect->x, rect->y,
                             rect->w, rect->h, palette->door, 1.0f);
        ki_td_soft_fill_rect(renderer, view,
                             rect->x + rect->w * 0.5f - 1.0f,
                             rect->y + rect->h * 0.5f - 1.0f, 2.0f, 2.0f,
                             palette->door_edge, 1.0f);
    }
    for (object = 0; object < clamp_int(room->object_count, 0,
                                        DESK_MAX_OBJECTS_PER_ROOM);
         ++object) {
        const desk_rect *rect = &room->objects[object].rect;
        ki_td_soft_fill_rect(renderer, view, rect->x - 1.0f, rect->y - 1.0f,
                             rect->w + 2.0f, rect->h + 2.0f,
                             UINT32_C(0x05070d), 0.55f);
        ki_td_soft_fill_rect(renderer, view, rect->x, rect->y,
                             rect->w, rect->h, palette->object, 1.0f);
        ki_td_soft_fill_rect(renderer, view, rect->x, rect->y, rect->w,
                             rect->h * 0.4f, palette->object_highlight,
                             0.35f);
        ki_td_soft_fill_rect(renderer, view, rect->x, rect->y,
                             rect->w, 1.0f, palette->object_highlight,
                             0.9f);
    }
    small_text(canvas, view, 8.0f, 36.0f, room->name, COLOR_INK);
}

static void base_style(kilix_ui_style *style, uint32_t accent)
{
    kilix_ui_style_init(style);
    style->panel_color = COLOR_PANEL;
    style->border_color = COLOR_PANEL_LIGHT;
    style->text_color = COLOR_INK;
    style->muted_color = COLOR_MUTED;
    style->accent_color = accent;
    style->panel_alpha = 0.97f;
}

static void draw_interact_prompt(ki_td_soft_renderer *renderer,
                                 const ki_td_view *view, sr_canvas *canvas,
                                 const desk_state *state,
                                 const desk_world *world)
{
    const char *prompt;
    char message[64];
    kilix_ui_style style;
    uint32_t accent = COLOR_GOLD;
    float width;
    float bob;
    float left;
    int talk_actor;
    if (state->mode != DESK_MODE_ROOM) return;
    prompt = desk_interact_prompt(state, world);
    if (!prompt || prompt[0] == '\0') return;
    talk_actor = desk_interact_npc(state, world);
    if (talk_actor >= 0)
        accent = desk_actor_color(visible_cast(state), talk_actor);
    (void)snprintf(message, sizeof message, "ENTER  %s", prompt);
    width = 26.0f + (float)strlen(message) * 8.0f;
    if (width < 170.0f) width = 170.0f;
    if (width > 440.0f) width = 440.0f;
    bob = (state->simulation_tick / 12u) % 2u == 0u ? 0.0f : -2.0f;
    left = 240.0f - width * 0.5f;
    base_style(&style, accent);
    style.panel_alpha = 0.93f;
    kilix_ui_draw_panel(renderer, view,
                        (ki_td_rect){(int)left, 238 + (int)bob,
                                     (int)width, 24},
                        &style, NULL);
    ki_td_soft_fill_rect(renderer, view, left + 2.0f, 240.0f + bob,
                         4.0f, 20.0f, accent, 1.0f);
    text_at(canvas, view, left + 13.0f, 243.0f + bob, message, COLOR_INK);
}

static void draw_toast(ki_td_soft_renderer *renderer, const ki_td_view *view,
                       sr_canvas *canvas, const desk_state *state)
{
    if (state->toast_ticks <= 0) return;
    ki_td_soft_fill_rect(renderer, view, 44.0f, 10.0f, 392.0f, 23.0f,
                         UINT32_C(0x070b14), 0.86f);
    ki_td_soft_fill_rect(renderer, view, 44.0f, 10.0f, 3.0f, 23.0f,
                         COLOR_GOLD, 1.0f);
    text_at(canvas, view, 55.0f, 12.0f, state->toast, COLOR_INK);
}

static bool portrait_image(const desk_graphics *graphics, desk_cast cast,
                           int speaker, ki_td_rgba8 *portrait)
{
    static const int standard_columns[DESK_CAST_COUNT][DESK_ACTOR_COUNT] = {
        {0, 0, 0, 0},
        {0, 1, 2, 3},
        {0, 2, 1, 3},
        {0, 1, 2, 3}
    };
    if (portrait) *portrait = (ki_td_rgba8){0};
    if (!portrait || speaker < DESK_ACTOR_HERO ||
        speaker > DESK_ACTOR_ALLY_3)
        return false;
    if (cast == DESK_CAST_LEGEND) {
        static const int legend_columns[DESK_ACTOR_COUNT] = {0, 2, 3, 0};
        static const int legend_rows[DESK_ACTOR_COUNT] = {0, 0, 0, 1};
        return desk_graphics_cell(graphics, DESK_GRAPHIC_LEGEND_PORTRAITS,
                                  legend_columns[speaker],
                                  legend_rows[speaker], portrait);
    }
    return desk_graphics_cell(graphics, portrait_graphic(cast),
                              standard_columns[cast_index(cast)][speaker],
                              0, portrait);
}

static const char *actor_display_name(const desk_state *state, int actor)
{
    if (actor == DESK_ACTOR_HERO && state->profile.name[0] != '\0')
        return state->profile.name;
    return desk_actor_name(visible_cast(state), actor);
}

static void draw_portrait(ki_td_soft_renderer *renderer,
                          const ki_td_view *view, const desk_state *state,
                          const desk_graphics *graphics, int speaker,
                          float panel_y)
{
    uint32_t color = desk_actor_color(visible_cast(state), speaker);
    ki_td_rgba8 portrait = {0};
    int portrait_y = (int)panel_y + 22;
    ki_td_soft_fill_rect(renderer, view, 19.0f, panel_y + 17.0f,
                         82.0f, 82.0f, UINT32_C(0x050810), 0.96f);
    ki_td_soft_fill_rect(renderer, view, 21.0f, panel_y + 19.0f,
                         78.0f, 78.0f, color, 0.86f);
    ki_td_soft_fill_rect(renderer, view, 24.0f, panel_y + 22.0f,
                         72.0f, 72.0f, UINT32_C(0x10192a), 1.0f);
    (void)portrait_image(graphics, visible_cast(state), speaker, &portrait);
    kilix_ui_draw_portrait(renderer, view,
                           (ki_td_rect){24, portrait_y, 72, 72},
                           &portrait, 1.0f);
}

static void wrap_text(const char *text, char lines[3][48])
{
    const size_t width = 42u;
    size_t line;
    size_t offset = 0u;
    size_t length = strlen(text);
    (void)memset(lines, 0, 3u * 48u);
    for (line = 0u; line < 3u && offset < length; ++line) {
        size_t remaining = length - offset;
        size_t take = remaining < width ? remaining : width;
        if (take < remaining) {
            size_t split = take;
            while (split > 20u && text[offset + split] != ' ') split--;
            if (split > 20u) take = split;
        }
        (void)snprintf(lines[line], 48u, "%.*s", (int)take, text + offset);
        offset += take;
        while (offset < length && text[offset] == ' ') offset++;
    }
}

static void visible_line(char output[48], const char *line,
                         size_t *remaining)
{
    size_t length = strlen(line);
    size_t count = *remaining < length ? *remaining : length;
    (void)snprintf(output, 48u, "%.*s", (int)count, line);
    if (*remaining > count) *remaining -= count;
    else *remaining = 0u;
}

static void draw_dialogue(ki_td_soft_renderer *renderer,
                          const ki_td_view *view, sr_canvas *canvas,
                          const desk_state *state,
                          const desk_graphics *graphics)
{
    char lines[3][48];
    char visible[3][48];
    const char *visible_lines[3];
    char counter[24];
    char speaker_name[24];
    const char *text = desk_dialogue_text(state);
    int speaker = clamp_int(desk_dialogue_speaker(state), DESK_ACTOR_HERO,
                            DESK_ACTOR_ALLY_3);
    int count = desk_dialogue_count(state);
    size_t remaining = desk_dialogue_visible_chars(state);
    size_t full_length = strlen(text);
    uint32_t accent = desk_actor_color(visible_cast(state), speaker);
    float slide = state->dialogue_age < 10 ?
                  (float)(10 - state->dialogue_age) : 0.0f;
    float panel_y = 151.0f + slide;
    int dialogue_y = (int)panel_y;
    kilix_ui_style panel_style;
    kilix_ui_style text_style;
    int line;
    int dot;

    ki_td_soft_fill_rect(renderer, view, 0.0f, 0.0f,
                         (float)DESK_LOGICAL_WIDTH,
                         (float)DESK_LOGICAL_HEIGHT,
                         UINT32_C(0x02040a), 0.20f);
    ki_td_soft_fill_rect(renderer, view, 9.0f, panel_y - 3.0f,
                         462.0f, 115.0f, UINT32_C(0x02040a), 0.72f);
    kilix_ui_style_init(&panel_style);
    panel_style.panel_color = COLOR_PANEL;
    panel_style.border_color = COLOR_PANEL;
    panel_style.text_color = COLOR_INK;
    panel_style.muted_color = COLOR_MUTED;
    panel_style.accent_color = accent;
    panel_style.panel_alpha = 0.97f;
    kilix_ui_draw_panel(renderer, view,
                        (ki_td_rect){12, dialogue_y, 456, 108},
                        &panel_style, NULL);
    ki_td_soft_fill_rect(renderer, view, 14.0f, panel_y + 2.0f,
                         452.0f, 3.0f, accent, 0.95f);
    ki_td_soft_fill_rect(renderer, view, 104.0f, panel_y + 13.0f,
                         126.0f, 22.0f, COLOR_PANEL_LIGHT, 1.0f);
    ki_td_soft_fill_rect(renderer, view, 104.0f, panel_y + 13.0f,
                         4.0f, 22.0f, accent, 1.0f);
    draw_portrait(renderer, view, state, graphics, speaker, panel_y);

    (void)snprintf(speaker_name, sizeof speaker_name, "%.14s",
                   actor_display_name(state, speaker));
    text_at(canvas, view, 114.0f, panel_y + 15.0f, speaker_name, accent);
    (void)snprintf(counter, sizeof counter, "%02d / %02d",
                   state->dialogue_beat + 1, count);
    text_at(canvas, view, 397.0f, panel_y + 15.0f, counter, COLOR_MUTED);

    wrap_text(text, lines);
    for (line = 0; line < 3; ++line) {
        visible_line(visible[line], lines[line], &remaining);
        visible_lines[line] = visible[line];
    }
    text_style = panel_style;
    text_style.panel_alpha = 0.0f;
    text_style.border_color = COLOR_PANEL;
    text_style.padding = 8;
    text_style.row_height = 18;
    text_style.font_scale = 1;
    kilix_ui_draw_dialogue(
        renderer, view, (ki_td_rect){103, dialogue_y + 14, 363, 77},
        &text_style, NULL, NULL, NULL, visible_lines, 3u, NULL);

    text_at(canvas, view, 111.0f, panel_y + 93.0f,
            desk_dialogue_visible_chars(state) < full_length ?
            "ENTER / SPACE  REVEAL" :
            (state->dialogue_beat + 1 < count ?
             "ENTER / SPACE  NEXT" : "ENTER / SPACE  FINISH"),
            COLOR_MUTED);
    text_at(canvas, view, 306.0f, panel_y + 93.0f,
            "ESC  CLOSE", COLOR_MUTED);
    for (dot = 0; dot < count; ++dot) {
        float x = 417.0f + (float)dot * 7.0f;
        if (x < 461.0f)
            ki_td_soft_fill_circle(renderer, view, x, panel_y + 99.0f,
                                   dot == state->dialogue_beat ?
                                   2.0f : 1.25f,
                                   dot <= state->dialogue_beat ?
                                   accent : COLOR_MUTED,
                                   dot <= state->dialogue_beat ?
                                   1.0f : 0.45f);
    }
}

static void draw_pause(ki_td_soft_renderer *renderer, const ki_td_view *view,
                       sr_canvas *canvas, const desk_state *state)
{
    static const char *const items[3] = {"RESUME", "CHARACTER", "QUIT"};
    kilix_ui_style outer_style;
    kilix_ui_style list_style;
    kilix_ui_focus focus;
    uint32_t accent = desk_actor_color(visible_cast(state),
                                       DESK_ACTOR_HERO);
    ki_td_soft_fill_rect(renderer, view, 0.0f, 0.0f,
                         (float)DESK_LOGICAL_WIDTH,
                         (float)DESK_LOGICAL_HEIGHT,
                         UINT32_C(0x02040a), 0.55f);
    base_style(&outer_style, accent);
    outer_style.border_color = COLOR_GOLD;
    outer_style.padding = 8;
    list_style = outer_style;
    list_style.border_color = COLOR_PANEL_LIGHT;
    list_style.row_height = 20;
    list_style.padding = 5;
    kilix_ui_focus_init(&focus, 3u, 3u);
    focus.selected = clamp_cursor(state->pause_cursor, 3u);
    kilix_ui_draw_panel(renderer, view, (ki_td_rect){172, 84, 136, 108},
                        &outer_style, NULL);
    ki_td_soft_fill_rect(renderer, view, 175.0f, 87.0f, 130.0f, 3.0f,
                         accent, 1.0f);
    center_text(canvas, view, 240.0f, 94.0f, "PAUSED", COLOR_INK,
                text_scale(view));
    kilix_ui_draw_list(renderer, view, (ki_td_rect){184, 108, 112, 70},
                       &list_style, NULL, &focus, items, NULL, 3u);
    center_text(canvas, view, 240.0f, 181.0f, "ESC RESUME", COLOR_MUTED, 1);
}

static void draw_confirm(ki_td_soft_renderer *renderer,
                         const ki_td_view *view, sr_canvas *canvas,
                         const desk_state *state)
{
    static const char *const items[2] = {"YES", "NO"};
    const char *question =
        state->confirm == DESK_CONFIRM_QUIT ?
        "Rest and leave the desktop?" :
        "Switch casts? Your housemates move out.";
    kilix_ui_style outer_style;
    kilix_ui_style list_style;
    kilix_ui_focus focus;
    uint32_t accent = desk_actor_color(visible_cast(state),
                                       DESK_ACTOR_HERO);
    ki_td_soft_fill_rect(renderer, view, 0.0f, 0.0f,
                         (float)DESK_LOGICAL_WIDTH,
                         (float)DESK_LOGICAL_HEIGHT,
                         UINT32_C(0x02040a), 0.55f);
    base_style(&outer_style, accent);
    outer_style.border_color = COLOR_GOLD;
    outer_style.padding = 8;
    list_style = outer_style;
    list_style.border_color = COLOR_PANEL_LIGHT;
    list_style.row_height = 20;
    list_style.padding = 4;
    kilix_ui_focus_init(&focus, 2u, 2u);
    focus.selected = clamp_cursor(state->confirm_cursor, 2u);
    kilix_ui_draw_panel(renderer, view, (ki_td_rect){130, 92, 220, 92},
                        &outer_style, NULL);
    ki_td_soft_fill_rect(renderer, view, 133.0f, 95.0f, 214.0f, 3.0f,
                         accent, 1.0f);
    center_text(canvas, view, 240.0f, 104.0f, question, COLOR_INK, 1);
    kilix_ui_draw_list(renderer, view, (ki_td_rect){200, 122, 80, 48},
                       &list_style, NULL, &focus, items, NULL, 2u);
    center_text(canvas, view, 240.0f, 173.0f, "ENTER CONFIRM   ESC BACK",
                COLOR_MUTED, 1);
}

static void draw_status(ki_td_soft_renderer *renderer,
                        const ki_td_view *view, sr_canvas *canvas,
                        const desk_state *state)
{
    kilix_ui_style outer_style;
    uint32_t accent = desk_actor_color(visible_cast(state),
                                       DESK_ACTOR_HERO);
    int count = clamp_int(state->status_line_count, 0,
                          DESK_STATUS_LINE_COUNT);
    int height = 52 + count * 14;
    int top = (DESK_LOGICAL_HEIGHT - height) / 2;
    int line;
    ki_td_soft_fill_rect(renderer, view, 0.0f, 0.0f,
                         (float)DESK_LOGICAL_WIDTH,
                         (float)DESK_LOGICAL_HEIGHT,
                         UINT32_C(0x02040a), 0.55f);
    base_style(&outer_style, accent);
    outer_style.border_color = COLOR_GOLD;
    outer_style.padding = 8;
    kilix_ui_draw_panel(renderer, view,
                        (ki_td_rect){120, top, 240, height},
                        &outer_style, NULL);
    ki_td_soft_fill_rect(renderer, view, 123.0f, (float)top + 3.0f,
                         234.0f, 3.0f, accent, 1.0f);
    center_text(canvas, view, 240.0f, (float)top + 10.0f, "NOTICE BOARD",
                COLOR_INK, text_scale(view));
    for (line = 0; line < count; ++line)
        small_text(canvas, view, 134.0f, (float)(top + 30 + line * 14),
                   state->status_lines[line],
                   line == 0 ? COLOR_GOLD : COLOR_INK);
    center_text(canvas, view, 240.0f, (float)(top + height - 15),
                "ENTER CLOSE", COLOR_MUTED, 1);
}

static void wizard_frame(ki_td_soft_renderer *renderer,
                         const ki_td_view *view, sr_canvas *canvas,
                         uint32_t accent, const char *title, int step,
                         const char *footer)
{
    char step_line[24];
    kilix_ui_style outer_style;
    ki_td_soft_fill_rect(renderer, view, 0.0f, 0.0f,
                         (float)DESK_LOGICAL_WIDTH,
                         (float)DESK_LOGICAL_HEIGHT,
                         UINT32_C(0x02040a), 0.66f);
    ki_td_soft_fill_rect(renderer, view, 19.0f, 32.0f, 442.0f, 207.0f,
                         UINT32_C(0x02040a), 0.78f);
    base_style(&outer_style, accent);
    outer_style.border_color = COLOR_GOLD;
    outer_style.panel_alpha = 0.98f;
    outer_style.padding = 8;
    kilix_ui_draw_panel(renderer, view, (ki_td_rect){24, 36, 432, 199},
                        &outer_style, NULL);
    ki_td_soft_fill_rect(renderer, view, 27.0f, 39.0f, 426.0f, 3.0f,
                         accent, 1.0f);
    text_at(canvas, view, 41.0f, 48.0f, title, COLOR_INK);
    (void)snprintf(step_line, sizeof step_line, "STEP %d / %d", step + 1,
                   WIZARD_STEP_TOTAL);
    small_text(canvas, view, 390.0f, 50.0f, step_line, COLOR_MUTED);
    text_at(canvas, view, 41.0f, 215.0f, footer, COLOR_MUTED);
}

static void wizard_panel(ki_td_soft_renderer *renderer,
                         const ki_td_view *view, int x, uint32_t accent,
                         bool active)
{
    kilix_ui_style panel_style;
    base_style(&panel_style, accent);
    panel_style.panel_alpha = 0.96f;
    panel_style.border_color = active ? accent : COLOR_PANEL_LIGHT;
    kilix_ui_draw_panel(renderer, view, (ki_td_rect){x, 76, 98, 106},
                        &panel_style, NULL);
    if (active)
        ki_td_soft_fill_rect(renderer, view, (float)x + 2.0f, 78.0f,
                             94.0f, 2.0f, accent, 1.0f);
}

static void draw_wizard_cast(ki_td_soft_renderer *renderer,
                             const ki_td_view *view, sr_canvas *canvas,
                             const desk_state *state,
                             const desk_graphics *graphics)
{
    int index;
    desk_cast cursor = wizard_cast(state);
    wizard_frame(renderer, view, canvas,
                 desk_actor_color(cursor, DESK_ACTOR_HERO),
                 "CHOOSE YOUR CAST", (int)DESK_WIZARD_CAST,
                 "LEFT/RIGHT SELECT   ENTER NEXT   ESC BACK");
    for (index = 0; index < DESK_CAST_COUNT; ++index) {
        desk_cast cast = (desk_cast)index;
        uint32_t accent = desk_actor_color(cast, DESK_ACTOR_HERO);
        bool active = index == (int)cursor;
        int x = 40 + index * 101;
        char line[16];
        ki_td_rgba8 portrait = {0};
        wizard_panel(renderer, view, x, accent, active);
        if (portrait_image(graphics, cast, DESK_ACTOR_HERO, &portrait))
            kilix_ui_draw_portrait(renderer, view,
                                   (ki_td_rect){x + 17, 82, 64, 64},
                                   &portrait, active ? 1.0f : 0.72f);
        /* A 98px panel fits ~14 small glyphs; longer titles describe the
         * active cast below the row instead of overflowing the grid. */
        (void)snprintf(line, sizeof line, "%.14s", desk_cast_name(cast));
        small_text(canvas, view, (float)x + 6.0f, 152.0f, line,
                   active ? accent : COLOR_INK);
    }
    center_text(canvas, view, 240.0f, 172.0f, desk_cast_name(cursor),
                desk_actor_color(cursor, DESK_ACTOR_HERO), 1);
    center_text(canvas, view, 240.0f, 184.0f, desk_cast_subtitle(cursor),
                COLOR_MUTED, 1);
    center_text(canvas, view, 240.0f, 196.0f,
                desk_cast_house_name(cursor), COLOR_GOLD, 1);
}

static void draw_wizard_actor(ki_td_soft_renderer *renderer,
                              const ki_td_view *view, sr_canvas *canvas,
                              const desk_state *state,
                              const desk_graphics *graphics)
{
    desk_cast cast = wizard_cast(state);
    int cursor = clamp_int(state->wizard_actor_cursor, 0,
                           DESK_ACTOR_COUNT - 1);
    int actor;
    wizard_frame(renderer, view, canvas,
                 desk_actor_color(cast, cursor), "PICK YOUR RESIDENT",
                 (int)DESK_WIZARD_ACTOR,
                 "LEFT/RIGHT SELECT   ENTER NEXT   ESC BACK");
    for (actor = 0; actor < DESK_ACTOR_COUNT; ++actor) {
        uint32_t accent = desk_actor_color(cast, actor);
        bool selected = actor == cursor;
        int x = 40 + actor * 101;
        char line[28];
        ki_td_rgba8 portrait = {0};
        wizard_panel(renderer, view, x, accent, selected);
        if (portrait_image(graphics, cast, actor, &portrait))
            kilix_ui_draw_portrait(renderer, view,
                                   (ki_td_rect){x + 17, 82, 64, 64},
                                   &portrait, selected ? 1.0f : 0.45f);
        (void)snprintf(line, sizeof line, "%.22s",
                       desk_actor_name(cast, actor));
        small_text(canvas, view, (float)x + 6.0f, 150.0f, line,
                   selected ? accent : COLOR_INK);
        small_text(canvas, view, (float)x + 6.0f, 162.0f,
                   selected ? "PLAY AS" : "HOUSEMATE",
                   selected ? accent : COLOR_MUTED);
    }
}

static void draw_wizard_name(ki_td_soft_renderer *renderer,
                             const ki_td_view *view, sr_canvas *canvas,
                             const desk_state *state)
{
    desk_cast cast = wizard_cast(state);
    kilix_ui_style panel_style;
    char shown[DESK_NAME_CAPACITY];
    char count_line[24];
    int length = clamp_int(state->wizard_name_len, 0,
                           DESK_NAME_CAPACITY - 1);
    int scale = text_scale(view);
    int name_width;
    int cursor_x;
    int cursor_y;
    bool blink = (state->simulation_tick / 30u) % 2u == 0u;
    uint32_t accent = desk_actor_color(cast, DESK_ACTOR_HERO);
    wizard_frame(renderer, view, canvas, accent, "NAME YOUR RESIDENT",
                 (int)DESK_WIZARD_NAME,
                 "TYPE NAME   BACKSPACE   ENTER NEXT   ESC BACK");
    base_style(&panel_style, accent);
    panel_style.panel_alpha = 0.96f;
    kilix_ui_draw_panel(renderer, view, (ki_td_rect){140, 96, 200, 62},
                        &panel_style, NULL);
    (void)snprintf(shown, sizeof shown, "%.*s", length,
                   state->wizard_name);
    name_width = sr_text_width(shown, scale);
    cursor_x = ki_td_screen_x(view, 240.0f) - name_width / 2;
    cursor_y = ki_td_screen_y(view, 114.0f);
    sr_text_shadow(canvas, (float)cursor_x, (float)cursor_y, shown,
                   COLOR_INK, 1.0f, scale);
    ki_td_soft_fill_rect_px(renderer,
                            (float)(cursor_x + name_width) + 2.0f,
                            (float)cursor_y, (float)(8 * scale),
                            (float)(16 * scale), COLOR_GOLD,
                            blink ? 0.9f : 0.25f);
    (void)snprintf(count_line, sizeof count_line, "%d / %d", length,
                   DESK_NAME_CAPACITY - 1);
    center_text(canvas, view, 240.0f, 138.0f, count_line, COLOR_MUTED, 1);
    center_text(canvas, view, 240.0f, 170.0f,
                "LETTERS, DIGITS AND SPACES", COLOR_MUTED, 1);
}

static void draw_wizard_outfit(ki_td_soft_renderer *renderer,
                               const ki_td_view *view, sr_canvas *canvas,
                               const desk_state *state,
                               const desk_graphics *graphics)
{
    desk_cast cast = wizard_cast(state);
    int cursor = clamp_int(state->wizard_outfit_cursor, 0,
                           DESK_OUTFIT_COUNT - 1);
    int preview_size = cast == DESK_CAST_LEGEND ?
        LEGEND_PLAYER_RENDER_SIZE * 2 : STANDARD_PLAYER_RENDER_SIZE * 2;
    ki_td_rgba8 cell;
    bool cell_ok;
    int outfit;
    wizard_frame(renderer, view, canvas,
                 desk_outfit_color(cast, cursor), "PICK AN OUTFIT",
                 (int)DESK_WIZARD_OUTFIT,
                 "UP/DOWN SELECT   ENTER NEXT   ESC BACK");
    for (outfit = 0; outfit < DESK_OUTFIT_COUNT; ++outfit) {
        bool selected = outfit == cursor;
        float y = 76.0f + (float)outfit * 18.0f;
        char line[28];
        ki_td_soft_fill_rect(renderer, view, 44.0f, y, 160.0f, 16.0f,
                             selected ? COLOR_PANEL_LIGHT :
                             UINT32_C(0x0e1524),
                             selected ? 1.0f : 0.9f);
        if (selected)
            ki_td_soft_fill_rect(renderer, view, 44.0f, y, 3.0f, 16.0f,
                                 desk_outfit_color(cast, outfit), 1.0f);
        ki_td_soft_fill_rect(renderer, view, 51.0f, y + 2.0f, 12.0f,
                             12.0f, UINT32_C(0x02040a), 1.0f);
        ki_td_soft_fill_rect(renderer, view, 52.0f, y + 3.0f, 10.0f,
                             10.0f, desk_outfit_color(cast, outfit), 1.0f);
        (void)snprintf(line, sizeof line, "%.22s",
                       desk_outfit_name(cast, outfit));
        small_text(canvas, view, 70.0f, y + 3.0f, line,
                   selected ? COLOR_INK : COLOR_MUTED);
    }
    cell_ok = cast == DESK_CAST_LEGEND ?
        desk_graphics_hero_cell(graphics, cast, player_idle_frame(state),
                                0, &cell) :
        desk_graphics_hero_cell(graphics, cast, 0, 0, &cell);
    draw_shadow(renderer, view, 350.0f, 218.0f,
                cast == DESK_CAST_LEGEND ? 26.0f : 30.0f);
    if (cell_ok)
        draw_foot_anchored(renderer, view, &cell, 350.0f, 218.0f,
                           preview_size, preview_size, 1.0f);
    small_text(canvas, view, 308.0f, 64.0f, "OUTFIT PREVIEW",
               COLOR_MUTED);
}

static void draw_wizard_confirm(ki_td_soft_renderer *renderer,
                                const ki_td_view *view, sr_canvas *canvas,
                                const desk_state *state,
                                const desk_graphics *graphics)
{
    static const char *const labels[5] = {
        "NAME", "CAST", "RESIDENT", "OUTFIT", "HOUSE"
    };
    static const char *const options[2] = {"BEGIN", "GO BACK"};
    desk_cast cast = wizard_cast(state);
    int actor = clamp_int(state->wizard_actor_cursor, 0,
                          DESK_ACTOR_COUNT - 1);
    int outfit = clamp_int(state->wizard_outfit_cursor, 0,
                           DESK_OUTFIT_COUNT - 1);
    int cursor = clamp_int(state->wizard_confirm_cursor, 0, 1);
    int name_length = clamp_int(state->wizard_name_len, 0,
                                DESK_NAME_CAPACITY - 1);
    uint32_t accent = desk_actor_color(cast, actor);
    char name[DESK_NAME_CAPACITY];
    const char *values[5];
    ki_td_rgba8 portrait = {0};
    int line;
    int option;
    wizard_frame(renderer, view, canvas, accent, "READY TO MOVE IN",
                 (int)DESK_WIZARD_CONFIRM,
                 state->wizard_editing_existing ?
                 "UP/DOWN CHOOSE   ENTER APPLY   ESC BACK" :
                 "UP/DOWN CHOOSE   ENTER BEGIN   ESC BACK");
    (void)snprintf(name, sizeof name, "%.*s", name_length,
                   state->wizard_name);
    values[0] = name;
    values[1] = desk_cast_name(cast);
    values[2] = desk_actor_name(cast, actor);
    values[3] = desk_outfit_name(cast, outfit);
    values[4] = desk_cast_house_name(cast);
    for (line = 0; line < 5; ++line) {
        float y = 82.0f + (float)line * 15.0f;
        small_text(canvas, view, 44.0f, y, labels[line], COLOR_MUTED);
        small_text(canvas, view, 110.0f, y, values[line], COLOR_INK);
    }
    ki_td_soft_fill_rect(renderer, view, 326.0f, 80.0f, 72.0f, 72.0f,
                         UINT32_C(0x050810), 0.96f);
    ki_td_soft_fill_rect(renderer, view, 328.0f, 82.0f, 68.0f, 68.0f,
                         accent, 0.86f);
    ki_td_soft_fill_rect(renderer, view, 330.0f, 84.0f, 64.0f, 64.0f,
                         UINT32_C(0x10192a), 1.0f);
    if (portrait_image(graphics, cast, actor, &portrait))
        kilix_ui_draw_portrait(renderer, view,
                               (ki_td_rect){330, 84, 64, 64},
                               &portrait, 1.0f);
    for (option = 0; option < 2; ++option) {
        bool selected = option == cursor;
        float y = 168.0f + (float)option * 16.0f;
        if (selected) {
            ki_td_soft_fill_rect(renderer, view, 44.0f, y - 2.0f, 110.0f,
                                 14.0f, COLOR_PANEL_LIGHT, 1.0f);
            ki_td_soft_fill_rect(renderer, view, 44.0f, y - 2.0f, 3.0f,
                                 14.0f, accent, 1.0f);
        }
        small_text(canvas, view, 52.0f, y, options[option],
                   selected ? COLOR_INK : COLOR_MUTED);
    }
}

static void draw_wizard(ki_td_soft_renderer *renderer,
                        const ki_td_view *view, sr_canvas *canvas,
                        const desk_state *state,
                        const desk_graphics *graphics)
{
    switch (state->wizard_step) {
    case DESK_WIZARD_CAST:
        draw_wizard_cast(renderer, view, canvas, state, graphics);
        break;
    case DESK_WIZARD_ACTOR:
        draw_wizard_actor(renderer, view, canvas, state, graphics);
        break;
    case DESK_WIZARD_NAME:
        draw_wizard_name(renderer, view, canvas, state);
        break;
    case DESK_WIZARD_OUTFIT:
        draw_wizard_outfit(renderer, view, canvas, state, graphics);
        break;
    case DESK_WIZARD_CONFIRM:
    default:
        draw_wizard_confirm(renderer, view, canvas, state, graphics);
        break;
    }
}

static const desk_room *active_room(const desk_state *state,
                                    const desk_world *world)
{
    if (state->room < 0 || state->room >= world->room_count ||
        state->room >= DESK_MAX_ROOMS)
        return NULL;
    return &world->rooms[state->room];
}

bool desk_render(ki_td_soft_renderer *renderer, const desk_state *state,
                 const desk_world *world, const desk_graphics *graphics)
{
    ki_td_fit_spec spec;
    ki_td_view view;
    sr_canvas *canvas;
    if (!renderer || !state || !world || !graphics ||
        !ki_td_fit_spec_init(&spec, DESK_LOGICAL_WIDTH,
                             DESK_LOGICAL_HEIGHT,
                             ki_td_soft_width(renderer),
                             ki_td_soft_height(renderer)))
        return false;
    spec.scale_policy = KI_TD_SCALE_PIXEL_ART;
    spec.integer_scale_threshold = 2.0f;
    spec.minimum_scale = 0.5f;
    if (!ki_td_view_fit(&view, &spec)) return false;

    ki_td_soft_clear(renderer, UINT32_C(0x05070d));
    canvas = ki_td_soft_canvas(renderer);
    if (state->mode == DESK_MODE_WIZARD) {
        draw_wizard(renderer, &view, canvas, state, graphics);
        /* Wizard feedback (ally gate, empty-name nudge) arrives as toasts. */
        draw_toast(renderer, &view, canvas, state);
        return ki_td_soft_pack_rgba(renderer) != NULL;
    }
    {
        const desk_room *room = active_room(state, world);
        if (room) {
            draw_room_scene(renderer, &view, canvas, state, graphics,
                            room, state->room);
            draw_actors(renderer, &view, state, graphics, room);
            draw_npc_tags(renderer, &view, canvas, state, room);
        }
    }
    draw_interact_prompt(renderer, &view, canvas, state, world);
    draw_toast(renderer, &view, canvas, state);
    if (state->mode == DESK_MODE_DIALOGUE)
        draw_dialogue(renderer, &view, canvas, state, graphics);
    else if (state->mode == DESK_MODE_PAUSE)
        draw_pause(renderer, &view, canvas, state);
    else if (state->mode == DESK_MODE_CONFIRM)
        draw_confirm(renderer, &view, canvas, state);
    else if (state->mode == DESK_MODE_STATUS)
        draw_status(renderer, &view, canvas, state);
    return ki_td_soft_pack_rgba(renderer) != NULL;
}
