#include "compositor.h"

#include <QProcessEnvironment>
#include <unistd.h>

Compositor::Compositor(QObject *parent) : QObject(parent) {
  const auto env = QProcessEnvironment::systemEnvironment();
  const QString signature = env.value("HYPRLAND_INSTANCE_SIGNATURE");
  const QString runtime = env.value(
      "XDG_RUNTIME_DIR", QStringLiteral("/run/user/%1").arg(::getuid()));
  if (!signature.isEmpty())
    m_socketPath = runtime + "/hypr/" + signature + "/.socket2.sock";

  connect(&m_socket, &QLocalSocket::readyRead, this, &Compositor::readEvents);
  connect(&m_socket, &QLocalSocket::connected, this, [this] {
    m_available = true;
    emit availableChanged();
  });
  connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
    m_available = false;
    emit availableChanged();
  });
}

void Compositor::setActive(bool active) {
  if (m_active == active)
    return;
  m_active = active;
  if (active && !m_socketPath.isEmpty())
    m_socket.connectToServer(m_socketPath, QIODevice::ReadOnly);
  else
    m_socket.disconnectFromServer();
}

void Compositor::readEvents() {
  m_buffer += m_socket.readAll();
  // A burst can arrive part-line; keep the remainder for next time.
  while (true) {
    const int newline = m_buffer.indexOf('\n');
    if (newline < 0)
      break;
    const QString line = QString::fromUtf8(m_buffer.left(newline));
    m_buffer.remove(0, newline + 1);
    injectEvent(line);
  }
  if (m_buffer.size() > 8192)
    m_buffer.clear(); // a control stream, not a data channel
}

void Compositor::injectEvent(const QString &line) {
  const int split = line.indexOf(QStringLiteral(">>"));
  if (split < 0)
    return;
  const QString name = line.left(split);
  const QString payload = line.mid(split + 2);

  if (name == "fullscreen") {
    const bool on = payload.trimmed() != "0";
    if (on != m_fullscreen) {
      m_fullscreen = on;
      emit fullscreenChanged(m_fullscreen);
    }
  } else if (name == "workspace" || name == "focusedmon") {
    emit workspaceChanged();
  } else if (name == "openwindow") {
    emit windowOpened();
  }
}
