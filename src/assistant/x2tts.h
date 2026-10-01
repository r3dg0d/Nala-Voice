#pragma once
#include "speech.h"
#include <QJsonObject>
#include <QTimer>
class QWebSocket;

// Native Qwen3TTS-Streaming gateway protocol with X2 policy extensions on the
// server. One logical session per utterance; acoustic state stays server-side.
class X2Tts : public TextToSpeech {
  Q_OBJECT
public:
  struct Config {
    QUrl endpoint = QUrl(QStringLiteral("ws://127.0.0.1:50052/v1/ws"));
    QString voice = QStringLiteral("robot_service_v1");
    bool reuse = true;
    QString inputMode = QStringLiteral("token");
  };
  explicit X2Tts(QObject *parent = nullptr);
  QString name() const override { return QStringLiteral("x2streaming"); }
  void configure(Config config);
  bool incremental() const override { return true; }
  void synthesize(const QString &text) override;
  void beginStream() override;
  void pushText(const QString &text) override;
  void finishStream() override;
  void stop() override;
  void prewarm(); // opens transport only; engine prewarming is a server concern
  static QUrl healthUrl(QUrl endpoint);
signals:
  void sessionStarted();
  void diagnostic(const QString &state);
  void segmentFinished(bool stateInherited);

private:
  void connectSocket();
  void sendStart();
  void send(const QJsonObject &message);
  void fail(const QString &reason);
  Config m_config;
  QWebSocket *m_socket = nullptr;
  QTimer m_timeout;
  QString m_session;
  QStringList m_pending;
  QByteArray m_carry;
  bool m_active = false, m_started = false, m_ended = false, m_format = false;
  bool m_received = false;
  int m_seq = 0;
};
