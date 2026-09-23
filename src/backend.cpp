#include "backend.h"
#include "activity.h"
#include "compositor.h"
#include "cursor.h"
#include "mascot.h"
#include "music.h"
#include "theme.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <iterator>
#include "mascot.h"
#include "music.h"
#include <QRegion>
#include <QSaveFile>
#include <QScreen>
#include <QStandardPaths>

#ifdef NALA_HAVE_LAYER_SHELL
#include <LayerShellQt/Window>
#endif

namespace {

constexpr qreal kMinSize = 0.55;
constexpr qreal kMaxSize = 2.20;

// The window is deliberately larger than the body so morphs that reach beyond
// the idle silhouette -- the exclamation mark, the orbit rings -- are not
// clipped. The reference reel uses the same headroom.
constexpr qreal kCanvasToBody = 1.89; // 1 / 0.529, the measured body radius
constexpr int kBaseBody = 116; // logical pixels at size 1.0

// Flight. Below this release speed she simply drops where she is put.
constexpr qreal kThrowSpeed = 900.0; // pixels per second
constexpr qreal kDrag = 1.5;         // per second
constexpr qreal kBounce = 0.62;      // energy kept when she hits an edge

// An input region that takes nothing. Not QRegion(): an empty mask means "no
// mask", which on Wayland makes the whole surface take input -- the opposite.
// A single pixel outside the surface leaves nothing inside it.
QRegion noInput() { return QRegion(-1, -1, 1, 1); }

QString autostartPath() {
  return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
         "/autostart/nala.desktop";
}

} // namespace

Backend::Backend(QString configPath, bool preview, bool testing, Mascot *mascot,
                 Theme *theme, Cursor *cursor, Activity *activity,
                 Compositor *compositor, Music *music, QObject *parent)
    : QObject(parent), m_configPath(std::move(configPath)), m_preview(preview),
      m_testing(testing), m_mascot(mascot), m_theme(theme), m_cursor(cursor),
      m_activity(activity), m_compositor(compositor), m_music(music) {
  m_saveTimer.setSingleShot(true);
  m_saveTimer.setInterval(400); // coalesce slider drags into one write
  connect(&m_saveTimer, &QTimer::timeout, this, &Backend::save);

  load();
  applyToMascot();

  if (m_theme)
    connect(m_theme, &Theme::changed, this, [this] { emit changed(); });

  if (m_cursor && m_mascot) {
    connect(m_cursor, &Cursor::moved, this, [this](QPoint position) {
      if (!m_followCursor)
        return;
      const qreal radius = bodyRadius();
      if (radius <= 0.0)
        return;
      const QPointF centre = centreOnScreen();
      m_mascot->lookAt((position.x() - centre.x()) / radius,
                       (position.y() - centre.y()) / radius);
    });
  }

  if (m_activity && m_mascot) {
    // A notification is what the badge has always been for.
    connect(m_activity, &Activity::notified, this, [this] {
      if (m_reactToDesktop)
        m_mascot->notify();
    });
  }

  if (m_compositor && m_mascot) {
    // Something went fullscreen: step aside rather than sitting on top of a
    // film. This is the one reaction that is about being considerate rather
    // than about being alive.
    connect(m_compositor, &Compositor::fullscreenChanged, this,
            [this](bool fullscreen) {
              if (m_outOfTheWay == fullscreen)
                return;
              m_outOfTheWay = fullscreen;
              applyInputRegion(false); // and stop swallowing clicks
              emit outOfTheWayChanged();
            });

    // You moved somewhere else, and she noticed.
    connect(m_compositor, &Compositor::workspaceChanged, this, [this] {
      if (m_outOfTheWay)
        return;
      m_mascot->wake();
      m_mascot->glanceAbout();
    });

    // Something new turned up.
    connect(m_compositor, &Compositor::windowOpened, this, [this] {
      if (m_outOfTheWay)
        return;
      m_mascot->wake();
      m_mascot->glanceAbout();
    });
  }

  if (m_music && m_mascot) {
    connect(m_music, &Music::playingChanged, this, [this](bool playing) {
      m_mascot->setSwaying(m_reactToDesktop && playing);
    });
    // A new track is worth looking up for.
    connect(m_music, &Music::trackChanged, this, [this] {
      if (m_reactToDesktop && m_music->playing())
        m_mascot->glanceAbout();
    });
  }

  if (!m_testing) {
    connect(qApp, &QGuiApplication::screenAdded, this,
            &Backend::screensChanged);
    connect(qApp, &QGuiApplication::screenRemoved, this,
            &Backend::screensChanged);
  }
}

