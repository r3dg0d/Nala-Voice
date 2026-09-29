#pragma once
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

// What the local inference server and the GPU look like right now. The parsers
// are pure (and tested); the queries are asynchronous and never block the UI.
namespace catalog {

struct Installed {
  QString name;
  qint64 sizeBytes = 0;   // on disk
  QString parameterSize;  // "27B"
  QString quantization;   // "Q4_K_M"
};

struct Loaded {
  QString name;
  qint64 sizeBytes = 0;     // total in memory
  qint64 vramBytes = 0;     // of which on the GPU
  QString expires;
  int contextLength = 0; // the window it was loaded with (Ollama /api/ps)
  // 100 = entirely on the GPU. Below that, part of it runs from system RAM,
  // which is what makes an interactive voice reply slow.
  int gpuPercent() const {
    return sizeBytes > 0 ? int(100 * vramBytes / sizeBytes) : 0;
  }
};

struct Gpu {
  bool present = false;
  QString name;
  qint64 usedMiB = 0;
  qint64 totalMiB = 0;
  int utilization = 0; // percent
  qint64 freeMiB() const { return totalMiB - usedMiB; }
};

// Ollama /api/tags and /api/ps.
QVector<Installed> parseTags(const QJsonObject &json);
QVector<Loaded> parsePs(const QJsonObject &json);
// `nvidia-smi --query-gpu=name,memory.used,memory.total,utilization.gpu
//  --format=csv,noheader,nounits`, first GPU.
Gpu parseNvidiaSmi(const QString &csv);

// Is there room on the GPU for `neededBytes` more, keeping `marginMiB` spare?
bool fits(const Gpu &gpu, qint64 neededBytes, qint64 marginMiB = 1024);

// Which loaded models to unload so `wanted` can load fully on the GPU: every
// other loaded model, largest first, until it fits. Never lists `wanted`.
QStringList evictionPlan(const Gpu &gpu, const QVector<Loaded> &loaded,
                         const QString &wanted, qint64 wantedBytes,
                         qint64 marginMiB = 1024);

QString formatBytes(qint64 bytes);

// Ollama's own OLLAMA_HOST ("127.0.0.1:11435", ":11435", "myhost", "0.0.0.0",
// "http://box:11434") as the OpenAI-style endpoint Nala uses. Empty when it
// says nothing usable.
QString endpointFromOllamaHost(const QString &host);

// nvidia-smi, asynchronously. Calls back with present=false when it is missing
// or fails; never blocks.
void queryGpu(QObject *context, std::function<void(Gpu)> done,
              const QString &binary = QStringLiteral("nvidia-smi"));

} // namespace catalog
