#include "activity.h"
#include "backend.h"
#include "cursor.h"
#include "mascot.h"
#include "orbits.h"
#include "selftest.h"
#include "trail.h"
#include "theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <unistd.h>

namespace {

// The tray glyph is drawn rather than shipped so it can take the panel's
// foreground colour -- a fixed black mascot disappears on a dark tray.
QIcon trayIcon(const QColor &ink) {
  QPixmap pixmap(64, 64);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(Qt::NoPen);
  painter.setBrush(ink);
  painter.drawEllipse(QPointF(32, 32), 26, 26);

  // Punch the eyes straight out so the icon stays readable when it is tinted.
  painter.setCompositionMode(QPainter::CompositionMode_Clear);
  QPainterPath eyes;
  eyes.addRoundedRect(QRectF(29.0, 19.6, 7.3, 12.6), 3.65, 3.65);
  eyes.addRoundedRect(QRectF(40.8, 19.6, 7.3, 12.6), 3.65, 3.65);
  painter.fillPath(eyes, Qt::black);
  painter.end();

  QIcon icon(pixmap);
  icon.setIsMask(false);
  return icon;
}

QString runtimeSocket() {
  return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) +
         "/nala-" + QString::number(getuid());
}

} // namespace

int main(int argc, char **argv) {
  qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
  if (qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP") &&
      qgetenv("QT_QUICK_BACKEND") != "software" &&
      qgetenv("QT_QPA_PLATFORM") != "offscreen")
    qputenv("QSG_RENDER_LOOP", "threaded");
  QQuickWindow::setDefaultAlphaBuffer(true);

  QApplication app(argc, argv);
  app.setApplicationName("nala");
  app.setApplicationDisplayName("Nala");
  app.setOrganizationName("Nala");
  app.setApplicationVersion("1.0.0");
  app.setDesktopFileName("nala");
  app.setQuitOnLastWindowClosed(false);

  QCommandLineParser parser;
  parser.setApplicationDescription("Nala — a desktop companion for Hyprland");
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addOption({"preview", "Open as a plain window, without the overlay"});
  parser.addOption({"self-test", "Exercise the app offscreen and report"});
  parser.addOption({"config", "Use a specific preferences file", "path"});
  parser.addOption({"capture-dir", "Write test captures here", "path"});
  parser.addOption({"poses", "Render one PNG per form into this directory "
                             "and exit (needs a display)", "path"});
  parser.addOption({"film", "Record a scripted sequence at 60 fps into this "
                            "directory and exit (needs a display)", "path"});
  parser.addPositionalArgument(
      "command",
      "run (default), settings, status, poke, wink, think, alert, notify, "
      "scatter, dash, demo, rest, reset, quit");
  parser.process(app);

  // Both capture modes drive the animation clock themselves, so they share the
  // same "the app is being measured, not used" flag: no cursor polling, no
  // stray input, no writing over the user's preferences.
  const bool testing = parser.isSet("self-test") || parser.isSet("poses") ||
                       parser.isSet("film");
  const bool preview = parser.isSet("preview") || testing;
  const QString requested = parser.positionalArguments().value(0, "run");
  const QString socketPath = runtimeSocket();

  // Hand the command to an already-running Nala rather than starting a second.
  if (!testing && !preview) {
    QLocalSocket client;
    client.connectToServer(socketPath);
    if (client.waitForConnected(300)) {
      client.write((requested == "run" ? "status" : requested).toUtf8() + "\n");
      client.flush();
      client.waitForBytesWritten(1000);
      if (client.waitForReadyRead(2500))
        QTextStream(stdout) << client.readAll();
      return 0;
    }
    if (requested == "status" || requested == "quit") {
      QTextStream(stderr) << "Nala is not running.\n";
      return 1;
    }
  }

  QLockFile lock(socketPath + ".lock");
  if (!testing && !preview && !lock.tryLock(1000)) {
    QTextStream(stderr) << "Nala is already starting.\n";
    return 1;
  }

  QTemporaryDir temp;
  QString configPath = parser.value("config");
  if (testing && configPath.isEmpty())
    configPath = temp.path() + "/preferences.json";
  if (configPath.isEmpty())
    configPath =
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
        "/nala/preferences.json";

  QString themeConfig =
      QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
  QString themeState =
      QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
  if (testing) {
    // Never read the real desktop's theme during a test run.
    themeConfig = temp.path() + "/config";
    themeState = temp.path() + "/state";
    QDir().mkpath(themeConfig + "/gtk-4.0");
    QDir().mkpath(themeState + "/noctalia");
    QFile css(themeConfig + "/gtk-4.0/noctalia.css");
    if (css.open(QIODevice::WriteOnly))
      css.write("@define-color window_bg_color #24273a;\n"
                "@define-color window_fg_color #cad3f5;\n"
                "@define-color card_bg_color #363a4f;\n"
                "@define-color accent_bg_color #b7bdf8;\n"
                "@define-color accent_fg_color #24273a;\n");
    QFile toml(themeState + "/noctalia/settings.toml");
    if (toml.open(QIODevice::WriteOnly))
      toml.write("[shell]\nfont_family = \"Google Sans Flex\"\n"
                 "corner_radius_scale = 0.75\n"
                 "[shell.animation]\nspeed = 0.6\n");
  }

  Theme theme(themeConfig, themeState);
  Mascot mascot;
  Orbits orbits;
  Cursor cursor;
  Activity activity(themeState);
  Backend backend(configPath, preview, testing, &mascot, &theme, &cursor,
                  &activity);

  qmlRegisterType<OrbitLayer>("Nala", 1, 0, "OrbitLayer");
  qmlRegisterType<Trail>("Nala", 1, 0, "Trail");
  qmlRegisterUncreatableType<Mascot>("Nala", 1, 0, "Mascot",
                                     "Provided as a context property");

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("backend", &backend);
  engine.rootContext()->setContextProperty("mascot", &mascot);
  engine.rootContext()->setContextProperty("orbits", &orbits);
  engine.rootContext()->setContextProperty("theme", &theme);

  QStringList warnings;
  QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                   [&warnings](const QList<QQmlError> &errors) {
                     for (const QQmlError &error : errors)
                       warnings << error.toString();
                   });

  engine.load(QUrl("qrc:/qml/Mascot.qml"));
  if (engine.rootObjects().isEmpty()) {
    QTextStream(stderr) << "Nala could not build its window.\n";
    return 2;
  }
  auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
  if (!window) {
    QTextStream(stderr) << "Unexpected root object.\n";
    return 3;
  }

  backend.attach(window);
  window->setIcon(QIcon(":/assets/nala.svg"));
  window->setProperty("ready", true);

  QLocalServer server;
  if (!testing && !parser.isSet("preview")) {
    server.setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(socketPath);
    if (!server.listen(socketPath)) {
      QTextStream(stderr) << "Unable to create Nala's control socket.\n";
      return 4;
    }
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&] {
      while (QLocalSocket *client = server.nextPendingConnection()) {
        QObject::connect(client, &QLocalSocket::readyRead, &app, [&, client] {
          QByteArray buffer =
              client->property("buffer").toByteArray() + client->readAll();
          if (buffer.size() > 4096) { // a control socket, not a data channel
            client->disconnectFromServer();
            return;
          }
          if (!buffer.contains('\n')) {
            client->setProperty("buffer", buffer);
            return;
          }
          const QString name =
              QString::fromUtf8(buffer.left(buffer.indexOf('\n'))).trimmed();
          if (name == "status") {
            client->write(backend.status().toUtf8() + "\n");
          } else {
            backend.command(name);
            client->write("ok\n");
          }
          client->flush();
          client->disconnectFromServer();
        });
        QObject::connect(client, &QLocalSocket::disconnected, client,
                         &QObject::deleteLater);
      }
    });
  }

  QSystemTrayIcon tray;
  QMenu menu;
  menu.addAction("Preferences…", &backend, &Backend::openSettings);
  menu.addAction("Say hello", &mascot, [&mascot] { mascot.poke(); });
  menu.addAction("Off you go", &backend,
                 [&backend] { backend.command("dash"); });
  menu.addAction("Show me everything", &backend, &Backend::demo);
  menu.addAction("Reset position", &backend, &Backend::resetPlace);
  menu.addSeparator();
  menu.addAction("Quit Nala", &app, &QApplication::quit);
  tray.setContextMenu(&menu);
  tray.setToolTip("Nala");

  const auto refreshTray = [&] {
    const QColor ink = theme.colors().value("text").value<QColor>();
    tray.setIcon(trayIcon(ink.isValid() ? ink : QColor("#e6e6ea")));
  };
  refreshTray();
  QObject::connect(&theme, &Theme::changed, &app, refreshTray);

  QObject::connect(&tray, &QSystemTrayIcon::activated, &backend,
                   [&](QSystemTrayIcon::ActivationReason reason) {
                     if (reason == QSystemTrayIcon::Trigger)
                       backend.openSettings();
                   });
  if (!testing)
    tray.show();

  if (requested == "settings")
    QTimer::singleShot(200, &backend, &Backend::openSettings);

  if (!parser.value("film").isEmpty()) {
    QTimer::singleShot(0, &app, [&] {
      captureFilm(app, mascot, orbits, window, parser.value("film"));
    });
    return app.exec();
  }

  if (!parser.value("poses").isEmpty()) {
    QTimer::singleShot(0, &app, [&] {
      capturePoses(app, mascot, orbits, window, parser.value("poses"));
    });
    return app.exec();
  }

  if (testing)
    return runSelfTest(app, backend, mascot, orbits, theme, activity, window,
                       warnings, parser.value("capture-dir"));

  return app.exec();
}
