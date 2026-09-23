#pragma once
#include "memory.h"
#include "policy.h"

#include <QDateTime>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QTimer>

class AssistantSettings;
class EventLog;
class LlmClient;

// Screen memory: the pipeline from "what is on screen" to a searchable
// episode.
//
//   paused? -> active window -> privacy gate -> capture -> still the same
//   window? -> downscale + hash -> duplicate? -> (vision check) -> keep
//
// The order is the point. Nothing is captured at all while paused, disabled
// or looking at an excluded window; a frame is held in memory until it has
// passed every check; and pausing bumps an epoch so a capture already in
// flight is thrown away when it lands rather than saved after the fact.
class ScreenMemory : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
  Q_PROPERTY(bool paused READ paused NOTIFY changed)
  Q_PROPERTY(bool recording READ recording NOTIFY changed)
  Q_PROPERTY(QDateTime pausedUntil READ pausedUntil NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  Q_PROPERTY(int count READ count NOTIFY stored)
  Q_PROPERTY(qint64 storageBytes READ storageBytes NOTIFY stored)

public:
  ScreenMemory(AssistantSettings *settings, MemoryStore *store, EventLog *log,
               LlmClient *llm, QObject *parent = nullptr);

  bool enabled() const;
  bool paused() const;
  // Actually capturing: enabled and not paused.
  bool recording() const { return enabled() && !paused(); }
  QDateTime pausedUntil() const;
  QString status() const;
  int count() const { return m_store->count(); }
  qint64 storageBytes() const { return m_store->storageBytes(); }

  // The killswitch. Takes effect before this returns: the setting is
  // written to disk and any capture in flight is invalidated. `minutes` 0
  // means until resumed by hand.
  Q_INVOKABLE void pause(int minutes = 0);
  Q_INVOKABLE void resume();
  Q_INVOKABLE void setEnabled(bool enabled);

  // Retention, now rather than on the timer.
  MemoryStore::Sweep sweep();

  // Test seam: run a frame through the pipeline from the privacy gate on, as
  // if it had just been captured from `window`.
  void ingest(const QImage &frame, const WindowInfo &window,
              const QDateTime &now = QDateTime::currentDateTime());
  // Test seam: stop the capture timer and the helpers from running.
  void setOffline(bool offline);

  // Why the last frame was not kept, for the developer page. Never the title.
  QString lastSkip() const { return m_lastSkip; }

signals:
  void changed();
  void stored(qint64 id);

private:
  void tick();
  void consider(const QImage &frame, const WindowInfo &window, int epoch,
                const QDateTime &now);
  void keep(const QByteArray &jpeg, quint64 hash, const WindowInfo &window,
            int epoch, const QDateTime &now);
  void describeNext();
  void skip(const QString &why);
  void schedule();

  AssistantSettings *m_settings;
  MemoryStore *m_store;
  EventLog *m_log;
  LlmClient *m_llm;
  QTimer m_capture;
  QTimer m_retention;
  QTimer m_resumeAt;
  int m_epoch = 0;
  bool m_busy = false;
  bool m_offline = false;
  int m_sinceSweep = 0;
  QString m_lastSkip;

  // The last kept moment, for folding duplicates into it.
  qint64 m_lastId = 0;
  quint64 m_lastHash = 0;
  QString m_lastWindow;
  QString m_lastTitle;

  QQueue<qint64> m_toDescribe;
  bool m_describing = false;
  qint64 m_describingId = 0;
  // Owns the handlers of the description in flight, so a privacy judgement
  // can take the model over without inheriting them.
  QPointer<QObject> m_describeHandlers;
};
