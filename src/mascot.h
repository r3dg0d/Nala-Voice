#pragma once
#include <QObject>
#include <QVariantList>
#include <QVector4D>
#include <QVector>
#include <QPointF>
#include <QRandomGenerator>

// Nala's behaviour. This class owns no rendering: it advances a small pile of
// animation state on `tick()` and publishes it as properties, which keeps the
// whole personality testable without a compositor or a GPU.
//
// Timings and proportions here were measured frame-by-frame from the reference
// animation (60 fps) rather than guessed -- see docs/animation.md.
class Mascot : public QObject {
  Q_OBJECT

  Q_PROPERTY(int formA READ formA NOTIFY frame)
  Q_PROPERTY(int formB READ formB NOTIFY frame)
  Q_PROPERTY(qreal formMix READ formMix NOTIFY frame)
  Q_PROPERTY(qreal dotsSpread READ dotsSpread NOTIFY frame)
  Q_PROPERTY(qreal dotsShrink READ dotsShrink NOTIFY frame)
  Q_PROPERTY(qreal dotsPhase READ dotsPhase NOTIFY frame)

  Q_PROPERTY(qreal squashX READ squashX NOTIFY frame)
  Q_PROPERTY(qreal squashY READ squashY NOTIFY frame)
  Q_PROPERTY(qreal bodyScale READ bodyScale NOTIFY frame)
  Q_PROPERTY(qreal roll READ roll NOTIFY frame)
  Q_PROPERTY(QVector4D bodyTransform READ bodyTransform NOTIFY frame)
  Q_PROPERTY(qreal bobX READ bobX NOTIFY frame)
  Q_PROPERTY(qreal bobY READ bobY NOTIFY frame)

  Q_PROPERTY(qreal eyeLeftX READ eyeLeftX NOTIFY frame)
  Q_PROPERTY(qreal eyeLeftY READ eyeLeftY NOTIFY frame)
  Q_PROPERTY(qreal eyeRightX READ eyeRightX NOTIFY frame)
  Q_PROPERTY(qreal eyeRightY READ eyeRightY NOTIFY frame)
  Q_PROPERTY(qreal eyeLeftScaleX READ eyeLeftScaleX NOTIFY frame)
  Q_PROPERTY(qreal eyeLeftScaleY READ eyeLeftScaleY NOTIFY frame)
  Q_PROPERTY(qreal eyeRightScaleX READ eyeRightScaleX NOTIFY frame)
  Q_PROPERTY(qreal eyeRightScaleY READ eyeRightScaleY NOTIFY frame)
  Q_PROPERTY(qreal eyeLeftAngle READ eyeLeftAngle NOTIFY frame)
  Q_PROPERTY(qreal eyeRightAngle READ eyeRightAngle NOTIFY frame)
  Q_PROPERTY(qreal eyeWidth READ eyeWidth NOTIFY frame)
  Q_PROPERTY(qreal eyeHeight READ eyeHeight NOTIFY frame)
  Q_PROPERTY(qreal eyeRound READ eyeRound NOTIFY frame)
  Q_PROPERTY(qreal gazeYaw READ gazeYaw NOTIFY frame)
  Q_PROPERTY(qreal gazePitch READ gazePitch NOTIFY frame)

  Q_PROPERTY(qreal badge READ badge NOTIFY frame)
  Q_PROPERTY(qreal rings READ rings NOTIFY frame)
  Q_PROPERTY(QVariantList droplets READ droplets NOTIFY frame)
  Q_PROPERTY(qreal dashAngle READ dashAngle NOTIFY frame)
  Q_PROPERTY(qreal dashLength READ dashLength NOTIFY frame)
  Q_PROPERTY(qreal dashIntensity READ dashIntensity NOTIFY frame)
  Q_PROPERTY(qreal time READ time NOTIFY frame)
  Q_PROPERTY(int mood READ mood NOTIFY moodChanged)
  Q_PROPERTY(qreal drowsiness READ drowsiness NOTIFY frame)
  Q_PROPERTY(bool sleeping READ sleeping NOTIFY moodChanged)
  Q_PROPERTY(bool dragging READ dragging NOTIFY moodChanged)

public:
  // Indices must match `form()` in shaders/mascot.frag.
  enum Form {
    Circle = 0,
    Egg = 1,
    Hex = 2,
    Triangle = 3,
    Exclaim = 4,
    Teardrop = 5,
    Dots = 6,
    Tiny = 7
  };
  Q_ENUM(Form)

