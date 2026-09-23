#include "screenmemory.h"
#include "desktop.h"
#include "eventlog.h"
#include "llm.h"
#include "settings.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtConcurrent/QtConcurrentRun>

namespace {

struct Processed {
  QByteArray jpeg;
  quint64 hash = 0;
};

// Off the UI thread: scaling and encoding a 4K frame is tens of milliseconds.
Processed process(const QImage &frame, int maxWidth, int quality) {
  Processed out;
  const QImage scaled =
      frame.width() > maxWidth
          ? frame.scaledToWidth(maxWidth, Qt::SmoothTransformation)
          : frame;
  out.hash = differenceHash(scaled);
  QBuffer buffer(&out.jpeg);
  buffer.open(QIODevice::WriteOnly);
  scaled.save(&buffer, "JPEG", quality);
  return out;
}

QJsonObject visionMessage(const QString &prompt, const QByteArray &jpeg) {
  return QJsonObject{
      {"role", "user"},
      {"content",
       QJsonArray{QJsonObject{{"type", "text"}, {"text", prompt}},
                  QJsonObject{{"type", "image_url"},
                              {"image_url",
                               QJsonObject{{"url", "data:image/jpeg;base64," +
                                                       QString::fromLatin1(
                                                           jpeg.toBase64())}}}}}}};
}

const char *kDescribePrompt =
    "This is a screenshot of one window on the user's desktop. Reply with "
    "only a JSON object: {\"activity\": one or two words such as coding, "
    "browsing, chatting, writing, gaming, watching; \"summary\": one factual "
    "sentence about what the user is doing, naming the project, document, "
    "site or topic; \"keywords\": up to eight search keywords; \"urls\": any "
    "URLs or file paths clearly visible}. Never transcribe passwords, codes, "
    "personal messages or anything that looks private.";

const char *kJudgePrompt =
    "Does this screenshot show any of: a password or login form, "
    "authentication codes, banking or payment details, private browsing, or "
    "sexual or nude content? Reply with exactly one word: SENSITIVE or SAFE.";

} // namespace

ScreenMemory::ScreenMemory(AssistantSettings *settings, MemoryStore *store,
                           EventLog *log, LlmClient *llm, QObject *parent)
    : QObject(parent), m_settings(settings), m_store(store), m_log(log),
      m_llm(llm) {
  connect(&m_capture, &QTimer::timeout, this, &ScreenMemory::tick);
  m_retention.setInterval(30 * 60 * 1000);
  connect(&m_retention, &QTimer::timeout, this, [this] { sweep(); });
  m_resumeAt.setSingleShot(true);
  connect(&m_resumeAt, &QTimer::timeout, this, [this] {
    // Only a pause the user gave an end to comes back on its own.
    if (paused() && pausedUntil().isValid() &&
        pausedUntil() <= QDateTime::currentDateTime().addSecs(1))
      resume();
  });
  connect(m_settings, &AssistantSettings::changed, this,
          [this](const QString &key) {
            if (key.startsWith("memory."))
              schedule();
          });
  schedule();
  m_retention.start();
}

bool ScreenMemory::enabled() const {
  return m_settings->flag("memory.enabled");
}

bool ScreenMemory::paused() const { return m_settings->flag("memory.paused"); }

QDateTime ScreenMemory::pausedUntil() const {
  const qint64 until = qint64(m_settings->number("memory.pausedUntil"));
  return until > 0 ? QDateTime::fromMSecsSinceEpoch(until) : QDateTime();
}

QString ScreenMemory::status() const {
  if (!enabled())
    return QStringLiteral("off");
  if (paused())
    return pausedUntil().isValid()
               ? QStringLiteral("paused until %1")
                     .arg(pausedUntil().toString("h:mm AP"))
               : QStringLiteral("paused");
  return QStringLiteral("recording");
}

