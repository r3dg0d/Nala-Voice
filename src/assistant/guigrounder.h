#pragma once
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QPoint>
#include <functional>
class QTimer;

// Desktop operations are injected: offline tests never touch the real pointer.
class GuiGrounder : public QObject {
public:
  struct Frame {
    QImage image;
    QRect desktop;
    QString window;
  };
  struct Options {
    int maxRefinements = 4;
    int maxRetries = 2;
    int tolerancePixels = 4;
    bool verify = true;
    bool crop = true;
    bool normalized = false;
    bool confirmTarget =
        false; // production: inspect a clean crop before clicking
  };
  using Done = std::function<void(QJsonObject)>;
  struct Ports {
    std::function<void(std::function<void(Frame, QString)>)> capture;
    std::function<void(QImage, QString, Done)> predict;
    std::function<bool(QPoint)> point;
    std::function<bool(QString *)> click;
    std::function<void(std::function<void(bool)>)> authorizeRetry;
  };
  GuiGrounder(Ports ports, Options options, QObject *parent = nullptr);
  void start(QString target, QString expected, Done done);
  void cancel();
  static QPoint globalPoint(QPoint point, QSize observation, QRect desktop);
  static QImage landmarks(QImage image, const QVector<QPoint> &points);
  static double difference(const QImage &before, const QImage &after);

private:
  void observe(bool coarse);
  void predict(Frame frame, bool coarse);
  void act(Frame frame);
  void confirmTarget(Frame frame, QPoint point);
  void finish(bool ok, QString error = {});
  Ports m_ports;
  Options m_options;
  QString m_target, m_expected;
  Done m_done;
  QRect m_crop, m_desktop;
  QString m_window;
  QSize m_imageSize;
  QVector<QPoint> m_points;
  int m_refinements = 0, m_retries = 0, m_actions = 0;
  bool m_cancelled = false;
  QTimer *m_deadline = nullptr;
};
