# kilix-land-desktop

A full Kilix desktop provider, alongside Kilix 95, Kilix Cap, and Kilix TUI,
that plays like kilix-land. On first login you create a character — pick a cast
member from the four-game crossover roster (Legend of Kilix, Chumrunner, Kilix
Fantasy, Pleb Bound), name them, choose an outfit color — and they wake up in a
house rendered in their source game's art style. Walk between rooms and the
yard; interacting with objects (desk computer, filing cabinet, TV, stereo,
mailbox, bed…) engages OS and desktop functions the way kilix-cap's rooms do,
but through a walkable avatar.

## Watch the episode

https://github.com/user-attachments/assets/66a1619d-0b16-4d7f-b4c9-8f860546f263

**[Desktop Four: Kilix Land](https://github.com/itsmygithubacct/kilix-land-desktop/releases/download/media-v1/08-kilix-land.mp4)**
— part eight of *Kilix, Pleb, and Plebian-OS: A Desktop Built Inside a Terminal*, the ten-part
stack series (1920×1080, 2m37s, 9 MB; published as a
[media release](https://github.com/itsmygithubacct/kilix-land-desktop/releases/tag/media-v1) so a
clone stays small). The [full series](https://github.com/itsmygithubacct/plebian-os#watch-the-series) (31m22s)
lives on `plebian-os` and plays at [plebian-os.com](https://plebian-os.com/#watch).

## Items

The house also holds portable items with per-style art drawn from the four
source games: pick them up off the floor (Enter), carry them in a
twelve-slot hotbar (`1`–`9` `0` `-` `=` select, `[` `]` cycle), and set
them down again (`Q`). Space uses the selected item: drink the coffee (the
effect lands on the swallow frame of a real animation), place the
houseplant on any clear floor a green ghost approves, put the record on
the stereo (which plays through the same launcher path as pressing it),
swing the toolbox at the shed to open maintenance on the impact frame,
hand a housemate a postcard at arm's reach, or pin on the lantern
accessory. Item state — inventory, placed decor, what the stereo is
holding, friendships — persists in a `world.state` record beside the
character profile; corrupting one never touches the other, and items whose
definitions disappear come back when the definition does.

## Build

```
make            # builds ./kilix-land-desktop
make test       # parity check + selftest + headless render fixtures
```

The engine stack comes from one pinned `kilix-game-sdk` submodule. Compatibility
symlinks expose kilix-game-kit, kilix-top-down-engine, kilix-assets, and
kilix-ui at their established `third_party/` paths.

## Layout

- `src/` — C11 sources: terminal host (`main.c`), world sim (`desk.c`), world
  manifest (`rooms.c`), shared strict JSON reader (`json_reader.c`), item
  catalog + inventory semantics (`items.c`), durable world record
  (`world_state.c`), config-home store (`state_store.c`), launch registry
  (`launcher.c`), atlas load + outfit recolor (`graphics.c`), drawing
  (`render.c`), cues (`audio.c`).
- `assets/world/world.json` — the room graph: walk rects, obstacles, doors,
  objects, authored item spawns. Objects reference launch *target ids*; argv
  lives only in `src/launcher.c`, and item data can never name a command,
  path, or callback.
- `assets/world/items.json` — the item catalog: qualified ids, families,
  compiled behavior names, tags, receiver rules.
- `assets/graphics/`, `assets/audio/` — parity-managed copies from the four
  source games (byte-identical to their committed history), maintained by
  `tools/sync_source_parity.py` (`make parity-sync` / `make parity-check`).
  Room plates land in `assets/graphics/rooms/<style>/` after art review;
  `assets/graphics/items/` is the desktop-items atlas composed from each
  style's own game by `tools/sync_item_art.py` (`make items-art`).
- `tools/` — parity sync, world/items validators, helpers launched in tabs.

The study's **Programs** object opens the shared `kilix-launcher`; its direct
PDF Conversion row therefore becomes a Land tab/pane. The stdlib-only local
catalog fallback exposes the same `kilix app run kilix-pdf-conversion` command
when the shared launcher package is not installed.

## Provider

Runs standalone in any kitty-graphics terminal (`./kilix-land-desktop` from
the checkout). Kilix also exposes it as a native executable provider:

```
kilix land
kilix desktop land
KILIX_DESKTOP_PROVIDER=land kilix desktop
```

On first use, Kilix clones its immutable pinned revision beneath
`${GPU_TERMINAL_SOURCE_HOME:-~/.local/gpu_terminal/sources}/kilix-desktops`, builds it, and
validates that the result is a regular executable. Existing development
checkouts are built in place and are never reset unless an explicit
`KILIX_LAND_DESKTOP_REF` is supplied.

Environment: `KILIX_LAND_DESKTOP_ASSETS` (checkout root containing `assets/`
and `tools/`), `KILIX_LAND_DESKTOP_CONFIG_HOME` (absolute profile-store
override), `KILIX_LAND_DESKTOP_EXTERNAL_APPS=0` (disable all launches),
`KILIX_LAND_DESKTOP_AUDIO=0` (mute). Provider installation can be controlled
with `KILIX_LAND_DESKTOP_AUTO_INSTALL`, `KILIX_LAND_DESKTOP_DIR`,
`KILIX_LAND_DESKTOP_REPO`, and `KILIX_LAND_DESKTOP_REF`.

Review CLI: `./kilix-land-desktop --screenshot out.ppm --room study
--style chumrunner` renders any room in any style headlessly.

## Licensing

Code and assets are licensed separately.

- **Source code, build files, and documentation** — MIT. See `LICENSE`.
- **Everything under `assets/`** — Creative Commons
  Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0).
  See `assets/LICENSE`. Share and adapt with attribution, non-commercially,
  under the same license.

The cast atlases and audio cues are copies of art committed by the four source
games; the room plates are original art for this project, produced with an
image-generation pipeline whose prompts are recorded verbatim in
`assets/graphics/rooms/PROMPTS.md`. `assets/LICENSE` documents the provenance
of each group.

The source games are separate repositories. `make test` verifies asset parity
against whichever of them are checked out beside this one and skips the rest,
so a clone without them still builds and tests cleanly; `make
parity-check-strict` fails unless all four are present.
