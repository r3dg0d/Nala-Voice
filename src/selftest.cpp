#include "selftest.h"
#include "activity.h"
#include "backend.h"
#include "mascot.h"
#include "orbits.h"
#include "theme.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QImage>
#include <QQuickWindow>
#include <QRect>
#include <algorithm>

// Mirrors the constant in mascot.cpp; see docs/animation.md.
static constexpr double kAlertOverleanRef = -0.19;
#include <utility>
#include <QTest>
#include <QTextStream>

namespace {

// Smallest rectangle covering pixels that pass `accept`.
QRect boundsOf(const QImage &image, bool (*accept)(const QColor &)) {
  QRect bounds;
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x)
      if (accept(image.pixelColor(x, y)))
        bounds |= QRect(x, y, 1, 1);
  return bounds;
}

bool isOpaque(const QColor &c) { return c.alpha() > 120; }
bool isEye(const QColor &c) {
  return c.alpha() > 120 && c.red() > 200 && c.green() > 200 && c.blue() > 200;
}

// Bounds of just the left or right eye, split at the midpoint of the pair.
QRect eyeBounds(const QImage &image, const QRect &pair, bool left) {
  const int mid = pair.center().x();
  QRect bounds;
  for (int y = pair.top(); y <= pair.bottom(); ++y)
    for (int x = pair.left(); x <= pair.right(); ++x) {
      if (left ? x > mid : x <= mid)
        continue;
      if (isEye(image.pixelColor(x, y)))
        bounds |= QRect(x, y, 1, 1);
    }
  return bounds;
}

} // namespace

int capturePoses(QApplication &app, Mascot &mascot, Orbits &orbits,
                 QQuickWindow *window, const QString &directory) {
  QDir().mkpath(directory);
  QTest::qWait(500);

  const auto shoot = [&](const QString &name) {
    // Never catch her mid-blink: a captured pose should be comparable.
    for (int i = 0; i < 60 && mascot.eyeLeftHeight() < 0.2; ++i)
      mascot.tick(1.0 / 60.0);
    orbits.setIntensity(mascot.rings()); // never inherit the previous pose
    window->requestUpdate();
    QTest::qWait(90);
    const QImage image = window->grabWindow();
    const bool ok = image.save(directory + "/" + name + ".png");
    QTextStream(stdout) << (ok ? "saved " : "FAILED ") << name << "\n";
  };

  mascot.setReducedMotion(true);
  struct Pose {
    const char *name;
    int form;
  };
  for (const Pose &pose : {Pose{"circle", Mascot::Circle},
                           Pose{"egg", Mascot::Egg},
                           Pose{"hex", Mascot::Hex},
                           Pose{"triangle", Mascot::Triangle},
                           Pose{"exclaim", Mascot::Exclaim},
                           Pose{"teardrop", Mascot::Teardrop},
                           Pose{"dots", Mascot::Dots},
                           Pose{"tiny", Mascot::Tiny}}) {
    mascot.rest();
    mascot.snapForm(pose.form);
    for (int i = 0; i < 40; ++i)
      mascot.tick(0.04); // let the dots spread and the springs settle
    shoot(pose.name);
  }

  // Transition sequences: five evenly-spaced frames through a morph, so the
  // in-between shapes can be checked against the reference's own frames.
  mascot.setReducedMotion(false);
  for (const auto &t : {std::pair<const char *, int>{"seq-dots", Mascot::Dots},
                        {"seq-exclaim", Mascot::Exclaim},
                        {"seq-triangle", Mascot::Triangle}}) {
    mascot.rest();
    mascot.changeForm(t.second);
    // The morph runs 0.22 s; sample it at 0, 25, 50, 75 and 100 per cent.
    const qreal step = 0.22 / 4.0;
    for (int i = 0; i < 5; ++i) {
      shoot(QStringLiteral("%1-%2").arg(t.first).arg(i));
      for (int f = 0; f < 4; ++f)
        mascot.tick(step / 4.0);
    }
  }
  mascot.setReducedMotion(true);

  // Gaze poses, for checking the eyes against the reference.
  for (const auto &g : {std::pair<const char *, std::pair<qreal, qreal>>
                            {"gaze-ahead", {0.0, 0.0}},
                        {"gaze-left", {-6.0, 0.0}},
                        {"gaze-right", {6.0, 0.0}},
                        {"gaze-up", {0.0, -6.0}},
                        {"gaze-down", {0.0, 6.0}},
                        {"gaze-upright", {4.0, -4.0}},
                        {"gaze-rest", {0.0, 0.0}}}) {
    mascot.rest();
    if (QString::fromLatin1(g.first) == "gaze-rest")
      mascot.lookIdle();
    else
      mascot.lookAt(g.second.first, g.second.second);
    for (int i = 0; i < 60; ++i)
      mascot.tick(0.04);
    shoot(g.first);
  }

  // Wink: one eye shut and held.
  mascot.rest();
  mascot.lookIdle();
  mascot.wink();
  for (int i = 0; i < 40; ++i)
    mascot.tick(1.0 / 60.0);
  shoot("wink");

  // Scatter, caught while the droplets are still spreading.
  mascot.rest();
  mascot.setReducedMotion(false);
  mascot.scatter();
  for (int i = 0; i < 14; ++i)
    mascot.tick(1.0 / 60.0);
  shoot("scatter");
  for (int i = 0; i < 12; ++i)
    mascot.tick(1.0 / 60.0);
  shoot("scatter-late");

  // Dash, at full stretch.
  mascot.rest();
  mascot.beginDash(2.5, 1.0);
  for (int i = 0; i < 25; ++i) {
    mascot.updateDash(2.5, 1.0);
    mascot.tick(1.0 / 60.0);
  }
  shoot("dash");
  mascot.rest();
  mascot.setReducedMotion(true);

  // Expression states on the idle body.
  mascot.rest();
  mascot.notify();
  for (int i = 0; i < 40; ++i)
    mascot.tick(0.04);
  shoot("notify");

  mascot.rest();
  mascot.setReducedMotion(false);
  mascot.think(6.0);
  for (int i = 0; i < 60; ++i) {
    mascot.tick(0.04);
    orbits.setIntensity(mascot.rings());
    orbits.advance(0.04);
  }
  shoot("thinking");

  // Mid-blink, caught deliberately rather than by luck: run until the eyes
  // are at their most closed, then grab that frame.
  mascot.rest();
  mascot.setReducedMotion(true);
  for (int i = 0; i < 1200; ++i) {
    mascot.tick(1.0 / 60.0);
    if (mascot.eyeHeight() < 0.05)
      break;
  }
  shoot("blink");

  app.exit(0);
  return 0;
}

