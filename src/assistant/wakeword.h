#pragma once
#include <QDateTime>
#include <QFile>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <array>
#include <cstdint>
#include <memory>

// Wake-word detection: a small, always-on listener that does one job --
// noticing her name -- so the recogniser and the model only wake up when
// someone is actually talking to her.
//
//   16 kHz audio -> mel spectrogram -> speech embedding (96-d every 80 ms)
//     -> one tiny classifier per wake phrase -> gate -> detected
//
// The first two stages are openWakeWord's feature models (ONNX), which are
// general-purpose and frozen. The classifiers are Nala's own: trained here,
// on the user's own recordings of whatever phrase they chose, so any phrase
// in any voice works without anyone having pre-trained it. The feature
// models are downloaded on request, not shipped; see docs/WAKEWORD.md.
namespace wake {

constexpr int kRate = 16000;
constexpr int kChunk = 1280;   // 80 ms: one embedding per chunk
constexpr int kMelBins = 32;
constexpr int kMelWindow = 76; // mel frames per embedding
constexpr int kEmbedding = 96;
using Embedding = std::array<float, kEmbedding>;

// --- features ---------------------------------------------------------------

// Streaming audio -> embeddings, exactly as openWakeWord computes them.
class Features {
public:
  Features();
  ~Features();
  Features(const Features &) = delete;
  Features &operator=(const Features &) = delete;

  // Where the two .onnx files are looked for when no directory is given.
  static QString defaultDir();
  static bool present(const QString &dir);
  bool load(const QString &dir, QString *error);
  bool loaded() const { return m_impl != nullptr; }

  // Start a fresh stream (a new clip, or after a long pause).
  void reset();
  // Feed samples; returns how many new embeddings were produced.
  int push(const int16_t *samples, int count);
  // The newest `count` embeddings flattened oldest first, or false if there
  // are not that many yet.
  bool window(int count, float *out) const;
  int available() const { return int(m_embeddings.size()); }

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
  QVector<float> m_audio;       // raw samples not yet turned into mel frames
  QVector<float> m_context;     // the samples before them, for the overlap
  QVector<std::array<float, kMelBins>> m_mel;
  QVector<Embedding> m_embeddings;
};

// General-purpose negative data: ~11 hours of real-world audio (dinner-party
// conversation, conversational English, reverberated music) as a stream of
// embeddings, from openWakeWord's published feature sets. Without it a
// phrase trained on one voice learns "anyone else talking" as a match.
class NegativeBank {
public:
  bool load(const QString &dir, QString *error = nullptr);
  bool loaded() const { return m_frames > 0; }
  qint64 frames() const { return m_frames; }
  const float *frame(qint64 index) const {
    return m_data + index * kEmbedding;
  }
  static QString fileName() { return QStringLiteral("validation_set_features.npy"); }

private:
  std::unique_ptr<QFile> m_file;
  const float *m_data = nullptr;
  qint64 m_frames = 0;
};

// --- a trained phrase ---------------------------------------------------------

// Two independent tests, both of which must agree:
//
// - a classifier over the last `windows` embeddings, which knows roughly
//   what the phrase sounds like against thousands of hours of other audio;
// - template matching: dynamic time warping of the recent embeddings
//   against each of the user's own recordings, which knows exactly what
//   *their* phrase sounds like, at whatever speed they say it.
//
// The classifier alone fires on the end of anyone's sentence; the templates
// alone are fooled by the user saying something similar. Together they are
// far stricter than either.
struct Model {
  QString phrase;
  int windows = 16;            // embeddings the classifier looks at
  QVector<float> mean, scale;  // standardisation, per feature
  QVector<float> weights;
  float bias = 0.0f;
  float threshold = 0.5f;      // on the combined confidence; tunable

  QVector<QVector<float>> templates; // each: frames x 96, centred, unit length
  QVector<float> center;             // subtracted before comparing
  float matchThreshold = 0.0f;       // template similarity that counts
  int query = 20;                    // recent frames templates are sought in

