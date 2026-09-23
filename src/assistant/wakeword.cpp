#include "wakeword.h"
#include "audioutil.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentMap>
#include <algorithm>
#include <cmath>
#include <numeric>

#ifdef NALA_HAVE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace wake {

namespace {

constexpr int kContext = 480; // 30 ms of overlap for the mel frames
constexpr int kWarmup = 10;   // embeddings still coloured by the initial buffer

QString dataRoot() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
         "/nala/wakeword";
}

float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

} // namespace

// --- features ---------------------------------------------------------------

#ifdef NALA_HAVE_ONNX
struct Features::Impl {
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "nala-wakeword"};
  Ort::MemoryInfo memory =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::unique_ptr<Ort::Session> mel, embed;
  std::string melIn, melOut, embedIn, embedOut;
};
#else
struct Features::Impl {};
#endif

Features::Features() { reset(); }
Features::~Features() = default;

QString Features::defaultDir() { return dataRoot() + "/features"; }

bool Features::present(const QString &dir) {
  return QFileInfo::exists(dir + "/melspectrogram.onnx") &&
         QFileInfo::exists(dir + "/embedding_model.onnx");
}

bool Features::load(const QString &dir, QString *error) {
#ifdef NALA_HAVE_ONNX
  const QString root = dir.isEmpty() ? defaultDir() : dir;
  if (!present(root)) {
    if (error)
      *error = QStringLiteral("wake-word feature models are not installed "
                              "(run \"nala wakeword setup\")");
    return false;
  }
  try {
    auto impl = std::make_unique<Impl>();
    Ort::SessionOptions options;
    // One thread: the whole point is to cost next to nothing.
    options.SetIntraOpNumThreads(1);
    options.SetInterOpNumThreads(1);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    const QByteArray melPath = QFile::encodeName(root + "/melspectrogram.onnx");
    const QByteArray embedPath =
        QFile::encodeName(root + "/embedding_model.onnx");
    impl->mel = std::make_unique<Ort::Session>(impl->env, melPath.constData(),
                                               options);
    impl->embed = std::make_unique<Ort::Session>(
        impl->env, embedPath.constData(), options);
    Ort::AllocatorWithDefaultOptions allocator;
    impl->melIn = impl->mel->GetInputNameAllocated(0, allocator).get();
    impl->melOut = impl->mel->GetOutputNameAllocated(0, allocator).get();
    impl->embedIn = impl->embed->GetInputNameAllocated(0, allocator).get();
    impl->embedOut = impl->embed->GetOutputNameAllocated(0, allocator).get();
    m_impl = std::move(impl);
  } catch (const Ort::Exception &e) {
    if (error)
      *error = QStringLiteral("could not load the wake-word models: %1")
                   .arg(QString::fromUtf8(e.what()));
    return false;
  }
  reset();
  return true;
#else
  Q_UNUSED(dir);
  if (error)
    *error = QStringLiteral("built without ONNX Runtime");
  return false;
#endif
}

void Features::reset() {
  m_audio.clear();
  m_context = QVector<float>(kContext, 0.0f);
  // openWakeWord starts its mel buffer full of ones; matching it keeps the
  // first embeddings comparable.
  m_mel = QVector<std::array<float, kMelBins>>(kMelWindow);
  for (auto &frame : m_mel)
    frame.fill(1.0f);
  m_embeddings.clear();
}

int Features::push(const int16_t *samples, int count) {
#ifdef NALA_HAVE_ONNX
  if (!m_impl)
    return 0;
  for (int i = 0; i < count; ++i)
    m_audio.append(float(samples[i])); // int16 values, as floats, unscaled
  int produced = 0;
  while (m_audio.size() >= kChunk) {
    QVector<float> input = m_context;
    input += m_audio.mid(0, kChunk);
    m_audio.remove(0, kChunk);
    m_context = input.mid(input.size() - kContext);

    try {
      const std::array<int64_t, 2> shape{1, int64_t(input.size())};
      Ort::Value tensor = Ort::Value::CreateTensor<float>(
          m_impl->memory, input.data(), size_t(input.size()), shape.data(),
          shape.size());
      const char *inName = m_impl->melIn.c_str();
      const char *outName = m_impl->melOut.c_str();
      auto out = m_impl->mel->Run(Ort::RunOptions{nullptr}, &inName, &tensor,
                                  1, &outName, 1);
      const auto info = out[0].GetTensorTypeAndShapeInfo();
      const size_t values = info.GetElementCount();
      const float *mel = out[0].GetTensorData<float>();
      for (size_t f = 0; f + kMelBins <= values; f += kMelBins) {
        std::array<float, kMelBins> frame;
        for (int b = 0; b < kMelBins; ++b)
          frame[b] = mel[f + b] / 10.0f + 2.0f; // openWakeWord's transform
        m_mel.append(frame);
      }
      while (m_mel.size() > kMelWindow + 16)
        m_mel.removeFirst();

      std::array<float, kMelWindow * kMelBins> window;
      const int start = int(m_mel.size()) - kMelWindow;
      for (int f = 0; f < kMelWindow; ++f)
        std::copy(m_mel[start + f].begin(), m_mel[start + f].end(),
                  window.begin() + f * kMelBins);
      const std::array<int64_t, 4> embedShape{1, kMelWindow, kMelBins, 1};
      Ort::Value embedTensor = Ort::Value::CreateTensor<float>(
          m_impl->memory, window.data(), window.size(), embedShape.data(),
          embedShape.size());
      const char *embedIn = m_impl->embedIn.c_str();
      const char *embedOut = m_impl->embedOut.c_str();
      auto emb = m_impl->embed->Run(Ort::RunOptions{nullptr}, &embedIn,
                                    &embedTensor, 1, &embedOut, 1);
      const float *vector = emb[0].GetTensorData<float>();
      Embedding e;
      std::copy(vector, vector + kEmbedding, e.begin());
      m_embeddings.append(e);
      while (m_embeddings.size() > 64)
        m_embeddings.removeFirst();
      ++produced;
    } catch (const Ort::Exception &) {
      return produced;
    }
  }
  return produced;
#else
  Q_UNUSED(samples);
  Q_UNUSED(count);
  return 0;
#endif
}