int captureFilm(QApplication &app, Mascot &mascot, Orbits &orbits,
                QQuickWindow *window, const QString &directory) {
  QDir().mkpath(directory);
  QTest::qWait(500);

  const qreal step = 1.0 / 60.0;
  int frame = 0;
  const auto run = [&](qreal seconds) {
    const int frames = int(std::lround(seconds / step));
    for (int i = 0; i < frames; ++i) {
      mascot.tick(step);
      orbits.setIntensity(mascot.rings());
      orbits.advance(step);
      window->requestUpdate();
      QTest::qWait(18);
      window->grabWindow().save(
          QStringLiteral("%1/f%2.png").arg(directory).arg(frame++, 5, 10,
                                                          QChar('0')));
    }
  };

  // The reference's opening: she settles, collapses into the "..." run, holds
  // it, then comes back. 2.4 s, which is 144 frames.
  mascot.rest();
  mascot.lookIdle();
  mascot.setIdleAntics(false);
  mascot.setReducedMotion(false);
  run(0.5);
  mascot.changeForm(Mascot::Dots);
  run(1.0);
  mascot.changeForm(Mascot::Circle, Mascot::kSettleBack);
  run(0.9);

  // Then a stretch of thinking, so the tumble can be checked against the
  // reference's own silhouette rather than only against its numbers.
  const int dotsFrames = frame;
  mascot.rest();
  mascot.think(6.0);
  run(3.0);

  QTextStream(stdout) << "captured " << frame << " frames ("
                      << dotsFrames << " dots, " << frame - dotsFrames
                      << " thinking)\n";
  app.exit(0);
  return 0;
}

