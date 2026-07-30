#include "kilix_land_desktop.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DESK_ASSET_MANIFEST "assets/graphics/manifest.json"
#define DESK_ASSET_MANIFEST_LIMIT (2u * 1024u * 1024u)
#define DESK_ASSET_CACHE_LIMIT (128u * 1024u * 1024u)
#define DESK_CHARACTER_CELL 222u
#define DESK_LEGEND_CELL 64u
#define DESK_LEGEND_SHEET_COLUMNS 16
#define DESK_LEGEND_SHEET_ROWS 8
#define DESK_PAINTED_HERO_COLUMNS 8
#define DESK_OUTFIT_PALETTE_CAPACITY 128
#define DESK_MIN_GARMENT_SATURATION 0.25f
#define DESK_MIN_LEGEND_SATURATION 0.28f
#define DESK_GARMENT_HUE_WINDOW 40.0f
#define DESK_LEGEND_CLUSTER_WINDOW 30.0f

typedef struct sprite_metrics {
    int left;
    int right;
    int top;
    int bottom;
    double spine_x;
} sprite_metrics;

typedef struct graphic_spec {
    const char *id;
    uint32_t width;
    uint32_t height;
    uint32_t columns;
    uint32_t rows;
} graphic_spec;

typedef struct hsv_color {
    float hue;
    float saturation;
    float value;
} hsv_color;

typedef struct outfit_palette {
    uint8_t source[DESK_OUTFIT_PALETTE_CAPACITY][3];
    uint8_t mapped[DESK_OUTFIT_PALETTE_CAPACITY][3];
    bool red_present[256];
    int count;
} outfit_palette;

static const graphic_spec GRAPHIC_SPECS[DESK_GRAPHICS_COUNT] = {
    {"legend-player", 1024u, 512u, 16u, 8u},
    {"legend-npcs", 256u, 256u, 4u, 4u},
    {"legend-portraits", 384u, 288u, 4u, 3u},
    {"chumrunner-characters", 1776u, 888u, 8u, 4u},
    {"chumrunner-portraits", 1776u, 888u, 4u, 2u},
    {"fantasy-characters", 1776u, 888u, 8u, 4u},
    {"fantasy-portraits", 1776u, 888u, 4u, 2u},
    {"pleb-bound-characters", 1776u, 888u, 8u, 4u},
    {"pleb-bound-portraits", 1776u, 888u, 4u, 2u}
};

static const char *const OUTFIT_NAMES[DESK_OUTFIT_COUNT] = {
    "ORIGINAL", "CRIMSON", "COBALT", "FOREST", "AMBER", "VIOLET"
};

/* Index 0 is the untouched authored palette; the wizard shows the cast's
 * accent color for it instead of a swatch anchor. */
static const uint32_t OUTFIT_SWATCHES[DESK_OUTFIT_COUNT] = {
    0u,
    UINT32_C(0xc93a52),
    UINT32_C(0x3e6ed8),
    UINT32_C(0x3c9a5e),
    UINT32_C(0xe8a83a),
    UINT32_C(0x9a5ad8)
};

static const uint32_t ORIGINAL_OUTFIT_COLORS[DESK_CAST_COUNT] = {
    UINT32_C(0xffa33c),
    UINT32_C(0x55d6d0),
    UINT32_C(0x55d6d0),
    UINT32_C(0xffc15c)
};

static desk_graphic character_graphic(desk_cast cast)
{
    if (cast == DESK_CAST_CHUMRUNNER) return DESK_GRAPHIC_CHUM_CHARACTERS;
    if (cast == DESK_CAST_FANTASY) return DESK_GRAPHIC_FANTASY_CHARACTERS;
    return DESK_GRAPHIC_PLEB_CHARACTERS;
}

static void make_locator(kilix_asset_locator *locator, const char *asset_root)
{
    const char *root = asset_root && asset_root[0] != '\0' ? asset_root : ".";
    kilix_asset_locator_init(locator);
    locator->environment_variable = "KILIX_LAND_DESKTOP_ASSETS";
    locator->source_root = root;
    locator->installed_root = root;
}

static void make_limits(kilix_asset_limits *limits)
{
    kilix_asset_limits_init(limits);
    limits->max_file_bytes = 16u * 1024u * 1024u;
    limits->max_image_bytes = 16u * 1024u * 1024u;
    limits->max_dimension = 2048u;
}

static bool measure_sprite(const ki_td_rgba8 *image, sprite_metrics *metrics)
{
    uint64_t spine_total = 0u;
    uint64_t spine_pixels = 0u;
    int y;
    if (!ki_td_rgba8_is_valid(image) || !metrics) return false;
    metrics->left = image->width;
    metrics->right = -1;
    metrics->top = image->height;
    metrics->bottom = -1;
    for (y = 0; y < image->height; ++y) {
        const uint8_t *row =
            image->pixels + (size_t)y * image->stride;
        int x;
        for (x = 0; x < image->width; ++x) {
            if (row[(size_t)x * 4u + 3u] < UINT8_C(8)) continue;
            if (x < metrics->left) metrics->left = x;
            if (x > metrics->right) metrics->right = x;
            if (y < metrics->top) metrics->top = y;
            if (y > metrics->bottom) metrics->bottom = y;
        }
    }
    if (metrics->bottom < metrics->top ||
        metrics->right < metrics->left)
        return false;
    {
        int upper_bottom =
            metrics->top + (metrics->bottom - metrics->top + 1) * 2 / 5;
        for (y = metrics->top; y <= upper_bottom; ++y) {
            const uint8_t *row =
                image->pixels + (size_t)y * image->stride;
            int x;
            for (x = metrics->left; x <= metrics->right; ++x) {
                if (row[(size_t)x * 4u + 3u] < UINT8_C(8)) continue;
                spine_total += (uint64_t)(unsigned int)x;
                ++spine_pixels;
            }
        }
    }
    metrics->spine_x = spine_pixels > 0u ?
        (double)spine_total / (double)spine_pixels :
        ((double)metrics->left + (double)metrics->right) * 0.5;
    return true;
}

