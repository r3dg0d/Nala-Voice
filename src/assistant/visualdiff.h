#pragma once
#include <QImage>
#include <QRect>
#include <QVector>

namespace visualdiff {
struct Delta {
  QVector<QRect> regions;
  QRect bounds;
  double changedFraction = 0;
};
// Native pixels, tiled to bound cost. Tiny controls survive overview scaling.
Delta measure(const QImage &before, const QImage &after, int tile = 16);
QImage evidence(const QImage &before, const QImage &after, const Delta &delta,
                QPoint target);
} // namespace visualdiff