bool Features::window(int count, float *out) const {
  if (count <= 0 || m_embeddings.size() < count)
    return false;
  const int start = int(m_embeddings.size()) - count;
  for (int i = 0; i < count; ++i)
    std::copy(m_embeddings[start + i].begin(), m_embeddings[start + i].end(),
              out + i * kEmbedding);
  return true;
}

// --- negative bank ------------------------------------------------------------

bool NegativeBank::load(const QString &dir, QString *error) {
  const QString path = (dir.isEmpty() ? Features::defaultDir() : dir) + "/" + fileName();
  auto file = std::make_unique<QFile>(path);
  if (!file->open(QIODevice::ReadOnly)) {
    if (error)
      *error = QStringLiteral("no negative data at %1").arg(path);
    return false;
  }
  // .npy: magic, version, header length, a Python dict, then the array.
  const QByteArray head = file->peek(256);
  if (!head.startsWith("\x93NUMPY") || head.size() < 10) {
    if (error)
      *error = QStringLiteral("%1 is not a .npy file").arg(path);
    return false;
  }
  const int headerLength = quint8(head[8]) | (quint8(head[9]) << 8);
  const QByteArray header = head.mid(10, headerLength);
  if (!header.contains("'<f4'") || header.contains("True") ||
      !header.contains(", 96)")) {
    if (error)
      *error = QStringLiteral("%1 is not float32 (N, 96)").arg(path);
    return false;
  }
  const qint64 offset = 10 + headerLength;
  const qint64 bytes = file->size() - offset;
  uchar *mapped = file->map(offset, bytes);
  if (!mapped) {
    if (error)
      *error = file->errorString();
    return false;
  }
  m_data = reinterpret_cast<const float *>(mapped);
  m_frames = bytes / qint64(sizeof(float) * kEmbedding);
  m_file = std::move(file);
  return true;
}

// --- models ---------------------------------------------------------------------

float Model::classify(const float *features) const {
  if (!valid())
    return 0.0f;
  float z = bias;
  for (int i = 0; i < weights.size(); ++i)
    z += weights[i] * (features[i] - mean[i]) / scale[i];
  return sigmoid(z);
}

namespace {

// Centre and normalise frames so a dot product is a cosine.
QVector<float> prepare(const float *frames, int count, const QVector<float> &center) {
  QVector<float> out(count * kEmbedding);
  for (int f = 0; f < count; ++f) {
    double norm = 0.0;
    for (int d = 0; d < kEmbedding; ++d) {
      const float v = frames[f * kEmbedding + d] - center[d];
      out[f * kEmbedding + d] = v;
      norm += double(v) * v;
    }
    const float k = norm > 1e-12 ? float(1.0 / std::sqrt(norm)) : 0.0f;
    for (int d = 0; d < kEmbedding; ++d)
      out[f * kEmbedding + d] *= k;
  }
  return out;
}

// Subsequence DTW: the template may start anywhere in the query, and must
// end within the last few frames of it -- the phrase has just finished.
// Returns a similarity: 1 - mean cosine distance along the best path.
float dtw(const float *query, int n, const float *tmpl, int m) {
  if (n <= 0 || m <= 0)
    return 0.0f;
  constexpr float stretch = 0.15f; // a small price for warping
  const float inf = 1e30f;
  QVector<float> prev(m, inf), cur(m, inf);
  QVector<int> prevLen(m, 0), curLen(m, 0);
  float best = inf;
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < m; ++j) {
      float dot = 0.0f;
      const float *q = query + i * kEmbedding;
      const float *t = tmpl + j * kEmbedding;
      for (int d = 0; d < kEmbedding; ++d)
        dot += q[d] * t[d];
      const float cost = 1.0f - dot;
      float from = inf;
      int length = 1;
      if (j == 0) {
        from = 0.0f; // start anywhere
      } else {
        if (prev[j - 1] < from) {
          from = prev[j - 1];
          length = prevLen[j - 1] + 1;
        }
        if (cur[j - 1] + stretch < from) {
          from = cur[j - 1] + stretch;
          length = curLen[j - 1] + 1;
        }
      }
      if (prev[j] + stretch < from) {
        from = prev[j] + stretch;
        length = prevLen[j] + 1;
      }
      cur[j] = from + cost;
      curLen[j] = length;
    }
    if (i >= n - 4)
      best = std::min(best, cur[m - 1] / std::max(m, curLen[m - 1]));
    std::swap(prev, cur);
    std::swap(prevLen, curLen);
    std::fill(cur.begin(), cur.end(), inf);
  }
  return std::clamp(1.0f - best, 0.0f, 1.0f);
}

} // namespace

float Model::match(const float *frames, int count) const {
  if (!hasTemplates())
    return 1.0f; // nothing to compare against: the classifier decides
  const QVector<float> q = prepare(frames, count, center);
  // The mean of the two best matches, so one lucky template is not enough.
  QVector<float> scores;
  for (const QVector<float> &t : templates)
    scores << dtw(q.constData(), count, t.constData(), int(t.size() / kEmbedding));
  std::sort(scores.begin(), scores.end(), std::greater<float>());
  return scores.size() >= 3 ? (scores[0] + scores[1]) / 2.0f : scores[0];
}

float Model::combine(float classified, float matched) const {
  if (!hasTemplates())
    return classified;
  // Put the template similarity on the classifier's scale, so both clear
  // `threshold` at the same moment, and take the weaker of the two.
  return std::min(classified, matched - matchThreshold + threshold);
}

namespace {
QJsonArray floats(const QVector<float> &values) {
  QJsonArray out;
  for (float v : values)
    out.append(double(v));
  return out;
}
QVector<float> floats(const QJsonValue &value) {
  QVector<float> out;
  for (const QJsonValue &v : value.toArray())
    out.append(float(v.toDouble()));
  return out;
}
} // namespace

QJsonObject Model::toJson() const {
  return {{"format", "nala-wakeword-1"},
          {"phrase", phrase},
          {"windows", windows},
          {"threshold", double(threshold)},
          {"bias", double(bias)},
          {"mean", floats(mean)},
          {"scale", floats(scale)},
          {"weights", floats(weights)},
          {"center", floats(center)},
          {"matchThreshold", double(matchThreshold)},
          {"query", query},
          {"templates", [this] {
             QJsonArray all;
             for (const QVector<float> &t : templates)
               all.append(floats(t));
             return all;
           }()},
          {"stats", stats},
          {"trained", trained.toString(Qt::ISODate)}};
}