int runSelfTest(QApplication &app, Backend &backend, Mascot &mascot,
                Orbits &orbits, Theme &theme, Activity &activity,
                QQuickWindow *window, const QStringList &warnings,
                const QString &captureDir) {
  int failures = 0;
  QTextStream out(stdout);

  const auto check = [&](bool condition, const char *name) {
    out << (condition ? "PASS " : "FAIL ") << name << "\n";
    if (!condition)
      ++failures;
  };

  if (!captureDir.isEmpty())
    QDir().mkpath(captureDir);

  const auto capture = [&](const QString &name) {
    // The scene only rebuilds when the model has published a frame, so make
    // sure one has been emitted and presented before grabbing.
    window->requestUpdate();
    QTest::qWait(60);
    const QImage image = window->grabWindow();
    if (!captureDir.isEmpty())
      image.save(captureDir + "/" + name + ".png");
    return image;
  };

  // Let the window map and the first frames land before measuring anything.
  QTest::qWait(400);
  check(window->isVisible(), "the companion window is up");

  // --- preferences ---------------------------------------------------------
  check(qFuzzyCompare(backend.size(), 1.0), "default size is 100%");
  backend.configure("size", 1.4);
  check(qFuzzyCompare(backend.size(), 1.4), "size applies");
  backend.configure("size", 9.0);
  check(qFuzzyCompare(backend.size(), 1.4), "out-of-range size is rejected");
  backend.configure("size", 1.0);

  check(backend.colorMode() == "ink", "default colour is ink");
  check(backend.mascotColor() == QColor("#0a090c"),
        "ink mode uses the reference silhouette colour");
  backend.configure("colorMode", "theme");
  check(backend.mascotColor() != QColor("#0a090c"),
        "wallpaper mode takes its colour from the theme");
  backend.configure("colorMode", "ink");

  // --- Noctalia theme ------------------------------------------------------
  check(theme.available(), "Noctalia palette parsed");
  check(theme.colors().value("surface").value<QColor>() == QColor("#24273a"),
        "surface colour comes from noctalia.css");
  check(theme.colors().value("accent").value<QColor>() == QColor("#b7bdf8"),
        "accent colour comes from noctalia.css");
  check(theme.fontFamily() == "Google Sans Flex", "shell font is honoured");
  check(qAbs(theme.radius() - 0.75) < 0.001, "corner radius scale is honoured");
  check(qAbs(theme.motionScale() - 0.6) < 0.001, "animation speed is honoured");

  // --- placement and drag --------------------------------------------------
  backend.resetPlace();
  QTest::qWait(40);
  check(qAbs(backend.nx() - 0.86) < 0.001 && qAbs(backend.ny() - 0.74) < 0.001,
        "reset restores the default corner");

  const qreal startX = backend.nx();
  backend.grabDrag();
  check(backend.dragging() && mascot.mood() == Mascot::Held,
        "pressing her starts a drag");
  backend.dragBy(-400.0, 30.0);
  check(backend.nx() < startX - 0.02, "dragging left moves Nala left");
  check(backend.ny() > 0.0, "dragging down moves Nala down");
  backend.releaseDrag();
  check(!backend.dragging() && mascot.mood() != Mascot::Held,
        "releasing ends the drag");

  // She must never be pushed off the edge of the screen.
  backend.grabDrag();
  backend.dragBy(-99999.0, -99999.0);
  backend.releaseDrag();
  check(backend.nx() >= 0.0 && backend.ny() >= 0.0,
        "she cannot be dragged off the screen");
  backend.resetPlace();

  // --- flight --------------------------------------------------------------
  //
  // Thrown hard enough she does not just drop: she tucks into a speck and
  // streaks off, bounces off the edges, and arrives with a bounce.
  {
    backend.resetPlace();
    const qreal startX = backend.nx();
    backend.launch(-4000.0, 0.0, 4000.0);
    check(backend.flying(), "a hard throw launches her");
    check(mascot.dashing(), "she tucks in to fly");

    for (int i = 0; i < 20; ++i) {
      backend.advance(1.0 / 60.0);
      mascot.tick(1.0 / 60.0);
    }
    check(backend.nx() < startX - 0.05, "she travels the way she was thrown");
    check(mascot.dashLength() > 0.2, "she trails behind her while flying");
    check(mascot.formB() == Mascot::Tiny, "she is a speck while flying");

    // She must come to rest on screen, not sail off the edge.
    for (int i = 0; i < 1200 && backend.flying(); ++i) {
      backend.advance(1.0 / 60.0);
      mascot.tick(1.0 / 60.0);
    }
    check(!backend.flying(), "she comes to rest");
    check(!mascot.dashing(), "she unfolds when she lands");
    check(backend.nx() >= 0.0 && backend.nx() <= 1.0 && backend.ny() >= 0.0 &&
              backend.ny() <= 1.0,
          "she stays on the screen");

    for (int i = 0; i < 90; ++i)
      mascot.tick(1.0 / 60.0);
    check(mascot.dashLength() < 0.05, "the trail fades once she lands");
    check(mascot.formB() == Mascot::Circle, "she is herself again");
    backend.resetPlace();
  }

  // A gentle release is not a throw.
  {
    backend.grabDrag();
    backend.dragBy(3.0, 0.0);
    backend.releaseDrag();
    check(!backend.flying(), "a gentle release just puts her down");
  }

  // --- demo ----------------------------------------------------------------
  //
  // One command that runs the whole repertoire, for when you would rather not
  // wait on her own timing.
  {
    backend.resetPlace();
    mascot.rest();
    backend.demo();
    check(backend.demoing(), "the demo starts");

    bool sawWink = false, sawThink = false, sawAlert = false;
    bool sawBadge = false, sawScatter = false, sawShape = false;
    for (int i = 0; i < 60 * 32 && backend.demoing(); ++i) {
      backend.advance(1.0 / 60.0);
      mascot.tick(1.0 / 60.0);
      if (mascot.winking()) sawWink = true;
      if (mascot.rings() > 0.5) sawThink = true;
      if (mascot.formB() == Mascot::Exclaim) sawAlert = true;
      if (mascot.badge() > 0.5) sawBadge = true;
      if (!mascot.droplets().isEmpty()) sawScatter = true;
      if (mascot.formB() == Mascot::Egg || mascot.formB() == Mascot::Hex)
        sawShape = true;
    }
    check(!backend.demoing(), "and finishes on its own");
    check(sawWink && sawShape, "the demo shows the quiet things");
    check(sawThink && sawAlert && sawBadge && sawScatter,
          "and every one of the showpieces");

    // Leave nothing running behind it. The demo ends on a dash, which takes
    // a while to come to rest, so wait it out rather than assuming.
    for (int i = 0; i < 60 * 20 &&
                    (backend.flying() || mascot.mood() != Mascot::Resting ||
                     !mascot.droplets().isEmpty());
         ++i) {
      backend.advance(1.0 / 60.0);
      mascot.tick(1.0 / 60.0);
    }
    check(!backend.flying() && mascot.droplets().isEmpty() &&
              mascot.mood() == Mascot::Resting,
          "and leaves her settled afterwards");
    backend.resetPlace();
  }

  // --- gaze ----------------------------------------------------------------
  mascot.setReducedMotion(true);
  mascot.lookAt(3.0, 0.0);
  for (int i = 0; i < 12; ++i)
    mascot.tick(0.05);
  const qreal lookRight = (mascot.eyeLeftX() + mascot.eyeRightX()) * 0.5;
  mascot.lookAt(-3.0, 0.0);
  for (int i = 0; i < 12; ++i)
    mascot.tick(0.05);
  const qreal lookLeft = (mascot.eyeLeftX() + mascot.eyeRightX()) * 0.5;
  check(lookRight > lookLeft, "eyes track the cursor horizontally");

  mascot.lookAt(0.0, 3.0);
  for (int i = 0; i < 12; ++i)
    mascot.tick(0.05);
  const qreal lookDown = (mascot.eyeLeftY() + mascot.eyeRightY()) * 0.5;
  mascot.lookAt(0.0, -3.0);
  for (int i = 0; i < 12; ++i)
    mascot.tick(0.05);
  check(lookDown > (mascot.eyeLeftY() + mascot.eyeRightY()) * 0.5,
        "eyes track the cursor vertically");
  // The compositor -> backend -> mascot path has arithmetic of its own (the
  // offset is converted into units of her radius), so exercise it end to end
  // rather than only calling lookAt() directly.
  const QPointF centre = backend.centreOnScreen();
  backend.injectCursor(int(centre.x()) + 600, int(centre.y()));
  for (int i = 0; i < 12; ++i)
    mascot.tick(0.05);
  const qreal trackedRight = (mascot.eyeLeftX() + mascot.eyeRightX()) * 0.5;
  backend.injectCursor(int(centre.x()) - 600, int(centre.y()));
  for (int i = 0; i < 12; ++i)
    mascot.tick(0.05);
  check(trackedRight > (mascot.eyeLeftX() + mascot.eyeRightX()) * 0.5,
        "a cursor reported by the compositor moves her gaze");

  // A cursor sitting exactly on her should leave the gaze pointing straight
  // out; if the centre were computed from the wrong origin this would skew.
  backend.injectCursor(int(centre.x()), int(centre.y()));
  for (int i = 0; i < 20; ++i)
    mascot.tick(0.05);
  const qreal centredX = (mascot.eyeLeftX() + mascot.eyeRightX()) * 0.5;
  const qreal centredY = (mascot.eyeLeftY() + mascot.eyeRightY()) * 0.5;
  check(qAbs(centredX) < 0.06 && qAbs(centredY) < 0.06,
        "a cursor on top of her leaves the gaze centred");

  backend.injectCursor(int(centre.x()), int(centre.y()) + 600);
  for (int i = 0; i < 20; ++i)
    mascot.tick(0.05);
  check((mascot.eyeLeftY() + mascot.eyeRightY()) * 0.5 > centredY + 0.05,
        "a cursor below her pulls the gaze down");

  // The eyes are carried on a sphere, so the reference shows three things a
  // flat translation cannot produce. All three were measured off the video.
  {
    mascot.setIdleAntics(false); // a glance would move the eyes underneath us
    const auto settleGaze = [&] {
      for (int i = 0; i < 40; ++i)
        mascot.tick(0.05);
    };

    mascot.lookAt(0.0, 0.0);
    settleGaze();
    const qreal gapAhead = mascot.eyeRightX() - mascot.eyeLeftX();

    mascot.lookAt(6.0, 0.0);
    settleGaze();
    const qreal gapAside = mascot.eyeRightX() - mascot.eyeLeftX();
    const qreal reach = (mascot.eyeLeftX() + mascot.eyeRightX()) * 0.5;

    check(gapAside < gapAhead * 0.85,
          "the eyes draw together as the gaze swings aside");
    // The old flat-eye model topped out at 0.34 R, which read as barely
    // moving. The fitted sphere carries the pair to about 0.48 R.
    check(reach > 0.42,
          "a glance carries the eyes as far as the reference does");
    check(mascot.eyeRightScaleX() < 0.9,
          "an eye foreshortens as it approaches the edge");

    mascot.lookAt(4.0, 4.0);
    settleGaze();
    check(qAbs(mascot.eyeRightY() - mascot.eyeLeftY()) > 0.02,
          "the pair tilts when the gaze is both aside and down");

    mascot.lookAt(0.0, 0.0);
    settleGaze();
    check(qAbs(mascot.eyeRightY() - mascot.eyeLeftY()) < 0.02,
          "the pair is level when looking straight ahead");
    mascot.setIdleAntics(true);
  }

  mascot.lookIdle();
  mascot.setReducedMotion(false);

  // --- morphing ------------------------------------------------------------
  // Held still: these time a specific morph, and a spontaneous one would land
  // on top of it.
  mascot.setIdleAntics(false);
  // Transitions in the reference run 0.20 s on average and never exceed
  // 0.33 s, so nothing here should be slower than that.
  {
    mascot.rest();
    const qreal step = 1.0 / 60.0;
    mascot.changeForm(Mascot::Triangle);
    int frames = 0;
    while (mascot.formMix() < 1.0 && frames < 120) {
      mascot.tick(step);
      ++frames;
    }
    const qreal seconds = frames * step;
    out << "   default morph " << int(seconds * 1000)
        << " ms (reference 200, never over 333)\n";
    check(seconds <= 0.34, "a morph is no slower than the reference's");
    check(seconds >= 0.12, "a morph is not instant");
  }

  mascot.rest();
  mascot.changeForm(Mascot::Hex, 0.4);
  mascot.tick(0.06);
  check(mascot.formMix() > 0.0 && mascot.formMix() < 1.0,
        "a morph blends over time rather than snapping");
  check(mascot.formA() == Mascot::Circle && mascot.formB() == Mascot::Hex,
        "a morph interpolates between the two forms");
  for (int i = 0; i < 20; ++i)
    mascot.tick(0.04);
  check(mascot.formMix() >= 1.0 && mascot.formA() == Mascot::Hex,
        "a morph settles on its target");

  // Interrupting mid-morph must not tear the silhouette.
  mascot.changeForm(Mascot::Triangle, 0.4);
  mascot.tick(0.05);
  mascot.changeForm(Mascot::Circle, 0.4);
  check(mascot.formB() == Mascot::Triangle,
        "an interrupted morph lands before the next one starts");
  for (int i = 0; i < 30; ++i)
    mascot.tick(0.04);
  check(mascot.formB() == Mascot::Circle, "the queued morph then runs");

  mascot.setIdleAntics(true);

  // --- blink ---------------------------------------------------------------
  //
  // Timings measured over the 34 blinks in the reference: a 243 ms cycle that
  // shuts completely, spending longer closing than opening.
  {
    mascot.setIdleAntics(false); // a flourish would move the eyes as well
    mascot.rest();
    mascot.lookIdle();
    const qreal open = mascot.eyeLeftHeight();
    const qreal step = 1.0 / 60.0;

    int closing = 0, opening = 0, shut = 0;
    bool seen = false, past = false;
    qreal minimum = open;
    for (int i = 0; i < 1200 && !past; ++i) { // up to 20 s
      mascot.tick(step);
      const qreal h = mascot.eyeLeftHeight();
      minimum = std::min(minimum, h);
      if (h < open * 0.92) {
        seen = true;
        if (h < open * 0.06)
          ++shut;
        else if (shut == 0)
          ++closing;
        else
          ++opening;
      } else if (seen) {
        past = true;
      }
    }

    check(seen, "she blinks");
    check(minimum < open * 0.05, "a blink shuts her eyes completely");

    const qreal cycle = (closing + shut + opening) * step;
    out << "   blink cycle " << int(cycle * 1000) << " ms (reference 243), "
        << "closing " << int(closing * step * 1000) << " ms vs opening "
        << int(opening * step * 1000) << " ms (reference 121 vs 84)\n";
    check(cycle > 0.18 && cycle < 0.32,
          "a blink takes about as long as the reference's");
    check(closing > opening,
          "her lids close more slowly than they open, as the reference's do");
    mascot.setIdleAntics(true);
  }

  // --- wink ----------------------------------------------------------------
  //
  // The reference holds one eye shut for two or three seconds while the other
  // stays a full slit, then blinks out of it.
  {
    mascot.setIdleAntics(false);
    mascot.rest();
    mascot.lookIdle();
    const qreal open = mascot.eyeLeftHeight();
    mascot.wink();
    for (int i = 0; i < 30; ++i)
      mascot.tick(1.0 / 60.0);

    check(mascot.winking(), "she can wink");
    check(mascot.eyeRightHeight() < open * 0.25,
          "a wink shuts one eye");
    check(mascot.eyeLeftHeight() > open * 0.75,
          "a wink leaves the other eye open");

    // Held, not a blink: still shut a second later.
    for (int i = 0; i < 60; ++i)
      mascot.tick(1.0 / 60.0);
    check(mascot.eyeRightHeight() < open * 0.25, "a wink is held");

    // She blinks on her own schedule, so look for the eye reaching full height
    // somewhere in the window rather than at one arbitrary instant.
    qreal recovered = 0.0;
    for (int i = 0; i < 400; ++i) {
      mascot.tick(1.0 / 60.0);
      if (!mascot.winking())
        recovered = std::max(recovered, mascot.eyeRightHeight());
    }
    check(!mascot.winking(), "the wink ends");
    check(recovered > open * 0.75, "and the eye opens again");
    mascot.setIdleAntics(true);
  }

  // --- scatter -------------------------------------------------------------
  {
    mascot.rest();
    check(mascot.droplets().isEmpty(), "no droplets at rest");
    mascot.scatter();
    mascot.tick(1.0 / 60.0);
    const QVariantList thrown = mascot.droplets();
    check(thrown.size() >= 5, "breaking apart throws off droplets");

    const auto spread = [&] {
      qreal furthest = 0.0;
      for (const QVariant &v : mascot.droplets()) {
        const QVariantMap d = v.toMap();
        furthest = std::max(furthest, std::hypot(d["x"].toDouble(),
                                                 d["y"].toDouble()));
      }
      return furthest;
    };
    const qreal near = spread();
    for (int i = 0; i < 12; ++i)
      mascot.tick(1.0 / 60.0);
    check(spread() > near, "droplets drift outward");
    check(mascot.formB() == Mascot::Tiny, "she collapses as she scatters");

    for (int i = 0; i < 240; ++i)
      mascot.tick(1.0 / 60.0);
    check(mascot.droplets().isEmpty(), "droplets fade away");
  }

  // --- reactions -----------------------------------------------------------
  // Held still throughout: these check what a specific interaction does, and
  // a spontaneous flourish landing in the middle would answer for it.
  mascot.setIdleAntics(false);
  mascot.poke();
  mascot.tick(0.05);
  check(mascot.mood() == Mascot::Happy, "a click makes her happy");
  check(mascot.squashX() > 1.0 && mascot.squashY() < 1.0,
        "a click squashes her on impact");
  // The triangle is the thinking shape and nothing else. A poke that also
  // produced one made every click look like the orbit rings had failed.
  for (int i = 0; i < 60; ++i)
    mascot.tick(1.0 / 60.0);
  check(mascot.formB() != Mascot::Triangle,
        "a click does not make the thinking shape");
  check(mascot.rings() < 0.05, "and brings up no rings");

  // The rings come up fast and fade slowly: 117 ms against 683 ms.
  mascot.rest();
  mascot.think(6.0);
  int ringsUp = 0;
  while (mascot.rings() < 0.8 && ringsUp < 120) {
    mascot.tick(1.0 / 60.0);
    ++ringsUp;
  }
  const qreal tumbleStart = mascot.tumble();
  qreal areaMin = 2.0, areaMax = 0.0;
  for (int i = 0; i < 240; ++i) { // 4 s, more than one full turn
    mascot.tick(1.0 / 60.0);
    areaMin = std::min(areaMin, mascot.projectedArea());
    areaMax = std::max(areaMax, mascot.projectedArea());
  }
  const qreal tumbled = (mascot.tumble() - tumbleStart) / 4.0;
  const qreal swing = (areaMax - areaMin) / ((areaMax + areaMin) / 2.0);
  out << "   rings up in " << int(ringsUp / 60.0 * 1000)
      << " ms (reference 117), tumble " << tumbled << " rad/s (reference 1.46)"
      << "\n   projected area " << areaMin << " to " << areaMax << ", swing "
      << qRound(swing * 100) << "% (reference 42%)\n";
  check(ringsUp / 60.0 < 0.22, "the rings come up quickly");
  // 1.46 rad/s of apparent spin, measured over the stretch where she is a
  // triangle throughout. An earlier figure of 1.015 came from a window that
  // also spanned her morph back to a circle, which drags the estimate down.
  check(tumbled > 1.1 && tumbled < 1.8, "she tumbles at the reference's rate");

  // The tumble is in three dimensions, not a flat spin. A flat spin keeps the
  // projected area at exactly 1; the reference's silhouette loses half its
  // area as she turns edge-on.
  check(areaMin < 0.75, "she foreshortens as she turns, rather than spinning flat");
  check(areaMax > 0.95, "and comes back to face-on");
  check(swing > 0.30 && swing < 0.60,
        "and foreshortens by about as much as the reference does");

  // She unfolds back to a circle while the rings are still up, not with them.
  {
    mascot.rest();
    mascot.think(2.0);
    bool circleWhileRingsUp = false;
    for (int i = 0; i < 170; ++i) {
      mascot.tick(1.0 / 60.0);
      if (mascot.formB() == Mascot::Circle && mascot.rings() > 0.85)
        circleWhileRingsUp = true;
    }
    check(circleWhileRingsUp,
          "she unfolds before the rings fade, rather than with them");
  }

  mascot.rest();
  int ringsDown = 0;
  mascot.think(0.1);
  for (int i = 0; i < 30; ++i)
    mascot.tick(1.0 / 60.0);
  while (mascot.rings() > 0.2 && ringsDown < 180) {
    mascot.tick(1.0 / 60.0);
    ++ringsDown;
  }
  out << "   rings fade over " << int(ringsDown / 60.0 * 1000)
      << " ms (reference 683)\n";
  check(ringsDown / 60.0 > 0.35, "and fade away slowly");

  mascot.think(4.0);
  for (int i = 0; i < 30; ++i)
    mascot.tick(0.04);
  check(mascot.rings() > 0.4, "thinking raises the orbit rings");
  orbits.setIntensity(mascot.rings());
  const qreal phaseBefore = orbits.rings().first().phase;
  orbits.advance(0.25);
  check(!qFuzzyCompare(orbits.rings().first().phase, phaseBefore),
        "orbit rings advance while she thinks");

  for (int i = 0; i < 30; ++i) // let the thinking morph land
    mascot.tick(0.04);
  mascot.alert();
  mascot.tick(0.05);
  check(mascot.formB() == Mascot::Exclaim, "an alert becomes an exclamation");
  check(mascot.mood() == Mascot::Alert, "an alert changes her mood");
  for (int i = 0; i < 40; ++i)
    mascot.tick(0.04);

  mascot.notify();
  {
    // The badge pops: measured on the reference it overshoots its settled size
    // by 19.5% about a third of a second in.
    qreal peak = 0.0;
    int toPeak = 0;
    for (int i = 0; i < 120; ++i) {
      mascot.tick(1.0 / 60.0);
      if (mascot.badge() > peak) {
        peak = mascot.badge();
        toPeak = i;
      }
    }
    out << "   badge peaks at " << qRound((peak - 1.0) * 100.0) << "% over, "
        << int(toPeak / 60.0 * 1000) << " ms in (reference +19.5%, ~330 ms)\n";
    check(peak > 1.08 && peak < 1.35, "the badge pops as it arrives");
    check(qAbs(mascot.badge() - 1.0) < 0.06, "and settles at its full size");
  }
  check(mascot.badge() > 0.5, "a notification shows the badge");
  check(mascot.eyeWidth() > 0.19, "a notification widens her eyes");

  // --- secondary motion ----------------------------------------------------
  //
  // Three things measured off the reference that are easy to get wrong.
  {
    const qreal step = 1.0 / 60.0;

    // She breathes: the height/width ratio swells by about 1.2% at 0.31 Hz,
    // and she sinks as she widens.
    mascot.rest();
    mascot.setReducedMotion(false);
    mascot.setIdleAntics(false); // a flourish would drown out the swell
    qreal wideX = 0.0, narrowX = 2.0, lowY = -1.0, highY = 1.0;
    qreal ratioMin = 10.0, ratioMax = 0.0;
    qreal atWidest = 0.0;
    for (int i = 0; i < 400; ++i) { // ~6.7 s, two full breaths
      mascot.tick(step);
      const qreal ratio = mascot.squashY() / mascot.squashX();
      if (mascot.squashX() > wideX) {
        wideX = mascot.squashX();
        atWidest = mascot.bobY();
      }
      narrowX = std::min(narrowX, mascot.squashX());
      lowY = std::max(lowY, mascot.bobY());
      highY = std::min(highY, mascot.bobY());
      ratioMin = std::min(ratioMin, ratio);
      ratioMax = std::max(ratioMax, ratio);
    }
    const qreal swell = (ratioMax - ratioMin) / 2.0;
    // bobY is in half-item units; on screen it is half that.
    out << "   idle swell " << swell * 100.0 << "% (reference 1.2-1.8%), "
        << "rise " << (lowY - highY) / 4.0 << " of the body (reference 0.0136)"
        << "\n";
    check(swell > 0.006 && swell < 0.025, "she breathes at rest");
    check(lowY - highY > 0.02, "and drifts vertically as she does");
    check(atWidest > 0.0, "she sinks as she widens, rather than rising");

    // Reduced motion means exactly that.
    mascot.setReducedMotion(true);
    for (int i = 0; i < 60; ++i)
      mascot.tick(step);
    check(qAbs(mascot.bobY()) < 0.001 &&
              qAbs(mascot.squashX() - 1.0) < 0.001,
          "reduced motion holds her still");
    mascot.setReducedMotion(false);
    mascot.setIdleAntics(true);

    // An alert swings into its lean and stops; the reference never wobbles.
    mascot.rest();
    mascot.alert();
    qreal previous = mascot.roll();
    const qreal initial = previous;
    int reversals = 0;
    for (int i = 0; i < 90; ++i) {
      mascot.tick(step);
      const qreal now = mascot.roll();
      if ((now - previous) * (initial > 0 ? -1.0 : 1.0) < -1e-4)
        ++reversals;
      previous = now;
    }
    check(qAbs(initial) > 0.1, "an alert arrives under-leaned");
    check(qAbs(mascot.roll()) < 0.02, "and settles into its lean");
    check(reversals == 0, "without wobbling on the way");

    mascot.rest();
    mascot.alert();
    int settleFrames = 0;
    while (qAbs(mascot.roll()) > qAbs(kAlertOverleanRef) * 0.05 &&
           settleFrames < 200) {
      mascot.tick(step);
      ++settleFrames;
    }
    out << "   alert settles in " << int(settleFrames * step * 1000)
        << " ms from " << qRound(qAbs(initial) * 180.0 / M_PI)
        << " deg (reference ~350 ms from 11 deg)\n";

    // Size eases; it does not bounce past where it is going.
    for (const char *what : {"poke", "wake", "notify"}) {
      mascot.rest();
      if (QString::fromLatin1(what) == "poke")
        mascot.poke();
      else if (QString::fromLatin1(what) == "notify")
        mascot.notify();
      else {
        mascot.setSleepWhenIdle(true);
        for (int i = 0; i < 1500; ++i) // comfortably past the sleep threshold
          mascot.tick(0.1);
        mascot.wake();
      }
      qreal peak = 0.0;
      for (int i = 0; i < 180; ++i) {
        mascot.tick(step);
        peak = std::max(peak, mascot.bodyScale());
      }
      check(peak < 1.02, "her size does not overshoot");
    }

    mascot.rest();
    mascot.setSleepWhenIdle(true);
    for (int i = 0; i < 1500; ++i) // comfortably past the sleep threshold
      mascot.tick(0.1);
    check(mascot.sleeping(), "she is asleep before the swell is measured");
    mascot.wake();
    int rise = 0;
    while (mascot.bodyScale() < 0.98 && rise < 300) {
      mascot.tick(step);
      ++rise;
    }
    out << "   size rises to full in " << int(rise * step * 1000)
        << " ms (reference 583-650)\n";
    check(rise * step > 0.35 && rise * step < 0.95,
          "she takes about as long as the reference to swell back");
  }

  // --- what she does when left alone ---------------------------------------
  //
  // The point of dealing antics from a bag is that every one of them turns up.
  // Drawn independently, the orbit rings were a 1-in-12 pick fired about five
  // times before she fell asleep, so better than half of all sessions never
  // showed them at all.
  {
    mascot.rest();
    mascot.setIdleAntics(true);
    mascot.setSleepWhenIdle(false); // measure the repertoire, not the nap
    mascot.setReducedMotion(false);
    mascot.lookIdle();

    bool sawThink = false, sawWink = false, sawScatter = false;
    bool sawEgg = false, sawHex = false;
    int frames = 0;
    const int limit = 60 * 100; // 100 s
    for (; frames < limit; ++frames) {
      mascot.tick(1.0 / 60.0);
      if (mascot.rings() > 0.5) sawThink = true;
      if (mascot.winking()) sawWink = true;
      if (!mascot.droplets().isEmpty()) sawScatter = true;
      if (mascot.formB() == Mascot::Egg) sawEgg = true;
      if (mascot.formB() == Mascot::Hex) sawHex = true;
      if (sawThink && sawWink && sawScatter && sawEgg && sawHex)
        break;
    }
    out << "   full repertoire seen within " << frames / 60 << " s\n";
    check(sawThink, "she thinks, rings and all, without being asked");
    check(sawWink, "she winks");
    check(sawScatter, "she comes apart");
    check(sawEgg && sawHex, "she tries other shapes");
    check(frames < limit, "and gets through the whole repertoire promptly");
  }

  // Reduced motion reduces rather than eliminates: she still looks about and
  // winks, but does not morph, tumble or come apart.
  {
    mascot.rest();
    mascot.setReducedMotion(true);
    mascot.setSleepWhenIdle(false);
    bool moved = false, morphed = false, scattered = false;
    const qreal restX = mascot.eyeLeftX();
    for (int i = 0; i < 60 * 90; ++i) {
      mascot.tick(1.0 / 60.0);
      if (qAbs(mascot.eyeLeftX() - restX) > 0.08) moved = true;
      if (mascot.formB() != Mascot::Circle) morphed = true;
      if (!mascot.droplets().isEmpty()) scattered = true;
    }
    check(moved, "reduced motion still lets her look about");
    check(!morphed, "but she keeps her shape");
    check(!scattered, "and does not come apart");
    mascot.setReducedMotion(false);
    mascot.setSleepWhenIdle(true);
  }

  // --- reacting to the desktop ---------------------------------------------
  //
  // A timer alone makes her a slideshow. These give the thinking and the badge
  // something to actually mean.
  {
    backend.configure("reactToDesktop", true);
    mascot.rest();
    mascot.setIdleAntics(false);
    mascot.setSleepWhenIdle(false);

    activity.injectLoad(1.4); // the machine gets busy
    check(activity.busy(), "she notices the machine working");
    bool thought = false;
    for (int i = 0; i < 60 * 20 && !thought; ++i) {
      backend.advance(1.0 / 60.0);
      mascot.tick(1.0 / 60.0);
      if (mascot.rings() > 0.5)
        thought = true;
    }
    check(thought, "and thinks while it does");

    activity.injectLoad(0.2); // and settles down again
    check(!activity.busy(), "she notices it going quiet");

    // Hysteresis: a load hovering near the line must not flicker her.
    activity.injectLoad(0.6);
    check(!activity.busy(), "a middling load does not set her off");

    // A notification is what the badge has always been for.
    mascot.rest();
    for (int i = 0; i < 40; ++i)
      mascot.tick(1.0 / 60.0);
    activity.injectNotification();
    for (int i = 0; i < 60; ++i)
      mascot.tick(1.0 / 60.0);
    check(mascot.badge() > 0.5, "a notification brings up the badge");

    // And the switch really switches it off.
    backend.configure("reactToDesktop", false);
    mascot.rest();
    activity.injectNotification();
    for (int i = 0; i < 60; ++i)
      mascot.tick(1.0 / 60.0);
    check(mascot.badge() < 0.1, "unless she has been told not to");
    backend.configure("reactToDesktop", true);
    mascot.setIdleAntics(true);
    mascot.setSleepWhenIdle(true);
  }

  // --- nodding off ---------------------------------------------------------
  //
  // Sleep used to arrive out of nowhere. She now flags first, which you can
  // see in her lids.
  {
    mascot.rest();
    mascot.setSleepWhenIdle(true);
    mascot.setIdleAntics(false);
    const qreal awake = mascot.eyeLeftHeight();
    check(mascot.drowsiness() < 0.01, "she starts out wide awake");

    for (int i = 0; i < 900; ++i) // 90 s, well into flagging
      mascot.tick(0.1);
    const qreal flagging = mascot.drowsiness();
    check(flagging > 0.1 && flagging < 0.9, "she flags before she sleeps");
    check(!mascot.sleeping(), "without being asleep yet");
    // Sample over a second and take the widest her eye gets, so a blink
    // cannot pass for a droop.
    qreal widest = 0.0;
    for (int i = 0; i < 60; ++i) {
      mascot.tick(1.0 / 60.0);
      widest = std::max(widest, mascot.eyeLeftHeight());
    }
    check(widest < awake * 0.85, "and her lids lower");

    for (int i = 0; i < 600; ++i)
      mascot.tick(0.1);
    check(mascot.sleeping(), "then she goes under");
    mascot.wake();
    mascot.setIdleAntics(true);
  }

  mascot.setIdleAntics(true);

  // --- sleep ---------------------------------------------------------------
  mascot.setSleepWhenIdle(true);
  mascot.lookIdle();
  for (int i = 0; i < 1500; ++i) // 150 s of neglect, at the dt cap
    mascot.tick(0.1);
  check(mascot.sleeping(), "she falls asleep when left alone");
  mascot.wake();
  check(!mascot.sleeping(), "she wakes on interaction");
  for (int i = 0; i < 30; ++i)
    mascot.tick(0.04);

  // --- rendering -----------------------------------------------------------
  mascot.setReducedMotion(true);
  mascot.rest();
  orbits.setIntensity(0.0);
  for (int i = 0; i < 25; ++i)
    mascot.tick(0.04);

  // The body is a GPU shader. The `offscreen` QPA plugin has no swapchain, so
  // the shader never compiles there and every pixel assertion below would fail
  // for a reason that has nothing to do with Nala. Detect that and skip rather
  // than cry wolf -- `ctest` covers behaviour, a display covers appearance.
  auto *shader = window->findChild<QObject *>("bodyShader");
  check(shader != nullptr, "the body shader exists");
  const bool rendered =
      shader && shader->property("status").toInt() == 0; // 0 == Compiled
  if (!rendered) {
    out << "SKIP appearance checks — no GPU surface (run with a display "
           "attached to verify them)\n";
    if (shader && !shader->property("log").toString().isEmpty())
      out << "   shader log: " << shader->property("log").toString() << "\n";
  }

  const QImage idle = capture("idle");
  if (rendered)
    check(!idle.isNull() && idle.width() > 8, "the window grab is usable");
  if (rendered) {
    const QRect bodyBox = boundsOf(idle, isOpaque);
    const QRect eyeBox = boundsOf(idle, isEye);

    check(qAlpha(idle.pixel(2, 2)) < 20, "the corners stay transparent");
    check(bodyBox.width() > idle.width() / 3 &&
              bodyBox.height() > idle.height() / 3,
          "the body renders");
    check(qAbs(bodyBox.width() - bodyBox.height()) <= 3,
          "the idle body is round");

    // Proportions measured from the reference animation. The drawing stage is
    // the largest square inside the window, so compare against that -- a
    // window manager may well have given us a non-square window.
    const int canvas = std::min(idle.width(), idle.height());
    const qreal bodyRatio = qreal(bodyBox.width()) / canvas;
    out << "   body/canvas " << bodyRatio << " (reference "
        << 1.0 / 1.89 << ")\n";
    check(qAbs(bodyRatio - 1.0 / 1.89) < 0.06,
          "the body matches the reference proportion");

    check(eyeBox.width() > 4 && eyeBox.height() > 4, "the eyes render");
    check(bodyBox.contains(eyeBox), "the eyes stay inside the body");
    check(eyeBox.center().y() < bodyBox.center().y(),
          "the eyes sit above the body's centre");

    const QRect leftEye = eyeBounds(idle, eyeBox, true);
    const QRect rightEye = eyeBounds(idle, eyeBox, false);
    check(leftEye.isValid() && rightEye.isValid(), "there are two eyes");
    if (leftEye.isValid() && rightEye.isValid()) {
      check(leftEye.height() > leftEye.width(),
            "a single idle eye is taller than it is wide");
      // The reference slit is about 1.7 times as tall as it is wide.
      const qreal aspect = qreal(leftEye.height()) / leftEye.width();
      check(aspect > 1.35 && aspect < 2.15,
            "the eye slit matches the reference aspect");
      const qreal radius = bodyBox.width() / 2.0;
      const qreal gap = (rightEye.center().x() - leftEye.center().x()) / radius;
      out << "   eye gap " << gap << "R (reference 0.47), slit aspect "
          << qreal(leftEye.height()) / leftEye.width()
          << " (reference 1.71)\n";
      check(qAbs(gap - 0.47) < 0.12,
            "the eyes sit the reference distance apart");
    }
  }

  // The morph must never blink out of existence mid-transition.
  mascot.setReducedMotion(false);
  for (int form : {int(Mascot::Dots), int(Mascot::Exclaim),
                   int(Mascot::Triangle), int(Mascot::Circle)}) {
    mascot.changeForm(form, 0.35);
    for (int step = 0; step < 3; ++step) {
      for (int i = 0; i < 3; ++i)
        mascot.tick(0.04);
      const QImage frame = capture(QStringLiteral("morph-%1-%2")
                                       .arg(form)
                                       .arg(step));
      if (!rendered)
        continue;
      const QRect box = boundsOf(frame, isOpaque);
      check(box.width() > 6 && box.height() > 6,
            "the silhouette survives every morph frame");
    }
    for (int i = 0; i < 20; ++i)
      mascot.tick(0.04);
  }

  // --- preferences window --------------------------------------------------
  backend.openSettings();
  QTest::qWait(400);
  auto *settings = window->findChild<QQuickWindow *>("settingsWindow");
  check(settings != nullptr, "the preferences window exists");
  if (settings && !captureDir.isEmpty())
    settings->grabWindow().save(captureDir + "/preferences.png");

  // --- autostart -----------------------------------------------------------
  const bool hadAutostart = backend.startAtLogin();
  backend.setStartAtLogin(true);
  check(backend.startAtLogin(), "autostart can be enabled");
  backend.setStartAtLogin(false);
  check(!backend.startAtLogin(), "autostart can be disabled");
  if (hadAutostart)
    backend.setStartAtLogin(true);

  check(warnings.isEmpty(), "no QML warnings");
  for (const QString &warning : warnings)
    QTextStream(stderr) << warning << "\n";

  out << "RESULT " << failures << " failures\n";
  app.exit(failures ? 1 : 0);
  return failures ? 1 : 0;
}