static void copy_cell(uint8_t *destination, const ki_td_rgba8 *source)
{
    int y;
    for (y = 0; y < source->height; ++y)
        (void)memcpy(
            destination + (size_t)y * (size_t)source->width * 4u,
            source->pixels + (size_t)y * source->stride,
            (size_t)source->width * 4u);
}

static bool build_leg_motion_cell(uint8_t *destination,
                                  const ki_td_rgba8 *base,
                                  const ki_td_rgba8 *step,
                                  bool mirror_step)
{
    sprite_metrics base_metrics;
    sprite_metrics step_metrics;
    int half_width;
    int left;
    int right;
    int split;
    int step_split;
    int offset_y;
    int y;
    if (!destination || !measure_sprite(base, &base_metrics) ||
        !measure_sprite(step, &step_metrics))
        return false;
    copy_cell(destination, base);
    half_width =
        (base_metrics.right - base_metrics.left + 1) * 31 / 100;
    if (half_width < 24) half_width = 24;
    left = (int)(base_metrics.spine_x + 0.5) - half_width;
    right = (int)(base_metrics.spine_x + 0.5) + half_width;
    if (left < 0) left = 0;
    if (right >= (int)DESK_CHARACTER_CELL)
        right = (int)DESK_CHARACTER_CELL - 1;
    split = base_metrics.bottom -
        (base_metrics.bottom - base_metrics.top + 1) * 34 / 100;
    step_split = step_metrics.bottom -
        (step_metrics.bottom - step_metrics.top + 1) * 34 / 100;
    offset_y = base_metrics.bottom - step_metrics.bottom;
    for (y = split; y < (int)DESK_CHARACTER_CELL; ++y) {
        int source_y = y - offset_y;
        int x;
        for (x = left; x <= right; ++x) {
            uint8_t *output =
                destination +
                ((size_t)y * DESK_CHARACTER_CELL + (size_t)x) * 4u;
            double aligned_x =
                (double)x - base_metrics.spine_x + step_metrics.spine_x;
            int source_x;
            (void)memset(output, 0, 4u);
            if (mirror_step)
                aligned_x = step_metrics.spine_x -
                            (aligned_x - step_metrics.spine_x);
            source_x = (int)(aligned_x + 0.5);
            if (source_y < step_split || source_y < 0 ||
                source_y >= step->height || source_x < 0 ||
                source_x >= step->width)
                continue;
            (void)memcpy(
                output,
                step->pixels + (size_t)source_y * step->stride +
                    (size_t)source_x * 4u,
                4u);
        }
    }
    return true;
}

/*
 * Arden's and Pip's authored front-walk cells turn the whole pose diagonally.
 * For straight-down movement, keep the frontal idle cell byte-for-byte above
 * the knees and alternately lift one of its existing legs.
 */
static bool build_straight_step_cell(uint8_t *destination,
                                     const ki_td_rgba8 *base,
                                     bool lift_left)
{
    sprite_metrics metrics;
    int half_width;
    int center;
    int left;
    int right;
    int split;
    int lift;
    int leg_height;
    int y;
    if (!destination || !measure_sprite(base, &metrics)) return false;
    copy_cell(destination, base);
    center = (int)(metrics.spine_x + 0.5);
    half_width = (metrics.right - metrics.left + 1) * 24 / 100;
    if (half_width < 20) half_width = 20;
    left = center - half_width;
    right = center + half_width;
    if (left < 0) left = 0;
    if (right >= (int)DESK_CHARACTER_CELL)
        right = (int)DESK_CHARACTER_CELL - 1;
    leg_height = (metrics.bottom - metrics.top + 1) * 27 / 100;
    split = metrics.bottom - leg_height;
    lift = (metrics.bottom - metrics.top + 24) / 24;
    if (lift < 5) lift = 5;
    for (y = split; y < (int)DESK_CHARACTER_CELL; ++y) {
        int x;
        for (x = left; x <= right; ++x)
            (void)memset(
                destination +
                    ((size_t)y * DESK_CHARACTER_CELL + (size_t)x) * 4u,
                0, 4u);
    }
    for (y = split; y <= metrics.bottom; ++y) {
        int x;
        for (x = left; x <= right; ++x) {
            bool pixel_is_left = x < center;
            int shift = pixel_is_left == lift_left && leg_height > 0 ?
                lift * (y - split) / leg_height : 0;
            int target_y = y - shift;
            if (target_y < 0 ||
                target_y >= (int)DESK_CHARACTER_CELL)
                continue;
            (void)memcpy(
                destination +
                    ((size_t)target_y * DESK_CHARACTER_CELL +
                     (size_t)x) * 4u,
                base->pixels + (size_t)y * base->stride +
                    (size_t)x * 4u,
                4u);
        }
    }
    return true;
}

/*
 * Kilix's eight authored walk cells are variations of one held stride. Keep
 * the authored upper-body pose, but reflect only the legs around the measured
 * torso center to create the missing opposite footfall.
 */
static bool build_opposite_step_cell(uint8_t *destination,
                                     const ki_td_rgba8 *step)
{
    sprite_metrics metrics;
    int center;
    int half_width;
    int left;
    int right;
    int split;
    int y;
    if (!destination || !measure_sprite(step, &metrics)) return false;
    copy_cell(destination, step);
    center = (int)(metrics.spine_x + 0.5);
    half_width = (metrics.right - metrics.left + 1) * 24 / 100;
    if (half_width < 7) half_width = 7;
    left = center - half_width;
    right = center + half_width;
    if (left < 0) left = 0;
    if (right >= step->width) right = step->width - 1;
    split = metrics.bottom -
        (metrics.bottom - metrics.top + 1) * 29 / 100;
    for (y = split; y <= metrics.bottom; ++y) {
        int x;
        for (x = left; x <= right; ++x) {
            uint8_t *output =
                destination +
                ((size_t)y * (size_t)step->width + (size_t)x) * 4u;
            int source_x = center * 2 - x;
            (void)memset(output, 0, 4u);
            if (source_x < 0 || source_x >= step->width) continue;
            (void)memcpy(
                output,
                step->pixels + (size_t)y * step->stride +
                    (size_t)source_x * 4u,
                4u);
        }
    }
    return true;
}

