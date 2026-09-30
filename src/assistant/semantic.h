#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>
namespace semantic {
QByteArray encode(const QVector<float> &values);
QVector<float> decode(const QByteArray &bytes);
double cosine(const QVector<float> &a, const QVector<float> &b);
QMap<qint64, double> fuse(const QVector<QVector<qint64>> &channels, int k = 60);
QString intent(const QString &query);
QString checkedAnswer(QString answer, const QSet<QString> &evidence,
                      bool debug);
QString hash(const QString &text);
} // namespace semantic
struct RetrievalHit {
  qint64 id = 0;
  int lexicalRank = 0, denseRank = 0, artifactRank = 0, entityRank = 0;
  double score = 0, similarity = 0;
};
