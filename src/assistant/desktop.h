#pragma once
#include "policy.h"

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

// The machine, as the agent and screen memory see it. Everything that talks to
// the compositor or runs a helper program is here, and every helper is run
// with an argument list -- never through a shell.
namespace desktop {

// --- Hyprland ---------------------------------------------------------------

QString hyprlandSocket();
bool hyprlandAvailable();
// One request on Hyprland's command socket. Empty on failure.
QByteArray hyprctl(const QByteArray &command, int timeoutMs = 600);

WindowInfo parseClient(const QJsonObject &client,
                       const QHash<int, QString> &monitors = {});
WindowInfo activeWindow();
QVector<WindowInfo> windows();
struct Monitor {
  QString name;
  QRect geometry; // logical, in the compositor's global layout
  double scale = 1.0;
  bool focused = false;
};
QVector<Monitor> monitors();
Monitor focusedMonitor();

// Window addresses come back from the compositor, but anything headed into a
// dispatch is checked anyway: it is the one place text meets a command.
bool validAddress(const QString &address);
bool focusWindow(const QString &address);
bool closeWindow(const QString &address);
bool moveCursor(int x, int y);

// --- applications -------------------------------------------------------------

struct App {
  QString id;       // desktop file id, e.g. "firefox.desktop"
  QString name;
  QString genericName;
  QString exec;
  QString icon;
  QStringList keywords;
  QStringList categories;
  bool terminal = false;
  QString path;     // the .desktop file
};

// Pure: the [Desktop Entry] group of a .desktop file. `ok` false when it is
// hidden, not an application, or has nothing to run.
App parseDesktopEntry(const QString &text, bool *ok);
// Pure: Exec= as an argument list, field codes removed.
QStringList execArguments(const QString &exec);

class AppIndex {
public:
  void scan();                  // XDG_DATA_HOME and XDG_DATA_DIRS
  void add(const App &app);     // for tests
  const QVector<App> &apps() const { return m_apps; }
  // Best match for a spoken name: "discord", "the terminal", "browser".
  const App *find(const QString &spoken) const;
  QStringList names() const;

private:
  QVector<App> m_apps;
};

// Start it detached, in the user's home. False if it could not be started.
bool launch(const App &app, QString *error = nullptr);

// --- input --------------------------------------------------------------------

struct Tools {
  bool grim = false;
  bool wtype = false;
  bool ydotool = false;   // the client
  bool ydotoold = false;  // and a daemon to talk to
  bool xdgOpen = false;
};
Tools detectTools();

// Run a helper with a timeout and collect its output. For short-lived tools
// only; never a shell.
struct Result {
  bool ok = false;
  int exitCode = -1;
  QByteArray out;
  QString error;
};
Result run(const QString &program, const QStringList &args,
           int timeoutMs = 5000, const QByteArray &input = {});

bool typeText(const QString &text, QString *error);
// "ctrl+c", "alt+Left", "Return", "super+shift+s".
bool pressKeys(const QString &combo, QString *error);
bool click(int button, int count, QString *error); // 1 left, 2 right, 3 middle
bool scroll(int dx, int dy, QString *error);

// --- screen -------------------------------------------------------------------

// Grab a region (logical global coordinates) or a whole output. Asynchronous;
// the callback gets a null image on failure. The image never touches disk.
void capture(const QRect &region, const QString &output,
             std::function<void(QImage, QString)> done, QObject *context);

} // namespace desktop
