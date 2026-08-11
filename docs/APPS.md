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
| `browser` | screen (study) | `kilix chawan`; tab |

The file-manager candidate table (used by `files`, `trash`, and folder
bindings): **`kilix-file`** (the kilix-tui-utils file manager) first, then
`mc`, `ranger`, `nnn`, `lf` — first found on PATH wins. All run in a tab
with the start directory as the positional argument.

### Rows reached from a fixture's panel

These are not authored on objects. A fixture that owns a family of
intents opens a **choice panel** (below) whose rows name them.

| Target | Reached from | Resolution |
|---|---|---|
| `settings` | **fuse box (yard)**, computer, pause menu | `kilix-settings` → `kilix settings`; tab |
| `update` | shed | `plebian-os` (the stack control TUI) → `kilix update`; tab |
| `catalog` | computer | `kilix-launcher` → `python3 tools/land_catalog.py`; tab |
| `dictation` | phone | `kilix-stt` → `kilix stt`; tab |
| `voice-help` | phone | `kilix voice status`; tab |
| `sessions` | dev rig | `kilix-pty` → `kilix pty`; tab |
| `tmux` | dev rig | `tmux-tui` → `kilix tmux`; tab |
| `mux` | dev rig | `kilix mux`; tab |
| `temps` | notice board | `kilix-temps` → `kilix temps`; tab |
| `password` | notice board (only while the login password is the default) | `passwd`; tab |
| `manual` | bookshelf | first readable of `$KILIX_HOME/README.md`, `$GPU_TERMINAL_SOURCE_HOME/kilix/README.md`, `/usr/local/share/doc/kilix/README.md`, this desktop's own README, in `less` |
| `recovery` | bookshelf | first readable of `$PLEB_RECOVERY_DOC_DST`, `/usr/local/share/doc/pleb/RECOVERY.md`, `$GPU_TERMINAL_SOURCE_HOME/pleb/docs/RECOVERY.md`, in `less` |
| `web` | screen | `kilix open-url` — a real browser when the machine has one, an in-pane render when it does not; tab |
| `power-logout` | bed | `loginctl terminate-session $XDG_SESSION_ID` |
| `power-reboot` | bed | `systemctl reboot` |
| `power-poweroff` | bed | `systemctl poweroff` |

**Power runs detached, not in a tab**, and skips the remote-control gate:
the machine is on its way down, a tab would close before anyone could
read a refusal, and a desktop that has lost remote control must still be
able to shut the machine off. The three argv vectors are the fleet's one
list — `kilix-tui-utils/src/kilix_tui/privileged.py` names exactly these —
mirrored rather than imported because a C desktop cannot import Python.
A host that grows `kilix power logout|reboot|poweroff` runs the same three
vectors, so the desktop needs no probe to agree with it. Each action is
confirmed first by the bed's own YES/NO panel.

**Programs** prefers a host-owned catalog (`kilix-launcher`, installed
alongside the shared catalog TUI) and falls back to
`tools/land_catalog.py`, which discovers XDG `.desktop` applications, the
user's own desktop-folder launchers, the stack's installed programs, and every
host catalog application reported by `kilix install --json`; catalog apps run
in the current tab through `kilix app run APP_ID`. It also offers a
run-a-command row. There is deliberately **no `kilix launcher`
rung** between them: a host that does not know the subcommand forwards it
to the terminal instead of refusing, so the ladder would spawn a broken
tab rather than fall through. Presence of the installed command is the
honest test.

**Games** hands the chosen id to `kilix games play GAME` when this
machine's `kilix` has that verb, and otherwise execs a local Kilix 95
checkout's `games.py` as before. `tools/land_games.py` probes by running
`kilix games play` with no id — every version refuses, and the refusal
names the verb only where it exists — so nothing launches to find out.

## Fixture choice panels