Model Model::fromJson(const QJsonObject &json) {
  Model m;
  if (json.value("format").toString() != "nala-wakeword-1")
    return m;
  m.phrase = json.value("phrase").toString();
  m.windows = json.value("windows").toInt(16);
  m.threshold = float(json.value("threshold").toDouble(0.5));
  m.bias = float(json.value("bias").toDouble());
  m.mean = floats(json.value("mean"));
  m.scale = floats(json.value("scale"));
  m.weights = floats(json.value("weights"));
  m.center = floats(json.value("center"));
  m.matchThreshold = float(json.value("matchThreshold").toDouble());
  m.query = std::clamp(json.value("query").toInt(20), 4, 64);
  for (const QJsonValue &t : json.value("templates").toArray()) {
    QVector<float> frames = floats(t);
    if (!frames.isEmpty() && frames.size() % kEmbedding == 0)
      m.templates << frames;
  }
  m.stats = json.value("stats").toObject();
  m.trained = QDateTime::fromString(json.value("trained").toString(), Qt::ISODate);
  for (float &s : m.scale)
    if (std::abs(s) < 1e-6f)
      s = 1.0f;
  return m;
}

QString slug(const QString &phrase) {
  QString s = phrase.toLower().simplified();
  s.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")),
            QStringLiteral("-"));
  s.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
  return s.left(60);
}

QString phraseDir(const QString &phrase) {
  return dataRoot() + "/phrases/" + slug(phrase);
}

bool saveModel(const Model &model, QString *error) {
  const QString dir = phraseDir(model.phrase);
  QDir().mkpath(dir);
  QFile::setPermissions(dataRoot(), QFileDevice::ReadOwner |
                                        QFileDevice::WriteOwner |
                                        QFileDevice::ExeOwner);
  QSaveFile file(dir + "/model.json");
  if (!file.open(QIODevice::WriteOnly)) {
    if (error)
      *error = file.errorString();
    return false;
  }
  file.write(QJsonDocument(model.toJson()).toJson(QJsonDocument::Compact));
  return file.commit();
}

Model loadModel(const QString &phrase) {
  QFile file(phraseDir(phrase) + "/model.json");
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return Model::fromJson(QJsonDocument::fromJson(file.readAll()).object());
}

QStringList trainedPhrases() {
  QStringList out;
  const QDir dir(dataRoot() + "/phrases");
  for (const QString &entry : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    QFile file(dir.filePath(entry) + "/model.json");
    if (!file.open(QIODevice::ReadOnly))
      continue;
    const Model m = Model::fromJson(QJsonDocument::fromJson(file.readAll()).object());
    if (m.valid())
      out << m.phrase;
  }
  return out;
}

bool deletePhrase(const QString &phrase) {
  const QString dir = phraseDir(phrase);
  if (slug(phrase).isEmpty() || !QFileInfo::exists(dir))
    return false;
  return QDir(dir).removeRecursively();
}

// --- augmentation -------------------------------------------------------------

namespace augment {

Clip trim(const Clip &clip) {
  // 10 ms frames; keep from the first to the last frame within 30 dB of the
  // loudest, with a little margin.
  constexpr int frame = 160;
  const int frames = int(clip.size()) / frame;
  if (frames < 3)
    return clip;
  QVector<double> levels(frames);
  double peak = 0.0;
  for (int f = 0; f < frames; ++f) {
    levels[f] = audio::rms(clip.constData() + f * frame, frame);
    peak = std::max(peak, levels[f]);
  }
  const double floor = peak * std::pow(10.0, -30.0 / 20.0);
  int first = 0, last = frames - 1;
  while (first < frames && levels[first] < floor)
    ++first;
  while (last > first && levels[last] < floor)
    --last;
  first = std::max(0, first - 3);
  last = std::min(frames - 1, last + 3);
  return clip.mid(first * frame, (last - first + 1) * frame);
}

Clip gain(const Clip &clip, double db) {
  const double k = std::pow(10.0, db / 20.0);
  Clip out(clip.size());
  for (int i = 0; i < clip.size(); ++i)
    out[i] = int16_t(std::clamp(std::lround(clip[i] * k), -32768L, 32767L));
  return out;
}

Clip speed(const Clip &clip, double factor) {
  if (clip.isEmpty() || factor <= 0.0)
    return clip;
  const int n = int(clip.size() / factor);
  Clip out(n);
  for (int i = 0; i < n; ++i) {
    const double pos = i * factor;
    const int a = std::min(int(pos), int(clip.size()) - 1);
    const int b = std::min(a + 1, int(clip.size()) - 1);
    const double t = pos - a;
    out[i] = int16_t(std::lround(clip[a] * (1.0 - t) + clip[b] * t));
  }
  return out;
}

Clip reverb(const Clip &clip, double decaySeconds, quint32 seed) {
  // A sparse, exponentially decaying impulse response: a room, roughly.
  QRandomGenerator random(seed);
  const int length = int(decaySeconds * kRate);
  QVector<QPair<int, double>> taps{{0, 1.0}};
  for (int i = 0; i < 40; ++i) {
    // Early reflections from 5 ms on; nothing lands past the tail.
    const int at = 80 + random.bounded(std::max(1, length));
    taps.append({at, (random.generateDouble() - 0.5) * 0.5 *
                         std::exp(-3.0 * at / std::max(1, length))});
  }
  QVector<double> wet(clip.size() + length + 81, 0.0);
  for (const auto &[at, weight] : taps)
    for (int i = 0; i < clip.size(); ++i)
      wet[i + at] += clip[i] * weight;
  double peak = 1.0;
  for (double v : wet)
    peak = std::max(peak, std::abs(v));
  const double k = peak > 32767.0 ? 32767.0 / peak : 1.0;
  Clip out(clip.size());
  for (int i = 0; i < clip.size(); ++i)
    out[i] = int16_t(std::lround(wet[i] * k));
  return out;
}

Clip mix(const Clip &clip, const Clip &noise, double snrDb, quint32 seed) {
  if (noise.isEmpty() || clip.isEmpty())
    return clip;
  QRandomGenerator random(seed);
  const double signal = std::max(1e-6, audio::rms(clip.constData(), int(clip.size())));
  const double level = std::max(1e-6, audio::rms(noise.constData(), int(noise.size())));
  const double k = signal / level / std::pow(10.0, snrDb / 20.0);
  const int offset = random.bounded(int(noise.size()));
  Clip out(clip.size());
  for (int i = 0; i < clip.size(); ++i) {
    const double v = clip[i] + noise[(offset + i) % noise.size()] * k;
    out[i] = int16_t(std::clamp(std::lround(v), -32768L, 32767L));
  }
  return out;
}

Clip pinkNoise(int samples, double level, quint32 seed) {
  // Paul Kellet's economy filter over white noise.
  QRandomGenerator random(seed);
  double b0 = 0, b1 = 0, b2 = 0;
  Clip out(samples);
  for (int i = 0; i < samples; ++i) {
    const double white = random.generateDouble() * 2.0 - 1.0;
    b0 = 0.99765 * b0 + white * 0.0990460;
    b1 = 0.96300 * b1 + white * 0.2965164;
    b2 = 0.57000 * b2 + white * 1.0526913;
    const double pink = (b0 + b1 + b2 + white * 0.1848) * 0.25;
    out[i] = int16_t(std::clamp(std::lround(pink * level * 32767.0), -32768L,
                                32767L));
  }
  return out;
}

} // namespace augment