  enum Mood { Resting, Happy, Thinking, Alert, Notifying, Asleep, Held,
              Dashing };
  Q_ENUM(Mood)

  // The things worth watching. Dealt from a shuffled bag rather than picked
  // independently, so every one turns up once per cycle instead of depending
  // on luck. Small filler movements are not in here: they run on their own,
  // faster cadence, which keeps the bag short enough to get all the way
  // through before she drifts off.
  enum Antic { BecomeEgg, BecomeHex, Ponder, Winking, ComeApart, AnticCount };
  Q_ENUM(Antic)

  // She never holds perfectly still. Measured from the reference: the body's
  // height/width ratio swells by about 1.2% at 0.31 Hz, and the whole shape
  // drifts vertically at that same frequency -- the two are coupled, which is
  // what makes it read as breathing rather than as jitter.
  // Settling back to rest is not the mirror of leaving it. Frame by frame the
  // reference collapses into the "..." run in 67 ms but takes 233 ms to come
  // back out of it -- reacting is snappy, relaxing is gentle.
  static constexpr qreal kSettleBack = 0.53;

  static constexpr qreal kBreatheRate = 1.95;  // rad/s == 0.31 Hz
  static constexpr qreal kBreatheDepth = 0.007;
  static constexpr qreal kBreatheRise = 0.027; // vertical, in half-item units

  explicit Mascot(QObject *parent = nullptr);

  int formA() const { return m_formA; }
  int formB() const { return m_formB; }
  qreal formMix() const { return m_formMix; }
  qreal dotsSpread() const { return m_dotsSpread; }
  qreal dotsShrink() const { return m_dotsShrink; }
  qreal dotsPhase() const { return m_dotsPhase; }
  // Reaction squash, with the idle swell riding on top of it.
  qreal squashX() const { return m_squashX * (1.0 + kBreatheDepth * m_breathe); }
  qreal squashY() const { return m_squashY * (1.0 - kBreatheDepth * m_breathe); }
  qreal bodyScale() const { return m_scale; }
  // The settling lean. The thinking tumble is not part of this: it is a
  // rotation in three dimensions and lives in bodyTransform().
  qreal roll() const { return m_roll; }
  qreal lean() const { return m_roll; }
  qreal tumble() const { return m_spin; }

  // Inverse of the 2x2 that projects her onto the screen, as
  // (m00, m01, m10, m11). The shader maps a screen point back through this to
  // find the point on her own surface, which is what lets a flat shape
  // foreshorten as it turns edge-on.
  QVector4D bodyTransform() const { return m_transform; }

  // Area the projection covers, 1.0 face-on. Used by the tests.
  qreal projectedArea() const { return m_projectedArea; }
  qreal bobX() const { return m_bobX; }
  qreal bobY() const { return m_bobY; }
  qreal eyeLeftX() const { return m_left.position.x(); }
  qreal eyeLeftY() const { return m_left.position.y(); }
  qreal eyeRightX() const { return m_right.position.x(); }
  qreal eyeRightY() const { return m_right.position.y(); }
  qreal eyeLeftScaleX() const { return m_left.scale.x(); }
  // The lid rides in the vertical scale, so a blink or a wink needs no extra
  // uniform: an eye that is shut simply has no height.
  qreal eyeLeftScaleY() const { return m_left.scale.y() * (1.0 - m_left.lid); }
  qreal eyeRightScaleX() const { return m_right.scale.x(); }
  qreal eyeRightScaleY() const {
    return m_right.scale.y() * (1.0 - m_right.lid);
  }
  qreal eyeLeftAngle() const { return m_left.angle; }
  qreal eyeRightAngle() const { return m_right.angle; }
  qreal gazeYaw() const { return m_yaw; }
  qreal gazePitch() const { return m_pitch; }
  qreal eyeWidth() const { return m_eyeWidth; }
  qreal eyeHeight() const { return m_eyeHeight; }
  qreal eyeRound() const { return m_eyeRound; }