static bool build_full_mirror(uint8_t *destination,
                              const ki_td_rgba8 *source)
{
    int y;
    if (!destination || !ki_td_rgba8_is_valid(source)) return false;
    for (y = 0; y < source->height; ++y) {
        int x;
        for (x = 0; x < source->width; ++x) {
            const uint8_t *pixel =
                source->pixels + (size_t)y * source->stride +
                (size_t)(source->width - 1 - x) * 4u;
            uint8_t *output =
                destination +
                ((size_t)y * (size_t)source->width + (size_t)x) * 4u;
            (void)memcpy(output, pixel, 4u);
        }
    }
    return true;
}

static float wrap_hue(float degrees)
{
    while (degrees < 0.0f) degrees += 360.0f;
    while (degrees >= 360.0f) degrees -= 360.0f;
    return degrees;
}

static float hue_offset(float hue, float reference)
{
    float offset = hue - reference;
    while (offset < -180.0f) offset += 360.0f;
    while (offset >= 180.0f) offset -= 360.0f;
    return offset;
}

static hsv_color rgb_to_hsv(uint8_t red, uint8_t green, uint8_t blue)
{
    hsv_color hsv;
    float r = (float)red / 255.0f;
    float g = (float)green / 255.0f;
    float b = (float)blue / 255.0f;
    float channel_max = r;
    float channel_min = r;
    float delta;
    if (g > channel_max) channel_max = g;
    if (b > channel_max) channel_max = b;
    if (g < channel_min) channel_min = g;
    if (b < channel_min) channel_min = b;
    delta = channel_max - channel_min;
    hsv.value = channel_max;
    hsv.saturation = channel_max > 0.0f ? delta / channel_max : 0.0f;
    if (delta <= 0.0f)
        hsv.hue = 0.0f;
    else if (channel_max == r)
        hsv.hue = wrap_hue(60.0f * (g - b) / delta);
    else if (channel_max == g)
        hsv.hue = wrap_hue(120.0f + 60.0f * (b - r) / delta);
    else
        hsv.hue = wrap_hue(240.0f + 60.0f * (r - g) / delta);
    return hsv;
}

static void hsv_to_rgb(hsv_color hsv, uint8_t *red, uint8_t *green,
                       uint8_t *blue)
{
    float saturation = hsv.saturation;
    float value = hsv.value;
    float chroma;
    float section;
    float ramp;
    float base;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    int sector;
    if (saturation < 0.0f) saturation = 0.0f;
    if (saturation > 1.0f) saturation = 1.0f;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    chroma = value * saturation;
    section = wrap_hue(hsv.hue) / 60.0f;
    sector = (int)section;
    if (sector > 5) sector = 5;
    ramp = sector % 2 == 0 ?
        chroma * (section - (float)sector) :
        chroma * (1.0f - (section - (float)sector));
    switch (sector) {
    case 0: r = chroma; g = ramp; break;
    case 1: r = ramp; g = chroma; break;
    case 2: g = chroma; b = ramp; break;
    case 3: g = ramp; b = chroma; break;
    case 4: r = ramp; b = chroma; break;
    default: r = chroma; b = ramp; break;
    }
    base = value - chroma;
    *red = (uint8_t)((r + base) * 255.0f + 0.5f);
    *green = (uint8_t)((g + base) * 255.0f + 0.5f);
    *blue = (uint8_t)((b + base) * 255.0f + 0.5f);
}

static float relative_luminance(uint8_t red, uint8_t green, uint8_t blue)
{
    return (0.2126f * (float)red + 0.7152f * (float)green +
            0.0722f * (float)blue) / 255.0f;
}

static bool near_skin_tone(hsv_color hsv)
{
    return hsv.hue >= 8.0f && hsv.hue <= 50.0f &&
           hsv.saturation <= 0.58f && hsv.value >= 0.45f;
}

static bool garment_pixel_hsv(const uint8_t *pixel, float min_saturation,
                              bool exclude_skin, hsv_color *hsv)
{
    if (pixel[3] < UINT8_C(8)) return false;
    *hsv = rgb_to_hsv(pixel[0], pixel[1], pixel[2]);
    if (hsv->saturation <= min_saturation || hsv->value < 0.08f)
        return false;
    if (exclude_skin && near_skin_tone(*hsv)) return false;
    return true;
}

/* Dominant saturated hue of a cell: coarse 10-degree histogram peak, refined
 * by the mean wrapped offset of nearby pixels (no trig needed). */
