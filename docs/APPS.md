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
| `maintenance` | shed (yard) | tab: `kilix update` (via resolved kilix launcher) |

The file-manager candidate table (used by `files` and `trash`):
`mc`, `ranger`, `nnn`, `lf` — first found on PATH wins. All run in a tab.

## Internal targets (never spawn; handled in desk.c)

| Target | Object (room) | Behavior |
|---|---|---|
| `wardrobe` | wardrobe (bedroom) | reopen the character wizard seeded with the profile |
| `bed` | bed (bedroom) | confirm → save profile → clean exit |
| `status-board` | notice board (kitchen) | in-process status panel (reads files only) |
| `gate-locked` | gate (yard) | toast — the street extension point |

Dialogue with housemate NPCs is proximity + interact, also fully in-process.
