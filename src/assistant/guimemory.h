#pragma once
#include <QJsonArray>
#include <QJsonObject>
class MemoryStore;
namespace guimemory {
QJsonObject semanticStep(QString target, QString expected);
bool validSteps(const QJsonArray &steps);
qint64 trajectory(MemoryStore &store, QString task, QString app,
                  QJsonObject step, QJsonObject result);
qint64 workflow(MemoryStore &store, QString task, QString app,
                const QJsonArray &steps);
QJsonArray candidates(const MemoryStore &store, QString task, QString app);
QJsonArray steps(const MemoryStore &store, qint64 id, QString app);
} // namespace guimemory