static bool measure_garment_hue(const ki_td_rgba8 *cell, float min_saturation,
                                bool exclude_skin, float *hue_out)
{
    uint32_t histogram[36] = {0u};
    uint32_t best_score = 0u;
    int best_bin = -1;
    double offset_total = 0.0;
    uint32_t offset_count = 0u;
    float center;
    int bin;
    int y;
    if (!ki_td_rgba8_is_valid(cell) || !hue_out) return false;
    for (y = 0; y < cell->height; ++y) {
        const uint8_t *row = cell->pixels + (size_t)y * cell->stride;
        int x;
        for (x = 0; x < cell->width; ++x) {
            hsv_color hsv;
            if (!garment_pixel_hsv(row + (size_t)x * 4u, min_saturation,
                                   exclude_skin, &hsv))
                continue;
            bin = (int)(hsv.hue / 10.0f);
            if (bin < 0) bin = 0;
            if (bin > 35) bin = 35;
            histogram[bin] += 1u;
        }
    }
    for (bin = 0; bin < 36; ++bin) {
        uint32_t score = histogram[bin] +
            histogram[(bin + 35) % 36] / 2u +
            histogram[(bin + 1) % 36] / 2u;
        if (histogram[bin] > 0u && score > best_score) {
            best_score = score;
            best_bin = bin;
        }
    }
    if (best_bin < 0) return false;
    center = (float)best_bin * 10.0f + 5.0f;
    for (y = 0; y < cell->height; ++y) {
        const uint8_t *row = cell->pixels + (size_t)y * cell->stride;
        int x;
        for (x = 0; x < cell->width; ++x) {
            hsv_color hsv;
            float offset;
            if (!garment_pixel_hsv(row + (size_t)x * 4u, min_saturation,
                                   exclude_skin, &hsv))
                continue;
            offset = hue_offset(hsv.hue, center);
            if (offset < -25.0f || offset > 25.0f) continue;
            offset_total += (double)offset;
            ++offset_count;
        }
    }
    *hue_out = wrap_hue(offset_count > 0u ?
        center + (float)(offset_total / (double)offset_count) : center);
    return true;
}

static void collect_legend_palette(const ki_td_rgba8 *idle, float garment_hue,
                                   outfit_palette *palette)
{
    int y;
    palette->count = 0;
    (void)memset(palette->red_present, 0, sizeof palette->red_present);
    for (y = 0; y < idle->height; ++y) {
        const uint8_t *row = idle->pixels + (size_t)y * idle->stride;
        int x;
        for (x = 0; x < idle->width; ++x) {
            const uint8_t *pixel = row + (size_t)x * 4u;
            hsv_color hsv;
            float offset;
            int entry;
            if (!garment_pixel_hsv(pixel, DESK_MIN_LEGEND_SATURATION,
                                   true, &hsv))
                continue;
            offset = hue_offset(hsv.hue, garment_hue);
            if (offset < -DESK_LEGEND_CLUSTER_WINDOW ||
                offset > DESK_LEGEND_CLUSTER_WINDOW)
                continue;
            for (entry = 0; entry < palette->count; ++entry)
                if (palette->source[entry][0] == pixel[0] &&
                    palette->source[entry][1] == pixel[1] &&
                    palette->source[entry][2] == pixel[2])
                    break;
            if (entry < palette->count ||
                palette->count >= DESK_OUTFIT_PALETTE_CAPACITY)
                continue;
            palette->source[palette->count][0] = pixel[0];
            palette->source[palette->count][1] = pixel[1];
            palette->source[palette->count][2] = pixel[2];
            palette->count++;
        }
    }
}

/* Map each collected palette entry onto a light/base/shadow ramp of the
 * swatch, keyed by the entry's relative luminance within the palette. */
static void map_legend_palette(outfit_palette *palette, uint32_t swatch)
{
    hsv_color swatch_hsv = rgb_to_hsv((uint8_t)(swatch >> 16),
                                      (uint8_t)(swatch >> 8),
                                      (uint8_t)swatch);
    float min_luma = 1.0f;
    float max_luma = 0.0f;
    int entry;
    for (entry = 0; entry < palette->count; ++entry) {
        float luma = relative_luminance(palette->source[entry][0],
                                        palette->source[entry][1],
                                        palette->source[entry][2]);
        if (luma < min_luma) min_luma = luma;
        if (luma > max_luma) max_luma = luma;
    }
    for (entry = 0; entry < palette->count; ++entry) {
        float luma = relative_luminance(palette->source[entry][0],
                                        palette->source[entry][1],
                                        palette->source[entry][2]);
        float position = max_luma > min_luma ?
            (luma - min_luma) / (max_luma - min_luma) : 0.5f;
        hsv_color mapped = swatch_hsv;
        mapped.value = swatch_hsv.value * (0.45f + 0.75f * position);
        mapped.saturation = swatch_hsv.saturation *
            (1.1f - 0.3f * position);
        hsv_to_rgb(mapped, &palette->mapped[entry][0],
                   &palette->mapped[entry][1], &palette->mapped[entry][2]);
        palette->red_present[palette->source[entry][0]] = true;
    }
}

static void remap_legend_pixels(uint8_t *pixels, size_t pixel_count,
                                const outfit_palette *palette)
{
    size_t index;
    for (index = 0; index < pixel_count; ++index) {
        uint8_t *pixel = pixels + index * 4u;
        int entry;
        if (pixel[3] < UINT8_C(8) || !palette->red_present[pixel[0]])
            continue;
        for (entry = 0; entry < palette->count; ++entry) {
            if (palette->source[entry][0] != pixel[0] ||
                palette->source[entry][1] != pixel[1] ||
                palette->source[entry][2] != pixel[2])
                continue;
            pixel[0] = palette->mapped[entry][0];
            pixel[1] = palette->mapped[entry][1];
            pixel[2] = palette->mapped[entry][2];
            break;
        }
    }
}

static void rotate_painted_pixels(uint8_t *pixels, size_t pixel_count,
                                  float garment_hue, float hue_delta)
{
    size_t index;
    for (index = 0; index < pixel_count; ++index) {
        uint8_t *pixel = pixels + index * 4u;
        hsv_color hsv;
        float offset;
        if (pixel[3] < UINT8_C(8)) continue;
        hsv = rgb_to_hsv(pixel[0], pixel[1], pixel[2]);
        if (hsv.saturation <= DESK_MIN_GARMENT_SATURATION) continue;
        /* Faces and hair share the warm hue band with several garments;
         * only saturated non-skin pixels belong to the outfit. */
        if (near_skin_tone(hsv)) continue;
        offset = hue_offset(hsv.hue, garment_hue);
        if (offset < -DESK_GARMENT_HUE_WINDOW ||
            offset > DESK_GARMENT_HUE_WINDOW)
            continue;
        hsv.hue = wrap_hue(hsv.hue + hue_delta);
        hsv_to_rgb(hsv, &pixel[0], &pixel[1], &pixel[2]);
    }
}

