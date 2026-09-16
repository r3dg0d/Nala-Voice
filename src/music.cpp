#include "music.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QVariantMap>

namespace {
constexpr auto kPrefix = "org.mpris.MediaPlayer2.";
constexpr auto kPath = "/org/mpris/MediaPlayer2";
constexpr auto kPlayer = "org.mpris.MediaPlayer2.Player";
} // namespace

Music::Music(QObject *parent) : QObject(parent) {}

void Music::setActive(bool active) {
  if (m_active == active)
    return;
  m_active = active;

  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!active || !bus.isConnected()) {
    bus.disconnect(QString(), kPath, "org.freedesktop.DBus.Properties",
                   "PropertiesChanged", this, SLOT(findPlayers()));
    m_service.clear();
    injectStatus(QStringLiteral("Stopped"));
    return;
  }

  // Players come and go, so follow the name list rather than one player.
  auto *watcher = new QDBusServiceWatcher(
      QString(), bus, QDBusServiceWatcher::WatchForOwnerChange, this);
  connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
          [this](const QString &name, const QString &, const QString &) {
            if (name.startsWith(kPrefix))
              findPlayers();
          });

  // Any player's property changes land here; we only act on the one we follow.
  bus.connect(QString(), kPath, "org.freedesktop.DBus.Properties",
              "PropertiesChanged", this, SLOT(findPlayers()));

  findPlayers();
}

void Music::findPlayers() {
  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected() || !bus.interface())
    return;

  // Prefer whichever player is actually playing; otherwise keep the first.
  QString playingService, anyService;
  for (const QString &name : bus.interface()->registeredServiceNames().value()) {
    if (!name.startsWith(kPrefix))
      continue;
    if (anyService.isEmpty())
      anyService = name;
    QDBusInterface player(name, kPath, "org.freedesktop.DBus.Properties", bus);
    const QDBusReply<QVariant> status =
        player.call("Get", kPlayer, "PlaybackStatus");
    if (status.isValid() && status.value().toString() == "Playing") {
      playingService = name;
      break;
    }
  }

  m_service = playingService.isEmpty() ? anyService : playingService;
  if (m_service.isEmpty()) {
    injectStatus(QStringLiteral("Stopped"));
    return;
  }
  refresh(m_service);
}

void Music::refresh(const QString &service) {
  QDBusConnection bus = QDBusConnection::sessionBus();
  QDBusInterface player(service, kPath, "org.freedesktop.DBus.Properties", bus);

  const QDBusReply<QVariant> status =
      player.call("Get", kPlayer, "PlaybackStatus");
  if (status.isValid())
    injectStatus(status.value().toString());

  const QDBusReply<QVariant> metadata = player.call("Get", kPlayer, "Metadata");
  if (metadata.isValid()) {
    QVariantMap map;
    metadata.value().value<QDBusArgument>() >> map;
    injectTrack(map.value(QStringLiteral("xesam:title")).toString());
  }
}

void Music::injectStatus(const QString &status) {
  const bool playing = status == QStringLiteral("Playing");
  if (playing == m_playing)
    return;
  m_playing = playing;
  emit playingChanged(m_playing);
}

void Music::injectTrack(const QString &title) {
  if (title == m_track)
    return;
  m_track = title;
  emit trackChanged();
}
