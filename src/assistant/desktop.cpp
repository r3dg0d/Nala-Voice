#include "desktop.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <unistd.h>

namespace desktop {

// --- Hyprland ---------------------------------------------------------------

QString hyprlandSocket() {
  const auto env = QProcessEnvironment::systemEnvironment();
  const QString signature = env.value("HYPRLAND_INSTANCE_SIGNATURE");
  if (signature.isEmpty() || signature.contains('/'))
    return {};
  const QString runtime = env.value(
      "XDG_RUNTIME_DIR", QStringLiteral("/run/user/%1").arg(::getuid()));
  return runtime + "/hypr/" + signature + "/.socket.sock";
}

bool hyprlandAvailable() {
  const QString path = hyprlandSocket();
  return !path.isEmpty() && QFileInfo::exists(path);
}

QByteArray hyprctl(const QByteArray &command, int timeoutMs) {
  const QString path = hyprlandSocket();
  if (path.isEmpty())
    return {};
  QLocalSocket socket;
  socket.connectToServer(path);
  if (!socket.waitForConnected(timeoutMs))
    return {};
  socket.write(command);
  socket.flush();
  // Hyprland answers and hangs up; read until it does.
  QByteArray reply;
  while (socket.waitForReadyRead(timeoutMs)) {
    reply += socket.readAll();
    if (reply.size() > 8 * 1024 * 1024)
      break;
  }
  reply += socket.readAll();
  return reply;
}

WindowInfo parseClient(const QJsonObject &c, const QHash<int, QString> &mons) {
  WindowInfo w;
  w.address = c.value("address").toString();
  if (w.address.isEmpty())
    return w;
  w.valid = true;
  w.appClass = c.value("class").toString();
  w.initialClass = c.value("initialClass").toString();
  w.title = c.value("title").toString();
  const QJsonArray at = c.value("at").toArray();
  const QJsonArray size = c.value("size").toArray();
  w.geometry = QRect(at.at(0).toInt(), at.at(1).toInt(), size.at(0).toInt(),
                     size.at(1).toInt());
  // A bool in older releases, a mode number in newer ones.
  const QJsonValue full = c.value("fullscreen");
  w.fullscreen = full.isBool() ? full.toBool() : full.toInt() > 0;
  w.xwayland = c.value("xwayland").toBool();
  w.pid = c.value("pid").toInt();
  w.monitorName = mons.value(c.value("monitor").toInt());
  return w;
}

QVector<Monitor> monitors() {
  QVector<Monitor> out;
  const QJsonArray list =
      QJsonDocument::fromJson(hyprctl("j/monitors")).array();
  for (const QJsonValue &value : list) {
    const QJsonObject m = value.toObject();
    Monitor monitor;
    monitor.name = m.value("name").toString();
    monitor.scale = m.value("scale").toDouble(1.0);
    if (monitor.scale <= 0.0)
      monitor.scale = 1.0;
    // width/height are in pixels; the layout is in logical units.
    const bool rotated = m.value("transform").toInt() % 2 == 1;
    const int w = m.value("width").toInt(), h = m.value("height").toInt();
    monitor.geometry = QRect(m.value("x").toInt(), m.value("y").toInt(),
                             int((rotated ? h : w) / monitor.scale),
                             int((rotated ? w : h) / monitor.scale));
    monitor.focused = m.value("focused").toBool();
    out << monitor;
  }
  return out;
}

Monitor focusedMonitor() {
  const QVector<Monitor> all = monitors();
  for (const Monitor &monitor : all)
    if (monitor.focused)
      return monitor;
  return all.isEmpty() ? Monitor{} : all.first();
}

namespace {
QHash<int, QString> monitorNames() {
  QHash<int, QString> names;
  const QJsonArray list =
      QJsonDocument::fromJson(hyprctl("j/monitors")).array();
  for (const QJsonValue &value : list)
    names.insert(value.toObject().value("id").toInt(),
                 value.toObject().value("name").toString());
  return names;
}
} // namespace

WindowInfo activeWindow() {
  const QJsonDocument doc = QJsonDocument::fromJson(hyprctl("j/activewindow"));
  if (!doc.isObject())
    return {};
  return parseClient(doc.object(), monitorNames());
}

QVector<WindowInfo> windows() {
  QVector<WindowInfo> out;
  const QHash<int, QString> names = monitorNames();
  const QJsonArray list = QJsonDocument::fromJson(hyprctl("j/clients")).array();
  for (const QJsonValue &value : list) {
    const QJsonObject c = value.toObject();
    // Unmapped and hidden clients are not windows anyone can see.
    if (!c.value("mapped").toBool(true) || c.value("hidden").toBool(false))
      continue;
    const WindowInfo w = parseClient(c, names);
    if (w.valid)
      out << w;
  }
  return out;
}

bool validAddress(const QString &address) {
  static const QRegularExpression re(QStringLiteral("^0x[0-9a-fA-F]{1,16}$"));
  return re.match(address).hasMatch();
}

bool focusWindow(const QString &address) {
  if (!validAddress(address))
    return false;
  return hyprctl("dispatch focuswindow address:" + address.toLatin1())
      .trimmed()
      .startsWith("ok");
}

bool closeWindow(const QString &address) {
  if (!validAddress(address))
    return false;
  return hyprctl("dispatch closewindow address:" + address.toLatin1())
      .trimmed()
      .startsWith("ok");
}

bool moveCursor(int x, int y) {
  return hyprctl(QByteArray("dispatch movecursor ") + QByteArray::number(x) +
                 " " + QByteArray::number(y))
      .trimmed()
      .startsWith("ok");
}

// --- applications -------------------------------------------------------------

App parseDesktopEntry(const QString &text, bool *ok) {
  App app;
  bool inEntry = false;
  bool hidden = false, noDisplay = false, application = false;
  const QStringList lines = text.split('\n');
  for (QString line : lines) {
    line = line.trimmed();
    if (line.isEmpty() || line.startsWith('#'))
      continue;
    if (line.startsWith('[')) {
      inEntry = line == QLatin1String("[Desktop Entry]");
      continue;
    }
    if (!inEntry)
      continue;
    const int eq = line.indexOf('=');
    if (eq <= 0)
      continue;
    const QString key = line.left(eq).trimmed();
    const QString value = line.mid(eq + 1).trimmed();
    // Unlocalised keys only; "Name[fr]" is somebody else's language.
    if (key == "Name")
      app.name = value;
    else if (key == "GenericName")
      app.genericName = value;
    else if (key == "Exec")
      app.exec = value;
    else if (key == "Icon")
      app.icon = value;
    else if (key == "Keywords")
      app.keywords = value.split(';', Qt::SkipEmptyParts);
    else if (key == "Categories")
      app.categories = value.split(';', Qt::SkipEmptyParts);
    else if (key == "Terminal")
      app.terminal = value == "true";
    else if (key == "Hidden")
      hidden = value == "true";
    else if (key == "NoDisplay")
      noDisplay = value == "true";
    else if (key == "Type")
      application = value == "Application";
  }
  if (ok)
    *ok = application && !hidden && !noDisplay && !app.name.isEmpty() &&
          !execArguments(app.exec).isEmpty();
  return app;
}

QStringList execArguments(const QString &exec) {
  // The spec escapes reserved characters inside quotes with backslashes;
  // QProcess::splitCommand handles the quoting itself.
  QStringList args = QProcess::splitCommand(exec);
  QStringList out;
  for (QString arg : args) {
    if (arg.size() == 2 && arg.startsWith('%')) {
      if (arg == "%%")
        out << "%";
      continue; // %f %u %F %U %i %c %k and the deprecated ones: no files here
    }
    arg.replace(QStringLiteral("%%"), QStringLiteral("%"));
    arg.remove(QRegularExpression(QStringLiteral("%[fFuUick]")));
    if (!arg.isEmpty())
      out << arg;
  }
  return out;
}

void AppIndex::scan() {
  m_apps.clear();
  QSet<QString> seen;
  // Earlier directories win, as the spec requires: the user's own overrides
  // come first.
  for (const QString &root :
       QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation)) {
    QDirIterator it(root, {"*.desktop"}, QDir::Files,
                    QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
    while (it.hasNext()) {
      const QString path = it.next();
      QString id = path.mid(root.size() + 1);
      id.replace('/', '-');
      if (seen.contains(id))
        continue;
      seen.insert(id);
      QFile file(path);
      if (file.size() > 256 * 1024 || !file.open(QIODevice::ReadOnly))
        continue;
      bool ok = false;
      App app = parseDesktopEntry(QString::fromUtf8(file.readAll()), &ok);
      if (!ok)
        continue;
      app.id = id;
      app.path = path;
      m_apps << app;
    }
  }
}

void AppIndex::add(const App &app) { m_apps << app; }

QStringList AppIndex::names() const {
  QStringList out;
  for (const App &app : m_apps)
    out << app.name;
  out.sort(Qt::CaseInsensitive);
  return out;
}

const App *AppIndex::find(const QString &spoken) const {
  QString want = spoken.toLower().simplified();
  want.remove(QRegularExpression(QStringLiteral(R"(^(the|my|a|an)\s+)")));
  want.remove(QRegularExpression(QStringLiteral(R"(\s+(app|application|program)$)")));
  if (want.isEmpty())
    return nullptr;

  // Generic requests go by category.
  static const QHash<QString, QString> kinds = {
      {"terminal", "TerminalEmulator"},  {"console", "TerminalEmulator"},
      {"shell", "TerminalEmulator"},     {"browser", "WebBrowser"},
      {"web browser", "WebBrowser"},     {"internet", "WebBrowser"},
      {"files", "FileManager"},          {"file manager", "FileManager"},
      {"file browser", "FileManager"},   {"text editor", "TextEditor"},
      {"editor", "TextEditor"},          {"calculator", "Calculator"},
      {"music player", "Player"},        {"email", "Email"},
      {"mail", "Email"}};

  const auto norm = [](QString s) {
    return s.toLower().remove(QRegularExpression(QStringLiteral(R"([^a-z0-9+]+)")));
  };
  const QString key = norm(want);

  int best = 0;
  const App *match = nullptr;
  for (const App &app : m_apps) {
    const QString name = norm(app.name);
    QString id = app.id.toLower();
    id.chop(8); // ".desktop"
    const QString idTail = norm(id.section('.', -1));
    const QString binary =
        norm(QFileInfo(execArguments(app.exec).value(0)).fileName());
    int score = 0;
    if (name == key || idTail == key || binary == key)
      score = 100;
    else if (kinds.contains(want) &&
             app.categories.contains(kinds.value(want)))
      score = 80;
    else if (name.startsWith(key) || idTail.startsWith(key))
      score = 60;
    else if (key.size() >= 4 && (name.contains(key) || norm(id).contains(key)))
      score = 40;
    else
      for (const QString &word : app.keywords)
        if (norm(word) == key)
          score = std::max(score, 50);
    // Prefer what people actually mean by a bare category: the entry that
    // is not someone's settings panel.
    if (score > 0 && app.categories.contains("Settings"))
      score -= 5;
    if (score > best) {
      best = score;
      match = &app;
    }
  }
  return match;
}

bool launch(const App &app, QString *error) {
  QStringList args = execArguments(app.exec);
  if (args.isEmpty()) {
    if (error)
      *error = QStringLiteral("nothing to run");
    return false;
  }
  if (app.terminal) {
    // A terminal program needs a terminal to live in.
    const QString term = QStandardPaths::findExecutable("xdg-terminal-exec");
    if (!term.isEmpty())
      args.prepend(term);
    else
      for (const char *candidate :
           {"kitty", "foot", "alacritty", "wezterm", "ghostty", "konsole"})
        if (!QStandardPaths::findExecutable(candidate).isEmpty()) {
          args = QStringList{candidate, "-e"} + args;
          break;
        }
  }
  const QString program = args.takeFirst();
  qint64 pid = 0;
  const bool ok =
      QProcess::startDetached(program, args, QDir::homePath(), &pid);
  if (!ok && error)
    *error = QStringLiteral("%1 could not be started").arg(program);
  return ok;
}

// --- helpers --------------------------------------------------------------------

Result run(const QString &program, const QStringList &args, int timeoutMs,
           const QByteArray &input) {
  Result result;
  const QString path = QStandardPaths::findExecutable(program);
  if (path.isEmpty()) {
    result.error = QStringLiteral("%1 is not installed").arg(program);
    return result;
  }
  QProcess process;
  process.start(path, args);
  if (!process.waitForStarted(2000)) {
    result.error = QStringLiteral("%1 did not start").arg(program);
    return result;
  }
  if (!input.isEmpty())
    process.write(input);
  process.closeWriteChannel();
  if (!process.waitForFinished(timeoutMs)) {
    process.kill();
    process.waitForFinished(500);
    result.error = QStringLiteral("%1 timed out").arg(program);
    return result;
  }
  result.exitCode = process.exitCode();
  result.out = process.readAllStandardOutput();
  result.ok = process.exitStatus() == QProcess::NormalExit &&
              result.exitCode == 0;
  if (!result.ok)
    result.error = QString::fromUtf8(process.readAllStandardError())
                       .trimmed()
                       .left(300);
  return result;
}

Tools detectTools() {
  Tools tools;
  const auto has = [](const char *name) {
    return !QStandardPaths::findExecutable(QString::fromLatin1(name)).isEmpty();
  };
  tools.grim = has("grim");
  tools.wtype = has("wtype");
  tools.ydotool = has("ydotool");
  tools.xdgOpen = has("xdg-open");
  const auto env = QProcessEnvironment::systemEnvironment();
  const QString runtime = env.value(
      "XDG_RUNTIME_DIR", QStringLiteral("/run/user/%1").arg(::getuid()));
  for (const QString &socket :
       {env.value("YDOTOOL_SOCKET"), runtime + "/.ydotool_socket",
        QStringLiteral("/tmp/.ydotool_socket")})
    if (!socket.isEmpty() && QFileInfo::exists(socket))
      tools.ydotoold = true;
  return tools;
}

bool typeText(const QString &text, QString *error) {
  // Through stdin, so what is typed never shows up in a process listing.
  const Result r = run("wtype", {"-"}, 20000, text.toUtf8());
  if (!r.ok && error)
    *error = r.error.isEmpty() ? QStringLiteral("wtype failed") : r.error;
  return r.ok;
}

bool pressKeys(const QString &combo, QString *error) {
  static const QHash<QString, QString> modifiers = {
      {"ctrl", "ctrl"},   {"control", "ctrl"}, {"shift", "shift"},
      {"alt", "alt"},     {"super", "logo"},   {"meta", "logo"},
      {"win", "logo"},    {"logo", "logo"},    {"altgr", "altgr"}};
  static const QHash<QString, QString> keys = {
      {"enter", "Return"},     {"return", "Return"},   {"esc", "Escape"},
      {"escape", "Escape"},    {"tab", "Tab"},         {"space", "space"},
      {"backspace", "BackSpace"}, {"delete", "Delete"}, {"del", "Delete"},
      {"up", "Up"},            {"down", "Down"},       {"left", "Left"},
      {"right", "Right"},      {"home", "Home"},       {"end", "End"},
      {"pageup", "Prior"},     {"pagedown", "Next"},   {"insert", "Insert"},
      {"print", "Print"},      {"menu", "Menu"}};

  QStringList mods, args;
  QString key;
  for (const QString &part : combo.split('+', Qt::SkipEmptyParts)) {
    const QString lower = part.trimmed().toLower();
    if (modifiers.contains(lower))
      mods << modifiers.value(lower);
    else if (keys.contains(lower))
      key = keys.value(lower);
    else if (QRegularExpression(QStringLiteral("^f([1-9]|1[0-2])$"))
                 .match(lower)
                 .hasMatch())
      key = lower.toUpper();
    else if (QRegularExpression(QStringLiteral("^[a-z0-9]$")).match(lower).hasMatch())
      key = lower;
    else if (QRegularExpression(QStringLiteral("^[A-Za-z_]{2,20}$"))
                 .match(part.trimmed())
                 .hasMatch())
      key = part.trimmed(); // an X keysym name, passed as it is
    else {
      if (error)
        *error = QStringLiteral("unknown key \"%1\"").arg(part);
      return false;
    }
  }
  if (key.isEmpty()) {
    if (error)
      *error = QStringLiteral("no key to press");
    return false;
  }
  for (const QString &mod : mods)
    args << "-M" << mod;
  args << "-k" << key;
  for (auto it = mods.crbegin(); it != mods.crend(); ++it)
    args << "-m" << *it;
  const Result r = run("wtype", args);
  if (!r.ok && error)
    *error = r.error;
  return r.ok;
}

bool click(int button, int count, QString *error) {
  const int code = 0xC0 + std::clamp(button - 1, 0, 2); // down and up
  QStringList args = {"click"};
  if (count > 1)
    args << "-r" << QString::number(std::min(count, 3)) << "-D" << "70";
  args << QStringLiteral("0x%1").arg(code, 2, 16, QChar('0'));
  const Result r = run("ydotool", args);
  if (!r.ok && error)
    *error = r.error;
  return r.ok;
}

bool scroll(int dx, int dy, QString *error) {
  const Result r = run("ydotool", {"mousemove", "--wheel", "-x",
                                   QString::number(std::clamp(dx, -50, 50)),
                                   "-y",
                                   QString::number(std::clamp(dy, -50, 50))});
  if (!r.ok && error)
    *error = r.error;
  return r.ok;
}

// --- screen -------------------------------------------------------------------

void capture(const QRect &region, const QString &output,
             std::function<void(QImage, QString)> done, QObject *context) {
  const QString grim = QStandardPaths::findExecutable("grim");
  if (grim.isEmpty()) {
    done({}, QStringLiteral("grim is not installed"));
    return;
  }
  QStringList args = {"-t", "jpeg", "-q", "90"};
  if (!output.isEmpty())
    args << "-o" << output;
  else if (region.isValid())
    args << "-g"
         << QStringLiteral("%1,%2 %3x%4")
                .arg(region.x())
                .arg(region.y())
                .arg(region.width())
                .arg(region.height());
  args << "-"; // to stdout: the frame stays in memory until it is judged

  auto *process = new QProcess(context);
  auto *timeout = new QTimer(process);
  timeout->setSingleShot(true);
  QObject::connect(timeout, &QTimer::timeout, process, [process] {
    process->kill();
  });
  QObject::connect(
      process, &QProcess::finished, context,
      [process, done](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        if (status != QProcess::NormalExit || code != 0) {
          done({}, QStringLiteral("grim failed: %1")
                       .arg(QString::fromUtf8(process->readAllStandardError())
                                .trimmed()
                                .left(200)));
          return;
        }
        QImage image;
        image.loadFromData(process->readAllStandardOutput(), "JPEG");
        done(image, image.isNull() ? QStringLiteral("grim sent no image")
                                   : QString());
      });
  QObject::connect(process, &QProcess::errorOccurred, context,
                   [process, done](QProcess::ProcessError error) {
                     if (error != QProcess::FailedToStart)
                       return;
                     process->deleteLater();
                     done({}, QStringLiteral("grim could not start"));
                   });
  timeout->start(10000);
  process->start(grim, args);
}

} // namespace desktop
