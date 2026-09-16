#include "mascot.h"
#include <QScopeGuard>
#include <QtMath>
#include <algorithm>

namespace {

// The eyes are features on a sphere just under the surface of her body, not
// decals sliding across a flat face. That is what the reference does, and it
// is why a glance carries them so far, draws them together as they approach
// the edge, and tilts the pair.
//
// Fitted to the reference by least squares over the frames where the eyes are
// normally open, solving (x/s)^2 + (gap/g0)^2 = 1. It reproduces the measured
// separation across the whole sweep:
//
//   |pair x|   0.00-0.12  0.12-0.25  0.25-0.38  0.38-0.75
//   measured       0.560      0.519      0.468      0.432
//   model          0.555      0.540      0.483      0.426
// The fit's two axes are s*cos(g) = 0.654 and 2*s*sin(g) = 0.559, not s and
// the gap themselves -- reading them off directly leaves the gaze travelling
// 10% short of the reference.
constexpr qreal kEyeSphere = 0.711;
constexpr qreal kEyeHalfGap = 0.404; // half the angular separation, radians

// Rest orientation, and how far a glance can carry the gaze. The reference
// sweeps the pair centre across roughly ±0.63 R horizontally and ±0.50 R
// vertically -- far further than a small translation would suggest.
constexpr qreal kRestYaw = 0.343;
constexpr qreal kRestPitch = -0.311;
// Capped so the outer eye never swings past the limb and vanishes. At the
// limit the pair centre reaches 0.53 R, which is where the reference's own
// wide glances sit (its rare 0.66 R outliers are morph frames, not glances).
constexpr qreal kMaxYaw = 0.95;
constexpr qreal kMaxPitch = 0.88;

// How much of the sphere's natural tilt to keep. The full rotation would swing
// the pair to 0.32 R of tilt at the extremes; the reference never exceeds
// 0.185, so only part of it is applied.
constexpr qreal kTiltDamping = 0.55;

// The slits lean with the sphere's surface. The reference shows the sign
// clearly -- leaning right when she looks up, left when she looks down -- but
// the magnitude is noisy, so only part of the geometric angle is applied.
constexpr qreal kSlitRotation = 0.45;

// How far short of its final lean the exclamation mark arrives: 11 degrees,
// measured off the reference's first frames.
constexpr qreal kAlertOverlean = -0.19;

// Rings: 117 ms to reach full, 683 ms to fade away again.
constexpr qreal kRingsRise = 13.8;
constexpr qreal kRingsFall = 2.7;

// Body tumble while thinking. She does not spin flat: the reference's
// silhouette loses half its area and swings its width/height ratio by 46% as
// she turns, and its bounding box goes from 0.75 full down to 0.33. A flat
// spin holds all three constant, so she is tumbling in three dimensions and
// foreshortening as she comes edge-on.
//
// Modelled as a flat plate that spins in the screen plane while tilting away
// from face-on. A plate tilted by `tilt` about an axis lying in the screen
// plane compresses along the perpendicular to that axis by cos(tilt), so the
// projected area is cos(tilt) and a tilt of 0.85 rad gives the measured swing.
//
// The tilt axis has to turn as well. Pinned to one direction she reads as
// squashed rather than tumbling, which is exactly how the first attempt at
// this looked.
constexpr qreal kSpinRate = 1.46;  // rad/s, in-plane
constexpr qreal kTiltMax = 0.85;   // rad away from face-on
constexpr qreal kTiltRate = 2.60;  // rad/s, how fast the tilt breathes
constexpr qreal kAxisRate = 0.90;  // rad/s, how fast the tilt direction turns

// Base half-extents. The reference keeps a near-constant height/width ratio of
// about 1.55 whatever direction she looks in.
constexpr qreal kEyeWidth = 0.170;
constexpr qreal kEyeHeight = 0.263;

// Measured over the 34 blinks in the reference: a full cycle runs 243 ms,
// spent 121 ms closing, ~50 ms fully shut, then 84 ms opening. The lids close
// more slowly than they open, which is the opposite of the obvious guess, and
// they shut completely rather than leaving a slit.
// The cycle is a touch longer than the 243 ms measured, because that figure
// is taken between the 90%-open crossings rather than end to end.
constexpr qreal kBlinkSeconds = 0.29;
constexpr qreal kBlinkClose = 0.47; // fraction of the cycle spent closing
constexpr qreal kBlinkHold = 0.75;  // ... and fully shut, up to this point

// Long enough that a whole bag of antics plays out before she naps. At 75 s
// she could drift off with a behaviour still undealt, which is half of why
// the orbit rings went unseen.
constexpr qreal kSleepAfter = 110.0;
// She starts to flag well before she actually goes under.
constexpr qreal kDrowsyAfter = 55.0;

qreal easeInOut(qreal t) {
  t = qBound(0.0, t, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

} // namespace

Mascot::Mascot(QObject *parent)
    : QObject(parent), m_random(QRandomGenerator::securelySeeded()) {
  m_yaw = m_yawTarget = kRestYaw;
  m_pitch = m_pitchTarget = kRestPitch;
  placeEyes();
  buildTransform();
  scheduleBlink();
}

qreal Mascot::random(qreal lo, qreal hi) {
  return lo + m_random.generateDouble() * (hi - lo);
}

void Mascot::settle(qreal &value, qreal target, qreal dt, qreal rate) const {
  // Frame-rate independent exponential approach. `rate` is roughly "how many
  // e-folds per second", so higher is snappier.
  if (m_reduced)
    rate *= 2.4;
  const qreal k = 1.0 - std::exp(-rate * dt);
  value += (target - value) * k;
}

// Carry each eye around the sphere and project it back to the screen.
//
// Yaw is applied before pitch on purpose: in that order the separation axis
// itself tilts once the gaze is both sideways and up or down, which is the
// asymmetry the reference shows (the pair measures -0.02 tilt looking up and
// +0.13 level). Pitch-then-yaw would keep the pair perfectly level.
void Mascot::placeEyes() {
  const qreal previousLeftLid = m_left.lid;
  const qreal previousRightLid = m_right.lid;
  const qreal cy = std::cos(m_yaw), sy = std::sin(m_yaw);
  const qreal cp = std::cos(m_pitch), sp = std::sin(m_pitch);

  const auto carry = [&](qreal side) {
    // Start on the front of the sphere, offset sideways by the half-gap.
    const qreal a = side * kEyeHalfGap;
    qreal x = std::sin(a), y = 0.0, z = std::cos(a);

    // Yaw about the vertical axis, then pitch about the horizontal one.
    qreal nx = x * cy + z * sy;
    qreal nz = -x * sy + z * cy;
    x = nx;
    z = nz;
    qreal ny = y * cp + z * sp;
    nz = -y * sp + z * cp;
    y = ny;
    z = nz;

    Eye eye;
    eye.position = QPointF(x * kEyeSphere, y * kEyeSphere);

    // Foreshortening is uniform, not a true tangent-plane projection. The
    // reference holds the eye's height/width ratio at about 1.5 whichever way
    // she looks (1.54 near centre, 1.48 far to the side), so both axes shrink
    // together as an eye nears the limb. Softening z by this much reproduces
    // the measured 0.324 -> 0.272 narrowing across the sweep.
    const qreal facing = 0.55 + 0.45 * std::max(0.0, z);
    eye.scale = QPointF(facing, facing);

    // Lean the slit along the sphere's local "up". The tangent frame at a
    // point e on the unit sphere has up = e x normalise(worldUp x e), whose
    // screen projection is (-xy/L, L) with L = hypot(x, z).
    const qreal L = std::hypot(x, z);
    if (L > 1e-4)
      eye.angle = std::atan2(-x * y / L, L) * kSlitRotation;

    return eye;
  };

  m_left = carry(-1.0);
  m_right = carry(1.0);

  // Carry the lids over: placeEyes() rebuilds the eyes from scratch each
  // frame, so reapply how far each one is shut.
  m_left.lid = previousLeftLid;
  m_right.lid = previousRightLid;

  // Damp the tilt towards the pair's own level without disturbing where the
  // pair as a whole is looking.
  const qreal level = (m_left.position.y() + m_right.position.y()) * 0.5;
  const auto damp = [&](Eye &eye) {
    eye.position.setY(level +
                      (eye.position.y() - level) * kTiltDamping);
  };
  damp(m_left);
  damp(m_right);
}

// Project her onto the screen: tumble in 3D, then the in-plane lean, then
// invert so the shader can map a screen point back to a point on her surface.
void Mascot::buildTransform() {
  // Foreshortening: compress along the perpendicular to the tilt axis by the
  // cosine of the tilt. This is what an orthographic projection does to a
  // tilted flat plate.
  const qreal dx = -std::sin(m_tiltAxis), dy = std::cos(m_tiltAxis);
  const qreal shrink = 1.0 - std::cos(m_tilt);
  const qreal f00 = 1.0 - shrink * dx * dx, f01 = -shrink * dx * dy;
  const qreal f10 = -shrink * dx * dy, f11 = 1.0 - shrink * dy * dy;

  // Then the in-plane spin, with her settling lean riding along on it.
  const qreal angle = m_spin + m_roll;
  const qreal c = std::cos(angle), s = std::sin(angle);
  const qreal r00 = c * f00 - s * f10, r01 = c * f01 - s * f11;
  const qreal r10 = s * f00 + c * f10, r11 = s * f01 + c * f11;

  qreal det = r00 * r11 - r01 * r10;
  m_projectedArea = std::abs(det);
  if (std::abs(det) < 0.12)
    det = det < 0 ? -0.12 : 0.12;

  m_transform = QVector4D(float(r11 / det), float(-r01 / det),
                          float(-r10 / det), float(r00 / det));
}

void Mascot::setMood(Mood mood) {
  if (m_mood == mood)
    return;
  m_mood = mood;
  emit moodChanged();
}

void Mascot::morphTo(int form, qreal seconds) {
  if (m_reduced)
    seconds = std::min(seconds, 0.12);

  if (m_formMix < 1.0) {
    // A morph is already in flight. Let it land rather than snapping, and
    // remember where we actually wanted to go.
    if (form != m_formB) {
      m_queuedForm = form;
      m_queuedSeconds = seconds;
    }
    return;
  }
  if (form == m_formB) {
    m_queuedForm = -1;
    return;
  }

  m_formA = m_formB;
  m_formB = form;
  m_formMix = 0.0;
  m_morphRate = 1.0 / std::max(seconds, 0.01);
  m_queuedForm = -1;
}

void Mascot::changeForm(int form, qreal seconds) { morphTo(form, seconds); }

void Mascot::snapForm(int form) {
  m_queuedForm = -1;
  m_formA = m_formB = form;
  m_formMix = 1.0;
  m_morphRate = 0.0;
}

void Mascot::advanceMorph(qreal dt) {
  if (m_formMix < 1.0) {
    m_formMix = std::min(1.0, m_formMix + m_morphRate * dt);
    if (m_formMix >= 1.0) {
      m_formA = m_formB;
      if (m_queuedForm >= 0) {
        const int form = m_queuedForm;
        const qreal seconds = m_queuedSeconds;
        m_queuedForm = -1;
        morphTo(form, seconds);
      }
    }
  }

  // The "..." run is a parameter of the dots form rather than a separate
  // shape, so the collapse reads as one continuous motion.
  //
  // These follow the morph exactly rather than settling on their own clock. An
  // independent spring lets them race ahead of the silhouette, which leaves
  // the blended shape reaching well past the dots it is supposed to be made
  // of -- and that showed up as the body greying out on the way in.
  // Strictly linear in the morph. Easing these ahead of the blend puts the
  // side dots inside a body that has not shrunk yet, and they grey it.
  const auto dotsness = [](int form) { return form == Dots ? 1.0 : 0.0; };
  const qreal amount = dotsness(m_formA) * (1.0 - m_formMix) +
                       dotsness(m_formB) * m_formMix;
  m_dotsSpread = m_dotsSpreadTarget = amount;
  m_dotsShrink = m_dotsShrinkTarget = amount;
  // Roughly one pulse per dot per second, as in the reference.
  if (m_dotsSpread > 0.01)
    m_dotsPhase += dt * 4.4;
}

// The reference blinks every 2.85 s on average, but as little as 0.6 s and as
// much as 6.4 s apart.
void Mascot::scheduleBlink() { m_nextBlink = random(0.7, 5.2); }

// How far shut the lid is at this point in a blink: 0 open, 1 shut.
qreal Mascot::lidFor(qreal phase) const {
  qreal closed;
  if (phase < kBlinkClose)
    closed = phase / kBlinkClose;
  else if (phase < kBlinkHold)
    closed = 1.0;
  else
    closed = 1.0 - (phase - kBlinkHold) / (1.0 - kBlinkHold);
  return easeInOut(closed);
}

// One eye squeezed shut and held, the way the reference holds it for a good
// two or three seconds before blinking out of it.
void Mascot::wink() {
  wake();
  if (m_mood != Resting || m_winkHold >= 0.0)
    return;
  m_winkHold = random(1.8, 3.2);
}

void Mascot::advanceBlink(qreal dt) {
  if (m_mood == Asleep)
    return;

  if (m_blinkPhase >= 0.0) {
    m_blinkPhase += dt / kBlinkSeconds;
    if (m_blinkPhase >= 1.0) {
      m_blinkPhase = -1.0;
      scheduleBlink();
    }
  } else {
    m_nextBlink -= dt;
    if (m_nextBlink <= 0.0)
      m_blinkPhase = 0.0;
  }
}

// Deal the next antic from a shuffled bag. A bag guarantees each behaviour
// appears once per cycle; independent random draws do not, which is how the
// orbit rings managed to go unseen for an entire session.
int Mascot::drawAntic() {
  if (m_anticBag.isEmpty()) {
    for (int i = 0; i < AnticCount; ++i)
      m_anticBag.append(i);
    // Fisher-Yates, and never open a bag with whatever closed the last one.
    for (int i = m_anticBag.size() - 1; i > 0; --i)
      std::swap(m_anticBag[i], m_anticBag[int(random(0.0, i + 1.0))]);
  }
  return m_anticBag.takeLast();
}

void Mascot::performAntic(int antic) {
  switch (antic) {
  case BecomeEgg:
    morphTo(Egg, 0.26);
    m_hold = 1.4;
    break;
  case BecomeHex:
    morphTo(Hex, 0.26);
    m_hold = 1.4;
    break;
  case Ponder:
    think(3.2);
    break;
  case Winking:
    wink();
    break;
  case ComeApart:
    scatter();
    break;
  default:
    break;
  }
}

void Mascot::advanceIdle(qreal dt) {
  if (!m_idleAntics || m_mood != Resting)
    return;

  // Amusing herself is not being interacted with: everything below runs
  // through the same entry points as a real interaction, so preserve the idle
  // clock across them or she would never settle down to sleep.
  const qreal idleBefore = m_idle;
  const auto keepIdleClock = qScopeGuard([this, idleBefore] {
    m_idle = idleBefore;
  });

  // Small movements, often. These are what stop her looking switched off
  // between the larger flourishes, and they are quiet enough to keep under
  // reduced motion.
  m_glance -= dt;
  if (m_glance <= 0.0) {
    m_glance = random(3.5, 7.0) * (1.0 + 1.2 * m_drowsy);
    // Never look away from a cursor that is still moving: she follows it while
    // you are using the mouse and amuses herself once you stop, rather than
    // the two fighting over where she is looking.
    if (m_sinceLook > 2.0) {
      m_yawTarget = random(-kMaxYaw * 0.75, kMaxYaw * 0.85);
      m_pitchTarget = random(-kMaxPitch * 0.8, kMaxPitch * 0.55);
      m_lookingAtCursor = false;
    } else if (!m_reduced) {
      m_squashXTarget = 1.07;
      m_squashYTarget = 0.93;
      m_hold = 0.22;
    }
  }

  // And the flourishes worth watching, paced so a whole bag runs through well
  // inside the time she stays awake.
  m_antic -= dt;
  if (m_antic > 0.0)
    return;
  // Flourishes thin out as she flags.
  m_antic = random(6.0, 11.0) * (1.0 + 1.6 * m_drowsy);

  int antic = drawAntic();
  if (m_reduced) {
    // Reduced motion reduces rather than eliminates. A wink is two small white
    // shapes moving; a tumble or a scatter is not.
    for (int tries = 0; tries < AnticCount && antic != Winking; ++tries)
      antic = drawAntic();
    if (antic != Winking)
      return;
  }
  performAntic(antic);
}

QVariantList Mascot::droplets() const {
  QVariantList list;
  list.reserve(m_droplets.size());
  for (const Droplet &drop : m_droplets) {
    // Fade over the back half of a droplet's life, as the reference's do.
    const qreal t = drop.span > 0.0 ? drop.life / drop.span : 0.0;
    list.append(QVariantMap{{"x", drop.position.x()},
                            {"y", drop.position.y()},
                            {"radius", drop.radius},
                            {"opacity", qBound(0.0, t * 1.8, 1.0)}});
  }
  return list;
}

// She comes apart: the body collapses to a speck and throws off a handful of
// droplets, which drift outward, slow down and fade.
void Mascot::scatter() {
  wake();
  setMood(Happy);
  m_droplets.clear();

  const int count = 5 + int(random(0.0, 2.0));
  for (int i = 0; i < count; ++i) {
    Droplet drop;
    const qreal angle = random(0.0, 2.0 * M_PI);
    const qreal speed = random(0.9, 2.3);
    drop.position = QPointF(std::cos(angle) * 0.12, std::sin(angle) * 0.12);
    drop.velocity = QPointF(std::cos(angle) * speed, std::sin(angle) * speed);
    // One or two proper blobs among several specks, as in the reference.
    drop.radius = i < 2 ? random(0.035, 0.06) : random(0.008, 0.025);
    drop.span = drop.life = random(0.5, 1.1);
    m_droplets.append(drop);
  }

  morphTo(Tiny, 0.22);
  m_scale = 1.0;
  m_scaleVelocity = 0.0;
  m_scaleTarget = 1.0;
  m_hold = 1.5;
}

// She tucks herself into a speck and streaks off. The backend moves the
// window; all this does is make her look like something in flight.
void Mascot::beginDash(qreal angle, qreal speed) {
  wake();
  setMood(Dashing);
  m_hold = 0.0;
  m_dashAngle = angle;
  m_dashSpeed = qBound(0.0, speed, 1.0);
  morphTo(Tiny, 0.16);
  m_squashXTarget = m_squashYTarget = 1.0;
  m_scaleTarget = 1.0;
}

void Mascot::updateDash(qreal angle, qreal speed) {
  if (m_mood != Dashing)
    return;
  m_dashAngle = angle;
  m_dashSpeed = qBound(0.0, speed, 1.0);
}

void Mascot::endDash() {
  if (m_mood != Dashing)
    return;
  setMood(Resting);
  m_dashSpeed = 0.0;
  m_idle = 0.0;
  morphTo(Circle, kSettleBack);
  // Arrive with a bounce, the way she does at the end of the reference's dash.
  m_scale = 0.72;
  m_scaleVelocity = 0.0;
  m_scaleTarget = 1.0;
  m_yawTarget = kRestYaw;
  m_pitchTarget = kRestPitch;
}

void Mascot::tick(qreal dt) {
  dt = qBound(0.0, dt, 0.1); // survive a stalled frame without a lurch
  m_time += dt;
  m_idle += dt;
  m_sinceLook += dt;
  if (m_cooldown > 0.0)
    m_cooldown = std::max(0.0, m_cooldown - dt);

  advanceMorph(dt);
  advanceBlink(dt);
  advanceIdle(dt);

  // Droplets: thrown clear, slowed by drag, gone when their life runs out.
  if (!m_droplets.isEmpty()) {
    for (Droplet &drop : m_droplets) {
      drop.life -= dt;
      drop.position += drop.velocity * dt;
      drop.velocity *= std::max(0.0, 1.0 - 2.6 * dt);
    }
    m_droplets.erase(std::remove_if(m_droplets.begin(), m_droplets.end(),
                                    [](const Droplet &d) {
                                      return d.life <= 0.0;
                                    }),
                     m_droplets.end());
  }

  // She comes back to a circle while the rings are still going, rather than
  // at the same moment they start to fade. The reference does the two in
  // sequence, and doing them together reads as one abrupt change.
  if (m_mood == Thinking && !m_thinkUnfolded && m_hold > 0.0 &&
      m_hold < kSettleBack + 0.2) {
    m_thinkUnfolded = true;
    morphTo(Circle, kSettleBack);
  }

  // Mood timers.
  if (m_hold > 0.0) {
    m_hold -= dt;
    if (m_hold <= 0.0) {
      m_hold = 0.0;
      if (m_mood != Held && m_mood != Asleep) {
        setMood(Resting);
        morphTo(Circle, kSettleBack);
        m_ringsTarget = 0.0;
        m_badgeTarget = 0.0;
        m_squashXTarget = m_squashYTarget = 1.0;
        m_eyeWidthTarget = kEyeWidth;
        m_eyeHeightTarget = kEyeHeight;
        m_eyeRoundTarget = 1.0;
        m_rollTarget = 0.0;
        m_scaleTarget = 1.0;
      }
    }
  }

  // How far gone she is, which lowers her lids and thins out her flourishes
  // rather than letting sleep arrive out of nowhere.
  m_drowsy = (m_mood == Resting && m_sleepWhenIdle)
                 ? qBound(0.0, (m_idle - kDrowsyAfter) /
                                   (kSleepAfter - kDrowsyAfter), 1.0)
                 : 0.0;

  // Drift off after a long stretch of being left alone.
  if (m_sleepWhenIdle && m_mood == Resting && m_idle > kSleepAfter) {
    setMood(Asleep);
    morphTo(Tiny, 0.30);
    m_scaleTarget = 1.0;
    m_eyeHeightTarget = 0.0;
  }

  // Springs and easings.
  settle(m_squashX, m_squashXTarget, dt, 11.0);
  settle(m_squashY, m_squashYTarget, dt, 11.0);
  settle(m_roll, m_rollTarget, dt, 8.0);
  // The rings come up in 117 ms and take 683 ms to fade: sharply asymmetric,
  // so one rate cannot serve for both.
  settle(m_rings, m_ringsTarget, dt,
         m_ringsTarget > m_rings ? kRingsRise : kRingsFall);

  // The badge pops. It overshoots its settled size by 19.5% about a third of
  // a second in, so it needs a spring rather than an approach.
  {
    m_badgeVelocity += ((m_badgeTarget - m_badge) * 115.0 -
                        m_badgeVelocity * 9.9) * dt;
    m_badge = std::max(0.0, m_badge + m_badgeVelocity * dt);
    if (m_badge <= 0.0 && m_badgeTarget <= 0.0)
      m_badgeVelocity = 0.0;
  }

  // Body scale eases rather than bounces. Both of the reference's
  // re-inflations rise to full over ~600 ms with exactly 0.00% overshoot, so
  // the spring is damped past critical (2*sqrt(52) = 14.4).
  {
    const qreal stiffness = m_reduced ? 220.0 : 52.0;
    const qreal damping = m_reduced ? 32.0 : 15.5;
    m_scaleVelocity += ((m_scaleTarget - m_scale) * stiffness -
                        m_scaleVelocity * damping) *
                       dt;
    m_scale += m_scaleVelocity * dt;
  }

  // Gaze. Saccades are quick, so this settles faster than the body does.
  qreal yaw = m_yawTarget, pitch = m_pitchTarget;
  if (!m_lookingAtCursor && m_mood == Resting && m_idle < kSleepAfter &&
      !m_reduced) {
    // A little drift so a held gaze never looks painted on.
    yaw += std::sin(m_time * 0.37) * 0.035;
    pitch += std::sin(m_time * 0.51 + 2.1) * 0.025;
  }
  settle(m_yaw, yaw, dt, 9.0);
  settle(m_pitch, pitch, dt, 9.0);
  placeEyes();

  settle(m_eyeWidth, m_eyeWidthTarget, dt, 13.0);
  settle(m_eyeRound, m_eyeRoundTarget, dt, 10.0);

  // Her lids lower as she flags, so sleep does not arrive out of nowhere.
  settle(m_eyeHeight, m_eyeHeightTarget * (1.0 - 0.5 * m_drowsy), dt, 13.0);

  // Lids. A blink shuts both; a wink shuts one and holds it. They are applied
  // per eye rather than to the shared height so the two can differ.
  const qreal blinkLid = m_blinkPhase >= 0.0 ? lidFor(m_blinkPhase) : 0.0;
  if (m_winkHold >= 0.0) {
    m_winkHold -= dt;
    // Squeezed, not shut: the reference leaves a clear horizontal dash rather
    // than closing the eye away altogether, which is what separates a wink
    // from a blink.
    settle(m_winkLid, 0.86, dt, 18.0);
    if (m_winkHold <= 0.0) {
      m_winkHold = -1.0;
      m_blinkPhase = 0.0; // she blinks as she comes out of it, as in the video
    }
  } else {
    settle(m_winkLid, 0.0, dt, 14.0);
  }
  m_left.lid = std::max(blinkLid, 0.0);
  m_right.lid = std::max(blinkLid, m_winkLid);
  // A squeezed eye spreads sideways as it flattens.
  m_right.scale.setX(m_right.scale.x() * (1.0 + 0.3 * m_winkLid));

  // The idle swell, and the drift it is coupled to: she sinks as she widens.
  if (m_reduced || m_mood == Asleep) {
    m_breathe = 0.0;
    m_bobX = m_bobY = 0.0;
  } else {
    m_breathe = std::sin(m_time * kBreatheRate);
    m_bobY = m_breathe * kBreatheRise;
    m_bobX = std::sin(m_time * 0.61 + 1.1) * 0.006;
  }

  // The trail lags the speed a little, so it streams out as she gets going
  // and lingers for a moment when she stops.
  settle(m_dashLength, m_mood == Dashing ? 0.35 + 0.65 * m_dashSpeed : 0.0, dt,
         m_mood == Dashing ? 11.0 : 6.0);
  settle(m_dashIntensity, m_mood == Dashing ? 1.0 : 0.0, dt, 9.0);

  // Tumble while she is thinking. Measured over the reference's thinking
  // stretch: +122 degrees in 2.10 s. It accumulates in its own term, then
  // unwinds once she stops -- by which point she is a circle again anyway.
  if (m_rings > 0.02 && !m_reduced) {
    m_spin += dt * kSpinRate * m_rings;
    m_tiltPhase += dt * kTiltRate;
    m_tiltAxis += dt * kAxisRate;
    // Scaled by the rings so the tumble arrives and leaves with them.
    m_tilt = kTiltMax * 0.5 * (1.0 - std::cos(m_tiltPhase)) * m_rings;
  } else {
    settle(m_spin, 0.0, dt, 3.0);
    settle(m_tilt, 0.0, dt, 4.0);
  }
  buildTransform();

  emit frame();
}

void Mascot::wake() {
  m_idle = 0.0;
  if (m_mood == Asleep) {
    setMood(Resting);
    morphTo(Circle, kSettleBack);
    m_scale = 0.75;
    m_scaleVelocity = 0.0;
    m_scaleTarget = 1.0;
    m_eyeHeightTarget = kEyeHeight;
    m_hold = 0.0;
  }
}

void Mascot::poke() {
  wake();
  if (m_cooldown > 0.0)
    return;
  m_cooldown = 0.28;

  setMood(Happy);
  // Squash on impact, then bounce past the rest size on the way back.
  m_squashXTarget = 1.16;
  m_squashYTarget = 0.86;
  m_scale = 0.9;
  m_scaleVelocity = 0.0;
  m_scaleTarget = 1.0;
  // Eyes go round and wide -- the "delighted" pose from the reference.
  m_eyeWidthTarget = 0.20;
  m_eyeHeightTarget = 0.225;
  m_eyeRoundTarget = 1.0;
  m_hold = 0.85;

  QMetaObject::invokeMethod(
      this,
      [this] {
        m_squashXTarget = 1.0;
        m_squashYTarget = 1.0;
      },
      Qt::QueuedConnection);
}

void Mascot::think(qreal seconds) {
  wake();
  setMood(Thinking);
  m_ringsTarget = 1.0;
  m_thinkUnfolded = false;
  morphTo(Triangle, 0.26);
  m_eyeWidthTarget = kEyeWidth;
  m_eyeHeightTarget = kEyeHeight;
  m_hold = std::max(seconds, 0.6);
}

void Mascot::alert() {
  wake();
  setMood(Alert);
  morphTo(Exclaim, 0.22);
  // She arrives under-leaned and swings into it over about a third of a
  // second. The reference does not oscillate at all -- measured frame by
  // frame it runs -6.6deg -> -11.6 -> -15.9 -> -17.4 and stops -- so this is
  // a settle, not a shake.
  m_roll = kAlertOverlean;
  m_rollTarget = 0.0;
  m_scale = 1.08;
  m_scaleVelocity = 0.0;
  m_scaleTarget = 1.0;
  m_hold = 2.1;
}

void Mascot::notify() {
  wake();
  setMood(Notifying);
  morphTo(Circle, 0.20);
  m_badgeTarget = 1.0;
  // Wide, round, surprised eyes: measured 0.442 x 0.495 R on the reference.
  m_eyeWidthTarget = 0.228;
  m_eyeHeightTarget = 0.257;
  m_eyeRoundTarget = 1.0;
  m_yawTarget = 0.0;
  m_pitchTarget = 0.12; // looking straight out, a touch downward
  m_scale = 0.94;
  m_scaleVelocity = 0.0;
  m_scaleTarget = 1.0;
  m_hold = 3.4;
}

void Mascot::beginDrag() {
  wake();
  setMood(Held);
  m_hold = 0.0;
  m_squashXTarget = 0.92;
  m_squashYTarget = 1.10; // stretched, as if hanging from the cursor
  m_eyeWidthTarget = 0.155;
  m_eyeHeightTarget = 0.275;
}

void Mascot::endDrag(qreal throwSpeed) {
  setMood(Resting);
  m_idle = 0.0;
  m_squashXTarget = 1.0;
  m_squashYTarget = 1.0;
  m_eyeWidthTarget = kEyeWidth;
  m_eyeHeightTarget = kEyeHeight;
  m_yawTarget = kRestYaw;
  m_pitchTarget = kRestPitch;
  m_lookingAtCursor = false;

  // Landing squash, scaled by how hard she was thrown.
  const qreal impact = qBound(0.0, throwSpeed, 1.0);
  m_squashX = 1.0 + 0.18 * impact;
  m_squashY = 1.0 - 0.16 * impact;
  m_scaleVelocity = 0.0;
  m_scale = 1.0 - 0.12 * impact;
  m_scaleTarget = 1.0;
}

void Mascot::setHovered(bool hovered) {
  if (m_hovered == hovered)
    return;
  m_hovered = hovered;
  if (hovered) {
    wake();
    if (m_mood == Resting) {
      m_eyeWidthTarget = 0.165;
      m_eyeHeightTarget = 0.27;
    }
  } else if (m_mood == Resting) {
    m_eyeWidthTarget = kEyeWidth;
    m_eyeHeightTarget = kEyeHeight;
  }
}

void Mascot::lookAt(qreal x, qreal y) {
  m_lookingAtCursor = true;
  m_sinceLook = 0.0;
  // Turn the cursor's offset into an orientation. tanh keeps a cursor far off
  // to one side from pinning the gaze at the very limit of its travel.
  m_yawTarget = std::tanh(x * 0.45) * kMaxYaw;
  m_pitchTarget = std::tanh(y * 0.45) * kMaxPitch;
}

void Mascot::glanceAbout() {
  if (m_mood != Resting)
    return;
  m_yawTarget = random(-kMaxYaw * 0.7, kMaxYaw * 0.8);
  m_pitchTarget = random(-kMaxPitch * 0.7, kMaxPitch * 0.4);
  m_lookingAtCursor = false;
  m_glance = random(3.5, 6.0); // do not immediately look somewhere else
}

void Mascot::lookIdle() {
  m_lookingAtCursor = false;
  m_yawTarget = kRestYaw;
  m_pitchTarget = kRestPitch;
}

void Mascot::setReducedMotion(bool reduced) {
  if (m_reduced == reduced)
    return;
  m_reduced = reduced;
  if (reduced) {
    m_bobX = m_bobY = 0.0;
  }
}

void Mascot::setSleepWhenIdle(bool enabled) {
  m_sleepWhenIdle = enabled;
  if (!enabled && m_mood == Asleep)
    wake();
}

void Mascot::setIdleAntics(bool enabled) { m_idleAntics = enabled; }

void Mascot::rest() {
  setMood(Resting);
  snapForm(Circle);
  m_hold = 0.0;
  m_idle = 0.0;
  m_rings = m_ringsTarget = 0.0;
  m_dashSpeed = m_dashLength = m_dashIntensity = 0.0;
  m_droplets.clear();
  m_badge = m_badgeTarget = 0.0;
  m_badgeVelocity = 0.0;
  m_dotsSpread = m_dotsSpreadTarget = 0.0;
  m_dotsShrink = m_dotsShrinkTarget = 0.0;
  m_dotsPhase = 0.0;
  m_squashX = m_squashY = m_squashXTarget = m_squashYTarget = 1.0;
  m_scale = m_scaleTarget = 1.0;
  m_scaleVelocity = 0.0;
  m_roll = m_rollTarget = 0.0;
  m_spin = m_tilt = m_tiltPhase = m_tiltAxis = 0.0;
  buildTransform();
  m_bobX = m_bobY = 0.0;
  m_breathe = 0.0;
  m_drowsy = 0.0;
  // Lids too: without this a rest() taken mid-blink leaves an eye half shut,
  // and anything that measures from here starts off a wrong baseline.
  m_blinkPhase = -1.0;
  m_winkHold = -1.0;
  m_winkLid = 0.0;
  m_left.lid = m_right.lid = 0.0;
  scheduleBlink();
  m_eyeWidth = m_eyeWidthTarget = kEyeWidth;
  m_eyeHeight = m_eyeHeightTarget = kEyeHeight;
  m_eyeRound = m_eyeRoundTarget = 1.0;
  m_yaw = m_yawTarget = kRestYaw;
  m_pitch = m_pitchTarget = kRestPitch;
  placeEyes();
  m_lookingAtCursor = false;
  emit frame();
}