/* Motion sources come through desk_graphics_hero_cell so the active cast is
 * rebuilt from its recolored cells and every other cast from the raw sheets. */
static bool rebuild_legend_steps(desk_graphics *graphics)
{
    const size_t cell_bytes =
        (size_t)DESK_LEGEND_CELL * DESK_LEGEND_CELL * 4u;
    int facing;
    if (!graphics->legend_opposite_step_pixels) return false;
    for (facing = DESK_FACING_DOWN; facing <= DESK_FACING_UP; ++facing) {
        uint8_t *destination =
            graphics->legend_opposite_step_pixels +
            (size_t)facing * cell_bytes;
        ki_td_rgba8 step;
        if (!desk_graphics_hero_cell(graphics, DESK_CAST_LEGEND, 4,
                                     facing * 2, &step) ||
            step.width != (int)DESK_LEGEND_CELL ||
            step.height != (int)DESK_LEGEND_CELL ||
            !build_opposite_step_cell(destination, &step))
            return false;
        graphics->legend_opposite_step_cells[facing] =
            ki_td_rgba8_make(destination, (int)DESK_LEGEND_CELL,
                             (int)DESK_LEGEND_CELL);
    }
    return true;
}

static bool rebuild_hero_motion_cast(desk_graphics *graphics, desk_cast cast)
{
    const size_t cell_bytes =
        (size_t)DESK_CHARACTER_CELL * DESK_CHARACTER_CELL * 4u;
    ki_td_rgba8 cells[6];
    int cast_index = (int)cast - 1;
    int column;
    int variant;
    if (!graphics->hero_motion_pixels) return false;
    for (column = 0; column < 6; ++column)
        if (!desk_graphics_hero_cell(graphics, cast, column, 0,
                                     &cells[column]))
            return false;
    for (variant = 0;
         variant < DESK_HERO_MOTION_VARIANT_COUNT; ++variant) {
        uint8_t *destination =
            graphics->hero_motion_pixels +
            ((size_t)cast_index * DESK_HERO_MOTION_VARIANT_COUNT +
             (size_t)variant) * cell_bytes;
        bool built;
        if (variant == DESK_HERO_MOTION_DOWN_A)
            built = cast == DESK_CAST_CHUMRUNNER ?
                build_leg_motion_cell(
                    destination, &cells[0], &cells[1], false) :
                build_straight_step_cell(
                    destination, &cells[0], false);
        else if (variant == DESK_HERO_MOTION_DOWN_B)
            built = cast == DESK_CAST_CHUMRUNNER ?
                build_leg_motion_cell(
                    destination, &cells[0], &cells[2], false) :
                build_straight_step_cell(
                    destination, &cells[0], true);
        else if (variant == DESK_HERO_MOTION_UP_STEP)
            built = build_leg_motion_cell(
                destination, &cells[3], &cells[3], true);
        else
            built = build_full_mirror(
                destination,
                &cells[
                    variant == DESK_HERO_MOTION_MIRRORED_SIDE ? 4 : 5]);
        if (!built) return false;
        graphics->hero_motion_cells[cast_index][variant] =
            ki_td_rgba8_make(destination, (int)DESK_CHARACTER_CELL,
                             (int)DESK_CHARACTER_CELL);
    }
    return true;
}

static bool rebuild_motion_cells(desk_graphics *graphics)
{
    int cast_index;
    if (!rebuild_legend_steps(graphics)) return false;
    for (cast_index = 1; cast_index < DESK_CAST_COUNT; ++cast_index)
        if (!rebuild_hero_motion_cast(graphics, (desk_cast)cast_index))
            return false;
    return true;
}

static bool load_graphic(desk_graphics *graphics,
                         const kilix_asset_locator *locator,
                         const kilix_asset_limits *limits,
                         int index)
{
    const graphic_spec *spec = &GRAPHIC_SPECS[index];
    const kilix_asset_manifest_atlas *record;
    const kilix_asset_image *image = NULL;
    char path[1024];
    kilix_asset_status status;

    record = kilix_asset_manifest_find_atlas(&graphics->manifest, spec->id);
    if (!record || record->width != spec->width ||
        record->height != spec->height ||
        record->columns != spec->columns || record->rows != spec->rows)
        return false;
    status = kilix_asset_resolve(locator, record->path, path, sizeof path);
    if (status == KILIX_ASSET_OK)
        status = kilix_asset_cache_load_png(&graphics->cache, path, limits,
                                            &image);
    if (status != KILIX_ASSET_OK || !image ||
        image->width != spec->width || image->height != spec->height ||
        !kilix_asset_atlas_init_grid(&graphics->atlases[index], image,
                                     spec->columns, spec->rows))
        return false;
    graphics->images[index] = image;
    return true;
}

