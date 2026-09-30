# Tools and permissions

The model never runs shell commands of its own. It reaches the machine only
through **typed tools**: each has a name, a JSON schema its arguments are
checked against *before* anything runs, a risk level, and a category you can
switch off. The underlying program is never shown to the model, and every
helper is started with an argument list, never through a shell.

## Two paths to the same tools

1. **Fast path.** The command router recognises common spoken commands with
   fixed patterns and runs the tool directly: no model, milliseconds.
2. **Model path.** For anything else the model chooses tools from the schema it
   is given. Both paths go through the same permission check and the same log.

| Say | Tool | Risk |
| --- | --- | --- |
| "Set the volume to 40%" / "turn my volume down" / "louder" / "mute" | `volume.set`, `volume.step`, `volume.mute` | safe |
| "Pause" / "resume" / "next song" / "previous track" | `media.control` | safe |
| "What time is it?" / "What's the date?" | `time.now` (answered locally) | safe |
| "Lock the screen" | `session.lock` | low |
| "Take a screenshot" | `screenshot.save` (PNG in `~/Pictures/Screenshots`) | low |
| "Start a video recording" / "stop the video recording" | `record.start` (asks first) / `record.stop` | medium / safe |
| "Open / close / switch to Discord" | `apps.launch`, `apps.close`, `apps.focus` | low / medium |

Open-ended requests ("play some jazz", "record a song about cats", "skip to the
part about Nix") are deliberately *not* matched; they go to the model.

## Tools only the model uses

| Tool | What | Risk |
| --- | --- | --- |
| `notify.send` | a desktop notification | safe |
| `clipboard.read` / `clipboard.write` | the Wayland clipboard | low |
| `system.info` | kernel, CPU, memory, uptime, model | safe |
| `system.gpu` | GPU name, memory, processes | safe |
| `system.run_safe` | one allow-listed inspection command | low |
| `volume.get` | current volume | safe |
| `window.*`, `apps.*`, `computer.*`, `files.*`, `browser.open`, `memory.*`, `nala.*` | see [ARCHITECTURE.md](ARCHITECTURE.md) | varies |
| `computer.locate_and_click` | target description → coarse observation, crop, marked refinement, click, verify; retries confirm again | high |
| `computer.locate_and_type` | visually focus a field, verify focus, type ordinary text and observe; secrets refused | high |
| `memory.search` | hybrid ranked evidence, time filters, optional local backends | safe |
| `memory.remember` | explicit fact with stable subject and provenance; updates preserve history | low |
| `shell.run` | arbitrary commands; **off by default** (`agent.shell`) and always asks | high |

`system.run_safe` accepts only these programs, with plain flags and nothing
else: `uname uptime date whoami hostname df free lsblk nproc id nvidia-smi
sensors lscpu`. No paths, pipes, redirection, substitution or quoting; the
arguments must look like `-h` or `--human-readable`. Anything else is
refused with the reason. This is not a way to run arbitrary commands; that is
`shell.run`, which is off and always asks.

## Permissions

`agent.confirm` (`everything`, `risky`, `high`) decides what she asks about:

| Risk | Meaning | Asks? |
| --- | --- | --- |
| safe | reads only, or harmless: volume, media keys, the clock, notifications | never |
| low | reversible and local: opening an app, screenshots, clipboard, locking | only if `everything` |
| medium | can lose state or is privacy-relevant: closing windows, typing, clicking, video recording | unless `high` |
| high | consequential: writing files, running commands, submitting forms | **always** |

Deleting files, changing system configuration, installing packages, writing
outside your home, killing unrelated processes, `sudo`, firewall and service
changes are not tools at all, so the model cannot do them except through
`shell.run` or `files.write`, which always ask. Ask means she says the question
aloud and shows a card; "yes" and "no" work by voice.

Each category can be switched off (`agent.input`, `agent.files`,
`agent.browser`, `agent.shell`, `agent.enabled`). A tool in a switched-off
category refuses to run and says why.

**Tainted turns.** Anything a tool reads that someone else may have written
(the clipboard, file contents, window titles, memories, command output) marks
the turn: afterwards, nothing that could leave the machine runs without asking.

## Adding a tool

Register it in `Assistant::registerSystemTools()` (or `registerTools()`):

```cpp
m_tools.add(Tool{
    "volume.set", "Set the speaker volume to a percentage from 0 to 100.",
    object({{"level", integer("Volume, 0 to 100", 0, 100)}}, {"level"}),
    Risk::Safe, "system", nullptr, nullptr,
    [](const QJsonObject &a, Tool::Done done) { /* ... */ }});
```

Build the command line in `systemtools.cpp` as a program plus arguments and add a
test for it in `tests/model_tests.cpp`; add a fast-path pattern in
`commandrouter.cpp` only if the request is unambiguous.

## Not implemented

**Weather.** It needs a network service and a location, which conflicts with
"nothing leaves your machine". It is not implemented rather than added as an
opt-out; if you want it, it should be a tool you switch on and configure with an
explicit location.

Named clicks also have a fast path. Coordinate screenshot/click/move tools stay
available for debugging. See [computer-use.md](computer-use.md) and
[semantic-search.md](semantic-search.md) for settings and limits.