// --- learning ---------------------------------------------------------------------

Fit fitLogistic(const QVector<QVector<float>> &positives,
                const QVector<QVector<float>> &negatives, int epochs,
                double l2, quint32 seed) {
  Fit fit;
  if (positives.isEmpty() || negatives.isEmpty())
    return fit;
  const int dims = int(positives.first().size());
  const int total = int(positives.size() + negatives.size());
  const auto row = [&](int i) -> const QVector<float> & {
    return i < positives.size() ? positives[i]
                                : negatives[i - int(positives.size())];
  };

  // Standardise, so one learning rate suits every feature.
  fit.mean = QVector<float>(dims, 0.0f);
  fit.scale = QVector<float>(dims, 0.0f);
  for (int i = 0; i < total; ++i)
    for (int d = 0; d < dims; ++d)
      fit.mean[d] += row(i)[d];
  for (float &m : fit.mean)
    m /= total;
  for (int i = 0; i < total; ++i)
    for (int d = 0; d < dims; ++d) {
      const float v = row(i)[d] - fit.mean[d];
      fit.scale[d] += v * v;
    }
  for (float &s : fit.scale)
    s = std::max(1e-3f, std::sqrt(s / total));

  // Positives are few and matter as much as the many negatives.
  const double posWeight =
      std::min(20.0, double(negatives.size()) / double(positives.size()));
  fit.weights = QVector<float>(dims, 0.0f);
  QVector<double> m(dims + 1, 0.0), v(dims + 1, 0.0);
  constexpr double rate = 0.01, beta1 = 0.9, beta2 = 0.999, eps = 1e-8;
  QVector<int> order(total);
  std::iota(order.begin(), order.end(), 0);
  QRandomGenerator random(seed);
  QVector<float> x(dims);
  QVector<double> grad(dims + 1);
  int step = 0;
  constexpr int batch = 128;
  for (int epoch = 0; epoch < epochs; ++epoch) {
    std::shuffle(order.begin(), order.end(), random);
    for (int start = 0; start < total; start += batch) {
      std::fill(grad.begin(), grad.end(), 0.0);
      const int end = std::min(total, start + batch);
      double norm = 0.0;
      for (int k = start; k < end; ++k) {
        const int i = order[k];
        const QVector<float> &r = row(i);
        double z = fit.bias;
        for (int d = 0; d < dims; ++d) {
          x[d] = (r[d] - fit.mean[d]) / fit.scale[d];
          z += fit.weights[d] * x[d];
        }
        const bool positive = i < positives.size();
        const double w = positive ? posWeight : 1.0;
        const double error = (1.0 / (1.0 + std::exp(-z)) - (positive ? 1.0 : 0.0)) * w;
        for (int d = 0; d < dims; ++d)
          grad[d] += error * x[d];
        grad[dims] += error;
        norm += w;
      }
      ++step;
      for (int d = 0; d <= dims; ++d) {
        double g = grad[d] / norm;
        if (d < dims)
          g += l2 * fit.weights[d];
        m[d] = beta1 * m[d] + (1 - beta1) * g;
        v[d] = beta2 * v[d] + (1 - beta2) * g * g;
        const double mh = m[d] / (1 - std::pow(beta1, step));
        const double vh = v[d] / (1 - std::pow(beta2, step));
        const double delta = rate * mh / (std::sqrt(vh) + eps);
        if (d < dims)
          fit.weights[d] -= float(delta);
        else
          fit.bias -= float(delta);
      }
    }
  }
  return fit;
}

float predict(const Fit &fit, const float *features, int count) {
  if (fit.weights.size() != count)
    return 0.0f;
  float z = fit.bias;
  for (int i = 0; i < count; ++i)
    z += fit.weights[i] * (features[i] - fit.mean[i]) / fit.scale[i];
  return sigmoid(z);
}

QVector<QVector<float>> Trainer::windows(const Clip &clip, int count,
                                         bool tailOnly, const Clip &before) {
  QVector<QVector<float>> out;
  if (!m_features || !m_features->loaded())
    return out;
  m_features->reset();
  // Enough lead-in for the window to be full of real audio by the time the
  // clip starts, and a short tail so the clip's end is seen settling.
  const int lead = (count + kWarmup) * kChunk;
  Clip stream = before.isEmpty() ? augment::pinkNoise(lead, 0.004, 7) : before;
  while (stream.size() < lead)
    stream += stream;
  stream = stream.mid(stream.size() - lead);
  const int clipEnd = int(stream.size() + clip.size());
  stream += clip;
  stream += augment::pinkNoise(kChunk * 5, 0.004, 11);

  int fed = 0, embeddings = 0;
  QVector<float> buffer(count * kEmbedding);
  while (fed + kChunk <= stream.size()) {
    const int made = m_features->push(stream.constData() + fed, kChunk);
    fed += kChunk;
    embeddings += made;
    if (made == 0 || embeddings < count + kWarmup ||
        !m_features->window(count, buffer.data()))
      continue;
    if (tailOnly) {
      // The detector should fire just after the phrase ends: take the
      // windows ending between its end and a quarter of a second later.
      const int after = fed - clipEnd;
      if (after < 0 || after > 4 * kChunk)
        continue;
    }
    out.append(buffer);
  }
  return out;
}

