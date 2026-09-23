#include "activity.h"
#include "assistant.h"
#include "eventlog.h"
#include "identity.h"
#include "screenmemory.h"
#include "settings.h"
#include "backend.h"
#include "compositor.h"
#include "cursor.h"
#include "mascot.h"
#include "music.h"
#include "orbits.h"
#include "selftest.h"
#include "trail.h"
#include "wakecli.h"
#include "theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QPointer>
#include <QAction>
#include <QFileInfo>
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
#include <memory>
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
  // Qt's QML disk cache has been seen to serve the previous build's QML after
  // an upgrade -- a new binary showing old windows. Compiling Nala's QML on
  // start costs tens of milliseconds, so it is not worth the risk.
  if (qEnvironmentVariableIsEmpty("QML_DISABLE_DISK_CACHE"))
    qputenv("QML_DISABLE_DISK_CACHE", "1");
  if (qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP") &&
      qgetenv("QT_QUICK_BACKEND") != "software" &&
      qgetenv("QT_QPA_PLATFORM") != "offscreen")
    qputenv("QSG_RENDER_LOOP", "threaded");
  QQuickWindow::setDefaultAlphaBuffer(true);

  // `nala wakeword …` is a tool, not the companion: no window, no second
  // instance check.
  const bool wakewordTool = argc > 1 && qstrcmp(argv[1], "wakeword") == 0;
  if (wakewordTool && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    qputenv("QT_QPA_PLATFORM", "offscreen");

  QApplication app(argc, argv);
  app.setApplicationName("nala");
  app.setApplicationDisplayName("Nala");
  app.setOrganizationName("Nala");
  app.setApplicationVersion(QStringLiteral(NALA_VERSION));
  app.setDesktopFileName("nala");
  app.setQuitOnLastWindowClosed(false);

  if (wakewordTool) {
    app.setApplicationName("nala");
    return runWakewordCli(app.arguments().mid(2));
  }

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
      "scatter, dash, demo, rest, reset, quit; and for the assistant: "
      "listen, ask <words>, stop, doctor, timeline, setup, "
      "profile export|import <file>, wakeword <command> (see \"nala wakeword\"), "
      "memory pause [minutes] | resume | status");
  parser.process(app);

  // Both capture modes drive the animation clock themselves, so they share the
  // same "the app is being measured, not used" flag: no cursor polling, no
  // stray input, no writing over the user's preferences.
  const bool testing = parser.isSet("self-test") || parser.isSet("poses") ||
                       parser.isSet("film");
  const bool preview = parser.isSet("preview") || testing;
  // Everything after the command is its argument: `nala ask what time is it`.
  const QString requested = parser.positionalArguments().isEmpty()
                                ? QStringLiteral("run")
                                : parser.positionalArguments().join(' ');
  const QString socketPath = runtimeSocket();

  // A profile path means the caller's directory, not the running copy's.
  QString forwarded = requested;
  if (requested.startsWith("hear ")) {
    forwarded = QStringLiteral("hear ") +
                QFileInfo(requested.section(' ', 1).trimmed()).absoluteFilePath();
  } else if (requested.startsWith("profile ")) {
    const QString path = requested.section(' ', 2).trimmed();
    if (!path.isEmpty() && !path.startsWith("~/"))
      forwarded = requested.section(' ', 0, 1) + ' ' +
                  QFileInfo(path).absoluteFilePath();
  }

  // Hand the command to an already-running Nala rather than starting a second.
  if (!testing && !preview) {
    QLocalSocket client;
    client.connectToServer(socketPath);
    if (client.waitForConnected(300)) {
      // One line on the wire: newlines in an argument would split it.
      QString line = requested == "run" ? QStringLiteral("status") : forwarded;
      line.replace('\n', ' ');
      client.write(line.toUtf8().left(4000) + "\n");
      client.flush();
      client.waitForBytesWritten(1000);
      // The health check waits on the network, so give it longer.
      if (client.waitForReadyRead(line == "doctor" ? 12000 : 2500))
        QTextStream(stdout) << client.readAll();
      return 0;
    }
    // Not running: a profile can still be read or written straight from the
    // settings file.
    if (requested.startsWith("profile ")) {
      const QString what = forwarded.section(' ', 1, 1);
      QString path = forwarded.section(' ', 2).trimmed();
      if (path.startsWith("~/"))
        path = QDir::homePath() + path.mid(1);
      AssistantSettings settings(
          (parser.value("config").isEmpty()
               ? QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/nala"
               : QFileInfo(parser.value("config")).absolutePath()) +
          "/assistant.json");
      QString error;
      if (what == "export") {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
          error = file.errorString();
        else {
          file.write(QJsonDocument(profile::exportProfile(settings))
                         .toJson(QJsonDocument::Indented));
          if (!file.commit())
            error = file.errorString();
        }
      } else if (what == "import") {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
          error = file.errorString();
        else if (profile::importProfile(settings,
                                        QJsonDocument::fromJson(file.readAll()).object(),
                                        &error)
                     .isEmpty() &&
                 error.isEmpty())
          error = QStringLiteral("nothing in it could be used");
        settings.flush();
      } else {
        error = QStringLiteral("usage: nala profile export|import <file.json>");
      }
      QTextStream(error.isEmpty() ? stdout : stderr)
          << (error.isEmpty() ? QStringLiteral("ok") : error) << "\n";
      return error.isEmpty() ? 0 : 1;
    }
    if (requested == "status" || requested == "quit" ||
        requested == "doctor" || requested.startsWith("ask ") ||
        requested.startsWith("memory")) {
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
    // Never touch the real desktop's configuration during a test run: the
    // autostart check writes and removes an entry, and must do it here rather
    // than in ~/.config/autostart.
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(temp.path() + "/config"));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(temp.path() + "/cache"));
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
  Compositor compositor;
  Music music;
  Backend backend(configPath, preview, testing, &mascot, &theme, &cursor,
                  &activity, &compositor, &music);

  // The assistant keeps its settings, memories and log beside the
  // companion's, or in the throwaway directory when being measured.
  Assistant::Paths paths;
  paths.settings = QFileInfo(configPath).absolutePath() + "/assistant.json";
  paths.memoryDir =
      testing ? temp.path() + "/memory"
              : QStandardPaths::writableLocation(
                    QStandardPaths::GenericDataLocation) +
                    "/nala/memory";
  paths.log = testing ? QString()
                      : QStandardPaths::writableLocation(
                            QStandardPaths::GenericStateLocation) +
                            "/nala/assistant.log";
  Assistant assistant(paths, testing);
  assistant.setCompanion([&backend](const QString &command) {
    backend.command(command);
  });
  QObject::connect(&assistant, &Assistant::stateChanged, &mascot,
                   [&] { mascot.setCue(assistant.state()); });
  QObject::connect(&assistant, &Assistant::levelsChanged, &mascot,
                   [&] { mascot.setVoiceLevel(assistant.voiceLevel()); });
  // Heard her wake phrase: she perks up now, from the detector itself,
  // before the recogniser or the model have done anything.
  QObject::connect(&assistant, &Assistant::wakeDetected, &mascot,
                   [&] { mascot.perk(); });
  const auto applyIdentity = [&] {
    mascot.setExpressiveness(assistant.identity().expressiveScale());
  };
  applyIdentity();
  QObject::connect(&assistant, &Assistant::identityChanged, &mascot,
                   applyIdentity);
  QObject::connect(&assistant, &Assistant::setupRequested, &backend,
                   &Backend::openSetup);
  // However screen memory gets paused -- voice, tray, a button -- she covers
  // her eyes, so the moment is visible.
  auto memoryWasPaused = std::make_shared<bool>(assistant.memory()->paused());
  QObject::connect(assistant.memory(), &ScreenMemory::changed, &mascot,
                   [&, memoryWasPaused] {
                     const bool paused = assistant.memory()->paused();
                     if (paused && !*memoryWasPaused)
                       mascot.coverEyes();
                     *memoryWasPaused = paused;
                   });
  QObject::connect(&assistant, &Assistant::settingsRequested, &backend,
                   &Backend::openSettings);
  QObject::connect(&assistant, &Assistant::settingsCloseRequested, &backend,
                   &Backend::closeSettings);
  QObject::connect(&assistant, &Assistant::timelineRequested, &backend,
                   &Backend::openTimeline);

  qmlRegisterType<OrbitLayer>("Nala", 1, 0, "OrbitLayer");
  qmlRegisterType<Trail>("Nala", 1, 0, "Trail");
  qmlRegisterUncreatableType<Mascot>("Nala", 1, 0, "Mascot",
                                     "Provided as a context property");

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("backend", &backend);
  engine.rootContext()->setContextProperty("mascot", &mascot);
  engine.rootContext()->setContextProperty("orbits", &orbits);
  engine.rootContext()->setContextProperty("theme", &theme);
  engine.rootContext()->setContextProperty("assistant", &assistant);
  engine.rootContext()->setContextProperty("assistantSettings",
                                           assistant.settings());
  engine.rootContext()->setContextProperty("screenMemory", assistant.memory());
  engine.rootContext()->setContextProperty("eventLog", assistant.log());

  QStringList warnings;
  QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                   [&warnings](const QList<QQmlError> &errors) {
                     for (const QQmlError &error : errors)
                       warnings << error.toString();
                   });

  engine.load(QUrl("qrc:/qml/Mascot.qml"));
  if (engine.rootObjects().isEmpty()) {
    QTextStream err(stderr);
    err << "Nala could not build its window.\n";
    for (const QString &warning : warnings)
      err << "  " << warning << "\n";
    return 2;
  }
  auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
  if (!window) {
    QTextStream(stderr) << "Unexpected root object.\n";
    return 3;
  }

  backend.attach(window);
  if (auto *bubble = window->findChild<QQuickWindow *>("bubbleWindow"))
    backend.attachBubble(bubble);
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
          const QString verb = name.section(' ', 0, 0);
          const QString rest = name.section(' ', 1).trimmed();
          const auto reply = [client](const QString &text) {
            client->write(text.toUtf8() + "\n");
            client->flush();
            client->disconnectFromServer();
          };
          if (name == "status") {
            reply(backend.status() + QStringLiteral("; assistant %1, screen "
                                                    "memory %2")
                                         .arg(assistant.state(),
                                              assistant.memory()->status()));
          } else if (verb == "doctor") {
            // Answered when the checks come back.
            QPointer<QLocalSocket> guard(client);
            assistant.diagnose([guard, reply](const QString &text) {
              if (guard)
                reply(text.trimmed());
            });
          } else if (verb == "listen") {
            assistant.toggleListening();
            reply(assistant.listening() ? "listening" : "not listening");
          } else if (verb == "ask") {
            if (rest.isEmpty()) {
              reply("usage: nala ask <words>");
            } else {
              assistant.ask(rest);
              reply("ok");
            }
          } else if (verb == "stop") {
            assistant.stop();
            reply("ok");
          } else if (verb == "profile") {
            const QString what = rest.section(' ', 0, 0);
            const QString path = rest.section(' ', 1).trimmed();
            if ((what != "export" && what != "import") || path.isEmpty()) {
              reply("usage: nala profile export|import <file.json>");
              return;
            }
            const QString error = what == "export"
                                      ? assistant.exportProfile(path)
                                      : assistant.importProfile(path);
            reply(error.isEmpty() ? QStringLiteral("ok") : error);
          } else if (verb == "hear") {
            const QString error = assistant.hearFile(rest);
            reply(error.isEmpty() ? QStringLiteral("ok") : error);
          } else if (verb == "setup") {
            backend.openSetup();
            reply("ok");
          } else if (verb == "timeline") {
            backend.openTimeline();
            reply("ok");
          } else if (verb == "memory") {
            const QString what = rest.section(' ', 0, 0);
            if (what == "pause") {
              assistant.memory()->pause(rest.section(' ', 1, 1).toInt());
            } else if (what == "resume") {
              assistant.memory()->resume();
            } else if (what != "status" && !what.isEmpty()) {
              reply("usage: nala memory pause [minutes] | resume | status");
              return;
            }
            reply(QStringLiteral("screen memory %1, %2 memories, %3")
                      .arg(assistant.memory()->status())
                      .arg(assistant.memory()->count())
                      .arg(assistant.formatBytes(
                          assistant.memory()->storageBytes())));
          } else {
            backend.command(name);
            reply("ok");
          }
        });
        QObject::connect(client, &QLocalSocket::disconnected, client,
                         &QObject::deleteLater);
      }
    });
  }

  QSystemTrayIcon tray;
  QMenu menu;
  menu.addAction("Preferences…", &backend, &Backend::openSettings);
  menu.addAction("Listen", &assistant, &Assistant::toggleListening);
  menu.addAction("Memories…", &backend, &Backend::openTimeline);
  QAction *privacy = menu.addAction("Pause screen memory");
  const auto refreshPrivacy = [&] {
    privacy->setVisible(assistant.memory()->enabled());
    privacy->setText(assistant.memory()->paused() ? "Resume screen memory"
                                                  : "Pause screen memory");
  };
  refreshPrivacy();
  QObject::connect(assistant.memory(), &ScreenMemory::changed, &menu,
                   refreshPrivacy);
  QObject::connect(privacy, &QAction::triggered, &assistant, [&] {
    if (assistant.memory()->paused()) {
      assistant.memory()->resume();
    } else {
      assistant.memory()->pause();
    }
  });
  menu.addAction("Say hello", &mascot, [&mascot] { mascot.poke(); });
  menu.addAction("Off you go", &backend,
                 [&backend] { backend.command("dash"); });
  menu.addAction("Show me everything", &backend, &Backend::demo);
  menu.addAction("Reset position", &backend, &Backend::resetPlace);
  menu.addSeparator();
  menu.addAction("Quit Nala", &app, &QApplication::quit);
  tray.setContextMenu(&menu);
  // The tray says who she is and whether the microphone is open.
  const auto refreshTooltip = [&] {
    const QString mic = assistant.micState();
    tray.setToolTip(
        mic == "off"         ? assistant.assistantName()
        : mic == "wake"      ? QStringLiteral("%1 -- listening for her name")
                                   .arg(assistant.assistantName())
        : mic == "recording" ? QStringLiteral("%1 -- recording")
                                   .arg(assistant.assistantName())
                             : QStringLiteral("%1 -- microphone open")
                                   .arg(assistant.assistantName()));
  };
  refreshTooltip();
  QObject::connect(&assistant, &Assistant::stateChanged, &tray, refreshTooltip);
  QObject::connect(&assistant, &Assistant::identityChanged, &tray, refreshTooltip);

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
    return runSelfTest(app, backend, mascot, orbits, theme, activity,
                       compositor, music, window, warnings,
                       parser.value("capture-dir"));

  return app.exec();
}
