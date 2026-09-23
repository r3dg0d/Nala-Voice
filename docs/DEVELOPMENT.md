# Development

## Building

```bash
nix develop            # Qt, whisper.cpp, grim, wtype, ydotool; sets QML/plugin paths
scripts/build.sh
ctest --test-dir build --output-on-failure
```

Without Nix, install Qt 6.10+ with Multimedia and the SQLite driver, plus
`layer-shell-qt`. On NixOS outside `nix develop`, an unwrapped build needs
`QML_IMPORT_PATH` and `QT_PLUGIN_PATH` pointed at the Qt store paths, or the
window fails to load (the error now says which module is missing).

`nix build` builds the package and runs both test suites in the sandbox.

## Tests

| Suite | What | How |
| --- | --- | --- |
| `nala --self-test` | the companion's behaviour and measurements (145 checks), opening every window; fails on any QML warning | `scripts/test.sh` or ctest |
| `nala-install` | install layout, the unit's `ExecStart`, the binary runs | ctest |
| `nala-assistant-tests` | identity and profiles, the wake-word gate/trainer/model and its integration (mock detector, scripted recogniser), router, VAD, WAV, settings, redaction, model replies, schema validation, permissions, path policy, privacy gate, time phrases, hashing, memory store and retention, the screen-memory killswitch, `.desktop` parsing, the assistant end to end | ctest, or run it directly |

The assistant tests unset `HYPRLAND_INSTANCE_SIGNATURE` so that no test can
reach the compositor of the desktop they run on.

Opt-in tests against real backends (skipped otherwise):

```bash
NALA_TEST_WAV=clip.wav NALA_TEST_WHISPER_MODEL=ggml-tiny.en.bin \
  NALA_TEST_WHISPER_SERVER=http://127.0.0.1:8178 build/nala-assistant-tests liveWhisper
NALA_TEST_LLM=http://127.0.0.1:11434/v1 NALA_TEST_LLM_MODEL=<model> \
  build/nala-assistant-tests liveModelCallsTools
NALA_TEST_FISH=http://127.0.0.1:8080 build/nala-assistant-tests liveFishSpeech
```

## Trying the UI without disturbing your desktop

Run a headless sway with a short `XDG_RUNTIME_DIR` (socket paths must fit in
108 bytes) and throwaway `XDG_CONFIG_HOME`/`XDG_DATA_HOME`/`XDG_STATE_HOME`,
start `build/nala` inside it, drive it with `nala ask …`, and capture with
`grim`. Delete `$XDG_CACHE_HOME` after changing QML, or Qt's disk cache may
serve the old file.

## Adding things

- **A voice command**: add a pattern in `CommandRouter::CommandRouter()` and
  handle the action in `Assistant::runFast()`. Add phrases to the
  `routerFastCommands` test. Anything consequential should go through
  `callTool()` so it gets the same permission check as the model.
- **A tool**: register it in `Assistant::registerTools()` with a schema, a
  risk and a category; give it a `summary` if it can need confirmation. It is
  validated, permission-checked, logged and offered to the model
  automatically. `toolSchemaIsWellFormed` checks the schema.
- **A setting**: declare it in `settings.cpp`'s table (default and
  validation), then read it with `settings()->flag/string/…`. The Preferences
  pages bind to `assistantSettings.values[key]` and `assistantSettings.set()`.
- **A backend**: subclass `SpeechToText` or `TextToSpeech`.

Keep the style of the surrounding code: comments explain why, in the same
voice as the rest; no warnings under `-Wall -Wextra -Wpedantic`.
