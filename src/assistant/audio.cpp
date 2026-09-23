#include "audio.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioSource>
#include <QMediaDevices>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

QAudioDevice pick(const QList<QAudioDevice> &all, const QAudioDevice &fallback,
                  const QString &wanted) {
  if (!wanted.isEmpty())
    for (const QAudioDevice &device : all)
      if (device.description() == wanted || device.id() == wanted.toUtf8())
        return device;
  return fallback;
}

} // namespace

// --- microphone ------------------------------------------------------------

Microphone::Microphone(QObject *parent) : QObject(parent) {}
Microphone::~Microphone() { stop(); }

QStringList Microphone::devices() {
  QStringList names;
  for (const QAudioDevice &device : QMediaDevices::audioInputs())
    names << device.description();
  return names;
}

bool Microphone::start(const QString &device,
                       const audio::VoiceActivity::Config &vad) {
  stop();
  const QAudioDevice input = pick(QMediaDevices::audioInputs(),
                                  QMediaDevices::defaultAudioInput(), device);
  if (input.isNull()) {
    emit failed(QStringLiteral("No microphone found."));
    return false;
  }

  // Ask for what the recogniser wants, and convert whatever we get instead.
  QAudioFormat format;
  format.setSampleRate(audio::kSttRate);
  format.setChannelCount(1);
  format.setSampleFormat(QAudioFormat::Int16);
  if (!input.isFormatSupported(format))
    format = input.preferredFormat();
  if (format.sampleFormat() != QAudioFormat::Int16 &&
      format.sampleFormat() != QAudioFormat::Float) {
    format.setSampleFormat(QAudioFormat::Int16);
    if (!input.isFormatSupported(format)) {
      emit failed(QStringLiteral("The microphone offers no usable format."));
      return false;
    }
  }

  m_format = format;
  m_vad = audio::VoiceActivity(vad);
  m_source = std::make_unique<QAudioSource>(input, format);
  m_source->setBufferSize(format.bytesForDuration(200000));
  m_io = m_source->start();
  if (!m_io) {
    m_source.reset();
    emit failed(QStringLiteral("Could not open the microphone."));
    return false;
  }
  connect(m_io, &QIODevice::readyRead, this, &Microphone::read);
  return true;
}

void Microphone::stop() {
  if (m_source) {
    m_source->stop();
    m_source.reset();
  }
  m_io = nullptr;
  m_vad.reset();
  if (m_level != 0.0) {
    m_level = 0.0;
    emit levelChanged(0.0);
  }
}

void Microphone::read() {
  if (!m_io)
    return;
  const QByteArray bytes = m_io->readAll();
  const int channels = std::max(1, m_format.channelCount());
  QVector<int16_t> interleaved;
  if (m_format.sampleFormat() == QAudioFormat::Float) {
    const int count = int(bytes.size() / sizeof(float));
    interleaved.resize(count);
    const auto *in = reinterpret_cast<const float *>(bytes.constData());
    for (int i = 0; i < count; ++i)
      interleaved[i] = int16_t(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f);
  } else {
    interleaved.resize(int(bytes.size() / sizeof(int16_t)));
    std::memcpy(interleaved.data(), bytes.constData(),
                interleaved.size() * sizeof(int16_t));
  }
  const int frames = int(interleaved.size()) / channels;
  if (frames <= 0)
    return;
  if (channels == 1 && m_format.sampleRate() == audio::kSttRate)
    process(interleaved);
  else
    process(audio::toMono16k(interleaved.constData(), frames, channels,
                             m_format.sampleRate()));
}

void Microphone::inject(const QVector<int16_t> &samples16k) {
  process(samples16k);
}

