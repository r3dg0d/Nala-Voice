#include "audioutil.h"

#include <QtEndian>
#include <algorithm>
#include <cmath>

namespace audio {

namespace {

constexpr int kFrameMs = 30;
constexpr int kFrameSamples = kSttRate * kFrameMs / 1000;

void put32(QByteArray &out, quint32 v) {
  char b[4];
  qToLittleEndian(v, b);
  out.append(b, 4);
}
void put16(QByteArray &out, quint16 v) {
  char b[2];
  qToLittleEndian(v, b);
  out.append(b, 2);
}

void appendSamples(QVector<int16_t> &to, const int16_t *from, int count) {
  const qsizetype at = to.size();
  to.resize(at + count);
  std::copy(from, from + count, to.begin() + at);
}

} // namespace

QByteArray wav(const QByteArray &pcm, int sampleRate, int channels,
               int bitsPerSample) {
  QByteArray out;
  out.reserve(44 + pcm.size());
  const int blockAlign = channels * bitsPerSample / 8;
  out.append("RIFF", 4);
  put32(out, quint32(36 + pcm.size()));
  out.append("WAVEfmt ", 8);
  put32(out, 16);
  put16(out, 1); // PCM
  put16(out, quint16(channels));
  put32(out, quint32(sampleRate));
  put32(out, quint32(sampleRate * blockAlign));
  put16(out, quint16(blockAlign));
  put16(out, quint16(bitsPerSample));
  out.append("data", 4);
  put32(out, quint32(pcm.size()));
  out.append(pcm);
  return out;
}

WavInfo parseWav(const QByteArray &bytes) {
  WavInfo info;
  if (bytes.size() < 12 || !bytes.startsWith("RIFF") ||
      bytes.mid(8, 4) != "WAVE")
    return info;
  int at = 12;
  bool haveFormat = false;
  // Walk the chunks: some writers put LIST or fact chunks before the data.
  while (at + 8 <= bytes.size()) {
    const QByteArray id = bytes.mid(at, 4);
    const quint32 size = qFromLittleEndian<quint32>(bytes.constData() + at + 4);
    const int body = at + 8;
    if (id == "fmt ") {
      if (body + 16 > bytes.size())
        return info;
      const char *f = bytes.constData() + body;
      const quint16 format = qFromLittleEndian<quint16>(f);
      // PCM, or WAVE_FORMAT_EXTENSIBLE carrying PCM.
      if (format != 1 && format != 0xFFFE)
        return info;
      info.channels = qFromLittleEndian<quint16>(f + 2);
      info.sampleRate = int(qFromLittleEndian<quint32>(f + 4));
      info.bitsPerSample = qFromLittleEndian<quint16>(f + 14);
      haveFormat = true;
    } else if (id == "data") {
      if (!haveFormat)
        return info;
      info.dataOffset = body;
      info.ok = info.channels > 0 && info.channels <= 8 &&
                info.sampleRate >= 8000 && info.sampleRate <= 192000 &&
                (info.bitsPerSample == 16 || info.bitsPerSample == 32 ||
                 info.bitsPerSample == 8);
      return info;
    }
    // A streamed header may claim an absurd size for its chunk; anything
    // that would run past the end cannot be skipped over.
    if (size > quint32(bytes.size() - body))
      return info;
    at = body + int(size) + int(size & 1);
  }
  return info;
}

QVector<int16_t> toMono16k(const int16_t *samples, int frames, int channels,
                           int sampleRate) {
  QVector<int16_t> out;
  if (frames <= 0 || channels <= 0 || sampleRate <= 0)
    return out;
  const double step = double(sampleRate) / kSttRate;
  const int produced = int(std::floor((frames - 1) / step)) + 1;
  out.reserve(produced);
  const auto mono = [&](int frame) {
    int sum = 0;
    for (int c = 0; c < channels; ++c)
      sum += samples[frame * channels + c];
    return double(sum) / channels;
  };
  for (int i = 0; i < produced; ++i) {
    const double pos = i * step;
    const int a = int(pos);
    const int b = std::min(a + 1, frames - 1);
    const double t = pos - a;
    const double v = mono(a) * (1.0 - t) + mono(b) * t;
    out.append(int16_t(std::clamp(std::lround(v), -32768L, 32767L)));
  }
  return out;
}

double rms(const int16_t *samples, int count) {
  if (count <= 0)
    return 0.0;
  double sum = 0.0;
  for (int i = 0; i < count; ++i) {
    const double v = samples[i] / 32768.0;
    sum += v * v;
  }
  return std::sqrt(sum / count);
}

double toDb(double level) { return 20.0 * std::log10(std::max(level, 1e-5)); }

VoiceActivity::VoiceActivity(Config config) : m_config(config) {}

void VoiceActivity::reset() {
  m_pending.clear();
  m_preRoll.clear();
  m_speech.clear();
  m_inSpeech = false;
  m_voicedMs = m_silentMs = 0;
}

VoiceActivity::Event VoiceActivity::feed(const int16_t *samples, int count) {
  Event result = None;
  int at = 0;
  // Top up a partial frame first, then take whole frames straight from the
  // input.
  while (at < count) {
    const int need = kFrameSamples - int(m_pending.size());
    const int take = std::min(need, count - at);
    appendSamples(m_pending, samples + at, take);
    at += take;
    if (m_pending.size() < kFrameSamples)
      break;
    const Event event = frame(m_pending.constData(), int(m_pending.size()));
    m_pending.clear();
    if (event == Ended)
      result = Ended;
    else if (event == Started && result == None)
      result = Started;
  }
  return result;
}

VoiceActivity::Event VoiceActivity::frame(const int16_t *samples, int count) {
  m_levelDb = toDb(rms(samples, count));
  if (!m_floorPrimed) {
    m_floorDb = m_levelDb;
    m_floorPrimed = true;
  }
  const bool voiced = m_levelDb > m_floorDb + m_config.thresholdDb &&
                      m_levelDb > m_config.absoluteFloorDb;

  // The floor tracks the room only while nobody is talking, falling quickly
  // and rising slowly, so speech never raises the bar against itself.
  if (!voiced && !m_inSpeech) {
    const double rate = m_levelDb < m_floorDb ? 0.2 : 0.02;
    m_floorDb += (m_levelDb - m_floorDb) * rate;
  }

  if (!m_inSpeech) {
    appendSamples(m_preRoll, samples, count);
    const int keep = kSttRate * m_config.preRollMs / 1000;
    if (m_preRoll.size() > keep)
      m_preRoll.remove(0, m_preRoll.size() - keep);
    m_voicedMs = voiced ? m_voicedMs + kFrameMs : 0;
    if (m_voicedMs >= m_config.minSpeechMs) {
      m_inSpeech = true;
      m_silentMs = 0;
      m_speech = m_preRoll; // keep the onset the detector needed to hear
      m_preRoll.clear();
      return Started;
    }
    return None;
  }

  appendSamples(m_speech, samples, count);
  m_silentMs = voiced ? 0 : m_silentMs + kFrameMs;
  const int lengthMs = int(m_speech.size() * 1000 / kSttRate);
  if (m_silentMs >= m_config.silenceMs ||
      lengthMs >= m_config.maxUtteranceMs) {
    m_done = m_speech;
    m_speech.clear();
    m_inSpeech = false;
    m_voicedMs = 0;
    return Ended;
  }
  return None;
}

QVector<int16_t> VoiceActivity::utterance() {
  QVector<int16_t> out;
  out.swap(m_done);
  return out;
}

} // namespace audio