// --- preferences -----------------------------------------------------------

void Backend::load() {
  QFile file(m_configPath);
  if (!file.open(QIODevice::ReadOnly))
    return;
  const QJsonObject json =
      QJsonDocument::fromJson(file.readAll()).object();

  m_size = qBound(kMinSize, json.value("size").toDouble(m_size), kMaxSize);
  const QString mode = json.value("colorMode").toString(m_colorMode);
  if (mode == "ink" || mode == "theme")
    m_colorMode = mode;
  m_followCursor = json.value("followCursor").toBool(m_followCursor);
  m_idleAntics = json.value("idleAntics").toBool(m_idleAntics);
  m_sleepWhenIdle = json.value("sleepWhenIdle").toBool(m_sleepWhenIdle);
  m_reducedMotion = json.value("reducedMotion").toBool(m_reducedMotion);
  m_reactToDesktop = json.value("reactToDesktop").toBool(m_reactToDesktop);
  m_stayOnTop = json.value("stayOnTop").toBool(m_stayOnTop);
  m_monitor = json.value("monitor").toString(m_monitor);
  m_place = QPointF(
      qBound(0.0, json.value("x").toDouble(m_place.x()), 1.0),
      qBound(0.0, json.value("y").toDouble(m_place.y()), 1.0));
}

void Backend::save() {
  if (m_testing)
    return; // a measured run must never overwrite real preferences

  QDir().mkpath(QFileInfo(m_configPath).absolutePath());
  const QJsonObject json{
      {"size", m_size},
      {"colorMode", m_colorMode},
      {"followCursor", m_followCursor},
      {"idleAntics", m_idleAntics},
      {"sleepWhenIdle", m_sleepWhenIdle},
      {"reducedMotion", m_reducedMotion},
      {"reactToDesktop", m_reactToDesktop},
      {"stayOnTop", m_stayOnTop},
      {"monitor", m_monitor},
      {"x", m_place.x()},
      {"y", m_place.y()},
  };

  QSaveFile file(m_configPath);
  if (!file.open(QIODevice::WriteOnly)) {
    note(QStringLiteral("Could not save preferences."));
    return;
  }
  file.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
  if (!file.commit())
    note(QStringLiteral("Could not save preferences."));
}

void Backend::note(const QString &message) {
  m_feedback = message;
  emit feedbackChanged();
}

void Backend::applyToMascot() {
  if (!m_mascot)
    return;
  m_mascot->setReducedMotion(m_reducedMotion);
  m_mascot->setSleepWhenIdle(m_sleepWhenIdle);
  m_mascot->setIdleAntics(m_idleAntics);
  if (m_cursor)
    m_cursor->setActive(m_followCursor && !m_testing);
  if (m_activity)
    m_activity->setActive(m_reactToDesktop && !m_testing);
  if (m_compositor)
    m_compositor->setActive(m_reactToDesktop && !m_testing);
  if (m_music) {
    m_music->setActive(m_reactToDesktop && !m_testing);
    if (m_mascot)
      m_mascot->setSwaying(m_reactToDesktop && m_music->playing());
  }
  if (!m_followCursor)
    m_mascot->lookIdle();
}

void Backend::configure(const QString &key, const QVariant &value) {
  if (key == "size") {
    const qreal wanted = value.toDouble();
    if (wanted < kMinSize || wanted > kMaxSize)
      return; // out of range: keep the current value rather than clamping
    m_size = wanted;
  } else if (key == "colorMode") {
    const QString mode = value.toString();
    if (mode != "ink" && mode != "theme")
      return;
    m_colorMode = mode;
  } else if (key == "followCursor") {
    m_followCursor = value.toBool();
  } else if (key == "idleAntics") {
    m_idleAntics = value.toBool();
  } else if (key == "sleepWhenIdle") {
    m_sleepWhenIdle = value.toBool();
  } else if (key == "reducedMotion") {
    m_reducedMotion = value.toBool();
  } else if (key == "reactToDesktop") {
    m_reactToDesktop = value.toBool();
  } else if (key == "stayOnTop") {
    m_stayOnTop = value.toBool();
  } else if (key == "monitor") {
    m_monitor = value.toString();
  } else {
    return;
  }

  applyToMascot();
  applyPlacement();
  emit changed();
  m_saveTimer.start();
}