  // Height of each eye as actually drawn, lid included.
  qreal eyeLeftHeight() const { return m_eyeHeight * eyeLeftScaleY(); }
  qreal eyeRightHeight() const { return m_eyeHeight * eyeRightScaleY(); }
  bool winking() const { return m_winkHold >= 0.0; }
  qreal dashAngle() const { return m_dashAngle; }
  qreal dashLength() const { return m_dashLength; }
  qreal dashIntensity() const { return m_dashIntensity; }
  bool dashing() const { return m_mood == Dashing; }
  qreal badge() const { return m_badge; }
  qreal rings() const { return m_rings; }

  // Flung-off droplets, as {x, y, radius, opacity} in body-radius units
  // relative to her centre. Empty unless she is scattering.
  QVariantList droplets() const;
  qreal time() const { return m_time; }
  int mood() const { return m_mood; }
  // 0 wide awake, 1 about to nod off. Sleep is a slope rather than a cliff:
  // her lids lower and her flourishes thin out on the way down, so you can
  // see it coming.
  qreal drowsiness() const { return m_drowsy; }
  bool sleeping() const { return m_mood == Asleep; }
  bool dragging() const { return m_mood == Held; }

  // Interaction.
  Q_INVOKABLE void poke();
  Q_INVOKABLE void wink();
  Q_INVOKABLE void scatter();

  // Flight. `speed` is 0..1; the backend drives the window, this drives how
  // she looks while it happens.
  Q_INVOKABLE void beginDash(qreal angle, qreal speed);
  Q_INVOKABLE void updateDash(qreal angle, qreal speed);
  Q_INVOKABLE void endDash();
  Q_INVOKABLE void think(qreal seconds = 3.2);
  Q_INVOKABLE void alert();
  Q_INVOKABLE void notify();
  Q_INVOKABLE void wake();
  Q_INVOKABLE void beginDrag();
  Q_INVOKABLE void endDrag(qreal throwSpeed = 0.0);
  Q_INVOKABLE void setHovered(bool hovered);

  // `x`/`y` are the cursor position relative to Nala's centre, in units of her
  // radius; the caller decides whether that comes from the compositor or from
  // a local hover.
  Q_INVOKABLE void lookAt(qreal x, qreal y);
  Q_INVOKABLE void lookIdle();

  // Look about, as if something just caught her attention.
  Q_INVOKABLE void glanceAbout();

  // Drive one frame. `dt` is seconds.
  Q_INVOKABLE void tick(qreal dt);

  void setReducedMotion(bool reduced);
  bool reducedMotion() const { return m_reduced; }
  void setSleepWhenIdle(bool enabled);
  void setIdleAntics(bool enabled);

  // Request a form directly. Like every other morph this waits for any
  // transition already in flight, so the silhouette never tears.
  Q_INVOKABLE void changeForm(int form, qreal seconds = 0.22);

  // Hard reset used by tests and by wake-up: abandon the current transition.
  Q_INVOKABLE void snapForm(int form);

  // Drop everything and return to the idle pose immediately: no rings, no
  // badge, no squash, eyes at rest. Backs the `rest` command.
  Q_INVOKABLE void rest();

signals:
  void frame();
  void moodChanged();

private:
  void setMood(Mood mood);
  void morphTo(int form, qreal seconds);
  void scheduleBlink();
  void advanceBlink(qreal dt);
  qreal lidFor(qreal phase) const;
  void advanceIdle(qreal dt);
  int drawAntic();
  void performAntic(int antic);
  void advanceMorph(qreal dt);
  void settle(qreal &value, qreal target, qreal dt, qreal rate) const;
  void placeEyes();
  void buildTransform();
  qreal random(qreal lo, qreal hi);

  Mood m_mood = Resting;

  int m_formA = Circle, m_formB = Circle;
  qreal m_formMix = 1.0, m_morphRate = 0.0;
  int m_queuedForm = -1;
  qreal m_queuedSeconds = 0.0;

