# Nala

A desktop companion for Hyprland. Nala floats on the desktop, follows the
cursor with her eyes, morphs between a small vocabulary of shapes, and can be
picked up and dropped anywhere. She lives in the notification tray, and she
takes her colours from Noctalia so she restyles herself when the wallpaper
changes.

This fork, **Nala-Voice**, also makes her a fully local voice assistant: she
listens through whisper.cpp, thinks with any OpenAI-compatible local model
(Ollama, llama.cpp, vLLM — a Qwen vision model fits well), answers in a
speech bubble and through Fish Speech, can act on the desktop through a
permissioned set of tools, and — only if you turn it on — keeps an episodic
memory of what you worked on, with a privacy switch that works without the
model. Nothing leaves your machine. See [The assistant](#the-assistant).

## Requirements

- Qt 6.10 or newer (`Core Gui Widgets Quick Qml ShaderTools Test Network
  Multimedia Sql Concurrent`)
- `layer-shell-qt` 6.7+ — optional, but without it Nala is an ordinary window
  that the compositor will tile rather than a free-floating companion
- A Wayland compositor supporting `wlr-layer-shell`; developed on Hyprland
- Noctalia — optional; without it Nala falls back to her own dark palette

For the assistant, all optional and reported by `nala doctor` when missing:
whisper.cpp, an OpenAI-compatible model server, Fish Speech, `grim`,
`wtype`, `ydotool`.

## Install

On NixOS or anywhere with Nix:

```bash
nix run github:r3dg0d/Nala-Voice          # try it
nix profile install github:r3dg0d/Nala-Voice
nix develop                               # a shell with Qt set up for hacking
```

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

# the assistant
nala listen                 # push to talk (bind this to a key)
nala ask open discord       # as if you had said it
nala stop                   # stop talking / thinking / acting
nala memory pause 60        # pause screen memory (minutes; omit for "until resumed")
nala memory resume
nala memory status
nala timeline               # the memory window
nala doctor                 # what is installed, running, and missing
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
something, steps out of sight while anything is fullscreen, and sways along
while music is playing.

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
| `src/music.*` | Whether anything is playing, over MPRIS. She sways to the fact of music; MPRIS carries no beat, so the rhythm is her own. |
| `src/compositor.*` | Hyprland's event stream. She glances at workspace changes and new windows, and fades out of the way while something is fullscreen. |
| `src/cursor.*` | Global pointer position. Wayland denies this to clients, so it asks Hyprland over its IPC socket and reports itself unavailable elsewhere. |
| `src/backend.*` | Preferences, placement, drag arithmetic and the control socket. |
| `src/assistant/*` | The assistant: speech, the model, tools, memory and privacy. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). |
| `qml/Bubble.qml`, `qml/MemoryTimeline.qml` | Her speech bubble, and the memory window. |

Nala is placed as a `wlr-layer-shell` surface. That is what makes her a
companion rather than a window: no decorations, no tiling, no taskbar entry,
and an input region shaped to her body.

## The assistant

Everything is local and every part is optional. With nothing installed she
is the same companion as before; each backend you add switches on the part
that needs it, and `nala doctor` (or Preferences → Developer) says what is
missing.

| | Status |
| --- | --- |
| Push-to-talk, wake word and always-listening, with voice-activity detection | implemented |
| whisper.cpp speech recognition (`whisper-server` or `whisper-cli`) | implemented |
| Fast command router — pause memory, stop, open settings, open apps… without the model | implemented |
| Any OpenAI-compatible model, with tool calling and optional vision | implemented |
| Fish Speech voice, streamed, interruptible, with a text fallback | implemented (tested against its API; see [docs/AI.md](docs/AI.md)) |
| Speech bubble, yes/no confirmation card, listening meter | implemented |
| Computer use: windows, apps, files, browser, mouse and keyboard, shell | implemented; mouse/keyboard experimental |
| Screen memory with privacy gate, dedup, retention, storage cap, timeline | implemented |
| Model-written memory descriptions and a vision privacy check | experimental |
| Semantic (embedding) search, audio-in models, a dedicated wake-word model | planned |

**Say it** (or `nala ask` it):

- "Hey Nala, open Discord." / "Close all windows." / "Open settings."
- "Stop." / "Never mind." — interrupts speech, thinking and actions. Tapping her works too.
- "Pause screen memory." / "Pause memory for one hour." / "Turn screen recording back on."
- "Forget the last five minutes." / "Keep this memory." / "Never record this app."
- "What GitHub project did I look at yesterday?" / "Didn't I already make a resume?"

**Quick start** with Ollama and whisper.cpp:

```bash
ollama pull qwen2.5vl     # an example; any model works — pick it in Preferences → Assistant
mkdir -p ~/.local/share/nala/whisper
whisper-cpp-download-ggml-model base.en ~/.local/share/nala/whisper   # "download-ggml-model.sh" outside Nix
nala doctor
```

Then bind push-to-talk in Hyprland: `bind = SUPER, N, exec, nala listen`.

**Safety.** The model reaches the desktop only through typed tools. Each has
a risk level; risky ones make her ask first ("Close Firefox?" — answer by
voice or with the card), and writing files or running commands *always* asks.
Categories (input, files, browser, shell) can be switched off; the shell is
off by default. Every call is written to `~/.local/state/nala/assistant.log`
with secrets redacted.

**Privacy.** Screen memory is off until you turn it on. A red dot on her means
it is recording; a struck-through eye means paused. The pause command is
handled without the model, is written to disk immediately, and discards any
frame already being processed. Password managers, logins, banking, private
browsing and adult content are never kept, and you can exclude any app or
window. See [docs/PRIVACY.md](docs/PRIVACY.md).

More: [architecture](docs/ARCHITECTURE.md) · [AI setup](docs/AI.md) ·
[memory](docs/MEMORY.md) · [privacy](docs/PRIVACY.md) ·
[development](docs/DEVELOPMENT.md) · [handoff notes](HANDOFF.md)

## Tests

```bash
scripts/test.sh     # behaviour, headless
scripts/poses.sh    # render one PNG per form, for comparing against the reference
ctest --test-dir build   # behaviour, the install layout, and the assistant's unit tests

build/nala --film build/film   # record a sequence at 60 fps, one PNG per frame
```

`scripts/test.sh` runs offscreen, where there is no GPU surface and the body
shader never compiles; it detects that and skips the appearance assertions
rather than failing them. Run `build/nala --self-test` with a display attached
to exercise those too — they check her proportions against the measurements in
[docs/animation.md](docs/animation.md).

## License

MIT. See [LICENSE](LICENSE).
