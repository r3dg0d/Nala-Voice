#include "audio.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioSource>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaDevices>
#include <QPointer>
#include <QProcess>
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

bool Microphone::sourceListedInPwDump(const QByteArray &json,
                                      const QString &device) {
  const QJsonDocument doc = QJsonDocument::fromJson(json);
  if (!doc.isArray())
    return true; // unreadable: cannot tell, so do not cry wolf
  for (const QJsonValue &value : doc.array()) {
    const QJsonObject props = value.toObject()
                                  .value(QStringLiteral("info"))
                                  .toObject()
                                  .value(QStringLiteral("props"))
                                  .toObject();
    if (!props.value(QStringLiteral("media.class"))
             .toString()
             .startsWith(QLatin1String("Audio/Source")))
      continue;
    for (const char *key : {"node.description", "node.name", "node.nick"})
      if (props.value(QLatin1String(key)).toString() == device)
        return true;
  }
  return false;
}

Microphone::PresenceCheck Microphone::pipewirePresenceCheck() {
  return [](const QString &device, std::function<void(bool)> done) {
    auto *proc = new QProcess;
    auto answered = std::make_shared<bool>(false);
    const auto finish = [proc, done, answered](bool present) {
      if (*answered)
        return;
      *answered = true;
      proc->disconnect();
      proc->deleteLater();
      done(present);
    };
    QObject::connect(proc, &QProcess::errorOccurred, proc,
                     [finish](QProcess::ProcessError) { finish(true); });
    QObject::connect(proc, &QProcess::finished, proc,
                     [proc, device, finish](int code, QProcess::ExitStatus st) {
                       finish(st != QProcess::NormalExit || code != 0 ||
                              sourceListedInPwDump(proc->readAllStandardOutput(),
                                                   device));
                     });
    QTimer::singleShot(4000, proc, [proc, finish] {
      proc->kill();
      finish(true);
    });
    proc->start(QStringLiteral("pw-dump"), {});
  };
}

Microphone::Microphone(QObject *parent) : QObject(parent) {
  m_presence = pipewirePresenceCheck();
  m_watch.setInterval(3000);
  connect(&m_watch, &QTimer::timeout, this, &Microphone::checkPresence);
  m_retry.setSingleShot(true);
  connect(&m_retry, &QTimer::timeout, this, &Microphone::retry);
  connect(&m_devices, &QMediaDevices::audioInputsChanged, this,
          &Microphone::onDevicesChanged);
}
Microphone::~Microphone() { stop(); }

QStringList Microphone::devices() {
  QStringList names;
  for (const QAudioDevice &device : QMediaDevices::audioInputs())
    names << device.description();
  return names;
}

int Microphone::retryDelayMs(int attempt) {
  return std::min(15000, 1000 << std::clamp(attempt, 0, 4));
}

bool Microphone::start(const QString &device,
                       const audio::VoiceActivity::Config &vad) {
  stop();
  m_device = device;
  m_vadConfig = vad;
  return open(device, vad, true);
}

bool Microphone::open(const QString &device,
                      const audio::VoiceActivity::Config &vad,
                      bool announceFailure) {
  const auto fail = [&](const QString &why) {
    if (announceFailure)
      emit failed(why);
    return false;
  };
  const QAudioDevice input = pick(QMediaDevices::audioInputs(),
                                  QMediaDevices::defaultAudioInput(), device);
  if (input.isNull())
    return fail(QStringLiteral("No microphone found."));

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
    if (!input.isFormatSupported(format))
      return fail(QStringLiteral("The microphone offers no usable format."));
  }

  m_format = format;
  m_vad = audio::VoiceActivity(vad);
  m_source = std::make_unique<QAudioSource>(input, format);
  m_source->setBufferSize(format.bytesForDuration(200000));
  m_io = m_source->start();
  if (!m_io) {
    m_source.reset();
    return fail(QStringLiteral("Could not open the microphone."));
  }
  m_openId = input.id();
  m_wantOpen = true;
  if (!device.isEmpty())
    m_watch.start();
  connect(m_io, &QIODevice::readyRead, this, &Microphone::read);
  // A source that stops with an error (unplugged, PipeWire restarted) would
  // otherwise leave us "open" and deaf.
  connect(m_source.get(), &QAudioSource::stateChanged, this,
          [this](QAudio::State state) {
            if (state == QAudio::StoppedState && m_source &&
                m_source->error() != QAudio::NoError)
              lose(QStringLiteral("The microphone stopped (audio error %1).")
                       .arg(int(m_source->error())));
          });
  return true;
}

