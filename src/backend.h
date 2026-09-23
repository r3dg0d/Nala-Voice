#pragma once
#include <QColor>
#include <QJsonObject>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QStringList>
#include <QElapsedTimer>
#include <QTimer>

class QQuickWindow;
class Mascot;
class Theme;
class Cursor;
class Activity;
class Compositor;
class Music;

// Preferences, placement and the glue between the mascot and the desktop.
class Backend : public QObject {
  Q_OBJECT

  Q_PROPERTY(qreal size READ size NOTIFY changed)
  Q_PROPERTY(QString colorMode READ colorMode NOTIFY changed)
  Q_PROPERTY(QColor mascotColor READ mascotColor NOTIFY changed)
  Q_PROPERTY(bool followCursor READ followCursor NOTIFY changed)
  Q_PROPERTY(bool idleAntics READ idleAntics NOTIFY changed)
  Q_PROPERTY(bool reactToDesktop READ reactToDesktop NOTIFY changed)
  Q_PROPERTY(bool sleepWhenIdle READ sleepWhenIdle NOTIFY changed)
  Q_PROPERTY(bool reducedMotion READ reducedMotion NOTIFY changed)
  Q_PROPERTY(bool stayOnTop READ stayOnTop NOTIFY changed)
  Q_PROPERTY(bool startAtLogin READ startAtLogin NOTIFY changed)
  Q_PROPERTY(QString monitor READ monitor NOTIFY changed)
  Q_PROPERTY(QStringList screens READ screens NOTIFY screensChanged)
  Q_PROPERTY(QString feedback READ feedback NOTIFY feedbackChanged)
  Q_PROPERTY(bool outOfTheWay READ outOfTheWay NOTIFY outOfTheWayChanged)
  Q_PROPERTY(bool testing READ testing CONSTANT)
  // Where the speech bubble sits relative to her: below when there is no
  // room above, and where along its width the tail should point.
  Q_PROPERTY(bool bubbleBelow READ bubbleBelow NOTIFY bubbleMoved)
  Q_PROPERTY(qreal bubbleTail READ bubbleTail NOTIFY bubbleMoved)

public:
  Backend(QString configPath, bool preview, bool testing, Mascot *mascot,
          Theme *theme, Cursor *cursor, Activity *activity,
          Compositor *compositor, Music *music, QObject *parent = nullptr);

  qreal size() const { return m_size; }
  QString colorMode() const { return m_colorMode; }
  QColor mascotColor() const;
  bool followCursor() const { return m_followCursor; }
  bool idleAntics() const { return m_idleAntics; }
  bool reactToDesktop() const { return m_reactToDesktop; }
  bool sleepWhenIdle() const { return m_sleepWhenIdle; }
  bool reducedMotion() const { return m_reducedMotion; }
  bool stayOnTop() const { return m_stayOnTop; }
  bool startAtLogin() const;
  QString monitor() const { return m_monitor; }
  QStringList screens() const;
  QString feedback() const { return m_feedback; }
  // True while she has stepped aside, which is to say while something is
  // fullscreen. QML fades her out rather than hiding the window, because a
  // layer surface cannot be given its role back once it has been destroyed.
  bool outOfTheWay() const { return m_outOfTheWay; }
  bool testing() const { return m_testing; }

  qreal nx() const { return m_place.x(); }
  qreal ny() const { return m_place.y(); }

  void attach(QQuickWindow *window);
  // The speech bubble: a second surface kept beside her.
  void attachBubble(QQuickWindow *bubble);
  bool bubbleBelow() const { return m_bubbleBelow; }
  qreal bubbleTail() const { return m_bubbleTail; }
  // Take clicks only while there is something to click.
  Q_INVOKABLE void setBubbleInteractive(bool interactive);

  Q_INVOKABLE void configure(const QString &key, const QVariant &value);
  Q_INVOKABLE void setStartAtLogin(bool enabled);
  Q_INVOKABLE void resetPlace();
  Q_INVOKABLE void openSettings();
  Q_INVOKABLE void closeSettings();
  Q_INVOKABLE void openTimeline();
  Q_INVOKABLE void quit();
  Q_INVOKABLE void command(const QString &name);
  QString status() const;

  // Drag. QML reports movement as a delta in the item's own coordinates,
  // because a Wayland client cannot trust mapToGlobal() for a layer surface --
  // it has no reliable idea where the compositor put it.
  Q_INVOKABLE void grabDrag();
  Q_INVOKABLE void dragBy(qreal dx, qreal dy);
  Q_INVOKABLE void releaseDrag();
  Q_INVOKABLE bool dragging() const { return m_dragging; }

  // Drive flight. Called from the same frame callback as the mascot's tick so
  // the two never drift apart.
  Q_INVOKABLE void advance(qreal dt);
  Q_INVOKABLE void launch(qreal dx, qreal dy, qreal speed);
  bool flying() const { return m_flying; }

  // Run through everything she can do, once, in order. Handy for seeing the
  // whole repertoire without waiting on her own timing.
  Q_INVOKABLE void demo();
  bool demoing() const { return m_demoStep >= 0; }

  // Test seams.
  void injectCursor(int x, int y);
  qreal windowSize() const;
  qreal bodyRadius() const;
  QPointF centreOnScreen() const;

signals:
  void changed();
  void outOfTheWayChanged();
  void screensChanged();
  void feedbackChanged();
  void settingsRequested();
  void settingsCloseRequested();
  void bubbleMoved();
  void timelineRequested();

private:
  void load();
  void save();
  void applyToMascot();
  void applyPlacement();
  void applyInputRegion(bool wholeWindow = false);
  void placeBubble();
  void note(const QString &message);
  QRect screenGeometry() const;

  QString m_configPath;
  bool m_preview = false;
  bool m_testing = false;
  Mascot *m_mascot = nullptr;
  Theme *m_theme = nullptr;
  Cursor *m_cursor = nullptr;
  Activity *m_activity = nullptr;
  Compositor *m_compositor = nullptr;
  Music *m_music = nullptr;
  bool m_outOfTheWay = false;
  qreal m_sinceBusyThought = 0.0;
  QQuickWindow *m_window = nullptr;
  bool m_layered = false;
  QQuickWindow *m_bubble = nullptr;
  bool m_bubbleLayered = false;
  bool m_bubbleInteractive = false;
  bool m_bubbleBelow = false;
  qreal m_bubbleTail = 0.5;

  qreal m_size = 1.0;
  QString m_colorMode = QStringLiteral("ink");
  bool m_followCursor = true;
  bool m_idleAntics = true;
  bool m_reactToDesktop = true;
  bool m_sleepWhenIdle = true;
  bool m_reducedMotion = false;
  bool m_stayOnTop = true;
  QString m_monitor;
  QPointF m_place{0.86, 0.74};

  QString m_feedback;
  QTimer m_saveTimer;

  // Drag bookkeeping.
  bool m_dragging = false;
  qreal m_dragSpeed = 0.0;
  QPointF m_dragVelocity;   // pixels per second, smoothed
  QElapsedTimer m_dragClock;

  // Flight, in fractions of the screen's travel per second.
  bool m_flying = false;
  QPointF m_flightVelocity;

  // Demo playback.
  int m_demoStep = -1;
  qreal m_demoClock = 0.0;
};