void Microphone::process(const QVector<int16_t> &mono) {
  const auto event = m_vad.feed(mono.constData(), int(mono.size()));
  // A 0..1 meter: -60 dBFS reads as silence, -10 as shouting.
  const double level =
      std::clamp((m_vad.levelDb() + 60.0) / 50.0, 0.0, 1.0);
  if (std::abs(level - m_level) > 0.02) {
    m_level = level;
    emit levelChanged(level);
  }
  if (event == audio::VoiceActivity::Started)
    emit speechStarted();
  else if (event == audio::VoiceActivity::Ended) {
    const QVector<int16_t> samples = m_vad.utterance();
    emit utterance(QByteArray(reinterpret_cast<const char *>(samples.data()),
                              samples.size() * sizeof(int16_t)));
  }
}

// --- speaker ---------------------------------------------------------------

Speaker::Speaker(QObject *parent) : QObject(parent) {
  m_pump.setInterval(15);
  connect(&m_pump, &QTimer::timeout, this, &Speaker::pump);
}
Speaker::~Speaker() { end(false); }

QStringList Speaker::devices() {
  QStringList names;
  for (const QAudioDevice &device : QMediaDevices::audioOutputs())
    names << device.description();
  return names;
}

bool Speaker::begin(int sampleRate, int channels, int bitsPerSample,
                    const QString &device, double volume) {
  end(false);
  const QAudioDevice output = pick(QMediaDevices::audioOutputs(),
                                   QMediaDevices::defaultAudioOutput(), device);
  if (output.isNull()) {
    emit failed(QStringLiteral("No audio output found."));
    return false;
  }
  QAudioFormat format;
  format.setSampleRate(sampleRate);
  format.setChannelCount(channels);
  format.setSampleFormat(bitsPerSample == 32   ? QAudioFormat::Int32
                         : bitsPerSample == 8 ? QAudioFormat::UInt8
                                              : QAudioFormat::Int16);
  m_format = format;
  m_sink = std::make_unique<QAudioSink>(output, format);
  m_sink->setVolume(volume);
  m_sink->setBufferSize(format.bytesForDuration(120000));
  m_io = m_sink->start();
  if (!m_io) {
    m_sink.reset();
    emit failed(QStringLiteral("Could not open the audio output."));
    return false;
  }
  m_pending.clear();
  m_inputDone = false;
  m_announced = false;
  m_pump.start();
  return true;
}

void Speaker::append(const QByteArray &pcm) {
  if (m_sink)
    m_pending.append(pcm);
}

void Speaker::finish() { m_inputDone = true; }

void Speaker::stop() { end(true); }

void Speaker::pump() {
  if (!m_sink || !m_io)
    return;
  const int frameBytes = std::max(1, m_format.bytesPerFrame());
  qsizetype room = m_sink->bytesFree();
  room -= room % frameBytes;
  if (room > 0 && !m_pending.isEmpty()) {
    const qsizetype take = std::min(room, m_pending.size() -
                                              m_pending.size() % frameBytes);
    if (take > 0) {
      const QByteArray chunk = m_pending.left(take);
      m_io->write(chunk);
      m_pending.remove(0, take);
      if (!m_announced) {
        m_announced = true;
        emit started();
      }
      if (m_format.sampleFormat() == QAudioFormat::Int16) {
        const double level = audio::rms(
            reinterpret_cast<const int16_t *>(chunk.constData()),
            int(chunk.size() / 2));
        // Speech sits well below full scale; stretch it into a usable 0..1.
        m_level = std::clamp(level * 4.0, 0.0, 1.0);
        emit levelChanged(m_level);
      }
    }
  }
  // Done once everything has been handed over and the device has drained.
  const bool drained = m_sink->bytesFree() >= m_sink->bufferSize() ||
                       m_sink->state() == QAudio::IdleState;
  if (m_inputDone && m_pending.size() < frameBytes && drained)
    end(true);
}

void Speaker::end(bool emitFinished) {
  m_pump.stop();
  const bool wasPlaying = m_sink != nullptr;
  if (m_sink) {
    m_sink->stop();
    m_sink.reset();
  }
  m_io = nullptr;
  m_pending.clear();
  if (m_level != 0.0) {
    m_level = 0.0;
    emit levelChanged(0.0);
  }
  if (wasPlaying && emitFinished)
    emit finished();
}
