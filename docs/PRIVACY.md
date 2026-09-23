# Privacy

Nala runs entirely on your machine. The only network connections she makes
are to the endpoints you configure — by default all on `127.0.0.1` — for the
model, whisper-server and Fish Speech.

## Screen memory is opt-in

It is off until you switch on *Preferences → Memory → Remember my screen*.
While it records, a small red dot sits on her; while paused, a struck-through
eye. When it is off, neither shows, and nothing is captured.

## The killswitch

"Nala, pause screen memory" (and its variants — see
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
