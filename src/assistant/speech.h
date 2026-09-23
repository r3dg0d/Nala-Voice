#pragma once
#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTemporaryDir;

// --- speech to text ----------------------------------------------------------

// Turns one utterance into text. Implementations never block the caller and
// report back through signals; at most one transcription is in flight, and
// starting another cancels the first.
class SpeechToText : public QObject {
  Q_OBJECT

public:
  using QObject::QObject;
  virtual QString name() const = 0;
  // `pcm16k` is 16 kHz mono s16. `language` is an ISO code or "auto".
  virtual void transcribe(const QByteArray &pcm16k, const QString &language) = 0;
  virtual void cancel() = 0;
  // Words to bias recognition towards. Whisper otherwise hears "Nala" as
  // "Arlo" or "Artler", and "pause" as "poor".
  void setPrompt(const QString &prompt) { m_prompt = prompt; }

signals:
  void transcribed(const QString &text, qint64 milliseconds);
  void failed(const QString &reason);

protected:
  QString m_prompt;
};

// whisper.cpp's `whisper-server`, which keeps the model loaded between
// utterances and so answers fastest.
class WhisperServer : public SpeechToText {
  Q_OBJECT

public:
  WhisperServer(QNetworkAccessManager *network, QObject *parent = nullptr);
  QString name() const override { return QStringLiteral("whisper-server"); }
  void setUrl(const QUrl &url) { m_url = url; }
  void transcribe(const QByteArray &pcm16k, const QString &language) override;
  void cancel() override;

private:
  QNetworkAccessManager *m_network;
  QUrl m_url;
  QPointer<QNetworkReply> m_reply;
  QElapsedTimer m_clock;
};

// whisper.cpp's `whisper-cli`, run once per utterance. Loads the model every
// time, so it is slower, but needs nothing running in the background.
class WhisperCli : public SpeechToText {
  Q_OBJECT

public:
  explicit WhisperCli(QObject *parent = nullptr);
  ~WhisperCli() override;
  QString name() const override { return QStringLiteral("whisper-cli"); }
  void configure(const QString &binary, const QString &model);
  // Off: whisper.cpp runs on the CPU and leaves the GPU to the model.
  void setGpu(bool gpu) { m_gpu = gpu; }
  void transcribe(const QByteArray &pcm16k, const QString &language) override;
  void cancel() override;

  // Where a model is looked for when none is configured.
  static QString defaultModel();

private:
  QString m_binary;
  QString m_model;
  bool m_gpu = true;
  QPointer<QProcess> m_process;
  QTemporaryDir *m_dir = nullptr;
  QElapsedTimer m_clock;
};

// --- text to speech ----------------------------------------------------------

// Turns text into audio, streamed as it is produced. Implementations emit a
// format once, then PCM chunks, then done(); or failed().
class TextToSpeech : public QObject {
  Q_OBJECT

public:
  using QObject::QObject;
  virtual QString name() const = 0;
  virtual void synthesize(const QString &text) = 0;
  virtual void stop() = 0;

signals:
  void format(int sampleRate, int channels, int bitsPerSample);
  void audio(const QByteArray &pcm);
  void done();
  void failed(const QString &reason);
};

// Fish Speech's local API server (`POST /v1/tts`).
class FishSpeech : public TextToSpeech {
  Q_OBJECT

public:
  FishSpeech(QNetworkAccessManager *network, QObject *parent = nullptr);
  QString name() const override { return QStringLiteral("fish-speech"); }
  void configure(const QUrl &endpoint, const QString &referenceId,
                 bool streaming, const QString &stylePrefix);
  void synthesize(const QString &text) override;
  void stop() override;

private:
  void readMore();

  QNetworkAccessManager *m_network;
  QUrl m_endpoint;
  QString m_referenceId;
  QString m_stylePrefix;
  bool m_streaming = true;
  QPointer<QNetworkReply> m_reply;
  QByteArray m_header; // bytes held back until the WAV header is complete
  bool m_formatSent = false;
  int m_frameBytes = 2;
  QByteArray m_carry; // a split sample waiting for its other half
};

// Split a reply into sentences, so the first can be spoken while the rest is
// still being synthesised.
QStringList splitSentences(const QString &text, int maxChars = 220);
