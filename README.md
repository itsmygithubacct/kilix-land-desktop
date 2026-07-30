# kilix-land-desktop

A full Kilix desktop provider, alongside Kilix 95, Kilix Cap, and Kilix TUI,
that plays like kilix-land. On first login you create a character — pick a cast
member from the four-game crossover roster (Legend of Kilix, Chumrunner, Kilix
Fantasy, Pleb Bound), name them, choose an outfit color — and they wake up in a
house rendered in their source game's art style. Walk between rooms and the
yard; interacting with objects (desk computer, filing cabinet, TV, stereo,
mailbox, bed…) engages OS and desktop functions the way kilix-cap's rooms do,
but through a walkable avatar.

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
  manifest (`rooms.c`), launch registry (`launcher.c`), atlas load + outfit
  recolor (`graphics.c`), drawing (`render.c`), cues (`audio.c`).
- `assets/world/world.json` — the room graph: walk rects, obstacles, doors,
  objects. Objects reference launch *target ids*; argv lives only in
  `src/launcher.c`.
- `assets/graphics/`, `assets/audio/` — parity-managed copies from the four
  source games (byte-identical to their committed history), maintained by
  `tools/sync_source_parity.py` (`make parity-sync` / `make parity-check`).
  Room plates land in `assets/graphics/rooms/<style>/` after art review; until
  then every room renders a procedural placeholder plate.
- `tools/` — parity sync, world validator, helpers launched in tabs.

## Provider

Runs standalone in any kitty-graphics terminal (`./kilix-land-desktop` from
the checkout). To boot it as a kilix desktop session **today**, use kilix's
existing `command` provider mode — no kilix changes needed. In
`~/.local/gpu_terminal/kilix/config/kilix.env`:

```
KILIX_DESKTOP_PROVIDER=command
KILIX_DESKTOP_COMMAND=cd ~/.local/gpu_terminal/sources/kilix-land-desktop && exec ./kilix-land-desktop
KILIX_DESKTOP_NAME=Kilix Land
```

Native executable-mode wiring (`kilix land`, `KILIX_DESKTOP_PROVIDER=land`,
pinned installer — the kilix-cap pattern) remains a separate M5 integration
step. That milestone changes how Kilix selects and pins the project, not
whether Kilix Land is a desktop. Until it lands, this repository is an optional
ecosystem desktop rather than part of Kilix’s pinned dependency closure or the
four-repository Plebian-OS release core.

Environment: `KILIX_LAND_DESKTOP_ASSETS` (checkout root containing `assets/`
and `tools/`), `KILIX_LAND_DESKTOP_CONFIG_HOME` (absolute profile-store
override), `KILIX_LAND_DESKTOP_EXTERNAL_APPS=0` (disable all launches),
`KILIX_LAND_DESKTOP_AUDIO=0` (mute).

Review CLI: `./kilix-land-desktop --screenshot out.ppm --room study
--style chumrunner` renders any room in any style headlessly.