  QJsonObject stats;           // how training went, for the UI
  QDateTime trained;

  bool valid() const {
    return !phrase.isEmpty() && weights.size() == windows * kEmbedding &&
           mean.size() == weights.size() && scale.size() == weights.size();
  }
  bool hasTemplates() const {
    return !templates.isEmpty() && center.size() == kEmbedding;
  }
  float classify(const float *features) const; // 0..1
  // Best template similarity for the newest `query` frames (flattened,
  // oldest first). 0..1, higher is closer.
  float match(const float *frames, int count) const;
  // What the detector compares with `threshold`: the classifier, held back
  // by the template similarity so that both have to clear their bars.
  float combine(float classified, float matched) const;
  float score(const float *features) const { return classify(features); }
  QJsonObject toJson() const;
  static Model fromJson(const QJsonObject &json);
};

// Where phrases live: <dataDir>/phrases/<slug>/{model.json,samples/*.wav}.
QString slug(const QString &phrase);
QString phraseDir(const QString &phrase);
bool saveModel(const Model &model, QString *error = nullptr);
Model loadModel(const QString &phrase);
QStringList trainedPhrases();
// Recordings and model, gone. The only copy of a voice is the user's to
// delete.
bool deletePhrase(const QString &phrase);

// --- training -----------------------------------------------------------------

using Clip = QVector<int16_t>;

namespace augment {
Clip trim(const Clip &clip);                       // strip leading/trailing quiet
Clip gain(const Clip &clip, double db);
Clip speed(const Clip &clip, double factor);       // tempo and pitch together
Clip reverb(const Clip &clip, double decaySeconds, quint32 seed);
Clip mix(const Clip &clip, const Clip &noise, double snrDb, quint32 seed);
Clip pinkNoise(int samples, double level, quint32 seed);
} // namespace augment

struct TrainReport {
  bool ok = false;
  QString error;
  int positives = 0, negatives = 0;
  double recall = 0.0;          // held-out positives detected at threshold
  double falseAcceptRate = 0.0; // held-out negative windows over threshold
  double falseAcceptsPerHour = -1.0; // on held-out real-world audio, if any
  double benchmarkHours = 0.0;
  float threshold = 0.5f;
  qint64 milliseconds = 0;
};

// Few-shot training on the user's recordings. `positives` are recordings of
// the phrase; `negatives` are anything that is not it (ordinary speech,
// room noise, similar-sounding phrases). Augmentation multiplies both.
class Trainer {
public:
  explicit Trainer(Features *features, const NegativeBank *bank = nullptr)
      : m_features(features), m_bank(bank) {}
  // `synthetic` are other voices saying the phrase (local TTS): they teach
  // the classifier what the phrase sounds like in general, but never become
  // templates, which stay the user's own.
  Model train(const QString &phrase, const QVector<Clip> &positives,
              const QVector<Clip> &negatives, TrainReport *report,
              quint32 seed = 1, const QVector<Clip> &synthetic = {});

  // Embedding windows from one clip: every window, or just those that end
  // shortly after the clip does (for positives).
  QVector<QVector<float>> windows(const Clip &clip, int count, bool tailOnly,
                                  const Clip &before = {});
  QVector<QVector<float>> tailWindows(const Clip &phrase, const Clip &withTail,
                                      int count, const Clip &before = {});
  // Every embedding produced while streaming `before` + `clip` + a little
  // quiet, with the sample position each one ends at (relative to the start
  // of `clip`).
  QVector<QPair<int, Embedding>> stream(const Clip &clip, const Clip &before);

private:
  Features *m_features;
  const NegativeBank *m_bank = nullptr;
};

// Logistic regression, the part that learns. Pure; tested on its own.
struct Fit {
  QVector<float> mean, scale, weights;
  float bias = 0.0f;
};
Fit fitLogistic(const QVector<QVector<float>> &positives,
                const QVector<QVector<float>> &negatives, int epochs,
                double l2, quint32 seed);
float predict(const Fit &fit, const float *features, int count);

// --- deciding ---------------------------------------------------------------

// Turns a stream of scores into detections. A detection needs the score
// over threshold for `consecutive` windows in a row, is followed by a
// cooldown, and cannot happen at all while suspended (she is talking) or in
// the short grace period after.
class Gate {
public:
  struct Config {
    float threshold = 0.5f;
    int consecutive = 1; // what training calibrates for
    int cooldownMs = 2000;
  };
  Gate() = default;
  explicit Gate(Config config) : m_config(config) {}
  void setConfig(Config config) { m_config = config; }
  const Config &config() const { return m_config; }