// The device died. Drop the source and keep trying to get it back; a headset
// being unplugged should not need a settings toggle to undo.
void Microphone::lose(const QString &reason) {
  if (!m_wantOpen)
    return;
  m_watch.stop();
  if (m_source) {
    // Disconnect first: stop() emits stateChanged and we must not re-enter.
    m_source->disconnect(this);
    m_source->stop();
    m_source.reset();
  }
  m_io = nullptr;
  m_vad.reset();
  if (m_level != 0.0) {
    m_level = 0.0;
    emit levelChanged(0.0);
  }
  if (!m_wasLost) {
    m_wasLost = true;
    emit lost(reason);
  }
  m_retry.start(retryDelayMs(m_attempt++));
}

void Microphone::checkPresence() {
  if (m_checking || !m_source || m_device.isEmpty() || !m_presence)
    return;
  m_checking = true;
  QPointer<Microphone> self(this);
  m_presence(m_device, [self](bool present) {
    if (!self)
      return;
    self->m_checking = false;
    if (!present && self->m_source)
      self->lose(QStringLiteral(
          "The selected microphone is no longer present."));
  });
}

void Microphone::retry() {
  if (!m_wantOpen || m_source)
    return;
  // A named device: Qt's list keeps a removed source, and opening it would let
  // the server attach us to the default microphone instead. Ask PipeWire.
  if (!m_device.isEmpty() && m_presence && !m_checking) {
    m_checking = true;
    QPointer<Microphone> self(this);
    m_presence(m_device, [self](bool present) {
      if (!self)
        return;
      self->m_checking = false;
      if (!self->m_wantOpen || self->m_source)
        return;
      if (present)
        self->reopen();
      else
        self->m_retry.start(retryDelayMs(self->m_attempt++));
    });
    return;
  }
  reopen();
}

void Microphone::reopen() {
  if (!m_wantOpen || m_source)
    return;
  if (open(m_device, m_vadConfig, false)) {
    m_attempt = 0;
    m_wasLost = false;
    emit recovered();
  } else {
    m_retry.start(retryDelayMs(m_attempt++));
  }
}

// A device appeared or disappeared. Reopen at once if we are waiting; and if we
// follow the system default, move to a new default rather than staying on the
// old one.
void Microphone::onDevicesChanged() {
  if (!m_wantOpen)
    return;
  if (!m_source) {
    m_retry.stop();
    retry();
    return;
  }
  const QAudioDevice def = QMediaDevices::defaultAudioInput();
  const bool present = [&] {
    for (const QAudioDevice &d : QMediaDevices::audioInputs())
      if (d.id() == m_openId)
        return true;
    return false;
  }();
  if (!present)
    lose(QStringLiteral("The microphone was disconnected."));
  else if (m_device.isEmpty() && !def.isNull() && def.id() != m_openId) {
    m_source->disconnect(this);
    m_source->stop();
    m_source.reset();
    m_io = nullptr;
    if (open(m_device, m_vadConfig, false))
      emit recovered();
    else
      m_retry.start(retryDelayMs(m_attempt++));
  }
}

void Microphone::stop() {
  m_watch.stop();
  m_wantOpen = false;
  m_wasLost = false;
  m_attempt = 0;
  m_retry.stop();
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
  emit frames(mono);
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

// The output device died mid-stream (unplugged, PipeWire restarted). Without
// this, pump() waits forever for a sink that never drains and the assistant
// stays "speaking"; failed() lets her give up on the voice and settle.
void Speaker::sinkFailed(const QString &reason) {
  end(false);
  emit failed(reason);
}

void Speaker::pump() {
  if (!m_sink || !m_io)
    return;
  if (m_sink->error() != QAudio::NoError) {
    sinkFailed(QStringLiteral("The audio output stopped (audio error %1).")
                   .arg(int(m_sink->error())));
    return;
  }
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
