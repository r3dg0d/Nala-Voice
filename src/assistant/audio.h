#pragma once
#include "audioutil.h"

#include <QAudioFormat>
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <memory>

class QAudioSink;
class QAudioSource;
class QIODevice;

// The microphone, with voice-activity detection on the way in. Emits whole
// utterances as 16 kHz mono PCM, which is what the recogniser wants.
class Microphone : public QObject {
  Q_OBJECT

public:
  explicit Microphone(QObject *parent = nullptr);
  ~Microphone() override;

  static QStringList devices();

  // `device` is a description from devices(); empty means the default.
  bool start(const QString &device, const audio::VoiceActivity::Config &vad);
  void stop();
  bool active() const { return m_source != nullptr; }
  double level() const { return m_level; }

  // Feed samples as if they came from the device. For tests.
  void inject(const QVector<int16_t> &samples16k);

signals:
  void speechStarted();
  void utterance(const QByteArray &pcm16k);
  void levelChanged(double level);
  void failed(const QString &reason);

private:
  void read();
  void process(const QVector<int16_t> &mono);

  std::unique_ptr<QAudioSource> m_source;
  QPointer<QIODevice> m_io;
  QAudioFormat m_format;
  audio::VoiceActivity m_vad;
  double m_level = 0.0;
};

// Plays PCM as it arrives, so speech can start before synthesis has finished,
// and stops dead when told to.
class Speaker : public QObject {
  Q_OBJECT

public:
  explicit Speaker(QObject *parent = nullptr);
  ~Speaker() override;

  static QStringList devices();

  // Begin a stream. Call append() as bytes arrive, then finish().
  bool begin(int sampleRate, int channels, int bitsPerSample,
             const QString &device, double volume);
  void append(const QByteArray &pcm);
  void finish();
  void stop();
  bool playing() const { return m_sink != nullptr; }
  double level() const { return m_level; }

signals:
  void started();
  void finished();
  void levelChanged(double level);
  void failed(const QString &reason);

private:
  void pump();
  void end(bool emitFinished);

  std::unique_ptr<QAudioSink> m_sink;
  QPointer<QIODevice> m_io;
  QAudioFormat m_format;
  QByteArray m_pending;
  bool m_inputDone = false;
  bool m_announced = false;
  double m_level = 0.0;
  QTimer m_pump;
};
