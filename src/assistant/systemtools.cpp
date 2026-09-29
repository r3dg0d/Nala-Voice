#include "systemtools.h"

#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <algorithm>

namespace systemtools {

namespace {
const QString kSink = QStringLiteral("@DEFAULT_AUDIO_SINK@");

std::optional<Command> refuse(QString *error, const QString &why) {
  if (error)
    *error = why;
  return std::nullopt;
}
} // namespace

std::optional<Command> setVolume(int percent, QString *error) {
  if (percent < 0 || percent > 100)
    return refuse(error, QStringLiteral("Volume must be between 0 and 100."));
  // wpctl takes a fraction; two decimals is finer than any slider.
  return Command{QStringLiteral("wpctl"),
                 {QStringLiteral("set-volume"), kSink,
                  QString::number(percent / 100.0, 'f', 2)},
                 {}};
}

std::optional<Command> stepVolume(int deltaPercent, QString *error) {
  if (deltaPercent == 0 || std::abs(deltaPercent) > 50)
    return refuse(error, QStringLiteral("A volume step is 1 to 50 points."));
  // `-l 1.0` caps it at 100 % so "louder" never clips the speakers.
  return Command{QStringLiteral("wpctl"),
                 {QStringLiteral("set-volume"), QStringLiteral("-l"),
                  QStringLiteral("1.0"), kSink,
                  QStringLiteral("%1%%2").arg(std::abs(deltaPercent))
                      .arg(deltaPercent > 0 ? '+' : '-')},
                 {}};
}

std::optional<Command> muteVolume(const QString &mode, QString *error) {
  QString flag;
  if (mode == QLatin1String("mute")) flag = QStringLiteral("1");
  else if (mode == QLatin1String("unmute")) flag = QStringLiteral("0");
  else if (mode == QLatin1String("toggle")) flag = QStringLiteral("toggle");
  else return refuse(error, QStringLiteral("Mute, unmute or toggle."));
  return Command{QStringLiteral("wpctl"),
                 {QStringLiteral("set-mute"), kSink, flag}, {}};
}

Command getVolume() {
  return {QStringLiteral("wpctl"), {QStringLiteral("get-volume"), kSink}, {}};
}

Volume parseVolume(const QString &out) {
  static const QRegularExpression re(QStringLiteral("Volume:\\s*([0-9.]+)(\\s*\\[MUTED\\])?"));
  const auto m = re.match(out);
  Volume v;
  if (!m.hasMatch())
    return v;
  v.percent = int(m.captured(1).toDouble() * 100.0 + 0.5);
  v.muted = !m.captured(2).isEmpty();
  v.ok = true;
  return v;
}

std::optional<Command> media(const QString &action, QString *error) {
  static const QStringList allowed = {"play", "pause", "play-pause", "next",
                                      "previous", "stop"};
  if (!allowed.contains(action))
    return refuse(error, QStringLiteral("Unknown media action."));
  return Command{QStringLiteral("playerctl"), {action}, {}};
}

Command lockScreen() {
  return {QStringLiteral("loginctl"), {QStringLiteral("lock-session")}, {}};
}

std::optional<Command> notify(const QString &title, const QString &body,
                              QString *error) {
  if (title.trimmed().isEmpty())
    return refuse(error, QStringLiteral("A notification needs a title."));
  // "--" so a title beginning with a dash is not read as an option.
  return Command{QStringLiteral("notify-send"),
                 {QStringLiteral("--app-name=Nala"), QStringLiteral("--"),
                  title.left(120), body.left(500)},
                 {}};
}

Command clipboardRead() {
  return {QStringLiteral("wl-paste"), {QStringLiteral("--no-newline")}, {}};
}

std::optional<Command> clipboardWrite(const QString &text, QString *error) {
  if (text.size() > 100000)
    return refuse(error, QStringLiteral("That is too much text for the clipboard."));
  return Command{QStringLiteral("wl-copy"), {}, text.toUtf8()};
}

QString screenshotPath(const QString &dir, const QDateTime &when) {
  return dir + QStringLiteral("/nala-%1.png")
                   .arg(when.toString(QStringLiteral("yyyyMMdd-HHmmss")));
}

Command screenshot(const QString &path) {
  return {QStringLiteral("grim"), {path}, {}};
}

QString recordingPath(const QString &dir, const QDateTime &when) {
  return dir + QStringLiteral("/nala-%1.mp4")
                   .arg(when.toString(QStringLiteral("yyyyMMdd-HHmmss")));
}

Command recordStart(const QString &recorder, const QString &path) {
  if (recorder.endsWith(QLatin1String("wf-recorder")))
    return {recorder, {QStringLiteral("-f"), path}, {}};
  // gpu-screen-recorder: the focused monitor's screen, 60 fps, default audio out.
  return {recorder,
          {QStringLiteral("-w"), QStringLiteral("screen"), QStringLiteral("-f"),
           QStringLiteral("60"), QStringLiteral("-a"),
           QStringLiteral("default_output"), QStringLiteral("-o"), path},
          {}};
}

// --- read-only commands -------------------------------------------------------------

QStringList safeCommandNames() {
  return {"uname", "uptime", "date", "whoami", "hostname", "df", "free",
          "lsblk", "nproc", "id", "nvidia-smi", "sensors", "lscpu"};
}

std::optional<Command> safeCommand(const QString &line, QString *error) {
  const QString trimmed = line.trimmed();
  if (trimmed.isEmpty())
    return refuse(error, QStringLiteral("There is nothing to run."));
  if (trimmed.size() > 120)
    return refuse(error, QStringLiteral("That command is too long."));
  // Anything a shell would treat specially is refused outright, even though
  // no shell is used: it is a sign the request is not a plain inspection.
  static const QRegularExpression meta(QStringLiteral("[;&|<>$`(){}*?\\\\\"'\\n\\r~!#=/]"));
  if (meta.match(trimmed).hasMatch())
    return refuse(error, QStringLiteral("Only plain inspection commands are allowed; no paths, pipes or substitution."));
  const QStringList parts = trimmed.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
  const QString program = parts.first();
  if (!safeCommandNames().contains(program))
    return refuse(error, QStringLiteral("%1 is not on the list of commands I may run. I can run: %2.")
                             .arg(program, safeCommandNames().join(", ")));
  static const QRegularExpression flag(QStringLiteral("^--?[A-Za-z][A-Za-z0-9-]{0,24}$"));
  for (int i = 1; i < parts.size(); ++i)
    if (!flag.match(parts.at(i)).hasMatch())
      return refuse(error, QStringLiteral("Only flags such as -h or --human-readable are allowed as arguments."));
  return Command{program, parts.mid(1), {}};
}

// --- speech -------------------------------------------------------------------------

QString spokenTime(const QDateTime &now) {
  // 12-hour, no leading zero, no seconds: "3:07 PM".
  return QLocale::c().toString(now.time(), QStringLiteral("h:mm AP"));
}

QString spokenDate(const QDateTime &now) {
  return QLocale::c().toString(now.date(), QStringLiteral("dddd, MMMM d, yyyy"));
}

int parseSpokenNumber(const QString &words) {
  static const QHash<QString, int> ones = {
      {"zero", 0},  {"one", 1},   {"two", 2},    {"three", 3},  {"four", 4},
      {"five", 5},  {"six", 6},   {"seven", 7},  {"eight", 8}, {"nine", 9},
      {"ten", 10},  {"eleven", 11}, {"twelve", 12}, {"thirteen", 13},
      {"fourteen", 14}, {"fifteen", 15}, {"sixteen", 16}, {"seventeen", 17},
      {"eighteen", 18}, {"nineteen", 19}};
  static const QHash<QString, int> tens = {
      {"twenty", 20}, {"thirty", 30}, {"forty", 40}, {"fifty", 50},
      {"sixty", 60},  {"seventy", 70}, {"eighty", 80}, {"ninety", 90}};
  const QString text = words.trimmed().toLower();
  bool digits = false;
  const int n = text.toInt(&digits);
  if (digits)
    return n;
  int total = 0;
  bool any = false;
  for (const QString &word : text.split(QRegularExpression("[\\s-]+"), Qt::SkipEmptyParts)) {
    if (word == QLatin1String("a") || word == QLatin1String("and"))
      continue;
    if (word == QLatin1String("hundred")) {
      total = (total == 0 ? 1 : total) * 100;
      any = true;
    } else if (ones.contains(word)) {
      total += ones.value(word);
      any = true;
    } else if (tens.contains(word)) {
      total += tens.value(word);
      any = true;
    } else {
      return -1;
    }
  }
  return any ? total : -1;
}

} // namespace systemtools
