#include "guimemory.h"
#include "eventlog.h"
#include "memory.h"
#include <QJsonDocument>

namespace {
bool safe(QString text, int max = 1000) {
  return !text.trimmed().isEmpty() && text.size() <= max &&
         EventLog::redact(text) == text;
}
qint64 save(MemoryStore &store, QString source, QString task, QString app,
            QJsonObject payload) {
  if (!safe(task) || !safe(app, 300))
    return 0;
  const auto json =
      QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
  if (EventLog::redact(json) != json)
    return 0;
  MemoryRecord r;
  r.started = r.lastSeen = QDateTime::currentDateTime();
  r.source = source;
  r.app = app;
  r.title = task;
  r.summary = json;
  r.activity = "GUI procedure";
  r.keywords = task;
  // Uses the existing private store: deletion, pause, expiry and provenance
  // follow normal memory rules. No screenshots or raw coordinates are retained.
  return store.insert(r);
}
} // namespace

QJsonObject guimemory::semanticStep(QString target, QString expected) {
  if (!safe(target, 300) || !safe(expected, 300))
    return {};
  return {
      {"type", "click_semantic"}, {"target", target}, {"expected", expected}};
}
bool guimemory::validSteps(const QJsonArray &steps) {
  if (steps.size() < 2 || steps.size() > 8)
    return false;
  for (const auto &value : steps) {
    const auto s = value.toObject();
    if (s.size() != 3 || s.value("type") != "click_semantic" ||
        !safe(s.value("target").toString(), 300) ||
        !safe(s.value("expected").toString(), 300))
      return false;
  }
  return true;
}
qint64 guimemory::trajectory(MemoryStore &store, QString task, QString app,
                             QJsonObject step, QJsonObject result) {
  if (step.isEmpty())
    return 0;
  QJsonObject record{{"step", step}};
  for (const auto *key : {"ok", "verified", "retries", "refinements", "actions",
                          "grounding_method", "confidence", "observation_hash",
                          "observed_effect", "error"})
    if (result.contains(key))
      record.insert(key, result.value(key));
  return save(store, "gui-trajectory", task, app, record);
}
qint64 guimemory::workflow(MemoryStore &store, QString task, QString app,
                           const QJsonArray &steps) {
  return validSteps(steps) ? save(store, "gui-workflow", task, app,
                                  {{"steps", steps}, {"successful", true}})
                           : 0;
}
QJsonArray guimemory::candidates(const MemoryStore &store, QString task,
                                 QString app) {
  QJsonArray out;
  for (const auto &r : store.search(task, QDateTime::currentDateTime(), 20)) {
    if (r.source != "gui-workflow" || r.app != app)
      continue;
    const auto plan = QJsonDocument::fromJson(r.summary.toUtf8())
                          .object()
                          .value("steps")
                          .toArray();
    if (validSteps(plan))
      out.append(QJsonObject{{"id", double(r.id)},
                             {"task", r.title},
                             {"app", r.app},
                             {"steps", plan},
                             {"untrusted", true}});
    if (out.size() == 3)
      break;
  }
  return out;
}
QJsonArray guimemory::steps(const MemoryStore &store, qint64 id, QString app) {
  const auto r = store.get(id);
  if (r.source != "gui-workflow" || r.app != app)
    return {};
  const auto plan = QJsonDocument::fromJson(r.summary.toUtf8())
                        .object()
                        .value("steps")
                        .toArray();
  return validSteps(plan) ? plan : QJsonArray{};
}
