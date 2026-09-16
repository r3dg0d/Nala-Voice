# Nala

A desktop companion for Hyprland. Nala floats on the desktop, follows the
cursor with her eyes, morphs between a small vocabulary of shapes, and can be
picked up and dropped anywhere. She lives in the notification tray, and she
takes her colours from Noctalia so she restyles herself when the wallpaper
changes.

## Requirements

- Qt 6.10 or newer (`Core Gui Widgets Quick Qml ShaderTools Test`)
- `layer-shell-qt` 6.7+ — optional, but without it Nala is an ordinary window
  that the compositor will tile rather than a free-floating companion
- A Wayland compositor supporting `wlr-layer-shell`; developed on Hyprland
- Noctalia — optional; without it Nala falls back to her own dark palette

## Install

On Arch or CachyOS:

```bash
makepkg -si
```

Then either start her by hand, or have the session start her:

```bash
systemctl --user enable --now nala
```

## Build and run

From a checkout, without installing:

```bash
scripts/build.sh
scripts/run.sh
```

Nala is single-instance. Running the command again talks to the copy that is
already there:

```bash
nala settings     # open preferences
nala status       # what she is doing right now
nala poke         # say hello
nala wink         # one eye shut, held
nala think        # spin up the orbit rings
nala alert        # become an exclamation mark
nala notify       # show the notification badge
nala scatter      # come apart, then pull back together
nala dash         # streak off across the desktop
nala demo         # run through everything she does, once
nala rest         # back to idle
nala reset        # move her back to her default corner
nala quit
```

## Using her

| Action | What happens |
| --- | --- |
| Drag | Picks her up; she stretches, then squashes where she lands |
| Click | A happy squash-and-morph |
| Double-click | She thinks, with orbit rings |
| Throw | Let go mid-drag and she streaks off, bounces off the edges and lands |
| Right-click | Preferences |
| Hover | She widens her eyes and looks at you |
| Leave her alone | She amuses herself, then falls asleep |
| Tray, "Show me everything" | Runs the whole repertoire without waiting |

Clicks outside her silhouette pass straight through to whatever is underneath,
so she never blocks the desktop.

## Preferences

Tray icon, right-click, or `nala settings`. Size, colour (ink or wallpaper),
which display, cursor-following, idle antics, sleep, layer, reduced motion and
start-at-login. Stored at `~/.config/nala/preferences.json`.

**React to your desktop** lets her respond to the machine rather than a timer:
she thinks while the load average says it is working, shows the notification
badge when one actually arrives, glances up when you change workspace or open
something, and steps out of sight while anything is fullscreen.

**Idle antics** are dealt from a shuffled bag, so every behaviour turns up once
per cycle rather than depending on luck. Small movements run on their own
faster cadence, and she will not look away from a cursor that is still moving.
`Reduce motion` keeps the looking and the winking and drops the rest.

**Colour** is `Ink` by default — the near-black of the reference. `Wallpaper`
derives her colour from Noctalia's accent instead, lightening or deepening it
so she stays legible against the desktop.

## How it is put together

| | |
| --- | --- |
| `shaders/mascot.frag` | The body: one signed distance field per form, blended by distance so morphs are continuous rather than cross-faded. Runs on the GPU, so she stays crisp at any size. |
| `src/mascot.*` | Her behaviour. Owns no rendering — it advances animation state on `tick()` and publishes properties, which makes the whole personality testable without a compositor or a GPU. |
| `src/orbits.*` | The coloured arcs. Each is a circle in 3D, projected every frame and split into the half behind her and the half in front, drawn as scene-graph geometry either side of the body. |
| `src/trail.*` | The comet trail she leaves when she dashes: a blade of bowed ribbons that straddles her. Shares its stroke geometry with the orbit arcs via `src/stroke.h`. |
| `src/theme.*` | Watches Noctalia's generated GTK palette and shell settings and re-emits when the wallpaper changes. |
| `src/compositor.*` | Hyprland's event stream. She glances at workspace changes and new windows, and fades out of the way while something is fullscreen. |
| `src/cursor.*` | Global pointer position. Wayland denies this to clients, so it asks Hyprland over its IPC socket and reports itself unavailable elsewhere. |
| `src/backend.*` | Preferences, placement, drag arithmetic and the control socket. |

Nala is placed as a `wlr-layer-shell` surface. That is what makes her a
companion rather than a window: no decorations, no tiling, no taskbar entry,
and an input region shaped to her body.

## Tests

```bash
scripts/test.sh     # behaviour, headless
scripts/poses.sh    # render one PNG per form, for comparing against the reference
ctest --test-dir build   # behaviour, plus the install layout

build/nala --film build/film   # record a sequence at 60 fps, one PNG per frame
```

`scripts/test.sh` runs offscreen, where there is no GPU surface and the body
shader never compiles; it detects that and skips the appearance assertions
rather than failing them. Run `build/nala --self-test` with a display attached
to exercise those too — they check her proportions against the measurements in
[docs/animation.md](docs/animation.md).

## License

MIT. See [LICENSE](LICENSE).