void ScreenMemory::schedule() {
  // A pause with an end time survives a restart; so does one without.
  if (paused() && pausedUntil().isValid()) {
    const qint64 left =
        QDateTime::currentDateTime().msecsTo(pausedUntil());
    m_resumeAt.start(int(std::clamp<qint64>(left, 0, 24LL * 3600 * 1000)));
  } else {
    m_resumeAt.stop();
  }
  if (recording() && !m_offline) {
    const int interval = m_settings->integer("memory.intervalSec") * 1000;
    if (!m_capture.isActive() || m_capture.interval() != interval)
      m_capture.start(interval);
  } else {
    m_capture.stop();
  }
  emit changed();
}

void ScreenMemory::pause(int minutes) {
  ++m_epoch; // anything in flight is now stale
  m_capture.stop();
  m_settings->set("memory.paused", true);
  m_settings->set("memory.pausedUntil",
                  minutes > 0 ? double(QDateTime::currentDateTime()
                                           .addSecs(qint64(minutes) * 60)
                                           .toMSecsSinceEpoch())
                              : 0.0);
  m_settings->flush();
  m_log->record("privacy", "memory-paused", {{"minutes", minutes}});
  schedule();
}

void ScreenMemory::resume() {
  m_settings->set("memory.paused", false);
  m_settings->set("memory.pausedUntil", 0.0);
  m_settings->flush();
  m_log->record("privacy", "memory-resumed");
  schedule();
}

void ScreenMemory::setEnabled(bool on) {
  if (!on)
    ++m_epoch;
  m_settings->set("memory.enabled", on);
  m_settings->flush();
  m_log->record("privacy", on ? "memory-enabled" : "memory-disabled");
  schedule();
}

void ScreenMemory::setOffline(bool offline) {
  m_offline = offline;
  if (offline) {
    m_capture.stop();
    m_retention.stop();
  }
  schedule();
}

void ScreenMemory::skip(const QString &why) {
  m_lastSkip = why;
  m_log->trace("memory", "frame-skipped", {{"reason", why}});
}

void ScreenMemory::tick() {
  if (!recording() || m_busy)
    return;
  const WindowInfo window = desktop::activeWindow();
  const PrivacyCheck check = privacyGate(
      window, m_settings->list("privacy.excludedApps"),
      m_settings->list("privacy.excludedTitles"),
      m_settings->flag("privacy.blockSensitive"),
      m_settings->flag("memory.requireWindowInfo"));
  if (!check.allowed) {
    // Not captured at all: the gate runs before the screen is read.
    skip(check.reason);
    return;
  }
  const bool wholeOutput = m_settings->string("memory.scope") == "monitor" ||
                           !window.valid;
  if (!wholeOutput && (window.geometry.width() < 16 ||
                       window.geometry.height() < 16)) {
    skip(QStringLiteral("window too small"));
    return;
  }
  m_busy = true;
  const int epoch = m_epoch;
  desktop::capture(wholeOutput ? QRect() : window.geometry,
                   wholeOutput ? window.monitorName : QString(),
                   [this, window, epoch](QImage frame, QString error) {
                     if (frame.isNull()) {
                       m_busy = false;
                       skip(error);
                       return;
                     }
                     // Focus may have moved to something excluded while
                     // grim was working. If so, this frame is not the window
                     // that was checked.
                     const WindowInfo now = desktop::activeWindow();
                     if (now.address != window.address ||
                         now.title != window.title) {
                       m_busy = false;
                       skip(QStringLiteral("focus changed during capture"));
                       return;
                     }
                     consider(frame, window, epoch,
                              QDateTime::currentDateTime());
                   },
                   this);
}

void ScreenMemory::ingest(const QImage &frame, const WindowInfo &window,
                          const QDateTime &now) {
  if (!recording()) {
    skip(QStringLiteral("not recording"));
    return;
  }
  const PrivacyCheck check = privacyGate(
      window, m_settings->list("privacy.excludedApps"),
      m_settings->list("privacy.excludedTitles"),
      m_settings->flag("privacy.blockSensitive"),
      m_settings->flag("memory.requireWindowInfo"));
  if (!check.allowed) {
    skip(check.reason);
    return;
  }
  m_busy = true;
  consider(frame, window, m_epoch, now);
}

