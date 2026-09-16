#pragma once
#include <QObject>
#include <QString>

// Whether anything is playing, from MPRIS.
//
// Worth being plain about the limits: MPRIS reports what is playing, whether
// it is playing and where the playhead is. It carries no amplitude and no
// beat. Nala therefore sways to the *fact* of music rather than to the music
// itself, at a rhythm of her own. Reacting to the sound would mean tapping
// the audio monitor, which this deliberately does not do.
class Music : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
  Q_PROPERTY(QString nowPlaying READ nowPlaying NOTIFY trackChanged)

public:
  explicit Music(QObject *parent = nullptr);

  bool playing() const { return m_playing; }
  QString nowPlaying() const { return m_track; }

  void setActive(bool active);

  // Test seams.
  void injectStatus(const QString &status);
  void injectTrack(const QString &title);

signals:
  void playingChanged(bool playing);
  void trackChanged();

private slots:
  // Slots proper: these are wired with the string-based connect() that
  // QDBusConnection::connect() requires.
  void findPlayers();

private:
  void refresh(const QString &service);

  QString m_service;  // the player we are following
  QString m_track;
  bool m_playing = false;
  bool m_active = false;
};
