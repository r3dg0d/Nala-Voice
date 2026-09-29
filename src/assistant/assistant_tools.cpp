// Desktop actions people ask for by voice: volume, media keys, the time, the
// clipboard, notifications, screenshots, video recording, locking, and a short
// list of read-only inspection commands. Each is a typed tool (arguments are
// validated, risk decides whether she asks first) and the common ones also
// have a deterministic fast path, so "turn the volume down" never waits on a
// model. The command lines themselves come from systemtools.h.
#include "assistant.h"
#include "eventlog.h"
#include "screenmemory.h"
#include "settings.h"
#include "systemtools.h"

#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QProcess>
#include <QSysInfo>
#include <QThread>
#include <QStandardPaths>
#include <csignal>

namespace {

QJsonObject ok(const QJsonObject &fields = {}) {
  QJsonObject out = fields;
  out.insert("ok", true);
  return out;
}

QJsonObject fail(const QString &why) {
  return QJsonObject{{"ok", false}, {"error", why}};
}

QJsonObject run(const std::optional<systemtools::Command> &command,
                const QString &error, int timeoutMs = 4000,
                QByteArray *output = nullptr) {
  if (!command)
    return fail(error);
  const desktop::Result result = desktop::run(command->program, command->args,
                                              timeoutMs, command->input);
  if (!result.ok)
    return fail(QStringLiteral("%1 failed: %2")
                    .arg(command->program, result.error.isEmpty()
                                               ? QStringLiteral("exit %1").arg(result.exitCode)
                                               : result.error));
  if (output)
    *output = result.out;
  return ok();
}

QString firstOnPath(const QStringList &names) {
  for (const QString &name : names) {
    const QString found = QStandardPaths::findExecutable(name);
    if (!found.isEmpty())
      return found;
  }
  return {};
}

} // namespace

void Assistant::confirmCommand(const QString &text) {
  // Trivial commands can be silent: the effect is the confirmation.
  say(text, m_settings->flag("tts.confirmCommands"));
}

void Assistant::stopVideoRecording() {
  if (!m_recorder)
    return;
  QProcess *recorder = m_recorder;
  m_recorder = nullptr;
  // SIGINT lets the recorder finish the file; SIGTERM can leave it truncated.
  if (recorder->state() != QProcess::NotRunning)
    ::kill(pid_t(recorder->processId()), SIGINT);
  if (!recorder->waitForFinished(4000))
    recorder->kill();
  recorder->deleteLater();
  m_log->record("tool", "recording-stopped");
}