bool desk_graphics_init(desk_graphics *graphics, const char *asset_root)
{
    kilix_asset_locator locator;
    kilix_asset_limits limits;
    char manifest_path[1024];
    kilix_asset_status status;
    int index;

    if (!graphics) return false;
    (void)memset(graphics, 0, sizeof *graphics);
    make_locator(&locator, asset_root);
    make_limits(&limits);
    if (!kilix_asset_cache_init(&graphics->cache,
                                (size_t)DESK_GRAPHICS_COUNT,
                                DESK_ASSET_CACHE_LIMIT))
        return false;
    graphics->cache_ready = true;

    status = kilix_asset_resolve(&locator, DESK_ASSET_MANIFEST,
                                 manifest_path, sizeof manifest_path);
    if (status == KILIX_ASSET_OK)
        status = kilix_asset_manifest_load_json(
            &graphics->manifest, manifest_path, DESK_ASSET_MANIFEST_LIMIT);
    if (status != KILIX_ASSET_OK || !graphics->manifest.game ||
        strcmp(graphics->manifest.game, "kilix-land-desktop") != 0) {
        desk_graphics_shutdown(graphics);
        return false;
    }
    for (index = 0; index < DESK_GRAPHICS_COUNT; ++index) {
        if (!load_graphic(graphics, &locator, &limits, index)) {
            desk_graphics_shutdown(graphics);
            return false;
        }
    }
    graphics->legend_opposite_step_pixels =
        malloc((size_t)DESK_LEGEND_CELL * DESK_LEGEND_CELL * 4u * 4u);
    graphics->hero_motion_pixels =
        malloc((size_t)DESK_CHARACTER_CELL * DESK_CHARACTER_CELL * 4u *
               (size_t)(DESK_CAST_COUNT - 1) *
               (size_t)DESK_HERO_MOTION_VARIANT_COUNT);
    if (!graphics->legend_opposite_step_pixels ||
        !graphics->hero_motion_pixels ||
        !rebuild_motion_cells(graphics)) {
        desk_graphics_shutdown(graphics);
        return false;
    }
    return true;
}

void desk_graphics_shutdown(desk_graphics *graphics)
{
    int index;
    if (!graphics) return;
    for (index = 0; index < DESK_MAX_ROOMS; ++index) {
        free(graphics->plate_pixels[index]);
        free(graphics->behind_masks[index]);
    }
    free(graphics->outfit_pixels);
    free(graphics->legend_opposite_step_pixels);
    free(graphics->hero_motion_pixels);
    kilix_asset_manifest_clear(&graphics->manifest);
    if (graphics->cache_ready) kilix_asset_cache_clear(&graphics->cache);
    (void)memset(graphics, 0, sizeof *graphics);
}

size_t desk_graphics_loaded_count(const desk_graphics *graphics)
{
    size_t count = 0u;
    int index;
    if (!graphics) return 0u;
    for (index = 0; index < DESK_GRAPHICS_COUNT; ++index)
        if (kilix_asset_image_is_valid(graphics->images[index])) ++count;
    return count;
}

bool desk_graphics_image(const desk_graphics *graphics, desk_graphic graphic,
                         ki_td_rgba8 *image)
{
    const kilix_asset_image *asset;
    if (image) *image = (ki_td_rgba8){0};
    if (!graphics || !image || (int)graphic < 0 ||
        (int)graphic >= DESK_GRAPHICS_COUNT)
        return false;
    asset = graphics->images[graphic];
    if (!kilix_asset_image_is_valid(asset) ||
        asset->width > (uint32_t)INT_MAX || asset->height > (uint32_t)INT_MAX)
        return false;
    *image = ki_td_rgba8_make(asset->pixels, (int)asset->width,
                              (int)asset->height);
    image->stride = asset->stride;
    return ki_td_rgba8_is_valid(image);
}

bool desk_graphics_cell(const desk_graphics *graphics, desk_graphic graphic,
                        int column, int row, ki_td_rgba8 *image)
{
    kilix_asset_region region;
    if (image) *image = (ki_td_rgba8){0};
    if (!graphics || !image || (int)graphic < 0 ||
        (int)graphic >= DESK_GRAPHICS_COUNT || column < 0 || row < 0)
        return false;
    region = kilix_asset_atlas_cell(&graphics->atlases[graphic],
                                    (uint32_t)column, (uint32_t)row);
    if (!kilix_asset_region_is_valid(&region) ||
        region.width > (uint32_t)INT_MAX ||
        region.height > (uint32_t)INT_MAX)
        return false;
    *image = ki_td_rgba8_make(region.pixels, (int)region.width,
                              (int)region.height);
    image->stride = region.stride;
    return ki_td_rgba8_is_valid(image);
}

/* Walk-behind mask: rooms/<style>/<plate>-behind.png, exactly plate-sized,
 * one region id per pixel (0 = none, 1..15 = region). The cook writes 8-bit
 * grayscale, which the kilix-assets decoder expands to RGBA with R = the
 * authored value, so the reduction takes the red channel; out-of-range
 * values (>= 16) reduce to 0, mirroring AGS validate_mask. A missing or
 * malformed mask is not an error — the room just has no walk-behinds. */
static uint8_t *load_behind_mask(const kilix_asset_locator *locator,
                                 const kilix_asset_limits *limits,
                                 const char *style_directory,
                                 const char *plate)
{
    char relative[96];
    char path[1024];
    kilix_asset_image image = {0};
    uint8_t *mask;
    int written;
    int y;

    written = snprintf(relative, sizeof relative,
                       "assets/graphics/rooms/%s/%s-behind.png",
                       style_directory, plate);
    if (written < 0 || (size_t)written >= sizeof relative ||
        !kilix_asset_path_is_safe(relative))
        return NULL;
    if (kilix_asset_resolve(locator, relative, path, sizeof path) !=
        KILIX_ASSET_OK)
        return NULL;
    if (kilix_asset_image_load_png(&image, path, limits) != KILIX_ASSET_OK)
        return NULL;
    if (!kilix_asset_image_is_valid(&image) ||
        image.width != (uint32_t)DESK_PLATE_WIDTH ||
        image.height != (uint32_t)DESK_PLATE_HEIGHT) {
        kilix_asset_image_clear(&image);
        return NULL;
    }
    mask = malloc((size_t)DESK_PLATE_WIDTH * (size_t)DESK_PLATE_HEIGHT);
    if (!mask) {
        kilix_asset_image_clear(&image);
        return NULL;
    }
    for (y = 0; y < DESK_PLATE_HEIGHT; ++y) {
        const uint8_t *row = image.pixels + (size_t)y * image.stride;
        uint8_t *out = mask + (size_t)y * (size_t)DESK_PLATE_WIDTH;
        int x;

        for (x = 0; x < DESK_PLATE_WIDTH; ++x) {
            uint8_t value = row[(size_t)x * 4u];

            out[x] = value < UINT8_C(16) ? value : UINT8_C(0);
        }
    }
    kilix_asset_image_clear(&image);
    return mask;
}

