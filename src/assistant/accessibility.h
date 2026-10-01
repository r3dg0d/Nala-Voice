#pragma once
#include <QObject>
#include <QRect>
#include <QString>
#include <QVector>
#include <functional>

namespace accessibility {
struct Element {
  QString name, role, bus, path;
  QRect bounds;
  bool enabled = false, showing = false;
};
struct Match {
  QRect bounds;
  QString error;
};
Match select(const QVector<Element> &elements, QString label, QRect window);
// Read-only, bounded traversal on a worker. Never enables accessibility
// globally and never enumerates unrelated applications. All input stays with
// Nala policy.
void locate(QObject *owner, int pid, QRect window, QString label,
            std::function<void(Match)> done);
} // namespace accessibility
