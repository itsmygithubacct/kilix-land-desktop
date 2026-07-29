#include "kilix_land_desktop.h"

#include <string.h>

#define DESK_AUDIO_CUE(cast, event) \
    ((uint32_t)(cast) * (uint32_t)DESK_AUDIO_EVENT_COUNT + \
     (uint32_t)(event))
#define DESK_AUDIO_CUE_COUNT \
    ((uint32_t)DESK_CAST_COUNT * (uint32_t)DESK_AUDIO_EVENT_COUNT)
#define CUE(cast, event, path) \
    {DESK_AUDIO_CUE((cast), (event)), 0u, (path), 1.0f, 1.0f, true}

static const kilix_game_audio_cue_spec AUDIO_CUES[] = {
    CUE(DESK_CAST_LEGEND, DESK_AUDIO_UI_MOVE,
        "assets/audio/legend/ui-move.wav"),
    CUE(DESK_CAST_LEGEND, DESK_AUDIO_UI_CONFIRM,
        "assets/audio/legend/ui-confirm.wav"),
    CUE(DESK_CAST_LEGEND, DESK_AUDIO_DIALOGUE,
        "assets/audio/legend/dialogue.wav"),

    CUE(DESK_CAST_CHUMRUNNER, DESK_AUDIO_UI_MOVE,
        "assets/audio/chumrunner/ui-move.wav"),
    CUE(DESK_CAST_CHUMRUNNER, DESK_AUDIO_UI_CONFIRM,
        "assets/audio/chumrunner/ui-confirm.wav"),
    CUE(DESK_CAST_CHUMRUNNER, DESK_AUDIO_DIALOGUE,
        "assets/audio/chumrunner/dialogue.wav"),

    CUE(DESK_CAST_FANTASY, DESK_AUDIO_UI_MOVE,
        "assets/audio/fantasy/ui-move.wav"),
    CUE(DESK_CAST_FANTASY, DESK_AUDIO_UI_CONFIRM,
        "assets/audio/fantasy/ui-confirm.wav"),
    CUE(DESK_CAST_FANTASY, DESK_AUDIO_DIALOGUE,
        "assets/audio/fantasy/dialogue.wav"),

    CUE(DESK_CAST_PLEB_BOUND, DESK_AUDIO_UI_MOVE,
        "assets/audio/pleb-bound/ui-move.wav"),
    CUE(DESK_CAST_PLEB_BOUND, DESK_AUDIO_UI_CONFIRM,
        "assets/audio/pleb-bound/ui-confirm.wav"),
    CUE(DESK_CAST_PLEB_BOUND, DESK_AUDIO_DIALOGUE,
        "assets/audio/pleb-bound/dialogue.wav")
};

_Static_assert(
    sizeof AUDIO_CUES / sizeof AUDIO_CUES[0] ==
        DESK_AUDIO_SOURCE_CUE_COUNT,
    "desktop audio cue table does not match its public contract");

bool desk_audio_init(desk_audio *audio, const char *asset_root, bool live)
{
    kilix_game_audio_options options;
    char error[DESK_ERROR_CAPACITY];
    if (!audio || !asset_root) return false;
    (void)memset(audio, 0, sizeof *audio);
    kilix_game_audio_options_init(&options);
    options.cue_count = DESK_AUDIO_CUE_COUNT;
    options.random_seed = UINT32_C(0x4445534b);
    options.data.environment_variable = "KILIX_LAND_DESKTOP_ASSETS";
    options.data.local_root = asset_root;
    options.cues = AUDIO_CUES;
    options.cue_spec_count =
        sizeof AUDIO_CUES / sizeof AUDIO_CUES[0];
    options.mixer.offline = !live;
    options.require_mixer = !live;
    return kilix_game_audio_init(
        &audio->runtime, &options, error, sizeof error);
}

void desk_audio_play(desk_audio *audio, desk_cast cast,
                     desk_audio_event event)
{
    if (!audio || cast < DESK_CAST_LEGEND ||
        cast >= DESK_CAST_COUNT ||
        event < DESK_AUDIO_UI_MOVE ||
        event >= DESK_AUDIO_EVENT_COUNT)
        return;
    /* Every desktop event is a UI-class cue (land routes UI_MOVE..DIALOGUE
     * onto the UI bus; the desktop bank stops there). */
    (void)kilix_game_audio_play(
        &audio->runtime, DESK_AUDIO_CUE(cast, event),
        KILIX_GAME_AUDIO_BUS_UI, 1.0f, 1.0f);
}

void desk_audio_update(desk_audio *audio, float seconds)
{
    if (audio) kilix_game_audio_update(&audio->runtime, seconds);
}

void desk_audio_shutdown(desk_audio *audio)
{
    if (!audio) return;
    kilix_game_audio_shutdown(&audio->runtime);
    (void)memset(audio, 0, sizeof *audio);
}

bool desk_audio_assets_selftest(const char *asset_root, size_t *loaded_cues)
{
    desk_audio audio;
    int16_t output[8192];
    size_t index;
    bool nonzero = false;
    if (loaded_cues) *loaded_cues = 0u;
    if (!desk_audio_init(&audio, asset_root, false)) return false;
    if (audio.runtime.loaded_cues !=
        sizeof AUDIO_CUES / sizeof AUDIO_CUES[0]) {
        desk_audio_shutdown(&audio);
        return false;
    }
    desk_audio_play(
        &audio, DESK_CAST_LEGEND, DESK_AUDIO_UI_MOVE);
    desk_audio_play(
        &audio, DESK_CAST_CHUMRUNNER, DESK_AUDIO_UI_CONFIRM);
    desk_audio_play(
        &audio, DESK_CAST_FANTASY, DESK_AUDIO_DIALOGUE);
    desk_audio_play(
        &audio, DESK_CAST_PLEB_BOUND, DESK_AUDIO_UI_CONFIRM);
    (void)memset(output, 0, sizeof output);
    pcmmix_mix_block(
        &audio.runtime.mixer, output,
        sizeof output / sizeof output[0]);
    for (index = 0u; index < sizeof output / sizeof output[0]; ++index)
        if (output[index] != 0) nonzero = true;
    if (loaded_cues) *loaded_cues = audio.runtime.loaded_cues;
    desk_audio_shutdown(&audio);
    return nonzero;
}

#undef CUE
