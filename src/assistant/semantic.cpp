#include "semantic.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
namespace semantic {
QByteArray encode(const QVector<float> &values) {
  if (values.isEmpty() || values.size() > 65536)
    return {};
  QByteArray bytes;
  QDataStream stream(&bytes, QIODevice::WriteOnly);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
  for (float value : values) {
    if (!std::isfinite(value))
      return {};
    stream << value;
  }
  return bytes;
}
QVector<float> decode(const QByteArray &bytes) {
  if (bytes.isEmpty() || bytes.size() % 4 || bytes.size() > 65536 * 4)
    return {};
  QDataStream stream(bytes);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
  QVector<float> values;
  while (!stream.atEnd()) {
    float value;
    stream >> value;
    if (!std::isfinite(value))
      return {};
    values << value;
  }
  return values;
}
double cosine(const QVector<float> &a, const QVector<float> &b) {
  if (a.isEmpty() || a.size() != b.size())
    return -1;
  double dot = 0, x = 0, y = 0;
  for (int i = 0; i < a.size(); ++i) {
    if (!std::isfinite(a[i]) || !std::isfinite(b[i]))
      return -1;
    dot += double(a[i]) * b[i];
    x += double(a[i]) * a[i];
    y += double(b[i]) * b[i];
  }
  return x > 0 && y > 0 ? dot / std::sqrt(x * y) : -1;
}
QMap<qint64, double> fuse(const QVector<QVector<qint64>> &channels, int k) {
  QMap<qint64, double> scores;
  k = std::max(1, k);
  for (const auto &channel : channels) {
    QSet<qint64> seen;
    int rank = 0;
    for (qint64 id : channel) {
      if (id <= 0 || seen.contains(id))
        continue;
      seen.insert(id);
      scores[id] += 1.0 / (k + (++rank));
    }
  }
  return scores;
}
QString checkedAnswer(QString answer, const QSet<QString> &evidence,
                      bool debug) {
  const QRegularExpression citation("\\[(memory|fact|artifact):([0-9]+)\\]");
  auto matches = citation.globalMatch(answer);
  bool cited = false;
  while (matches.hasNext()) {
    const auto match = matches.next();
    if (!evidence.contains(match.captured(1) + ":" + match.captured(2)))
      return "I couldn't verify that answer against the retrieved memory "
             "evidence.";
    cited = true;
  }
  if (!cited || evidence.isEmpty())
    return "I couldn't find enough cited memory evidence to answer that "
           "reliably.";
  if (!debug)
    answer.remove(citation);
  return answer.simplified();
}
QString hash(const QString &text) {
  return QString::fromLatin1(
      QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256)
          .toHex());
}
QString intent(const QString &query) {
  const QString q = query.toLower();
  if (!QRegularExpression("^(what|which|where|when|find|recall|remember|search|"
                          "did|have|do)\\b")
           .match(q.trimmed())
           .hasMatch())
    return "none";
  if (QRegularExpression("\\b(prefer|preference|decid(e|ed)|remember about|my "
                         ".*configuration)\\b")
          .match(q)
          .hasMatch())
    return "durable";
  if (QRegularExpression("\\b(what|which|where|when|find|remember|search)\\b")
          .match(q)
          .hasMatch() &&
      QRegularExpression(
          "\\b(yesterday|last week|earlier|before|looked|viewed|discussed|that "
          "repo|that file|that pdf|considered|worked on)\\b")
          .match(q)
          .hasMatch())
    return q.contains("considered") ? "associative" : "episodic";
  return "none";
}
} // namespace semantic
