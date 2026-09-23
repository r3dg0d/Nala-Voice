#pragma once
#include <QJsonObject>
#include <QObject>
#include <QVariantList>

// A structured record of what the assistant did: state changes, speech, model
// requests, tool calls, memory writes and deletions, privacy switches, errors.
//
// Written as JSON lines to $XDG_STATE_HOME/nala/assistant.log, capped in size,
// and kept in a short ring for the developer page. Everything passes through
// redact() on the way in, so a password spoken aloud or an API key in a tool
// argument is not preserved by the log that was meant to make things
// accountable.
class EventLog : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList recent READ recent NOTIFY appended)
  Q_PROPERTY(bool debug READ debug WRITE setDebug NOTIFY debugChanged)

public:
  explicit EventLog(QString path, QObject *parent = nullptr);

  // `category` is short and fixed ("state", "stt", "llm", "tool", "tts",
  // "memory", "privacy", "error"); `fields` carries the detail.
  void record(const QString &category, const QString &event,
              const QJsonObject &fields = {});
  // Only kept when debug is on: request bodies, raw transcripts and the like.
  void trace(const QString &category, const QString &event,
             const QJsonObject &fields = {});

  QVariantList recent() const;
  bool debug() const { return m_debug; }
  void setDebug(bool debug);

  // Blank out anything that looks like a credential.
  static QString redact(const QString &text);
  static QJsonObject redact(const QJsonObject &object);

signals:
  void appended();
  void debugChanged();

private:
  void write(const QJsonObject &entry);

  QString m_path;
  bool m_debug = false;
  QList<QJsonObject> m_ring;
};
