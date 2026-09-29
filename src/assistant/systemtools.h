#pragma once
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <optional>

// The small desktop actions people ask for by voice -- volume, media keys,
// the time, the clipboard, a notification, locking the screen. Everything here
// is pure: it turns a request into a program and an argument list (never a
// shell string), or parses a program's output. Running them, and asking
// permission first, is the assistant's job; testing them needs no audio,
// compositor or clipboard.
namespace systemtools {

struct Command {
  QString program;
  QStringList args;
  QByteArray input; // stdin, for wl-copy
};

// --- volume (PipeWire, through wpctl) ---------------------------------------
// Absolute level 0..100. Louder than 100 needs a deliberate `overdrive`.
std::optional<Command> setVolume(int percent, QString *error = nullptr);
// A step up or down, in percentage points (1..50).
std::optional<Command> stepVolume(int deltaPercent, QString *error = nullptr);
// "mute", "unmute" or "toggle".
std::optional<Command> muteVolume(const QString &mode, QString *error = nullptr);
Command getVolume();
struct Volume { int percent = -1; bool muted = false; bool ok = false; };
Volume parseVolume(const QString &wpctlOutput); // "Volume: 0.40 [MUTED]"

// --- media (MPRIS, through playerctl) ------------------------------------------
// play, pause, play-pause, next, previous, stop.
std::optional<Command> media(const QString &action, QString *error = nullptr);

// --- session -----------------------------------------------------------------
Command lockScreen();
std::optional<Command> notify(const QString &title, const QString &body,
                              QString *error = nullptr);
Command clipboardRead();
std::optional<Command> clipboardWrite(const QString &text, QString *error = nullptr);
// A PNG under `dir`, named by time, so nothing is overwritten.
QString screenshotPath(const QString &dir, const QDateTime &when);
Command screenshot(const QString &path);

// --- video recording (gpu-screen-recorder or wf-recorder) -------------------------
Command recordStart(const QString &recorder, const QString &path);
QString recordingPath(const QString &dir, const QDateTime &when);

// --- read-only commands -------------------------------------------------------------
// A short allow-list of inspection commands the model may run without a shell:
// `uname -a`, `df -h`, `free -h`, `nvidia-smi`, ... No paths, no pipes, no
// redirection, no substitution, no unknown programs. `error` says why not.
std::optional<Command> safeCommand(const QString &line, QString *error = nullptr);
QStringList safeCommandNames();

// --- speech -------------------------------------------------------------------
// "Ten past three." style is left to the voice; this is plain and unambiguous.
QString spokenTime(const QDateTime &now);
QString spokenDate(const QDateTime &now);

// "forty five" -> 45, "a hundred" -> 100, "40" -> 40; -1 when not a number.
int parseSpokenNumber(const QString &words);

} // namespace systemtools