QVector<QPair<int, Embedding>> Trainer::stream(const Clip &clip,
                                               const Clip &before) {
  QVector<QPair<int, Embedding>> out;
  if (!m_features || !m_features->loaded())
    return out;
  m_features->reset();
  const int lead = (24 + kWarmup) * kChunk;
  Clip audio = before.isEmpty() ? augment::pinkNoise(lead, 0.004, 7) : before;
  while (audio.size() < lead)
    audio += audio;
  audio = audio.mid(audio.size() - lead);
  const int start = int(audio.size());
  audio += clip;
  audio += augment::pinkNoise(kChunk * 6, 0.004, 13);
  int fed = 0, made = 0;
  float frame[kEmbedding];
  while (fed + kChunk <= audio.size()) {
    const int n = m_features->push(audio.constData() + fed, kChunk);
    fed += kChunk;
    made += n;
    if (n == 0 || made < kWarmup || !m_features->window(1, frame))
      continue;
    Embedding e;
    std::copy(frame, frame + kEmbedding, e.begin());
    out.append({fed - start, e});
  }
  return out;
}

QVector<QVector<float>> Trainer::tailWindows(const Clip &phrase,
                                             const Clip &withTail, int count,
                                             const Clip &before) {
  // Like windows(..., tailOnly), but the phrase may be followed by more
  // audio: the windows wanted are those ending just after the phrase does.
  QVector<QVector<float>> out;
  if (!m_features || !m_features->loaded())
    return out;
  m_features->reset();
  const int lead = (count + kWarmup) * kChunk;
  Clip stream = before.isEmpty() ? augment::pinkNoise(lead, 0.004, 7) : before;
  while (stream.size() < lead)
    stream += stream;
  stream = stream.mid(stream.size() - lead);
  const int phraseEnd = int(stream.size() + phrase.size());
  stream += withTail;
  stream += augment::pinkNoise(kChunk * 5, 0.004, 11);
  int fed = 0, embeddings = 0;
  QVector<float> buffer(count * kEmbedding);
  while (fed + kChunk <= stream.size()) {
    const int made = m_features->push(stream.constData() + fed, kChunk);
    fed += kChunk;
    embeddings += made;
    if (made == 0 || embeddings < count + kWarmup ||
        !m_features->window(count, buffer.data()))
      continue;
    const int after = fed - phraseEnd;
    if (after >= 0 && after <= 3 * kChunk)
      out.append(buffer);
  }
  return out;
}