Most fixtures do one thing. A few own a family of intents, and those open
a small panel instead of launching straight away. The menus are compiled
tables in `src/desk.c` keyed by the object's target, so `world.json` gains
no new vocabulary, and **a `bindings.conf` override still applies to the
fixture's own default row** — the row whose target matches the object's
compiled one — never to the rows the panel adds.

| Fixture | Rows |
|---|---|
| bed (bedroom) | rest (leave the desktop) · sleep (log out) · wake anew (restart) · power down the house · stay up |
| shed (yard) | tune the appliances (bindings) · service the house (update) · shut the shed |
| phone (living) | listen (read aloud) · speak (dictation) · ask the operator (voice status) · hang up |
| dev rig (study) | coding agents · persistent sessions · tmux manager · share this session · leave it running |
| bookshelf (study) | the system manual · the recovery guide · man pages · put it back |
| screen (study) | read the screen (text browser) · open the web · switch it off |
| computer (study) | open a terminal · browse the programs · desktop settings · step away |
| notice board (kitchen) | read the board · house temperature · change the locks¹ · walk away |

¹ Only while the Plebian-OS helper confirms the login password is still
the shipped default; the same check adds a note to the board itself. Any
uncertainty — no helper, no sudo rule, a timeout — reads as "not
default", so the nag never appears spuriously.

The **notice board** reads files only, and now reports: who and which
house, the hall clock (`KILIX_CHROME_CLOCK_FORMAT` picks 12- or 24-hour),
the version and room count, host, uptime, load, free memory, battery,
network carrier, and the CPU package temperature. Lines that this machine
cannot answer are simply absent.

The **pause menu** (Esc) also carries SETTINGS: whichever room you are
standing in, the shared settings TUI is one keystroke away.

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
| `bed` | bed (bedroom) | the power panel; resting saves the profile and exits cleanly |
| `status-board` | notice board (kitchen) | the board panel; reading it is in-process (files only) |
| `gate-locked` | gate (yard) | toast — the street extension point |
| `kettle` | kettle (kitchen) | receiver fixture; empty interaction shows a patient hum toast |

Dialogue with housemate NPCs is proximity + interact, also fully in-process.

## The laptop

The study spawns a **laptop** (`core:tool/laptop`) — the one item whose
Enter does not pocket it. A set-up laptop **opens its screen** instead;
picking it up is the screen's explicit **PICK UP LAPTOP** row. Carried, it is
an ordinary placeable: **Space** sets it up again wherever you stand — any
desk or clear surface in any room — and the placement persists in
`world.state` like every other world item.

Opening the lid hands over **the whole canvas**: no room behind it, no
hotbar over it, a bezel and a title bar instead. You are looking at a
machine, not at a room with a machine in it. Escape closes the lid (or
steps back one page); the room is exactly as you left it.

The screen's home page lists **laptop profiles**, one convention shared by
every kilix desktop that ships a laptop (kilix-cap's Study laptop reads the
same directory):

```
~/.local/gpu_terminal/laptop/<id>.profile
```

(`KILIX_LAPTOP_PROFILES` overrides the directory with an absolute path —
tests use this.) A profile is a plain `KEY=value` file; `#` comments. The
first time the directory has to be created it is seeded with the bundled
examples from `assets/laptop/`; an existing directory is never reseeded.
The home page shows the first 8 profiles, re-scanned every time it opens,
and **Enter on a profile opens that session** — the fast path stays one
keystroke deep.

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

Choosing a profile prefers the host's own verb when it exists: the
launcher probes `kilix laptop help` once (the same probe-never-assume
pattern as the games handoff) and delegates to `kilix laptop open <id>`,
which spawns the session and records it in the shared run registry. On a
host that predates the verb, the fallback keeps everything working: a
**pane profile** writes a generated kitty `--session` file under the
config home and runs `kilix --session <file> --start-as=fullscreen` —
un-detached, so the spawned pid is the session window itself and this
desktop records it in the registry — and a **desktop profile** runs
`kilix <provider>` (`95` maps to `kilix desktop 95`) with no such flag: a
provider owns its own presentation. Everything is a fixed argv vector
through `posix_spawnp` under the same session gate, kill switch, and
toast reporting as every row above.

