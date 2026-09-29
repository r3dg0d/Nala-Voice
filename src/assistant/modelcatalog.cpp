#include "modelcatalog.h"

#include <QJsonArray>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <algorithm>

namespace catalog {

QVector<Installed> parseTags(const QJsonObject &json) {
  QVector<Installed> out;
  for (const QJsonValue &v : json.value("models").toArray()) {
    const QJsonObject m = v.toObject();
    Installed i;
    i.name = m.value("name").toString();
    if (i.name.isEmpty())
      i.name = m.value("model").toString();
    if (i.name.isEmpty())
      continue;
    i.sizeBytes = qint64(m.value("size").toDouble());
    const QJsonObject d = m.value("details").toObject();
    i.parameterSize = d.value("parameter_size").toString();
    i.quantization = d.value("quantization_level").toString();
    out.append(i);
  }
  return out;
}

QVector<Loaded> parsePs(const QJsonObject &json) {
  QVector<Loaded> out;
  for (const QJsonValue &v : json.value("models").toArray()) {
    const QJsonObject m = v.toObject();
    Loaded l;
    l.name = m.value("name").toString();
    if (l.name.isEmpty())
      l.name = m.value("model").toString();
    if (l.name.isEmpty())
      continue;
    l.sizeBytes = qint64(m.value("size").toDouble());
    l.vramBytes = qint64(m.value("size_vram").toDouble());
    l.expires = m.value("expires_at").toString();
    l.contextLength = m.value("context_length").toInt();
    out.append(l);
  }
  return out;
}

Gpu parseNvidiaSmi(const QString &csv) {
  Gpu gpu;
  const QString line = csv.split('\n', Qt::SkipEmptyParts).value(0).trimmed();
  const QStringList f = line.split(',');
  if (f.size() < 3)
    return gpu;
  bool a = false, b = false;
  gpu.name = f.at(0).trimmed();
  gpu.usedMiB = f.at(1).trimmed().toLongLong(&a);
  gpu.totalMiB = f.at(2).trimmed().toLongLong(&b);
  gpu.utilization = f.size() > 3 ? f.at(3).trimmed().toInt() : 0;
  gpu.present = a && b && gpu.totalMiB > 0;
  return gpu;
}

bool fits(const Gpu &gpu, qint64 neededBytes, qint64 marginMiB) {
  if (!gpu.present)
    return true; // nothing to protect, or nothing we can measure
  return gpu.freeMiB() - marginMiB >= neededBytes / (1024 * 1024);
}

QStringList evictionPlan(const Gpu &gpu, const QVector<Loaded> &loaded,
                         const QString &wanted, qint64 wantedBytes,
                         qint64 marginMiB) {
  QStringList plan;
  if (!gpu.present)
    return plan;
  qint64 freeMiB = gpu.freeMiB();
  const qint64 needMiB = wantedBytes / (1024 * 1024) + marginMiB;
  if (freeMiB >= needMiB)
    return plan;
  QVector<Loaded> others;
  for (const Loaded &l : loaded)
    if (l.name != wanted)
      others.append(l);
  std::sort(others.begin(), others.end(), [](const Loaded &a, const Loaded &b) {
    return a.vramBytes > b.vramBytes;
  });
  for (const Loaded &l : others) {
    if (freeMiB >= needMiB)
      break;
    plan << l.name;
    freeMiB += l.vramBytes / (1024 * 1024);
  }
  return plan;
}

QString formatBytes(qint64 bytes) {
  if (bytes >= (1LL << 30))
    return QStringLiteral("%1 GB").arg(double(bytes) / double(1LL << 30), 0, 'f', 1);
  if (bytes >= (1LL << 20))
    return QStringLiteral("%1 MB").arg(bytes >> 20);
  return QStringLiteral("%1 B").arg(bytes);
}

void queryGpu(QObject *context, std::function<void(Gpu)> done,
              const QString &binary) {
  auto *process = new QProcess(context);
  QPointer<QProcess> guard(process);
  QObject::connect(process, &QProcess::finished, context,
                   [process, done](int code, QProcess::ExitStatus status) {
                     Gpu gpu;
                     if (status == QProcess::NormalExit && code == 0)
                       gpu = parseNvidiaSmi(
                           QString::fromUtf8(process->readAllStandardOutput()));
                     process->deleteLater();
                     done(gpu);
                   });
  QObject::connect(process, &QProcess::errorOccurred, context,
                   [process, done](QProcess::ProcessError error) {
                     if (error != QProcess::FailedToStart)
                       return;
                     process->deleteLater();
                     done(Gpu{});
                   });
  process->start(binary,
                 {"--query-gpu=name,memory.used,memory.total,utilization.gpu",
                  "--format=csv,noheader,nounits"});
  // A hung driver must not leave this pending forever.
  QTimer::singleShot(4000, context, [guard, done] {
    if (guard && guard->state() != QProcess::NotRunning) {
      guard->disconnect();
      guard->kill();
      guard->deleteLater();
      done(Gpu{});
    }
  });
}

} // namespace catalog