bool desk_graphics_load_plates(desk_graphics *graphics, const char *asset_root,
                               const desk_world *world, desk_cast style)
{
    static const char *const STYLE_DIRECTORIES[DESK_CAST_COUNT] = {
        "legend", "chumrunner", "fantasy", "pleb-bound"
    };
    kilix_asset_locator locator;
    kilix_asset_limits limits;
    int room;
    int room_count;
    if (!graphics || !world || (int)style < 0 ||
        (int)style >= DESK_CAST_COUNT)
        return false;
    for (room = 0; room < DESK_MAX_ROOMS; ++room) {
        free(graphics->plate_pixels[room]);
        graphics->plate_pixels[room] = NULL;
        graphics->plates[room] = (ki_td_rgba8){0};
        graphics->plate_loaded[room] = false;
        free(graphics->behind_masks[room]);
        graphics->behind_masks[room] = NULL;
    }
    make_locator(&locator, asset_root);
    make_limits(&limits);
    room_count = world->room_count;
    if (room_count > DESK_MAX_ROOMS) room_count = DESK_MAX_ROOMS;
    for (room = 0; room < room_count; ++room) {
        char relative[96];
        char path[1024];
        kilix_asset_image image = {0};
        uint8_t *pixels;
        int width;
        int height;
        int written;
        int y;
        if (world->rooms[room].plate[0] == '\0') continue;
        written = snprintf(relative, sizeof relative,
                           "assets/graphics/rooms/%s/%s.png",
                           STYLE_DIRECTORIES[style],
                           world->rooms[room].plate);
        if (written < 0 || (size_t)written >= sizeof relative ||
            !kilix_asset_path_is_safe(relative))
            continue;
        /* A missing plate is not an error; render falls back procedurally. */
        if (kilix_asset_resolve(&locator, relative, path,
                                sizeof path) != KILIX_ASSET_OK)
            continue;
        if (kilix_asset_image_load_png(&image, path, &limits) !=
            KILIX_ASSET_OK)
            continue;
        /* The plate contract is exactly 1280x720 (IMPLEMENTATION.md section
         * 9): a wrong-size cook is a pipeline mistake, and rendering it
         * scaled would hide that. Fall back procedurally instead. */
        if (!kilix_asset_image_is_valid(&image) ||
            image.width != (uint32_t)DESK_PLATE_WIDTH ||
            image.height != (uint32_t)DESK_PLATE_HEIGHT) {
            kilix_asset_image_clear(&image);
            continue;
        }
        width = (int)image.width;
        height = (int)image.height;
        pixels = malloc((size_t)image.width * (size_t)image.height * 4u);
        if (!pixels) {
            kilix_asset_image_clear(&image);
            return false;
        }
        for (y = 0; y < height; ++y)
            (void)memcpy(pixels + (size_t)y * (size_t)width * 4u,
                         image.pixels + (size_t)y * image.stride,
                         (size_t)width * 4u);
        kilix_asset_image_clear(&image);
        graphics->plate_pixels[room] = pixels;
        graphics->plates[room] = ki_td_rgba8_make(pixels, width, height);
        graphics->plate_loaded[room] =
            ki_td_rgba8_is_valid(&graphics->plates[room]);
        if (!graphics->plate_loaded[room]) {
            free(pixels);
            graphics->plate_pixels[room] = NULL;
            graphics->plates[room] = (ki_td_rgba8){0};
            continue;
        }
        graphics->behind_masks[room] =
            load_behind_mask(&locator, &limits, STYLE_DIRECTORIES[style],
                             world->rooms[room].plate);
    }
    return true;
}

bool desk_graphics_plate(const desk_graphics *graphics, int room,
                         ki_td_rgba8 *image)
{
    if (image) *image = (ki_td_rgba8){0};
    if (!graphics || !image || room < 0 || room >= DESK_MAX_ROOMS ||
        !graphics->plate_loaded[room])
        return false;
    *image = graphics->plates[room];
    return ki_td_rgba8_is_valid(image);
}

const uint8_t *desk_graphics_behind_mask(const desk_graphics *graphics,
                                         int room)
{
    if (!graphics || room < 0 || room >= DESK_MAX_ROOMS ||
        !graphics->plate_loaded[room])
        return NULL;
    return graphics->behind_masks[room];
}

