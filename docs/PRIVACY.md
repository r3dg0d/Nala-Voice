# Privacy

Nala runs entirely on your machine. The only network connections she makes
are to the endpoints you configure — by default all on `127.0.0.1` — for the
model, whisper-server and Fish Speech.

## The microphone

- **Push to talk** (the default until the wake word is set up): the
  microphone is open only for the one request.
- **Wake word**: the microphone is open, but only the wake-word detector hears
  it: 80 ms blocks, processed in memory and dropped. Speech that did not follow
  her wake phrase is thrown away **before** transcription. A thin ring on her
  (and "listening for her name" in the tray) means exactly this.
- **Taking a request**: a filled dot; the utterance goes to whisper.cpp on
  this computer, then is discarded. With `whisper-cli` it sits in a private
  temporary file for the second it takes.
- **Always**: everything is transcribed (grey dot). Use with care.
- **Recording**: red dot, only while you record wake-word training samples.

Wake-word training recordings are made only when you press Record, stored
owner-only under `~/.local/share/nala/wakeword/phrases/`, and deleted with
Preferences → Wake word → × or *Delete all wake-word recordings*. The
downloaded detector models and negative data contain no one's recordings of
you.

## Screen memory is opt-in

It is off until you switch on *Preferences → Memory → Remember my screen*.
While it records, a small red dot sits on her; while paused, a struck-through
eye. When it is off, neither shows, and nothing is captured.

## The killswitch

"Nala, pause screen memory" — or "Hey Nova…", whatever she is called — (and its variants — see
[MEMORY.md](MEMORY.md)) is handled by the fast command router, **without the
language model**: it works when the model is slow, wrong or not running. It is
also honoured when her name is misheard or not said at all, because pausing
is harmless if it was meant for someone else. The tray menu, the timeline
window, Preferences and `nala memory pause` do the same.

When it is pressed:

- the pause is written to disk before the call returns, so it survives a
  crash or restart;
- the capture timer stops;
- an epoch counter is bumped, so a frame already being captured, hashed or
  judged is discarded when it lands rather than saved;
- she briefly covers her eyes.

A pause without a time limit lasts until you resume it. A timed pause
("for one hour") resumes when its time is up — and only then. The model is
never given a tool to resume screen memory.

The rest of the assistant keeps working while memory is paused.

## Voice controls

All of these are handled by the command router, without a model, and none of
them can be undone by the model. Say them in your own words:

| Say | Effect |
| --- | --- |
| "Stop screen recording." / "Turn off screen memory." / "Stop looking at my screen." / "Go private." | stops capture at once and discards any frame in flight; also stops a video recording she started |
| "Pause screen memory for an hour." | the same, for a while |
| "Resume screen recording." / "Turn screen memory back on." | starts again -- **only by voice or the tray, never by the model** |

When capture is off: no screenshot is taken, none is processed, no visual
context reaches a model, and nothing restarts on its own. The struck-through eye
on her body shows the state.

**Video recording is a different thing** from screen memory: "start a video
recording" asks first and saves a file you can keep to `~/Videos/Recordings/`.
"Stop recording" stops that too.

## How long pictures are kept

`memory.screenshotRetention` (Preferences → Memory), applied every five minutes:

| Value | Meaning |
| --- | --- |
| `off` | pictures are kept only long enough to be described, then deleted |
| `1h` `1d` `7d` `30d` | deleted once older than that |
| `manual` | never deleted by age (the storage cap still applies) |
| `custom` (default) | `memory.screenshotDays` days, 7 by default |

The **storage cap** (`memory.maxStorageMB`, default 5 GB) always applies: when
history is over it, the oldest unpinned pictures go first, so it cannot grow
without limit. Descriptions (the rows) have their own age limit
(`memory.semanticDays`). Pinned memories are spared by every automatic rule.

Clear it yourself:

```bash
nala memory clear screen        # all screen history; notes and pinned memories stay
nala memory clear screen all    # ...including pinned ones
```

Notes you asked her to keep, and files or pages she recorded, are not screen
history and are not touched. "Forget everything" by voice still asks first.

## The models are local

Requests go only to the model server you configured, on your machine by
default. Falling back from one model to another only ever picks another
*local* model; nothing switches to a hosted service. Pointing `llm.endpoint`
at a remote server is a choice you make, and then transcripts (and screenshots,
if vision is on) go there.

## What is never kept

The privacy gate runs **before** the screen is read, on the focused window's
class and title (from Hyprland), and again after capture to make sure focus
did not move meanwhile. A frame is dropped if:

- the app is in `privacy.excludedApps` — by default Bitwarden, KeePassXC,
  KeePass, 1Password, Enpass, Seahorse, KWalletManager, Proton Pass,
  authenticators, GNOME Keyring, polkit and pinentry dialogs, and Nala herself;
- the title contains anything in `privacy.excludedTitles`;
- with `privacy.blockSensitive` (on by default), the title suggests private
  browsing, a login/password/2FA/recovery-code screen, banking or payment, or
  adult content;
- the focused window cannot be identified (`memory.requireWindowInfo`, on by
  default — this is why screen memory needs Hyprland);
- with `privacy.visionFilter`, the model does not answer a clear "SAFE".

Blocked frames are logged by category only ("credentials"), never by title.

**Limits, honestly.** Title matching cannot see what is inside a window: a
password typed into an ordinary-looking page, or sensitive content in a tab
whose title is innocuous, will not be caught by the title rules. The vision
check helps but is only as good as the model, and costs a model call per
kept frame. Private-browsing detection relies on the browser putting it in
the title (Firefox and Chromium do). If in doubt, pause.

## Other data

- `~/.config/nala/assistant.json` — settings, including any API key. 0600.
- `~/.local/state/nala/assistant.log` — what she did. 0600, capped at 4 MB
  plus one old copy. Credentials that look like tokens, keys, `Bearer …` and
  phrases like "my password is …" are replaced with `[redacted]`. Transcripts
  and prompts are only logged with *Developer → Debug logging* on.
- Microphone audio is held in memory for one utterance; with `whisper-cli` it
  is written to a private temporary directory and deleted as soon as it has
  been transcribed.
- Screenshots handed to the model for a single question are never stored, and
  are refused for windows the privacy gate would block.

## Deleting everything

"Forget everything" (asks first), or quit Nala and remove
`~/.local/share/nala/memory/`.