Model Trainer::train(const QString &phrase, const QVector<Clip> &positives,
                     const QVector<Clip> &negatives, TrainReport *report,
                     quint32 seed, const QVector<Clip> &synthetic) {
  QElapsedTimer clock;
  clock.start();
  TrainReport local;
  TrainReport &r = report ? *report : local;
  r = TrainReport{};
  Model model;
  if (!m_features || !m_features->loaded()) {
    r.error = QStringLiteral("the wake-word feature models are not loaded");
    return model;
  }
  QVector<Clip> clean;
  int longest = 0;
  for (const Clip &clip : positives) {
    const Clip t = augment::trim(clip);
    if (t.size() < kRate / 5)
      continue; // under 200 ms is not a phrase
    clean << t;
    longest = std::max(longest, int(t.size()));
  }
  if (clean.size() < 3) {
    r.error = QStringLiteral("need at least three usable recordings of the phrase");
    return model;
  }
  // Long enough to hear the whole phrase, plus a little either side.
  const int span =
      std::clamp(int(std::ceil(double(longest) / kChunk)) + 3, 12, 24);
  QRandomGenerator random(seed);

  // Background to set phrases in: the user's own negatives where there are
  // any (their room, their voice), pink noise otherwise.
  Clip room;
  for (const Clip &clip : negatives)
    room += clip;
  if (room.size() < kRate)
    room += augment::pinkNoise(kRate * 4, 0.01, seed);
  const auto lead = [&] {
    return augment::mix(augment::pinkNoise(kRate * 3, 0.002, random.generate()),
                        room, 25.0 + random.generateDouble() * 10.0,
                        random.generate());
  };

  // Speech that is not the phrase, cut into pieces. Every piece ends
  // somewhere, so the classifier sees plenty of "someone just stopped
  // talking" that is not her name -- without these it learns to fire on the
  // end of any sentence.
  QVector<Clip> speech;
  for (const Clip &clip : negatives)
    if (clip.size() > kRate / 2)
      speech << clip;
  QVector<Clip> pieces;
  for (const Clip &clip : speech)
    for (int i = 0; i < int(clip.size()) / (kRate / 2); ++i) {
      const int length = int(kRate * (0.4 + random.generateDouble() * 1.4));
      if (clip.size() <= length)
        continue;
      const int from = random.bounded(int(clip.size()) - length);
      pieces << clip.mid(from, length);
    }

  const auto variants = [&](const Clip &base, int n, quint32 s) {
    QVector<Clip> out{base};
    QRandomGenerator rng(s);
    for (int i = 0; i < n; ++i) {
      Clip c = augment::speed(base, 0.9 + rng.generateDouble() * 0.2);
      c = augment::gain(c, rng.generateDouble() * 14.0 - 8.0);
      if (rng.bounded(2) == 0)
        c = augment::reverb(c, 0.15 + rng.generateDouble() * 0.45, rng.generate());
      c = augment::mix(c, room, 8.0 + rng.generateDouble() * 22.0, rng.generate());
      out << c;
    }
    return out;
  };
  // Half the time the phrase runs straight on into a request ("Hey Nala,
  // open Discord"), which must still count.
  const auto followed = [&](const Clip &clip, quint32 s) {
    QRandomGenerator rng(s);
    if (pieces.isEmpty() || rng.bounded(2) == 0)
      return clip;
    const Clip &next = pieces[rng.bounded(int(pieces.size()))];
    Clip out = clip;
    out += Clip(int(kRate * rng.generateDouble() * 0.15), 0);
    return out + next.mid(0, kChunk * 4);
  };

  // Hold one recording back to measure against, when there are enough.
  const int heldOut = clean.size() >= 4 ? int(clean.size()) - 1 : -1;
  QVector<QVector<float>> pos, neg, valPos, valNeg;
  for (int i = 0; i < clean.size(); ++i) {
    auto &target = i == heldOut ? valPos : pos;
    int k = 0;
    for (const Clip &variant : variants(clean[i], 16, seed + 17 * quint32(i))) {
      // The phrase ends where the variant does, before anything that follows.
      const Clip withTail = followed(variant, seed + 31 * quint32(i) + quint32(k++));
      target += tailWindows(variant, withTail, span, lead());
    }
  }

  // Other voices: fewer variations each, as there are many of them.
  for (int i = 0; i < synthetic.size(); ++i) {
    const Clip t = augment::trim(synthetic[i]);
    if (t.size() < kRate / 5 || t.size() > kChunk * (span - 2))
      continue;
    int k = 0;
    for (const Clip &variant : variants(t, 2, seed + 104729 * quint32(i))) {
      const Clip withTail = followed(variant, seed + 7 * quint32(i) + quint32(k++));
      pos += tailWindows(variant, withTail, span, lead());
    }
  }

  // Hard negatives: the phrase's own pieces. "Hey Nala" must not fire on
  // "hey" or on "Nala" alone, or on the phrase cut short.
  QVector<Clip> hard;
  for (const Clip &clip : clean) {
    const int n = int(clip.size());
    hard << clip.mid(0, n * 45 / 100) << clip.mid(n * 55 / 100)
         << clip.mid(0, n * 70 / 100);
  }
  for (int i = 0; i < hard.size(); ++i)
    for (const Clip &variant : variants(hard[i], 3, seed + 991 * quint32(i)))
      neg += windows(variant, span, true, lead());

  // Everything else: the negatives as a stream, every window; and each
  // piece of speech on its own, ending, in a few variations. Every sixth is
  // held out for measuring.
  for (int i = 0; i < negatives.size(); ++i) {
    auto &target = i % 6 == 5 ? valNeg : neg;
    target += windows(negatives[i], span, false, lead());
  }
  for (int i = 0; i < pieces.size(); ++i) {
    auto &target = i % 6 == 5 ? valNeg : neg;
    for (const Clip &variant : variants(pieces[i], 2, seed + 7919 * quint32(i)))
      target += windows(variant, span, true, lead());
  }
  // Plain noise at a few levels, so silence and hiss are never her name.
  for (int i = 0; i < 4; ++i)
    neg += windows(augment::pinkNoise(kRate * 6, 0.002 * (1 << (2 * i)),
                                      seed + 5000 + quint32(i)),
                   span, false, lead());

  // Keep training fast: at most ~12k of these.
  if (neg.size() > 12000) {
    std::shuffle(neg.begin(), neg.end(), random);
    neg.resize(12000);
  }

  // Many voices, rooms and songs that are not the phrase. The last 15% of
  // the stream is never trained on: it is the benchmark.
  qint64 benchStart = 0;
  if (m_bank && m_bank->loaded() && m_bank->frames() > span * 100) {
    benchStart = m_bank->frames() * 85 / 100;
    const int wanted = 15000;
    for (int i = 0; i < wanted; ++i) {
      const qint64 at = qint64(random.generateDouble() * double(benchStart - span));
      QVector<float> w(span * kEmbedding);
      std::copy(m_bank->frame(at), m_bank->frame(at + span), w.begin());
      neg << w;
    }
  }
  if (pos.isEmpty() || neg.isEmpty()) {
    r.error = QStringLiteral("not enough audio to learn from");
    return model;
  }

  // Templates: each recording's own embeddings, from a little after it
  // starts (every embedding looks back 760 ms) to just after it ends.
  QVector<float> center(kEmbedding, 0.0f);
  {
    qint64 n = 0;
    QVector<double> sum(kEmbedding, 0.0);
    if (m_bank && m_bank->loaded()) {
      for (qint64 at = 0; at < m_bank->frames(); at += 97, ++n)
        for (int d = 0; d < kEmbedding; ++d)
          sum[d] += m_bank->frame(at)[d];
    } else {
      for (const auto &w : neg)
        for (int f = 0; f < span; ++f, ++n)
          for (int d = 0; d < kEmbedding; ++d)
            sum[d] += w[f * kEmbedding + d];
    }
    for (int d = 0; d < kEmbedding; ++d)
      center[d] = n ? float(sum[d] / n) : 0.0f;
  }
  const auto templateOf = [&](const Clip &clip) {
    QVector<float> frames;
    const auto streamed = stream(clip, lead());
    const int from = int(kRate * 0.3), to = int(clip.size()) + kChunk * 2;
    for (const auto &[at, e] : streamed)
      if (at >= from && at <= to)
        frames += QVector<float>(e.begin(), e.end());
    // Centre and normalise, as matching expects.
    QVector<float> out(frames.size());
    for (int f = 0; f < frames.size() / kEmbedding; ++f) {
      double norm = 0.0;
      for (int d = 0; d < kEmbedding; ++d) {
        const float v = frames[f * kEmbedding + d] - center[d];
        out[f * kEmbedding + d] = v;
        norm += double(v) * v;
      }
      const float k = norm > 1e-12 ? float(1.0 / std::sqrt(norm)) : 0.0f;
      for (int d = 0; d < kEmbedding; ++d)
        out[f * kEmbedding + d] *= k;
    }
    return out;
  };
  QVector<QVector<float>> templates;
  int longestTemplate = 1;
  for (const Clip &clip : clean) {
    QVector<float> t = templateOf(clip);
    if (t.size() >= 3 * kEmbedding) {
      longestTemplate = std::max(longestTemplate, int(t.size() / kEmbedding));
      templates << t;
    }
  }
  const int query = std::clamp(longestTemplate * 3 / 2 + 2, 8, 40);

  // Measured on held-out speech: stronger regularisation trades recall for
  // false wakes without improving either.
  constexpr double l2 = 1e-3;
  Fit fit = fitLogistic(pos, neg, 30, l2, seed);

  // Calibration positives: each recording (or the held-out one), scored
  // against templates from the *other* recordings, in the variants and
  // with the follow-on speech the detector will actually meet. For each
  // variant, the best (classifier, template) pair near its end.
  struct Pair {
    float lr, match;
  };
  QVector<Pair> calPos;
  {
    Model probe;
    probe.center = center;
    probe.query = query;
    const QVector<int> subjects =
        heldOut >= 0 ? QVector<int>{heldOut} : [&] {
          QVector<int> all;
          for (int i = 0; i < clean.size(); ++i)
            all << i;
          return all;
        }();
    for (int subject : subjects) {
      probe.templates.clear();
      for (int i = 0; i < templates.size(); ++i)
        if (i != subject)
          probe.templates << templates[i];
      int k = 0;
      for (const Clip &variant : variants(clean[subject], 12, seed + 4099 * quint32(subject))) {
        const Clip withTail = followed(variant, seed + 131 * quint32(k++));
        const auto streamed = stream(withTail, lead());
        QVector<float> buffer;
        Pair bestPair{0.0f, 0.0f};
        float best = -1.0f;
        for (int i = 0; i < streamed.size(); ++i) {
          const int after = streamed[i].first - int(variant.size());
          if (after < 0 || after > 3 * kChunk || i + 1 < std::max(span, query))
            continue;
          buffer.resize(span * kEmbedding);
          for (int f = 0; f < span; ++f)
            std::copy(streamed[i - span + 1 + f].second.begin(),
                      streamed[i - span + 1 + f].second.end(),
                      buffer.begin() + f * kEmbedding);
          const float lr = predict(fit, buffer.constData(), int(buffer.size()));
          QVector<float> q(query * kEmbedding);
          for (int f = 0; f < query; ++f)
            std::copy(streamed[i - query + 1 + f].second.begin(),
                      streamed[i - query + 1 + f].second.end(),
                      q.begin() + f * kEmbedding);
          const float m = probe.hasTemplates() ? probe.match(q.constData(), query) : 1.0f;
          if (std::min(lr, m) > best) {
            best = std::min(lr, m);
            bestPair = {lr, m};
          }
        }
        if (best >= 0.0f)
          calPos << bestPair;
      }
    }
  }

  // Calibration negatives: the held-out real-world audio, every window,
  // scored by both tests. Matching is the costly part; spread it out.
  QVector<Pair> calNeg;
  double hours = 0.0;
  if (benchStart > 0) {
    Model probe;
    probe.center = center;
    probe.templates = templates;
    const int context = std::max(span, query);
    QVector<qint64> ends;
    for (qint64 at = benchStart + context; at <= m_bank->frames(); ++at)
      ends << at;
    calNeg = QtConcurrent::blockingMapped(ends, [&](qint64 end) {
      QVector<float> w(span * kEmbedding);
      std::copy(m_bank->frame(end - span), m_bank->frame(end), w.begin());
      const float lr = predict(fit, w.constData(), int(w.size()));
      // Only windows the classifier would pass need matching.
      if (lr < 0.3f)
        return Pair{lr, 0.0f};
      QVector<float> q(query * kEmbedding);
      std::copy(m_bank->frame(end - query), m_bank->frame(end), q.begin());
      return Pair{lr, probe.hasTemplates() ? probe.match(q.constData(), query) : 1.0f};
    });
    hours = calNeg.size() * double(kChunk) / kRate / 3600.0;
    r.benchmarkHours = hours;
  }

  // Search both thresholds together: the most of the user's own phrase
  // that can be caught at no more than half a false wake an hour.
  const auto falseWakes = [&](float t, float mt) {
    Gate gate(Gate::Config{t, 1, 2000});
    int n = 0;
    for (int i = 0; i < calNeg.size(); ++i) {
      const float c = std::min(calNeg[i].lr, calNeg[i].match - mt + t);
      n += gate.update(c, qint64(i) * 80);
    }
    return n;
  };
  const auto recallAt = [&](float t, float mt) {
    int hit = 0;
    for (const Pair &p : calPos)
      hit += p.lr >= t && p.match >= mt;
    return calPos.isEmpty() ? 0.0 : double(hit) / calPos.size();
  };
  float threshold = 0.9f, matchThreshold = templates.isEmpty() ? 0.0f : 0.9f;
  double bestRecall = -1.0, bestFa = 1e9;
  const QVector<float> mts = templates.isEmpty() ? QVector<float>{0.0f} : [] {
    QVector<float> v;
    for (int i = 30; i <= 95; i += 1)
      v << i / 100.0f;
    return v;
  }();
  struct Candidate {
    float t, mt;
    double recall, fa;
  };
  QVector<Candidate> feasible;
  for (int ti = 30; ti <= 98; ti += 2) {
    const float t = ti / 100.0f;
    for (float mt : mts) {
      const double recall = recallAt(t, mt);
      if (recall + 1e-9 < bestRecall - 0.05)
        continue;
      const double fa = hours > 0.0 ? falseWakes(t, mt) / hours : 0.0;
      if (fa > 0.5)
        continue;
      feasible << Candidate{t, mt, recall, fa};
      bestRecall = std::max(bestRecall, recall);
    }
  }
  // Of the pairs that catch the most, take the middle of the region rather
  // than its edge: margin against false wakes on one side, and against the
  // user sounding a little different tomorrow on the other.
  QVector<Candidate> top;
  for (const Candidate &c : feasible)
    if (c.recall + 1e-9 >= bestRecall)
      top << c;
  if (!top.isEmpty()) {
    QVector<float> ts;
    for (const Candidate &c : top)
      ts << c.t;
    std::sort(ts.begin(), ts.end());
    // The middle measured best: stricter placements lost recall faster
    // than they removed false wakes.
    constexpr double place = 0.5;
    threshold = ts[std::min(int(ts.size()) - 1, int(ts.size() * place))];
    QVector<float> ms;
    for (const Candidate &c : top)
      if (qFuzzyCompare(c.t, threshold))
        ms << c.mt;
    std::sort(ms.begin(), ms.end());
    matchThreshold = ms[std::min(int(ms.size()) - 1, int(ms.size() * place))];
    bestFa = hours > 0.0 ? falseWakes(threshold, matchThreshold) / hours : 0.0;
    bestRecall = recallAt(threshold, matchThreshold);
  } else {
    bestRecall = -1.0;
  }
  if (bestRecall < 0.0) {
    // Nothing met the target: be strict and say so.
    threshold = 0.98f;
    matchThreshold = templates.isEmpty() ? 0.0f : 0.95f;
    bestRecall = recallAt(threshold, matchThreshold);
    bestFa = hours > 0.0 ? falseWakes(threshold, matchThreshold) / hours : -1.0;
  }
  r.recall = calPos.isEmpty() ? -1.0 : bestRecall;
  r.falseAcceptsPerHour = hours > 0.0 ? bestFa : -1.0;
  {
    int rejected = 0;
    QVector<float> negScores;
    for (const auto &w : valNeg)
      rejected += predict(fit, w.constData(), int(w.size())) >= threshold;
    r.falseAcceptRate = valNeg.isEmpty() ? 0.0 : double(rejected) / valNeg.size();
  }

  // The final fit sees everything.
  pos += valPos;
  neg += valNeg;
  fit = fitLogistic(pos, neg, 30, l2, seed + 1);

  model.phrase = phrase.toLower().simplified();
  model.windows = span;
  model.mean = fit.mean;
  model.scale = fit.scale;
  model.weights = fit.weights;
  model.bias = fit.bias;
  model.threshold = threshold;
  model.templates = templates;
  model.center = center;
  model.matchThreshold = matchThreshold;
  model.query = query;
  model.trained = QDateTime::currentDateTime();

  r.ok = true;
  r.positives = int(pos.size());
  r.negatives = int(neg.size());
  r.threshold = threshold;
  r.milliseconds = clock.elapsed();
  model.stats = QJsonObject{{"recordings", int(clean.size())},
                            {"matchThreshold", double(matchThreshold)},
                            {"positives", r.positives},
                            {"negatives", r.negatives},
                            {"recall", r.recall},
                            {"falseAcceptRate", r.falseAcceptRate},
                            {"falseAcceptsPerHour", r.falseAcceptsPerHour},
                            {"benchmarkHours", r.benchmarkHours},
                            {"ms", r.milliseconds}};
  return model;
}