QColor Backend::mascotColor() const {
  if (m_colorMode == "theme" && m_theme)
    return m_theme->mascotColor();
  return QColor("#0a090c"); // the reference silhouette
}

// --- placement -------------------------------------------------------------

QStringList Backend::screens() const {
  QStringList names;
  for (const QScreen *screen : QGuiApplication::screens())
    names << screen->name();
  return names;
}

QRect Backend::screenGeometry() const {
  const QList<QScreen *> all = QGuiApplication::screens();
  for (QScreen *screen : all)
    if (!m_monitor.isEmpty() && screen->name() == m_monitor)
      return screen->geometry();
  QScreen *primary = QGuiApplication::primaryScreen();
  if (primary)
    return primary->geometry();
  return all.isEmpty() ? QRect(0, 0, 1920, 1080) : all.first()->geometry();
}

qreal Backend::windowSize() const {
  return kBaseBody * kCanvasToBody * m_size;
}

qreal Backend::bodyRadius() const {
  return windowSize() / kCanvasToBody * 0.5;
}

// Where Nala actually is, in compositor coordinates.
//
// Deliberately derived from the placement we asked for rather than from
// m_window->x()/y(): a layer-shell surface is positioned by the shell, and the
// QWindow's own coordinates do not describe where it ended up. Reading them
// here is what made the gaze aim at the wrong point.
QPointF Backend::centreOnScreen() const {
  // An ordinary window is placed by the compositor, so ask it where it ended
  // up. A layer-shell surface is not: its QWindow coordinates say nothing
  // about where the shell actually put it, so use the placement we asked for.
  if (m_window && !m_layered)
    return QPointF(m_window->x() + m_window->width() * 0.5,
                   m_window->y() + m_window->height() * 0.5);

  const QRect screen = screenGeometry();
  const qreal extent = windowSize();
  return QPointF(
      screen.x() + m_place.x() * (screen.width() - extent) + extent * 0.5,
      screen.y() + m_place.y() * (screen.height() - extent) + extent * 0.5);
}

void Backend::attach(QQuickWindow *window) {
  m_window = window;
  if (!m_window)
    return;

  m_window->setFlag(Qt::FramelessWindowHint, true);
  m_window->setColor(Qt::transparent);

#ifdef NALA_HAVE_LAYER_SHELL
  // Must happen while the window is still hidden: LayerShellQt can only give a
  // QWindow the layer-surface role before its platform surface exists. This is
  // what makes Nala a free-floating companion instead of a tiled window.
  if (!m_preview) {
    if (auto *layer = LayerShellQt::Window::get(m_window)) {
      layer->setScope(QStringLiteral("nala"));
      layer->setAnchors({LayerShellQt::Window::AnchorTop |
                         LayerShellQt::Window::AnchorLeft});
      layer->setExclusiveZone(-1); // never reserve space from other windows
      layer->setKeyboardInteractivity(
          LayerShellQt::Window::KeyboardInteractivityNone);
      layer->setCloseOnDismissed(false);
      m_layered = true;
    }
  }
#endif

  applyPlacement();
  m_window->setVisible(true);
  applyInputRegion(false);
}