  qreal m_dotsSpread = 0.0, m_dotsSpreadTarget = 0.0;
  qreal m_dotsShrink = 0.0, m_dotsShrinkTarget = 0.0;
  qreal m_dotsPhase = 0.0;

  qreal m_squashX = 1.0, m_squashY = 1.0;
  qreal m_squashXTarget = 1.0, m_squashYTarget = 1.0;
  qreal m_scale = 1.0, m_scaleTarget = 1.0, m_scaleVelocity = 0.0;
  qreal m_roll = 0.0, m_rollTarget = 0.0;
  // Kept apart from m_roll: the roll settles towards a target, and a spin
  // folded into it would simply be pulled back out again every frame.
  // Tumble state: an in-plane spin, a tilt away from face-on, and the
  // direction that tilt leans in. The third is what separates a tumble from a
  // squash: hold it fixed and she just looks compressed.
  qreal m_spin = 0.0, m_tilt = 0.0, m_tiltPhase = 0.0, m_tiltAxis = 0.0;
  // She unfolds back to a circle before the rings fade, not with them.
  bool m_thinkUnfolded = false;
  QVector4D m_transform{1, 0, 0, 1};
  qreal m_projectedArea = 1.0;
  qreal m_bobX = 0.0, m_bobY = 0.0;

  // Where an eye ended up on screen, and how much the sphere's curvature
  // squashes it there.
  struct Eye {
    QPointF position;
    QPointF scale{1.0, 1.0};
    qreal angle = 0.0; // slit rotation, radians
    qreal lid = 0.0;   // 0 = open, 1 = squeezed shut
  };
  Eye m_left, m_right;

  // The gaze is an orientation, not a translation: the eyes are carried around
  // a sphere just under the surface of her body.
  qreal m_yaw = 0.0, m_pitch = 0.0;
  qreal m_yawTarget = 0.0, m_pitchTarget = 0.0;

  qreal m_eyeWidth = 0.155, m_eyeHeight = 0.26, m_eyeRound = 1.0;
  qreal m_eyeWidthTarget = 0.155, m_eyeHeightTarget = 0.26;
  qreal m_eyeRoundTarget = 1.0;

  qreal m_blinkPhase = -1.0; // <0 means "not blinking"
  qreal m_nextBlink = 2.6;
  // A wink shuts one eye and leaves it shut, so it is held rather than timed
  // like a blink. Negative means "not winking".
  qreal m_winkHold = -1.0;
  qreal m_winkLid = 0.0;

  // A droplet thrown clear when she breaks apart.
  struct Droplet {
    QPointF position;
    QPointF velocity;
    qreal radius = 0.0;
    qreal life = 0.0;  // seconds remaining
    qreal span = 1.0;  // seconds it started with
  };
  QVector<Droplet> m_droplets;

  qreal m_dashAngle = 0.0, m_dashSpeed = 0.0;
  qreal m_dashLength = 0.0, m_dashIntensity = 0.0;

  qreal m_badge = 0.0, m_badgeTarget = 0.0, m_badgeVelocity = 0.0;
  qreal m_rings = 0.0, m_ringsTarget = 0.0;

  qreal m_time = 0.0;
  qreal m_hold = 0.0;     // seconds left in the current mood
  qreal m_idle = 0.0;     // seconds since the last interaction
  qreal m_antic = 7.0;     // seconds until the next flourish worth watching
  qreal m_glance = 4.0;    // seconds until the next small movement
  QVector<int> m_anticBag; // shuffled, dealt one at a time, refilled when empty
  qreal m_breathe = 0.0;  // -1..1, the slow idle swell
  qreal m_drowsy = 0.0;   // 0..1, how close she is to nodding off
  qreal m_cooldown = 0.0; // rate-limit on reactions

  bool m_reduced = false;
  bool m_sleepWhenIdle = true;
  bool m_idleAntics = true;
  bool m_hovered = false;
  bool m_lookingAtCursor = false;
  qreal m_sinceLook = 99.0; // seconds since the cursor last moved

  QRandomGenerator m_random;
};
