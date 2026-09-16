#include "activity.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

namespace {

// Hysteresis, in load per core: she notices the machine getting busy well
// before she decides it has gone quiet again, so a load hovering near the
// threshold does not flicker her in and out of thinking.
constexpr qreal kBusyAbove = 0.75;
constexpr qreal kIdleBelow = 0.45;

} // namespace

Activity::Activity(QString stateRoot, QObject *parent) : QObject(parent) {
  m_cores = std::max(1, QThread::idealThreadCount());

  const QString history = stateRoot + "/noctalia/notification_history.json";
  if (QFileInfo::exists(history))
    m_historyPath = history;

  m_loadTimer.setInterval(2000);
  connect(&m_loadTimer, &QTimer::timeout, this, &Activity::pollLoad);

  // The daemon rewrites the file wholesale, so collapse the burst.
  m_debounce.setSingleShot(true);
  m_debounce.setInterval(120);
  connect(&m_debounce, &QTimer::timeout, this, &Activity::readHistory);
  connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] {
    // A replaced file drops the watch, so put it back.
    if (!m_watcher.files().contains(m_historyPath) &&
        QFileInfo::exists(m_historyPath))
      m_watcher.addPath(m_historyPath);
    m_debounce.start();
  });
}

void Activity::setActive(bool active) {
  if (m_active == active)
    return;
  m_active = active;

  if (active) {
    m_loadTimer.start();
    pollLoad();
    if (!m_historyPath.isEmpty()) {
      m_watcher.addPath(m_historyPath);
      readHistory(); // establish the baseline without announcing it
    }
  } else {
    m_loadTimer.stop();
    if (!m_watcher.files().isEmpty())
      m_watcher.removePaths(m_watcher.files());
  }
}

void Activity::pollLoad() {
  QFile file(QStringLiteral("/proc/loadavg"));
  if (!file.open(QIODevice::ReadOnly))
    return;
  const QString text = QString::fromLatin1(file.readLine());
  bool ok = false;
  const qreal load = text.section(u' ', 0, 0).toDouble(&ok);
  if (ok)
    injectLoad(load / m_cores);
}

void Activity::injectLoad(qreal perCore) {
  const bool wasBusy = m_busy;
  if (!m_busy && perCore > kBusyAbove)
    m_busy = true;
  else if (m_busy && perCore < kIdleBelow)
    m_busy = false;
  if (m_busy != wasBusy)
    emit busyChanged(m_busy);
}

void Activity::readHistory() {
  QFile file(m_historyPath);
  if (!file.open(QIODevice::ReadOnly))
    return;
  const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();

  // Every entry carries a serial that only ever climbs; the highest one is
  // the most recent thing that happened.
  qint64 newest = 0;
  for (const QJsonValue &value : root.value("entries").toArray())
    newest = std::max<qint64>(
        newest, value.toObject().value("event_serial").toInteger());

  const bool firstLook = m_lastSerial < 0;
  const bool arrived = !firstLook && newest > m_lastSerial;
  m_lastSerial = newest;
  if (arrived)
    emit notified();
}

void Activity::injectNotification() { emit notified(); }