void Backend::applyPlacement() {
  if (!m_window)
    return;

  const int extent = int(std::lround(windowSize()));
  const QRect screen = screenGeometry();
  const int x = int(std::lround(m_place.x() * (screen.width() - extent)));
  const int y = int(std::lround(m_place.y() * (screen.height() - extent)));

#ifdef NALA_HAVE_LAYER_SHELL
  if (m_layered) {
    if (auto *layer = LayerShellQt::Window::get(m_window)) {
      QScreen *target = nullptr;
      for (QScreen *candidate : QGuiApplication::screens())
        if (!m_monitor.isEmpty() && candidate->name() == m_monitor)
          target = candidate;
      if (target)
        layer->setScreen(target);

      layer->setLayer(m_stayOnTop ? LayerShellQt::Window::LayerTop
                                  : LayerShellQt::Window::LayerBottom);
      // A layer surface is sized and placed by the shell, not by x/y.
      layer->setDesiredSize(QSize(extent, extent));
      layer->setMargins(QMargins(x, y, 0, 0));
      if (m_window->width() != extent || m_window->height() != extent)
        m_window->resize(extent, extent);
      applyInputRegion(m_dragging);
      return;
    }
  }
#endif

  if (m_window->width() != extent || m_window->height() != extent)
    m_window->resize(extent, extent);
  m_window->setPosition(screen.x() + x, screen.y() + y);
  applyInputRegion(m_dragging);
}

void Backend::applyInputRegion(bool wholeWindow) {
  if (!m_window)
    return;
  const int extent = m_window->width();
  if (extent <= 0)
    return;

  if (m_outOfTheWay) {
    // Stepped aside: take no input at all, or she would still be catching
    // clicks meant for whatever is fullscreen.
    m_window->setMask(noInput());
    return;
  }

  if (wholeWindow) {
    // While she is being dragged the pointer wanders outside her silhouette;
    // widen the input region so the motion and the release still arrive.
    m_window->setMask(QRegion(0, 0, extent, extent));
    return;
  }

  // Otherwise only the mascot herself swallows clicks, so the transparent
  // margin around her stays click-through and never blocks the desktop.
  const int body = int(std::lround(extent / kCanvasToBody));
  const int inset = (extent - body) / 2;
  m_window->setMask(QRegion(inset, inset, body, body, QRegion::Ellipse));
}

void Backend::resetPlace() {
  m_place = QPointF(0.86, 0.74);
  applyPlacement();
  emit changed();
  m_saveTimer.start();
}

// --- drag ------------------------------------------------------------------

void Backend::grabDrag() {
  if (!m_window || m_dragging)
    return;
  m_dragging = true;
  m_flying = false;
  m_dragSpeed = 0.0;
  m_dragVelocity = QPointF();
  m_dragClock.restart();
  applyInputRegion(true);
  if (m_mascot)
    m_mascot->beginDrag();
}

void Backend::dragBy(qreal dx, qreal dy) {
  if (!m_dragging || !m_window)
    return;

  const QRect screen = screenGeometry();
  const int extent = m_window->width();
  const qreal spanX = std::max(1, screen.width() - extent);
  const qreal spanY = std::max(1, screen.height() - extent);

  m_place = QPointF(qBound(0.0, m_place.x() + dx / spanX, 1.0),
                    qBound(0.0, m_place.y() + dy / spanY, 1.0));

  // Exponential average: a single jittery event should not read as a throw.
  m_dragSpeed = m_dragSpeed * 0.6 + std::hypot(dx, dy) * 0.4;

  // Velocity needs the interval between events, not just their size -- the
  // same 20 px means very different things 5 ms and 80 ms apart.
  const qreal elapsed =
      m_dragClock.isValid() ? m_dragClock.restart() / 1000.0 : 0.0;
  if (elapsed > 0.001 && elapsed < 0.2) {
    const QPointF sample(dx / elapsed, dy / elapsed);
    m_dragVelocity = m_dragVelocity * 0.55 + sample * 0.45;
  }

  applyPlacement();
  emit changed();
}

void Backend::releaseDrag() {
  if (!m_dragging)
    return;
  m_dragging = false;
  applyInputRegion(false);

  // Let go of her hard enough and she does not just drop -- she streaks off.
  const qreal speed = std::hypot(m_dragVelocity.x(), m_dragVelocity.y());
  if (speed > kThrowSpeed) {
    launch(m_dragVelocity.x(), m_dragVelocity.y(), speed);
    return;
  }

  if (m_mascot)
    m_mascot->endDrag(qBound(0.0, m_dragSpeed / 26.0, 1.0));
  m_saveTimer.start();
}