  // True when this score completes a detection.
  bool update(float score, qint64 nowMs);
  void suspend(qint64 nowMs);
  void resume(qint64 nowMs, int graceMs);
  bool suspended(qint64 nowMs) const;
  bool coolingDown(qint64 nowMs) const { return nowMs < m_coolUntil; }
  void reset() { m_run = 0; }

private:
  Config m_config;
  int m_run = 0;
  bool m_suspended = false;
  qint64 m_coolUntil = 0;
};

// --- the backend interface ------------------------------------------------------

struct Detection {
  QString phrase;
  float score = 0.0f;
  qint64 latencyMs = 0; // from the audio that completed it to the signal
};

class WakeWordBackend : public QObject {
  Q_OBJECT

public:
  using QObject::QObject;
  virtual QString name() const = 0;
  virtual bool initialize(QString *error) = 0;
  virtual bool ready() const = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  // While she speaks: nothing she says can wake her.
  virtual void pause() = 0;
  // Listening again after `graceMs` of quiet.
  virtual void resume(int graceMs) = 0;
  virtual void processAudio(const int16_t *samples, int count) = 0;
  virtual bool registerWakeword(const Model &model) = 0;
  virtual void removeWakeword(const QString &phrase) = 0;
  virtual QStringList wakewords() const = 0;
  // `sensitivity` 0..1 moves every threshold from its trained value:
  // 0.5 keeps it, higher is more eager, lower is stricter.
  virtual void setSensitivity(double sensitivity) = 0;
  virtual void setConfirmation(int consecutive, int cooldownMs) = 0;

signals:
  void detected(const wake::Detection &detection);
  // Every window's best score, for the tuning meter.
  void scored(const QString &phrase, float score, float threshold);
  void failed(const QString &reason);
};

// openWakeWord features + per-phrase classifiers, on the CPU, one thread.
class NeuralBackend : public WakeWordBackend {
  Q_OBJECT

public:
  explicit NeuralBackend(QString featureDir = {}, QObject *parent = nullptr);
  ~NeuralBackend() override;
  QString name() const override { return QStringLiteral("neural"); }
  bool initialize(QString *error) override;
  bool ready() const override;
  void start() override;
  void stop() override;
  void pause() override;
  void resume(int graceMs) override;
  void processAudio(const int16_t *samples, int count) override;
  bool registerWakeword(const Model &model) override;
  void removeWakeword(const QString &phrase) override;
  QStringList wakewords() const override;
  void setSensitivity(double sensitivity) override;
  void setConfirmation(int consecutive, int cooldownMs) override;

  // For the tuning tools: the clock is the audio itself, in milliseconds.
  qint64 audioClock() const { return m_clockMs; }

private:
  struct Entry {
    Model model;
    Gate gate;
  };
  void applyGates();

  QString m_featureDir;
  Features m_features;
  QVector<Entry> m_entries;
  QVector<float> m_window;
  bool m_running = false;
  double m_sensitivity = 0.5;
  int m_consecutive = 1;
  int m_cooldownMs = 2000;
  qint64 m_clockMs = 0; // audio time, so tests and files are deterministic
};

} // namespace wake

Q_DECLARE_METATYPE(wake::Detection)
