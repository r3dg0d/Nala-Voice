#pragma once
#include <QByteArray>
#include <QVector>
#include <cstdint>

// Small, pure pieces of the audio path, kept apart so they can be tested
// without a sound card.
namespace audio {

constexpr int kSttRate = 16000; // what whisper.cpp wants: 16 kHz mono s16

// A canonical 44-byte-header PCM WAV around `pcm`.
QByteArray wav(const QByteArray &pcm, int sampleRate, int channels = 1,
               int bitsPerSample = 16);

struct WavInfo {
  bool ok = false;
  int sampleRate = 0;
  int channels = 0;
  int bitsPerSample = 0;
  int dataOffset = 0; // where the samples start
};
// Reads the header, tolerating the zero or 0xFFFFFFFF sizes a streaming
// server writes because it does not yet know the length.
WavInfo parseWav(const QByteArray &bytes);

// Mix interleaved s16 down to mono and resample linearly. Good enough for
// speech recognition, which is all it is for.
QVector<int16_t> toMono16k(const int16_t *samples, int frames, int channels,
                           int sampleRate);

// Root-mean-square level of s16 samples, 0..1.
double rms(const int16_t *samples, int count);
double toDb(double level);

struct VadConfig {
  double thresholdDb = 9.0;
  int silenceMs = 700;
  int minSpeechMs = 90;
  int preRollMs = 300;
  int maxUtteranceMs = 20000;
  double absoluteFloorDb = -55.0; // never call anything quieter speech
};

// Decides where speech starts and stops.
//
// Energy against an adaptive noise floor: the floor follows the room while
// nobody is talking, and a frame counts as speech when it is `thresholdDb`
// louder than that. Speech has to last a few frames to start an utterance, and
// silence has to last `silenceMs` to end one, so a cough or a pause for breath
// does neither.
class VoiceActivity {
public:
  using Config = VadConfig;
  enum Event { None, Started, Ended };

  VoiceActivity() = default;
  explicit VoiceActivity(Config config);

  // Feed 16 kHz mono samples in any chunk size. Returns Ended once an
  // utterance is complete; take it with utterance().
  Event feed(const int16_t *samples, int count);
  QVector<int16_t> utterance();
  bool speaking() const { return m_inSpeech; }
  double noiseFloorDb() const { return m_floorDb; }
  double levelDb() const { return m_levelDb; }
  void reset();

private:
  Event frame(const int16_t *samples, int count);

  Config m_config;
  QVector<int16_t> m_pending; // a partial frame
  QVector<int16_t> m_preRoll;
  QVector<int16_t> m_speech;
  QVector<int16_t> m_done;
  double m_floorDb = -60.0;
  double m_levelDb = -90.0;
  bool m_floorPrimed = false;
  bool m_inSpeech = false;
  int m_voicedMs = 0;
  int m_silentMs = 0;
};

} // namespace audio