// --- gate -------------------------------------------------------------------------

bool Gate::update(float score, qint64 nowMs) {
  if (m_suspended) {
    m_run = 0;
    return false;
  }
  // Nothing heard during a cooldown or the grace after she spoke counts
  // towards a detection: the phrase has to be heard afresh afterwards.
  if (nowMs < m_coolUntil) {
    m_run = 0;
    return false;
  }
  m_run = score >= m_config.threshold ? m_run + 1 : 0;
  if (m_run < m_config.consecutive)
    return false;
  m_run = 0;
  m_coolUntil = nowMs + m_config.cooldownMs;
  return true;
}

void Gate::suspend(qint64) {
  m_suspended = true;
  m_run = 0;
}

void Gate::resume(qint64 nowMs, int graceMs) {
  m_suspended = false;
  m_run = 0;
  m_coolUntil = std::max(m_coolUntil, nowMs + graceMs);
}

bool Gate::suspended(qint64) const { return m_suspended; }

// --- neural backend -----------------------------------------------------------------

NeuralBackend::NeuralBackend(QString featureDir, QObject *parent)
    : WakeWordBackend(parent), m_featureDir(std::move(featureDir)) {}

NeuralBackend::~NeuralBackend() = default;

bool NeuralBackend::initialize(QString *error) {
  if (m_features.loaded())
    return true;
  if (!m_features.load(m_featureDir, error))
    return false;
  return true;
}