### Running sessions, the lid, and closing

The registry — `run/<id>.pid` beside the profiles, one pid per file,
written at spawn, and never believed without a real `kill(pid, 0)` check
(a stale or garbled file is deleted by whichever reader notices) — is one
contract shared with kilix-cap, `kilix laptop`, and the launcher TUI, so
a session opened anywhere shows as running everywhere. In the house it
shows twice:

- The **set-up laptop itself is stateful art**: closed while nothing
  runs, open while any profile session is live, with a short lid
  animation between (the closed frame comes from the optional per-style
  `items/laptop-lid.png` sheet; without it the open sprite compresses
  toward the desk instead, so the states stay readable with no art at
  all). A **carried laptop always reads as closed** in the hotbar and
  inventory.
- The **home page marks running profiles** (`RUNNING - ENTER CLOSES`),
  and Enter on such a row closes that session — `kilix laptop close`
  when the host verb exists, a SIGTERM to the recorded pid otherwise —
  instead of opening a duplicate. Walking away just leaves the session
  running and the lid open.

### Configuring profiles from inside the house

**CONFIGURE PROFILES** on the home page opens the authoring pages, so a
profile can be written without leaving the desktop or knowing the file
format:

| Page | Rows |
|---|---|
| profiles | every profile, **NEW PROFILE**, back |
| profile | **name**, **opens** (a session / a desktop), **layout** (splits / tabs) or **desktop** (which provider), **panes**, **save**, **delete**, back |
| panes | every pane with a one-line summary, **add a pane**, back |
| pane | **title**, **directory**, **ssh host**, **command**, **remove this pane**, back |

Field rows edit in place: Enter opens the field, typing edits it, Enter
keeps it, Escape puts it back. Toggle rows (opens, layout) flip on Enter
and clear whichever half they leave behind, so a profile can never end up
as a desktop *and* panes. Removing a pane closes the gap, because the
loader rejects gaps.

**Nothing reaches the disk until SAVE**, and saving runs the same rules
the loader enforces — a value the reader would reject is refused at the
field, with the reason on the footer line, so a profile the pages produce
is always one the loader accepts. A new profile's file id is derived from
its name (`Test Bench` → `test-bench.profile`, `-2`, `-3` … when taken).
Deleting asks first.

`./kilix-land-desktop --laptop-test` covers profile parsing, the screen's
page machine, the whole configuration round trip (create, rename, toggle
kind, pick a provider, add and edit a pane, a refused ssh destination,
save, reload from disk, delete), pickup and set-up, world.state
persistence, and the run registry (a recorded live session opens the lid
frame by frame, the home page marks it, Enter raises one close request,
and a cleared registry closes the lid again).

## Debug menu

The pause menu (Esc) grows a **DEBUG** entry whose submenu holds the
walkable-space editor: `tools/region_editor.py` opens in a tab and asks
which room. Painting is done by `kilix-mask` (pinned in `third_party/`),
which shows the current style's plate on the 6-unit grid with walkable
cells tinted and doors, objects, NPC anchors, spawns and item drops drawn
around them. Drag paints, right-drag erases, `?` lists the keys. On save
the paint is decomposed into the world.json model — a walk bounding rect
plus up to 64 exact-cover obstacle rects, with the running count shown
against that cap while you work — the file is rewritten and the validator
runs immediately.

`b<n>` from the room list edits that room's walk-behind mask instead, per
pixel, with `B` setting the selected region's baseline from the cursor.
The mask stays the plate-sized 8-bit greyscale PNG the engine decodes;
only the painting changed.

The entry is controlled by `<config-home>/desktop.conf`
(`~/.local/gpu_terminal/kilix-land-desktop/desktop.conf`):

```
debug_menu = off
```

Absent file or key means enabled. The flag is re-read every time the pause
menu opens.
