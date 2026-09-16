#pragma once
#include <QLocalSocket>
#include <QObject>

// What the compositor is doing, so she responds to your actual work rather
// than only to a timer.
//
// Hyprland streams events down a socket as `name>>payload` lines. Elsewhere
// this simply reports itself unavailable and stays quiet.
class Compositor : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool available READ available NOTIFY availableChanged)
  Q_PROPERTY(bool fullscreen READ fullscreen NOTIFY fullscreenChanged)

public:
  explicit Compositor(QObject *parent = nullptr);

  bool available() const { return m_available; }
  bool fullscreen() const { return m_fullscreen; }

  void setActive(bool active);

  // Test seam: hand it a line exactly as the compositor would.
  void injectEvent(const QString &line);

signals:
  void availableChanged();
  void fullscreenChanged(bool fullscreen);
  void workspaceChanged();
  void windowOpened();

private:
  void readEvents();

  QString m_socketPath;
  QLocalSocket m_socket;
  QByteArray m_buffer;
  bool m_available = false;
  bool m_fullscreen = false;
  bool m_active = false;
};