bool NeuralBackend::ready() const {
  return m_features.loaded() && !m_entries.isEmpty();
}

void NeuralBackend::start() {
  m_running = true;
  m_features.reset();
}

void NeuralBackend::stop() { m_running = false; }

void NeuralBackend::pause() {
  for (Entry &e : m_entries)
    e.gate.suspend(m_clockMs);
}

void NeuralBackend::resume(int graceMs) {
  for (Entry &e : m_entries)
    e.gate.resume(m_clockMs, graceMs);
}

void NeuralBackend::applyGates() {
  for (Entry &e : m_entries) {
    Gate::Config config;
    // Sensitivity moves the trained threshold: 0.5 leaves it where training
    // put it.
    config.threshold = std::clamp(
        e.model.threshold + float((0.5 - m_sensitivity) * 0.4), 0.05f, 0.99f);
    config.consecutive = m_consecutive;
    config.cooldownMs = m_cooldownMs;
    e.gate.setConfig(config);
  }
}

bool NeuralBackend::registerWakeword(const Model &model) {
  if (!model.valid())
    return false;
  removeWakeword(model.phrase);
  m_entries.append({model, Gate{}});
  applyGates();
  return true;
}

void NeuralBackend::removeWakeword(const QString &phrase) {
  m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                 [&](const Entry &e) {
                                   return e.model.phrase == phrase;
                                 }),
                  m_entries.end());
}

QStringList NeuralBackend::wakewords() const {
  QStringList out;
  for (const Entry &e : m_entries)
    out << e.model.phrase;
  return out;
}

void NeuralBackend::setSensitivity(double sensitivity) {
  m_sensitivity = std::clamp(sensitivity, 0.0, 1.0);
  applyGates();
}

void NeuralBackend::setConfirmation(int consecutive, int cooldownMs) {
  m_consecutive = std::max(1, consecutive);
  m_cooldownMs = std::max(0, cooldownMs);
  applyGates();
}

void NeuralBackend::processAudio(const int16_t *samples, int count) {
  if (!m_running || !m_features.loaded() || m_entries.isEmpty())
    return;
  // Feed in chunk-sized pieces so every embedding gets scored, whatever
  // size of buffer the sound card hands over.
  int at = 0;
  while (at < count) {
    const int take = std::min(kChunk, count - at);
    QElapsedTimer clock;
    clock.start();
    const int made = m_features.push(samples + at, take);
    at += take;
    m_clockMs += qint64(take) * 1000 / kRate;
    if (made == 0)
      continue;
    for (Entry &e : m_entries) {
      const int context = std::max(e.model.windows, e.model.query);
      m_window.resize(context * kEmbedding);
      if (!m_features.window(context, m_window.data()))
        continue;
      const float *newest = m_window.constData() + (context - e.model.windows) * kEmbedding;
      const float classified = e.model.classify(newest);
      // Matching costs more; only bother when the classifier is interested.
      const float matched =
          classified >= 0.3f
              ? e.model.match(m_window.constData() + (context - e.model.query) * kEmbedding,
                              e.model.query)
              : 0.0f;
      const float score = e.model.combine(classified, matched);
      emit scored(e.model.phrase, score, e.gate.config().threshold);
      if (e.gate.update(score, m_clockMs)) {
        Detection detection;
        detection.phrase = e.model.phrase;
        detection.score = score;
        detection.latencyMs = clock.elapsed();
        emit detected(detection);
      }
    }
  }
}

} // namespace wake