void ScreenMemory::consider(const QImage &frame, const WindowInfo &window,
                            int epoch, const QDateTime &now) {
  const int maxWidth = m_settings->integer("memory.maxWidth");
  const int quality = m_settings->integer("memory.jpegQuality");
  auto *watcher = new QFutureWatcher<Processed>(this);
  connect(watcher, &QFutureWatcher<Processed>::finished, this,
          [this, watcher, window, epoch, now] {
            watcher->deleteLater();
            const Processed result = watcher->result();
            if (epoch != m_epoch || !recording()) {
              m_busy = false;
              skip(QStringLiteral("paused while processing"));
              return;
            }
            // The same window showing much the same thing: fold it in.
            if (m_settings->flag("memory.dedupe") && m_lastId != 0 &&
                m_lastWindow == window.address && m_lastTitle == window.title &&
                hammingDistance(result.hash, m_lastHash) <=
                    m_settings->integer("memory.dedupeDistance")) {
              m_store->touch(m_lastId, now);
              m_busy = false;
              skip(QStringLiteral("duplicate"));
              return;
            }
            const bool judge = m_settings->flag("privacy.visionFilter") &&
                               vision() &&
                               m_settings->flag("llm.enabled");
            if (!judge) {
              keep(result.jpeg, result.hash, window, epoch, now);
              return;
            }
            // Ask the model whether this is something that should never be
            // kept. The frame stays in memory meanwhile, and anything short of
            // a clear SAFE drops it.
            auto *client = m_llm;
            if (m_describeHandlers) {
              // Judging comes first; the description can wait its turn.
              delete m_describeHandlers;
              m_describing = false;
              m_toDescribe.prepend(m_describingId);
            }
            QObject *once = new QObject(this);
            connect(client, &LlmClient::replied, once,
                    [this, once, result, window, epoch, now](const LlmReply &r) {
                      once->deleteLater();
                      const bool safe = r.content.trimmed().toUpper().startsWith(
                          QStringLiteral("SAFE"));
                      if (!safe || epoch != m_epoch || !recording()) {
                        m_busy = false;
                        skip(safe ? QStringLiteral("paused while judging")
                                  : QStringLiteral("judged sensitive"));
                        describeNext();
                        return;
                      }
                      keep(result.jpeg, result.hash, window, epoch, now);
                    });
            connect(client, &LlmClient::failed, once, [this, once] {
              once->deleteLater();
              m_busy = false;
              skip(QStringLiteral("could not be judged"));
              describeNext();
            });
            client->chat(QJsonArray{
                visionMessage(QString::fromLatin1(kJudgePrompt), result.jpeg)});
          });
  watcher->setFuture(QtConcurrent::run(process, frame, maxWidth, quality));
}