void Backend::launch(qreal dx, qreal dy, qreal speed) {
  if (!m_window)
    return;
  const QRect screen = screenGeometry();
  const int extent = m_window->width();
  const qreal spanX = std::max(1, screen.width() - extent);
  const qreal spanY = std::max(1, screen.height() - extent);

  m_flying = true;
  m_dragging = false;
  // Convert pixels per second into the place fractions the window moves in.
  m_flightVelocity = QPointF(dx / spanX, dy / spanY);
  if (m_mascot)
    m_mascot->beginDash(std::atan2(-dy, dx),
                        qBound(0.0, speed / (kThrowSpeed * 4.0), 1.0));
}

namespace {

// The demo reel: what she does, and how far into the run it happens.
struct DemoStep {
  qreal at;
  const char *what;
};
constexpr DemoStep kDemo[] = {
    {0.0, "wink"},   {2.6, "egg"},    {4.6, "hex"},    {6.6, "circle"},
    {7.6, "poke"},   {9.4, "think"},  {15.0, "alert"}, {17.8, "notify"},
    {21.8, "scatter"}, {24.4, "dash"}, {28.5, "rest"},
};

} // namespace

void Backend::demo() {
  if (!m_mascot)
    return;
  m_mascot->rest();
  m_demoStep = 0;
  m_demoClock = 0.0;
}

void Backend::advance(qreal dt) {
  // Demo playback. Driven from the same frame callback as everything else, so
  // it cannot drift out of step with her animation.
  if (m_demoStep >= 0 && m_mascot) {
    m_demoClock += dt;
    const int count = int(std::size(kDemo));
    while (m_demoStep < count && m_demoClock >= kDemo[m_demoStep].at) {
      const QString what = QString::fromLatin1(kDemo[m_demoStep].what);
      ++m_demoStep;
      if (what == "egg")
        m_mascot->changeForm(Mascot::Egg);
      else if (what == "hex")
        m_mascot->changeForm(Mascot::Hex);
      else if (what == "circle")
        m_mascot->changeForm(Mascot::Circle, Mascot::kSettleBack);
      else if (what == "dash")
        command("dash");
      else
        command(what);
    }
    if (m_demoStep >= count)
      m_demoStep = -1;
  }

  // She notices the machine working. Re-triggered on a cadence rather than
  // held, so a long build gets an occasional glance rather than a mascot
  // stuck in a permanent spin.
  if (m_reactToDesktop && m_activity && m_mascot) {
    m_sinceBusyThought += dt;
    if (m_activity->busy() && m_mascot->mood() == Mascot::Resting &&
        m_sinceBusyThought > 14.0) {
      m_sinceBusyThought = 0.0;
      m_mascot->think(3.4);
    }
  }

  // Asleep, she stirs when the cursor comes near rather than needing a poke.
  if (m_mascot && m_mascot->sleeping() && m_cursor && m_cursor->available()) {
    const qreal radius = bodyRadius();
    const QPointF centre = centreOnScreen();
    const QPointF to = QPointF(m_cursor->position()) - centre;
    if (radius > 0.0 && std::hypot(to.x(), to.y()) < radius * 3.0)
      m_mascot->wake();
  }

  if (!m_flying || !m_window)
    return;

  const QRect screen = screenGeometry();
  const int extent = m_window->width();
  const qreal spanX = std::max(1.0, qreal(screen.width() - extent));
  const qreal spanY = std::max(1.0, qreal(screen.height() - extent));

  QPointF place = m_place + m_flightVelocity * dt;

  // Bounce off the edges of the screen rather than sticking to them.
  if (place.x() < 0.0 || place.x() > 1.0) {
    place.setX(qBound(0.0, place.x() < 0.0 ? -place.x() : 2.0 - place.x(), 1.0));
    m_flightVelocity.setX(-m_flightVelocity.x() * kBounce);
  }
  if (place.y() < 0.0 || place.y() > 1.0) {
    place.setY(qBound(0.0, place.y() < 0.0 ? -place.y() : 2.0 - place.y(), 1.0));
    m_flightVelocity.setY(-m_flightVelocity.y() * kBounce);
  }
  m_place = place;

  // Air resistance.
  m_flightVelocity *= std::max(0.0, 1.0 - kDrag * dt);

  const qreal pixelsPerSecond =
      std::hypot(m_flightVelocity.x() * spanX, m_flightVelocity.y() * spanY);
  if (m_mascot)
    m_mascot->updateDash(
        std::atan2(-m_flightVelocity.y() * spanY, m_flightVelocity.x() * spanX),
        qBound(0.0, pixelsPerSecond / (kThrowSpeed * 4.0), 1.0));

  applyPlacement();
  emit changed();

  if (pixelsPerSecond < kThrowSpeed * 0.35) {
    m_flying = false;
    m_flightVelocity = QPointF();
    if (m_mascot)
      m_mascot->endDash();
    m_saveTimer.start();
  }
}