bool Assistant::runSystemFast(const Route &route) {
  const QString &a = route.action;
  const QVariantMap &args = route.args;
  if (!(a.startsWith("volume.") || a.startsWith("media.") || a == "time.now" ||
        a == "date.today" || a == "session.lock" || a == "screenshot.take" ||
        a.startsWith("record.")))
    return false;

  const auto reportFailure = [this](const QJsonObject &r) {
    say(r.value("error").toString(), false);
  };
  if (a == "volume.set") {
    if (args.value("invalid").toBool()) {
      say(QStringLiteral("Volume goes from 0 to 100."), false);
      return true;
    }
    const int level = args.value("level").toInt();
    callTool("volume.set", {{"level", level}}, [=, this](const QJsonObject &r) {
      r.value("ok").toBool() ? confirmCommand(QStringLiteral("Volume %1.").arg(level))
                             : reportFailure(r);
    });
    return true;
  }
  if (a == "volume.step") {
    const int delta = args.value("delta").toInt();
    callTool("volume.step", {{"delta", delta}}, [=, this](const QJsonObject &r) {
      if (!r.value("ok").toBool()) {
        reportFailure(r);
        return;
      }
      const int now = r.value("percent").toInt(-1);
      confirmCommand(now >= 0 ? QStringLiteral("Volume %1.").arg(now)
                              : (delta > 0 ? QStringLiteral("Louder.")
                                           : QStringLiteral("Quieter.")));
    });
    return true;
  }
  if (a == "volume.mute" || a == "volume.unmute") {
    const bool mute = a == "volume.mute";
    callTool("volume.mute", {{"mode", mute ? "mute" : "unmute"}},
             [=, this](const QJsonObject &r) {
               r.value("ok").toBool()
                   ? confirmCommand(mute ? QStringLiteral("Muted.")
                                         : QStringLiteral("Unmuted."))
                   : reportFailure(r);
             });
    return true;
  }
  if (a.startsWith("media.")) {
    const QString action = a == "media.pause"  ? "pause"
                           : a == "media.play" ? "play"
                           : a == "media.next" ? "next"
                                               : "previous";
    callTool("media.control", {{"action", action}}, [=, this](const QJsonObject &r) {
      if (!r.value("ok").toBool()) {
        // playerctl says so when nothing is playing.
        say(r.value("error").toString().contains("No players")
                ? QStringLiteral("Nothing is playing.")
                : r.value("error").toString(),
            false);
        return;
      }
      confirmCommand(action == "pause"  ? QStringLiteral("Paused.")
                     : action == "play" ? QStringLiteral("Playing.")
                     : action == "next" ? QStringLiteral("Next.")
                                        : QStringLiteral("Previous."));
    });
    return true;
  }
  if (a == "time.now" || a == "date.today") {
    // The clock needs no tool and no model.
    const QDateTime now = QDateTime::currentDateTime();
    say(a == "time.now" ? QStringLiteral("It's %1.").arg(systemtools::spokenTime(now))
                        : QStringLiteral("It's %1.").arg(systemtools::spokenDate(now)));
    return true;
  }
  if (a == "session.lock") {
    callTool("session.lock", {}, [=, this](const QJsonObject &r) {
      r.value("ok").toBool() ? confirmCommand(QStringLiteral("Locking.")) : reportFailure(r);
    });
    return true;
  }
  if (a == "screenshot.take") {
    callTool("screenshot.save", {}, [=, this](const QJsonObject &r) {
      r.value("ok").toBool()
          ? confirmCommand(QStringLiteral("Saved a screenshot."))
          : reportFailure(r);
    });
    return true;
  }
  if (a == "record.start" || a == "record.stop") {
    callTool(a == "record.start" ? "record.start" : "record.stop", {},
             [=, this](const QJsonObject &r) {
               if (!r.value("ok").toBool()) {
                 reportFailure(r);
                 return;
               }
               confirmCommand(a == "record.start" ? QStringLiteral("Recording.")
                                                  : QStringLiteral("Stopped recording."));
             });
    return true;
  }
  return false;
}

