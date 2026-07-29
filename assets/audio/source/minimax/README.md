# Kilix Land MiniMax source masters

This directory is the retained-source side of the M4 audio pipeline. No master
has been generated or approved yet. Five reviewed instrumental MP3 masters will
be committed here; runtime WAVs are derived offline and live under
`assets/audio/generated/music/`.

The source contract is MiniMax Music `music-2.5+`, instrumental mode, MP3 at
44,100 Hz and 256 kb/s, with no reference audio. `tools/generate_audio.py`
never calls the API. It only prints the requests, records reviewed local
masters, and builds from already-pinned files:

```sh
python3 tools/generate_audio.py requests
# From the project root, run only the requests you deliberately approve.
# Their command records invoke ~/minimax_asset_generation/minimax/minimax_music.py.
# Listen to all five retained MP3s before recording their source ledger.
python3 tools/generate_audio.py record-minimax --generated-on YYYY-MM-DD
python3 tools/generate_audio.py build
python3 tools/validate_audio.py
```

`record-minimax` probes all five files, requires the format above, and writes
`source/minimax/provenance.json` with the exact prompt, date, model, byte count,
SHA-256, instrumental flag, and reference-audio declaration. Do not record,
build, or commit a master until it has been listened to and approved.

## `source/minimax/legend-home.mp3`

Original instrumental home ambience for a warm post-industrial pixel-art cottage. Psychedelic folk-electronica with plucked dulcimer, muted guitar, woody bass, hand percussion, and soft luminous analog synthesizer. Cozy, curious, lightly adventurous, and unobtrusive beneath dialogue. 104 BPM in 4/4 with phrase changes exactly every four bars and a stable sixteen-bar middle section suitable for looping. Immediate musical opening, steady energy, no long intro, no finale, no fade, no vocals, no lyrics, no sound effects, and no reference to any existing melody.

The retained loop begins at bar 4, keeps 16 bars, and folds the next 2 bars
over the head.

## `source/minimax/chumrunner-home.mp3`

Original instrumental cyberpunk safehouse ambience for a neon noir apartment. Restrained downtempo electro with dry drum machine, rounded sub bass, glassy FM keys, filtered arpeggios, and sparse noir electric piano. Alert but safe, technological without harshness, and unobtrusive beneath dialogue. 112 BPM in 4/4 with phrase changes exactly every four bars and a stable sixteen-bar middle section suitable for looping. Immediate musical opening, steady energy, no long intro, no finale, no fade, no vocals, no lyrics, no sound effects, and no reference to any existing melody.

The retained loop begins at bar 4, keeps 16 bars, and folds the next 2 bars
over the head.

## `source/minimax/fantasy-home.mp3`

Original instrumental fantasy lodge ambience with the clarity of a classic sixteen-bit role-playing game. Gentle lute, harp, celesta, warm strings, wooden flute, and restrained frame drum, with an original hopeful modal motif. Hearth-lit, magical, domestic, and unobtrusive beneath dialogue. 96 BPM in 4/4 with phrase changes exactly every four bars and a stable sixteen-bar middle section suitable for looping. Immediate musical opening, steady energy, no long intro, no finale, no fade, no vocals, no lyrics, no sound effects, and no reference to any existing melody.

The retained loop begins at bar 4, keeps 16 bars, and folds the next 2 bars
over the head.

## `source/minimax/pleb-bound-home.mp3`

Original instrumental quirky suburban role-playing home theme. Friendly lo-fi electric piano, clean guitar, melodic bass, tiny toy percussion, brushed drums, and warm cartridge-like synth colors. Everyday, gently funny, sincere, and unobtrusive beneath dialogue. 108 BPM in 4/4 with phrase changes exactly every four bars and a stable sixteen-bar middle section suitable for looping. Immediate musical opening, steady energy, no long intro, no finale, no fade, no vocals, no lyrics, no sound effects, and no reference to any existing melody.

The retained loop begins at bar 4, keeps 16 bars, and folds the next 2 bars
over the head.

## `source/minimax/shared-yard.mp3`

Original instrumental outdoor home-yard theme that can bridge pixel folk, cyberpunk, fantasy, and quirky suburban art styles. Acoustic guitar harmonics, marimba, soft hand percussion, warm bass, airy flute, and subtle analog pads in a calm original motif. Open-air, neighborly, exploratory, and unobtrusive beneath dialogue. 100 BPM in 4/4 with phrase changes exactly every four bars and a stable sixteen-bar middle section suitable for looping. Immediate musical opening, steady energy, no long intro, no finale, no fade, no vocals, no lyrics, no sound effects, and no reference to any existing melody.

The retained loop begins at bar 4, keeps 16 bars, and folds the next 2 bars
over the head.
