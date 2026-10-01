#pragma once
#include "policy.h"
#include <QVector>
namespace windowtarget {
struct Match {
  WindowInfo window;
  int score = 0;
};
QVector<Match> rank(const QVector<WindowInfo> &windows, QString query,
                    QString app = {}, QString title = {}, int workspace = 0);
bool ambiguous(const QVector<Match> &matches);
} // namespace windowtarget