void Assistant::registerSystemTools() {
  using namespace schema;

  m_tools.add(Tool{
      "volume.set", "Set the speaker volume to a percentage from 0 to 100.",
      object({{"level", integer("Volume, 0 to 100", 0, 100)}}, {"level"}),
      Risk::Safe, "system", nullptr, nullptr,
      [](const QJsonObject &a, Tool::Done done) {
        QString error;
        done(run(systemtools::setVolume(a.value("level").toInt(), &error), error));
      }});
  m_tools.add(Tool{
      "volume.step",
      "Make the speakers louder (positive) or quieter (negative) by a number "
      "of percentage points. Returns the new volume.",
      object({{"delta", integer("Points to change by, -50 to 50", -50, 50)}}, {"delta"}),
      Risk::Safe, "system", nullptr, nullptr,
      [](const QJsonObject &a, Tool::Done done) {
        QString error;
        QJsonObject result = run(systemtools::stepVolume(a.value("delta").toInt(), &error), error);
        if (result.value("ok").toBool()) {
          const systemtools::Command get = systemtools::getVolume();
          const desktop::Result r = desktop::run(get.program, get.args, 2000);
          const systemtools::Volume v = systemtools::parseVolume(QString::fromUtf8(r.out));
          if (v.ok)
            result.insert("percent", v.percent);
        }
        done(result);
      }});
  m_tools.add(Tool{
      "volume.mute", "Mute, unmute or toggle the speakers.",
      object({{"mode", oneOf("What to do", {"mute", "unmute", "toggle"})}}, {"mode"}),
      Risk::Safe, "system", nullptr, nullptr,
      [](const QJsonObject &a, Tool::Done done) {
        QString error;
        done(run(systemtools::muteVolume(a.value("mode").toString(), &error), error));
      }});
  m_tools.add(Tool{
      "volume.get", "How loud the speakers are, and whether they are muted.",
      object({}), Risk::Safe, "system", nullptr, nullptr,
      [](const QJsonObject &, Tool::Done done) {
        const systemtools::Command get = systemtools::getVolume();
        const desktop::Result r = desktop::run(get.program, get.args, 2000);
        const systemtools::Volume v = systemtools::parseVolume(QString::fromUtf8(r.out));
        done(v.ok ? ok({{"percent", v.percent}, {"muted", v.muted}})
                  : fail(QStringLiteral("Could not read the volume (is PipeWire running?).")));
      }});
  m_tools.add(Tool{
      "media.control",
      "Control whatever is playing music or video: play, pause, next, previous.",
      object({{"action", oneOf("What to do", {"play", "pause", "play-pause", "next", "previous"})}},
             {"action"}),
      Risk::Safe, "system", nullptr, nullptr,
      [](const QJsonObject &a, Tool::Done done) {
        QString error;
        const auto command = systemtools::media(a.value("action").toString(), &error);
        if (!command) {
          done(fail(error));
          return;
        }
        const desktop::Result r = desktop::run(command->program, command->args, 3000);
        done(r.ok ? ok()
                  : fail(r.out.contains("No players") || r.error.contains("No players")
                             ? QStringLiteral("No players found")
                             : QStringLiteral("playerctl failed: %1").arg(r.error)));
      }});
  m_tools.add(Tool{
      "time.now", "The current local time and date.", object({}), Risk::Safe,
      "system", nullptr, nullptr,
      [](const QJsonObject &, Tool::Done done) {
        const QDateTime now = QDateTime::currentDateTime();
        done(ok({{"time", systemtools::spokenTime(now)},
                 {"date", systemtools::spokenDate(now)},
                 {"timezone", QString::fromUtf8(now.timeZone().id())}}));
      }});
  m_tools.add(Tool{
      "session.lock", "Lock the screen.", object({}), Risk::Low, "system",
      nullptr, nullptr,
      [](const QJsonObject &, Tool::Done done) {
        done(run(systemtools::lockScreen(), {}));
      }});
  m_tools.add(Tool{
      "notify.send", "Show a desktop notification.",
      object({{"title", string("Short title", 120)}, {"body", string("Message", 500)}},
             {"title"}),
      Risk::Safe, "system", nullptr, nullptr,
      [](const QJsonObject &a, Tool::Done done) {
        QString error;
        done(run(systemtools::notify(a.value("title").toString(),
                                     a.value("body").toString(), &error), error));
      }});
  m_tools.add(Tool{
      "clipboard.read", "Read the text on the clipboard.", object({}), Risk::Low,
      "system", nullptr, nullptr,
      [this](const QJsonObject &, Tool::Done done) {
        QByteArray out;
        const systemtools::Command c = systemtools::clipboardRead();
        QJsonObject r = run(c, {}, 3000, &out);
        if (r.value("ok").toBool()) {
          // Whatever was copied may have been written by someone else, and
          // may carry instructions: nothing leaves the machine unasked now.
          m_turnTainted = true;
          r.insert("text", QString::fromUtf8(out).left(8000));
        }
        done(r);
      }});
  m_tools.add(Tool{
      "clipboard.write", "Put text on the clipboard.",
      object({{"text", string("Text to copy", 20000)}}, {"text"}), Risk::Low,
      "system", nullptr, nullptr,
      [](const QJsonObject &a, Tool::Done done) {
        QString error;
        done(run(systemtools::clipboardWrite(a.value("text").toString(), &error), error));
      }});
  m_tools.add(Tool{
      "screenshot.save",
      "Save a screenshot of all screens to the Screenshots folder as a PNG. "
      "Returns the file path. (To look at the screen yourself, use "
      "computer.screenshot.)",
      object({}), Risk::Low, "system", nullptr, nullptr,
      [](const QJsonObject &, Tool::Done done) {
        const QString dir = QDir::homePath() + "/Pictures/Screenshots";
        QDir().mkpath(dir);
        const QString path = systemtools::screenshotPath(dir, QDateTime::currentDateTime());
        const systemtools::Command c = systemtools::screenshot(path);
        QJsonObject r = run(c, {}, 8000);
        if (r.value("ok").toBool())
          r.insert("path", path);
        done(r);
      }});
  m_tools.add(Tool{
      "record.start",
      "Start recording the screen to a video file in the Recordings folder. "
      "This is a video the user can keep -- not screen memory.",
      object({}), Risk::Medium, "system",
      [](const QJsonObject &) { return QStringLiteral("Start recording your screen?"); },
      nullptr,
      [this](const QJsonObject &, Tool::Done done) {
        if (m_recorder) {
          done(fail(QStringLiteral("A recording is already running.")));
          return;
        }
        const QString recorder =
            firstOnPath({"gpu-screen-recorder", "wf-recorder"});
        if (recorder.isEmpty()) {
          done(fail(QStringLiteral("No screen recorder is installed (gpu-screen-recorder or wf-recorder).")));
          return;
        }
        const QString dir = QDir::homePath() + "/Videos/Recordings";
        QDir().mkpath(dir);
        m_recordingPath = systemtools::recordingPath(dir, QDateTime::currentDateTime());
        const systemtools::Command c = systemtools::recordStart(recorder, m_recordingPath);
        m_recorder = new QProcess(this);
        m_recorder->setProgram(c.program);
        m_recorder->setArguments(c.args);
        connect(m_recorder, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
          if (m_recorder) {
            m_recorder->deleteLater();
            m_recorder = nullptr;
          }
        });
        m_recorder->start();
        if (!m_recorder->waitForStarted(2000)) {
          m_recorder->deleteLater();
          m_recorder = nullptr;
          done(fail(QStringLiteral("The recorder would not start.")));
          return;
        }
        m_log->record("tool", "recording-started");
        done(ok({{"path", m_recordingPath}}));
      }});
  m_tools.add(Tool{
      "record.stop", "Stop the video recording that was started.", object({}),
      Risk::Safe, "system", nullptr, nullptr,
      [this](const QJsonObject &, Tool::Done done) {
        if (!m_recorder) {
          done(fail(QStringLiteral("No recording is running.")));
          return;
        }
        const QString path = m_recordingPath;
        stopVideoRecording();
        done(ok({{"path", path}}));
      }});
  m_tools.add(Tool{
      "system.run_safe",
      "Run one read-only inspection command, one of: " +
          systemtools::safeCommandNames().join(", ") +
          ". Flags only (for example \"df -h\"); no paths, pipes or shell syntax.",
      object({{"command", string("The command line", 120)}}, {"command"}),
      Risk::Low, "system", nullptr, nullptr,
      [this](const QJsonObject &a, Tool::Done done) {
        QString error;
        QByteArray out;
        const auto command = systemtools::safeCommand(a.value("command").toString(), &error);
        QJsonObject r = run(command, error, 6000, &out);
        if (r.value("ok").toBool())
          r.insert("output", QString::fromUtf8(out).left(m_settings->integer("llm.maxToolOutputChars")));
        done(r);
      }});
  m_tools.add(Tool{
      "system.info",
      "A summary of this computer: kernel, CPU count, memory, uptime, the "
      "time zone and the language model in use.",
      object({}), Risk::Safe, "system", nullptr, nullptr,
      [this](const QJsonObject &, Tool::Done done) {
        QJsonObject info{{"kernel", QSysInfo::kernelVersion()},
                         {"os", QSysInfo::prettyProductName()},
                         {"host", QSysInfo::machineHostName()},
                         {"cpuThreads", QThread::idealThreadCount()},
                         {"model", m_llm->model()}};
        QFile mem("/proc/meminfo");
        if (mem.open(QIODevice::ReadOnly)) {
          for (const QByteArray &line : mem.readAll().split('\n')) {
            const auto parts = line.simplified().split(' ');
            if (parts.size() >= 2 && parts[0] == "MemTotal:")
              info.insert("memoryTotalMiB", parts[1].toLongLong() / 1024);
            if (parts.size() >= 2 && parts[0] == "MemAvailable:")
              info.insert("memoryAvailableMiB", parts[1].toLongLong() / 1024);
          }
        }
        QFile up("/proc/uptime");
        if (up.open(QIODevice::ReadOnly))
          info.insert("uptimeMinutes", int(up.readAll().split(' ').value(0).toDouble() / 60));
        info.insert("ok", true);
        done(info);
      }});
}