void ScreenMemory::keep(const QByteArray &jpeg, quint64 hash,
                        const WindowInfo &window, int epoch,
                        const QDateTime &now) {
  m_busy = false;
  if (epoch != m_epoch || !recording()) {
    skip(QStringLiteral("paused before saving"));
    return;
  }
  const QString dayDir = m_store->shotsDir() + "/" + now.toString("yyyy-MM-dd");
  QDir().mkpath(dayDir);
  QFile::setPermissions(dayDir, QFileDevice::ReadOwner |
                                    QFileDevice::WriteOwner |
                                    QFileDevice::ExeOwner);
  const QString path =
      QStringLiteral("%1/%2-%3.jpg")
          .arg(dayDir)
          .arg(now.toMSecsSinceEpoch())
          .arg(hash, 16, 16, QChar('0'));
  QFile file(path);
  // Created private, before a single byte is written.
  if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
      !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
      file.write(jpeg) != jpeg.size()) {
    file.remove();
    skip(QStringLiteral("could not write"));
    return;
  }
  file.close();

  MemoryRecord record;
  record.started = now;
  record.lastSeen = now;
  record.app = window.appClass;
  record.title = window.title.left(500);
  record.shotPath = path;
  record.shotBytes = jpeg.size();
  record.hash = hash;
  const qint64 id = m_store->insert(record);
  if (id == 0) {
    QFile::remove(path);
    skip(QStringLiteral("database refused it"));
    return;
  }
  for (Artifact artifact : findArtifacts(window.title)) {
    artifact.title = window.title.left(300);
    artifact.lastSeen = now;
    artifact.memoryId = id;
    m_store->addArtifact(artifact);
  }
  m_lastId = id;
  m_lastHash = hash;
  m_lastWindow = window.address;
  m_lastTitle = window.title;
  m_lastSkip.clear();
  m_log->record("memory", "stored",
                {{"id", id}, {"app", window.appClass}, {"bytes", jpeg.size()}});
  emit stored(id);

  if (m_settings->flag("memory.describe") && vision() &&
      m_settings->flag("llm.enabled")) {
    if (m_toDescribe.size() >= 20)
      m_toDescribe.dequeue(); // behind: describe the recent ones
    m_toDescribe.enqueue(id);
    describeNext();
  }
  if (++m_sinceSweep >= 20) {
    m_sinceSweep = 0;
    sweep();
  }
}

void ScreenMemory::describeNext() {
  if (m_describing || m_busy || m_toDescribe.isEmpty() || m_llm->busy())
    return;
  const MemoryRecord record = m_store->get(m_toDescribe.dequeue());
  if (record.id == 0 || record.shotPath.isEmpty()) {
    describeNext();
    return;
  }
  QFile file(record.shotPath);
  if (!file.open(QIODevice::ReadOnly))
    return;
  m_describing = true;
  const qint64 id = record.id;
  m_describingId = id;
  QObject *once = new QObject(this);
  m_describeHandlers = once;
  connect(m_llm, &LlmClient::replied, once, [this, once, id](const LlmReply &r) {
    once->deleteLater();
    m_describing = false;
    QString text = r.content.trimmed();
    // Tolerate a fenced reply.
    const int open = text.indexOf('{'), close = text.lastIndexOf('}');
    if (open >= 0 && close > open)
      text = text.mid(open, close - open + 1);
    const QJsonObject json = QJsonDocument::fromJson(text.toUtf8()).object();
    if (!json.isEmpty() && m_store->get(id).id != 0) {
      QStringList keywords, urls;
      for (const QJsonValue &v : json.value("keywords").toArray())
        keywords << v.toString();
      for (const QJsonValue &v : json.value("urls").toArray())
        urls << v.toString();
      const QString summary =
          EventLog::redact(json.value("summary").toString());
      m_store->describe(id, json.value("activity").toString(), summary,
                        keywords.join(' '), urls.join('\n'));
      for (Artifact artifact : findArtifacts(summary + "\n" + urls.join('\n'))) {
        artifact.memoryId = id;
        m_store->addArtifact(artifact);
      }
      m_log->record("memory", "described", {{"id", id}});
    }
    describeNext();
  });
  connect(m_llm, &LlmClient::failed, once, [this, once](const QString &why) {
    once->deleteLater();
    m_describing = false;
    m_log->record("memory", "describe-failed", {{"reason", why}});
  });
  m_llm->chat(QJsonArray{
      visionMessage(QString::fromLatin1(kDescribePrompt), file.readAll())});
}

MemoryStore::Sweep ScreenMemory::sweep() {
  const MemoryStore::Sweep result = m_store->enforce(
      m_settings->integer("memory.screenshotDays"),
      m_settings->integer("memory.semanticDays"),
      qint64(m_settings->integer("memory.maxStorageMB")) * 1024 * 1024,
      QDateTime::currentDateTime());
  if (result.screenshots || result.rows)
    m_log->record("memory", "retention",
                  {{"screenshots", result.screenshots},
                   {"rows", result.rows},
                   {"bytes", result.bytes}});
  if (result.screenshots || result.rows)
    emit stored(0);
  return result;
}
