#include "visualdiff.h"
#include <QPainter>
#include <algorithm>
#include <cmath>

visualdiff::Delta visualdiff::measure(const QImage &before, const QImage &after,
                                      int tile) {
  Delta out;
  if (before.isNull() || after.isNull())
    return out;
  if (before.size() != after.size()) {
    out.bounds = after.rect();
    out.regions << out.bounds;
    out.changedFraction = 1;
    return out;
  }
  const auto a = before.convertToFormat(QImage::Format_RGB32);
  const auto b = after.convertToFormat(QImage::Format_RGB32);
  tile = std::max(4, tile);
  qint64 changed = 0;
  for (int y = 0; y < a.height(); y += tile) {
    for (int x = 0; x < a.width(); x += tile) {
      const QRect r = QRect(x, y, tile, tile).intersected(a.rect());
      int pixels = 0;
      for (int row = r.top(); row <= r.bottom(); ++row) {
        const auto *u = reinterpret_cast<const QRgb *>(a.constScanLine(row));
        const auto *v = reinterpret_cast<const QRgb *>(b.constScanLine(row));
        for (int col = r.left(); col <= r.right(); ++col)
          if (std::abs(qRed(u[col]) - qRed(v[col])) +
                  std::abs(qGreen(u[col]) - qGreen(v[col])) +
                  std::abs(qBlue(u[col]) - qBlue(v[col])) >
              36)
            ++pixels;
      }
      changed += pixels;
      if (pixels >= 4) {
        if (out.regions.size() < 256)
          out.regions << r;
        out.bounds = out.bounds.united(r);
      }
    }
  }
  out.changedFraction = double(changed) / (qint64(a.width()) * a.height());
  return out;
}

QImage visualdiff::evidence(const QImage &before, const QImage &after,
                            const Delta &delta, QPoint target) {
  if (before.size() != after.size()) {
    if (before.isNull() || after.isNull())
      return {};
    auto a = before.scaledToWidth(std::min(640, before.width()),
                                  Qt::SmoothTransformation);
    auto b = after.scaledToWidth(std::min(640, after.width()),
                                 Qt::SmoothTransformation);
    QImage pair(a.width() + b.width(), std::max(a.height(), b.height()) + 28,
                QImage::Format_RGB32);
    pair.fill(Qt::black);
    QPainter p(&pair);
    p.setPen(Qt::white);
    p.drawText(8, 20, "BEFORE (original window)");
    p.drawText(a.width() + 8, 20, "AFTER (new layout)");
    p.drawImage(0, 28, a);
    p.drawImage(a.width(), 28, b);
    return pair;
  }
  QRect region =
      delta.bounds.united(QRect(target - QPoint(96, 96), QSize(192, 192)));
  region = region.adjusted(-24, -24, 24, 24).intersected(after.rect());
  if (region.isEmpty() || before.size() != after.size())
    return {};
  auto a = before.copy(region), b = after.copy(region);
  if (a.width() > 640) {
    a = a.scaledToWidth(640, Qt::SmoothTransformation);
    b = b.scaled(a.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  }
  QImage pair(a.width() * 2, a.height() + 28, QImage::Format_RGB32);
  pair.fill(Qt::black);
  QPainter p(&pair);
  p.setPen(Qt::white);
  p.drawText(8, 20, "BEFORE");
  p.drawText(a.width() + 8, 20, "AFTER");
  p.drawImage(0, 28, a);
  p.drawImage(a.width(), 28, b);
  p.setPen(QPen(Qt::magenta, 2));
  const double scale = double(a.width()) / region.width();
  for (auto r : delta.regions) {
    r = r.intersected(region);
    if (!r.isEmpty())
      p.drawRect(QRectF(a.width() + (r.x() - region.x()) * scale,
                        28 + (r.y() - region.y()) * scale, r.width() * scale,
                        r.height() * scale));
  }
  return pair;
}