void Backend::injectCursor(int x, int y) {
  if (m_cursor)
    m_cursor->inject(QPoint(x, y));
}

// --- commands --------------------------------------------------------------

void Backend::openSettings() { emit settingsRequested(); }

void Backend::quit() { QCoreApplication::quit(); }

void Backend::command(const QString &name) {
  if (!m_mascot)
    return;
  if (name == "poke")
    m_mascot->poke();
  else if (name == "think")
    m_mascot->think();
  else if (name == "alert")
    m_mascot->alert();
  else if (name == "notify")
    m_mascot->notify();
  else if (name == "wake")
    m_mascot->wake();
  else if (name == "rest")
    m_mascot->rest();
  else if (name == "wink")
    m_mascot->wink();
  else if (name == "scatter")
    m_mascot->scatter();
  else if (name == "demo")
    demo();
  else if (name == "dash") {
    // Off in some direction of her own choosing.
    const qreal angle = QRandomGenerator::global()->generateDouble() * 2 * M_PI;
    launch(std::cos(angle) * kThrowSpeed * 2.2,
           std::sin(angle) * kThrowSpeed * 2.2, kThrowSpeed * 2.2);
  }
  else if (name == "settings")
    openSettings();
  else if (name == "reset")
    resetPlace();
  else if (name == "quit")
    quit();
}

QString Backend::status() const {
  static const char *moods[] = {"resting",   "happy",  "thinking", "alert",
                                "notifying", "asleep", "held",     "dashing"};
  const int mood = m_mascot ? m_mascot->mood() : 0;
  const int moodCount = int(std::size(moods));
  return QStringLiteral("Nala is running — size %1%, %2, at %3%/%4%, %5, %6 "
                        "(form %7→%8 %9%)%10")
      .arg(int(std::lround(m_size * 100)))
      .arg(m_colorMode == "theme" ? "following the desktop theme" : "ink")
      .arg(int(std::lround(m_place.x() * 100)))
      .arg(int(std::lround(m_place.y() * 100)))
      .arg(mood >= 0 && mood < moodCount ? moods[mood] : "?")
      .arg(!m_followCursor        ? "gaze free"
           : m_cursor && m_cursor->available()
               ? "tracking the cursor"
               : "cursor unavailable")
      .arg(m_mascot ? m_mascot->formA() : -1)
      .arg(m_mascot ? m_mascot->formB() : -1)
      .arg(m_mascot ? int(std::lround(m_mascot->formMix() * 100)) : 0)
      .arg(m_mascot && m_mascot->swaying() ? ", swaying" : "");
}

// --- autostart -------------------------------------------------------------

bool Backend::startAtLogin() const { return QFile::exists(autostartPath()); }

void Backend::setStartAtLogin(bool enabled) {
  const QString path = autostartPath();
  if (!enabled) {
    if (QFile::exists(path) && !QFile::remove(path))
      note(QStringLiteral("Could not remove the autostart entry."));
    emit changed();
    return;
  }

  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    note(QStringLiteral("Could not create the autostart entry."));
    emit changed();
    return;
  }
  const QString entry = QStringLiteral("[Desktop Entry]\n"
                                       "Type=Application\n"
                                       "Name=Nala\n"
                                       "Comment=Desktop companion\n"
                                       "Exec=%1\n"
                                       "Terminal=false\n"
                                       "X-GNOME-Autostart-enabled=true\n")
                            .arg(QCoreApplication::applicationFilePath());
  file.write(entry.toUtf8());
  if (!file.commit())
    note(QStringLiteral("Could not create the autostart entry."));
  emit changed();
}
