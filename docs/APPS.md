# Launch targets

Every interactive object in `assets/world/world.json` names a *target id*.
The mapping from target id to program lives only in `src/launcher.c` — world
data can never introduce a new argv. This file documents that registry.

## Rules (kilix-cap / kilix-tui conventions)

- **No shell strings.** Every launch is a fixed argv array. External programs
  run via `posix_spawnp` with stdio detached; tab launches go through
  authenticated kitty remote control with an argv command, never `sh -c`.
- **Tabs**: `kitten @ --password-file $KILIX_RC_PASSWORD_FILE launch
  --type=tab --self --tab-title <T> -- <argv>`. The kitten binary comes from
  `$KILIX_KITTEN`, falling back to `kitten` on PATH. Remote control is
  available only when `KITTY_LISTEN_ON` is set and the password file is
  readable; this is checked per activation, never cached.
- **Resolution order** for sibling kilix features: installed command on PATH
  first, then `kilix <subcommand>` (launcher located via `$KILIX_HOME`, then
  `$GPU_TERMINAL_SOURCE_HOME/kilix/kilix`, then PATH), **never a foreign
  source checkout**. Unresolvable targets surface as a toast, not an error.
- **Kill switch**: `KILIX_LAND_DESKTOP_EXTERNAL_APPS=0` disables every
  external row below; navigation, dialogue, and internal targets keep working.
- Failures and results report through the toast/name bar; no modal dialogs.

## External targets

| Target | Object (room) | Resolution |
|---|---|---|
| `terminal` | computer (study) | tab: `bash -l` |
| `coding-agents` | dev rig (study) | `kilix-rollout-resume` → `kilix rollout`; tab |
| `files` | filing cabinet (study) | first of `mc`, `ranger`, `nnn`, `lf` on PATH, in a tab, cwd `$HOME` |
| `manuals` | bookshelf (study) | tab: `man man` (the system manual browser; richer doc shelf is post-v1) |
| `models` | model shelf (study) | `kilix-bonsai` → `kilix bonsai`; tab |
| `games` | TV (living) | `python3 tools/land_games.py` in a tab (reads the host game catalog) |
| `music` | stereo (living) | `kilix-amp` → `kilix amp`; tab |
| `voice` | phone (living) | `kilix-tts` → `kilix tts`; tab |
| `trash` | trash can (kitchen) | file manager candidate table on `~/.local/share/Trash/files`, tab |
| `mailbox` | mailbox (yard) | `kilix-memory` → `kilix memory`; tab |
| `maintenance` | shed (yard) | tab: `tools/land_config.py` — the binding configuration TUI |

The file-manager candidate table (used by `files`, `trash`, and folder
bindings): **`kilix-file`** (the kilix-tui-utils file manager) first, then
`mc`, `ranger`, `nnn`, `lf` — first found on PATH wins. All run in a tab
with the start directory as the positional argument.

## Per-object bindings

`tools/land_config.py` (the shed, or run directly / via `kilix` in a tab)
writes `<config-home>/bindings.conf`:

```
<room>.<object> = app <command and arguments>
<room>.<object> = folder </absolute/path>
```

The launcher re-reads the file on every activation; a binding overrides the
registry row for that one object. `app` values are whitespace-split into a
fixed argv — no shell, no quoting, no expansion (the same trust level as
kilix.env: user-owned config, still never a shell string). `folder` values
open in the file-manager candidate table at that path. Deleting the line
(or choosing Default in the TUI) restores the registry behavior.
`python3 tools/land_config.py --check` validates the file and runs in
`make test`.

## Internal targets (never spawn; handled in desk.c)

| Target | Object (room) | Behavior |
|---|---|---|
| `wardrobe` | wardrobe (bedroom) | reopen the character wizard seeded with the profile |
| `bed` | bed (bedroom) | confirm → save profile → clean exit |
| `status-board` | notice board (kitchen) | in-process status panel (reads files only) |
| `gate-locked` | gate (yard) | toast — the street extension point |
| `kettle` | kettle (kitchen) | receiver fixture; empty interaction shows a patient hum toast |

Dialogue with housemate NPCs is proximity + interact, also fully in-process.

## The laptop

The study spawns a **laptop** (`core:tool/laptop`) — the one item whose
Enter does not pocket it. A set-up laptop opens a session menu instead;
picking it up is the menu's explicit **PICK UP LAPTOP** row. Carried, it is
an ordinary placeable: **Space** sets it up again wherever you stand — any
desk or clear surface in any room — and the placement persists in
`world.state` like every other world item.

The menu lists **laptop profiles**, one convention shared by every kilix
desktop that ships a laptop (kilix-cap's Study laptop reads the same
directory):

```
~/.local/gpu_terminal/laptop/<id>.profile
```

(`KILIX_LAPTOP_PROFILES` overrides the directory with an absolute path —
tests use this.) A profile is a plain `KEY=value` file; `#` comments. The
first time the directory has to be created it is seeded with the bundled
examples from `assets/laptop/`; an existing directory is never reseeded.
The menu shows the first 8 profiles, re-scanned every time it opens.

| Key            | Meaning |
|---|---|
| `name=`        | display name (defaults to the file stem) |
| `desktop=`     | open a provider instead of a session: `desktop`, `95`, `xp`, `cap`, `tui`, `land` |
| `layout=`      | `splits` (panes tile one tab, default) or `tabs` |
| `pane.N.title=`| pane/tab title (N = 1..8, contiguous) |
| `pane.N.cwd=`  | working directory; `~/…` allowed. With `ssh=`, the remote directory |
| `pane.N.ssh=`  | remote destination, `[user@]host` only |
| `pane.N.cmd=`  | command to run (default: your shell). With `ssh=`, runs remotely |

A profile is either `desktop=` **or** panes, never both. Values cannot
contain double quotes or control characters — they feed a kitty session
file — and `ssh=` accepts only `[user@]host` characters, so a destination
can never smuggle options or a command. Example:

```
name=Remote Ops
layout=tabs
pane.1.cwd=~
pane.2.ssh=admin@example-host
pane.2.cwd=/var/log
pane.2.cmd=tail -f syslog
```

Choosing a **pane profile** writes a generated kitty `--session` file
under the config home and runs `kilix --detach --session <file>` — the
laptop gets its own kilix window. Choosing a **desktop profile** runs
`kilix <provider>` (`95` maps to `kilix desktop 95`). Both are fixed argv
vectors through `posix_spawnp` under the same session gate, kill switch,
and toast reporting as every row above. `./kilix-land-desktop
--laptop-test` covers profile parsing, the menu state machine, pickup and
set-up, and world.state persistence.

## Debug menu

The pause menu (Esc) grows a **DEBUG** entry whose submenu holds the
walkable-space editor: `tools/walk_editor.py` opens in a tab showing the
current style's plate on a 6px grid, walkable cells tinted translucent
white, doors/objects/NPCs/spawns marked. Left mouse paints walkable, right
paints blocked; `s` decomposes the paint into the world.json model (walk
bounding rect + up to 24 exact-cover obstacle rects), rewrites the file,
and runs the validator immediately.

The entry is controlled by `<config-home>/desktop.conf`
(`~/.local/gpu_terminal/kilix-land-desktop/desktop.conf`):

```
debug_menu = off
```

Absent file or key means enabled. The flag is re-read every time the pause
menu opens.
