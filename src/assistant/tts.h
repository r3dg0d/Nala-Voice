#pragma once
#include "speech.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QUrl>
#include <QVector>
#include <functional>

// Qwen3-TTS through an OpenAI-compatible speech server -- the API several
// community servers for Qwen3-TTS expose (POST /v1/audio/speech with
// {model, input, voice, response_format, stream}; with stream and
// response_format "pcm" the body is raw 16-bit mono PCM as it is generated,
// otherwise a WAV file). vLLM-Omni serves the same route. The sample rate of
// raw PCM is not in the stream, so it is a setting (Qwen3-TTS: 24 kHz).
class QwenTts : public TextToSpeech {
  Q_OBJECT

public:
  struct Config {
    QUrl endpoint;                 // e.g. http://127.0.0.1:8880
    QString model = QStringLiteral("tts-1");
    QString voice = QStringLiteral("alloy");
    int sampleRate = 24000;
    bool streaming = true;
    QString instruct;              // optional style prompt, sent only if set
    QString apiKey;
  };
  QwenTts(QNetworkAccessManager *network, QObject *parent = nullptr);
  QString name() const override { return QStringLiteral("qwen3-tts"); }
  void configure(const Config &config) { m_config = config; }
  void synthesize(const QString &text) override;
  void stop() override;

  // The request body, for tests and for the docs.
  QJsonObject requestBody(const QString &text) const;
  // Is a server there? Calls back with an empty string on success.
  void check(std::function<void(QString error)> done);

private:
  void readMore();

  QNetworkAccessManager *m_network;
  Config m_config;
  QPointer<QNetworkReply> m_reply;
  QByteArray m_header;
  bool m_formatSent = false;
  bool m_wav = false;
  int m_frameBytes = 2;
  QByteArray m_carry;
};

// Tries engines in order. A sentence that fails before any audio has played is
// retried on the next engine, so a fallback voice takes over without a gap in
// the reply; an engine that has failed is left alone for a while so every later
// sentence does not pay for the timeout again.
class TtsChain : public TextToSpeech {
  Q_OBJECT

public:
  explicit TtsChain(QObject *parent = nullptr) : TextToSpeech(parent) {}
  // Takes no ownership.
  void setEngines(const QVector<TextToSpeech *> &engines);
  QString name() const override;
  void synthesize(const QString &text) override;
  void stop() override;
  // Milliseconds an engine that failed is skipped for.
  void setCooldownMs(int ms) { m_cooldownMs = ms; }
  // The engine that spoke the last sentence, for `nala tts status`.
  QString lastEngine() const { return m_lastEngine; }
  // Which engines are currently being skipped.
  QStringList downEngines() const;

private:
  void tryFrom(int index);

  QVector<TextToSpeech *> m_engines;
  QVector<qint64> m_downUntil; // ms since epoch
  QString m_text;
  int m_current = -1;
  bool m_audioStarted = false;
  bool m_stopped = true;
  QString m_lastEngine;
  QStringList m_errors;
  int m_cooldownMs = 30000;
};