bool desk_graphics_set_outfit(desk_graphics *graphics, desk_cast cast,
                              int outfit)
{
    outfit_palette palette;
    int columns;
    int rows;
    uint32_t cell_size;
    size_t cell_bytes;
    size_t total_cells;
    int index;
    if (!graphics || (int)cast < 0 || (int)cast >= DESK_CAST_COUNT ||
        outfit < 0 || outfit >= DESK_OUTFIT_COUNT)
        return false;
    columns = cast == DESK_CAST_LEGEND ?
        DESK_LEGEND_SHEET_COLUMNS : DESK_PAINTED_HERO_COLUMNS;
    rows = cast == DESK_CAST_LEGEND ? DESK_LEGEND_SHEET_ROWS : 1;
    cell_size = cast == DESK_CAST_LEGEND ?
        DESK_LEGEND_CELL : DESK_CHARACTER_CELL;
    cell_bytes = (size_t)cell_size * cell_size * 4u;
    total_cells = (size_t)columns * (size_t)rows;
    graphics->outfit_ready = false;
    free(graphics->outfit_pixels);
    graphics->outfit_pixels = malloc(cell_bytes * total_cells);
    if (!graphics->outfit_pixels) return false;
    for (index = 0; index < (int)total_cells; ++index) {
        uint8_t *destination =
            graphics->outfit_pixels + (size_t)index * cell_bytes;
        ki_td_rgba8 source;
        int column = index % columns;
        int row = index / columns;
        bool loaded = cast == DESK_CAST_LEGEND ?
            desk_graphics_cell(graphics, DESK_GRAPHIC_LEGEND_PLAYER,
                               column, row, &source) :
            desk_graphics_cell(graphics, character_graphic(cast),
                               column, 0, &source);
        if (!loaded || source.width != (int)cell_size ||
            source.height != (int)cell_size) {
            free(graphics->outfit_pixels);
            graphics->outfit_pixels = NULL;
            return false;
        }
        copy_cell(destination, &source);
        graphics->outfit_cells[index] =
            ki_td_rgba8_make(destination, (int)cell_size, (int)cell_size);
    }
    if (outfit != 0) {
        /* Garment hue is measured from the idle cell every time; a failed
         * measurement leaves the authored colors rather than guessing. */
        float garment_hue;
        uint32_t swatch = OUTFIT_SWATCHES[outfit];
        if (cast == DESK_CAST_LEGEND) {
            if (measure_garment_hue(&graphics->outfit_cells[0],
                                    DESK_MIN_LEGEND_SATURATION, true,
                                    &garment_hue)) {
                collect_legend_palette(&graphics->outfit_cells[0],
                                       garment_hue, &palette);
                if (palette.count > 0) {
                    map_legend_palette(&palette, swatch);
                    remap_legend_pixels(graphics->outfit_pixels,
                                        cell_bytes * total_cells / 4u,
                                        &palette);
                }
            }
        } else if (measure_garment_hue(&graphics->outfit_cells[0],
                                       DESK_MIN_GARMENT_SATURATION, true,
                                       &garment_hue)) {
            hsv_color swatch_hsv = rgb_to_hsv((uint8_t)(swatch >> 16),
                                              (uint8_t)(swatch >> 8),
                                              (uint8_t)swatch);
            rotate_painted_pixels(graphics->outfit_pixels,
                                  cell_bytes * total_cells / 4u,
                                  garment_hue,
                                  hue_offset(swatch_hsv.hue, garment_hue));
        }
    }
    graphics->outfit_cast = cast;
    graphics->outfit_index = outfit;
    graphics->outfit_columns = columns;
    graphics->outfit_rows = rows;
    /* outfit_ready must be set before the rebuild so hero_cell serves the
     * recolored cells to it, but a failed rebuild would leave motion cells
     * mixing old and new outfits — withdraw the flag in that case. */
    graphics->outfit_ready = true;
    if (!rebuild_motion_cells(graphics)) {
        graphics->outfit_ready = false;
        return false;
    }
    return true;
}

bool desk_graphics_hero_cell(const desk_graphics *graphics, desk_cast cast,
                             int column, int row, ki_td_rgba8 *image)
{
    int columns;
    int rows;
    if (image) *image = (ki_td_rgba8){0};
    if (!graphics || !image || (int)cast < 0 ||
        (int)cast >= DESK_CAST_COUNT || column < 0 || row < 0)
        return false;
    columns = cast == DESK_CAST_LEGEND ?
        DESK_LEGEND_SHEET_COLUMNS : DESK_PAINTED_HERO_COLUMNS;
    rows = cast == DESK_CAST_LEGEND ? DESK_LEGEND_SHEET_ROWS : 1;
    if (column >= columns || row >= rows) return false;
    if (graphics->outfit_ready && graphics->outfit_cast == cast) {
        *image = graphics->outfit_cells[
            (size_t)row * (size_t)graphics->outfit_columns +
            (size_t)column];
        return ki_td_rgba8_is_valid(image);
    }
    if (cast == DESK_CAST_LEGEND)
        return desk_graphics_cell(graphics, DESK_GRAPHIC_LEGEND_PLAYER,
                                  column, row, image);
    return desk_graphics_cell(graphics, character_graphic(cast),
                              column, 0, image);
}

bool desk_graphics_hero_motion_cell(const desk_graphics *graphics,
                                    desk_cast cast,
                                    desk_hero_motion_variant variant,
                                    ki_td_rgba8 *image)
{
    int cast_index;
    if (image) *image = (ki_td_rgba8){0};
    if (!graphics || !image || (int)cast < (int)DESK_CAST_CHUMRUNNER ||
        (int)cast > (int)DESK_CAST_PLEB_BOUND || (int)variant < 0 ||
        (int)variant >= DESK_HERO_MOTION_VARIANT_COUNT)
        return false;
    cast_index = (int)cast - 1;
    *image = graphics->hero_motion_cells[cast_index][variant];
    return ki_td_rgba8_is_valid(image);
}

bool desk_graphics_legend_opposite_step(const desk_graphics *graphics,
                                        desk_facing facing,
                                        ki_td_rgba8 *image)
{
    if (image) *image = (ki_td_rgba8){0};
    if (!graphics || !image || (int)facing < 0 ||
        (int)facing > (int)DESK_FACING_UP)
        return false;
    *image = graphics->legend_opposite_step_cells[facing];
    return ki_td_rgba8_is_valid(image);
}

uint32_t desk_outfit_color(desk_cast cast, int outfit)
{
    if ((int)cast < 0 || (int)cast >= DESK_CAST_COUNT ||
        outfit < 0 || outfit >= DESK_OUTFIT_COUNT)
        return 0u;
    return outfit == 0 ? ORIGINAL_OUTFIT_COLORS[cast] :
                         OUTFIT_SWATCHES[outfit];
}

const char *desk_outfit_name(desk_cast cast, int outfit)
{
    if ((int)cast < 0 || (int)cast >= DESK_CAST_COUNT ||
        outfit < 0 || outfit >= DESK_OUTFIT_COUNT)
        return "";
    return OUTFIT_NAMES[outfit];
}
