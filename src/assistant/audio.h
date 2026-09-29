#pragma once
#include "audioutil.h"

#include <QAudioFormat>
#include <QMediaDevices>
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
  // True while the device has gone away and we are waiting to reopen it.
  bool reconnecting() const { return m_wantOpen && !m_source; }
  double level() const { return m_level; }

  // Delay before reconnect attempt `attempt` (0-based): 1 s doubling to 15 s.
  static int retryDelayMs(int attempt);

  // Feed samples as if they came from the device. For tests.
  void inject(const QVector<int16_t> &samples16k);
  // Behave as if the device died under us. For tests.
  void simulateLostForTest(const QString &reason) {
    m_wantOpen = true;
    lose(reason);
  }

signals:
  // Every block of 16 kHz mono audio as it arrives, for the wake-word
  // detector. Not kept anywhere.
  void frames(const QVector<int16_t> &samples16k);
  void speechStarted();
  void utterance(const QByteArray &pcm16k);
  void levelChanged(double level);
  void failed(const QString &reason);
  // The device vanished or errored mid-session; we keep retrying quietly.
  void lost(const QString &reason);
  // A lost microphone is open again.
  void recovered();

private:
  bool open(const QString &device, const audio::VoiceActivity::Config &vad,
            bool announceFailure);
  void read();
  void process(const QVector<int16_t> &mono);
  void lose(const QString &reason);
  void retry();
  void onDevicesChanged();

  std::unique_ptr<QAudioSource> m_source;
  QMediaDevices m_devices;
  QTimer m_retry;
  QString m_device;
  QByteArray m_openId; // id of the device actually opened
  audio::VoiceActivity::Config m_vadConfig;
  bool m_wantOpen = false; // start() was called and stop() has not been
  bool m_wasLost = false;
  int m_attempt = 0;
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
