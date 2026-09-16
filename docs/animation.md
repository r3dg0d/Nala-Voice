# Where the numbers come from

Every proportion and timing in Nala was measured from the reference animation
(1458×1458, 60 fps, 1845 frames) rather than eyeballed. This note records what
was measured and how, so the constants in `shaders/mascot.frag` and
`src/mascot.cpp` can be checked or re-derived.

## Method

Frames were sampled with ffmpeg and measured with a small numpy script:

```bash
ffmpeg -i reference.mp4 -vf "select='not(mod(n\,3))',scale=180:180,tile=8x8" -frames:v 1 sheet.png
```

For a single frame, the body is the set of pixels darker than mid-grey; the
eyes are the *enclosed* light regions inside it, found by flood-filling the
background inward so the page around her is not mistaken for an eye.

Extents below are **half-extents in the shader's coordinate space**, where the
item spans `[-1, 1]`. A shape covering fraction `f` of the frame's width has a
half-extent of `f`.

## Palette

| Role | Value | Notes |
| --- | --- | --- |
| Body | `#0a090c` | Near-black with a faint violet cast |
| Eyes | `#fdfdfd` | |
| Notification badge | `#3ea1f5` | |
| Orbit arcs | `hsv(h, 0.55, 0.84)` | Hue swept around the wheel |

The arc colours were recovered by clustering every saturated pixel of a
"thinking" frame by hue: all sixteen populated bins landed at S ≈ 0.5–0.6 and
V ≈ 0.75–0.88, i.e. one rainbow at constant saturation and value.

## Silhouettes

Measured against Nala's rendered output (`scripts/poses.sh`):

| Form | Reference w × h | Nala w × h |
| --- | --- | --- |
| Circle | 0.529 × 0.536 | 0.528 × 0.528 |
| Egg | 0.436 × 0.525 | 0.440 × 0.499 |
| Hexagon | 0.483 × 0.530 | 0.490 × 0.541 |
| Triangle | 0.528 × 0.486 | 0.525 × 0.478 |
| Exclamation | 0.132 × 0.285 | 0.114 × 0.281 |
| Teardrop | 0.227 × 0.298 | 0.214 × 0.293 |
| Sleeping dot | 0.083 × 0.082 | 0.082 × 0.082 |

Four findings worth recording, because all of them are easy to get backwards:

- **The hexagon is pointy-top.** It measures taller than wide (0.530 vs 0.483),
  and the silhouette is only 0.043 wide four pixels below its apex — a vertex,
  not an edge.
- **The hexagon and triangle are rounded far harder than textbook shapes.**
  Fitting `half_height - half_width = (1.1547 - 1) · r` to the hexagon gives a
  circumradius of 0.30 against a corner radius of 0.18.
- **The teardrop points down.** It is round at the top and drawn to a point at
  the bottom, widest 40% of the way down. Building it the other way up is an
  easy mistake and scores badly against the reference (0.64 IoU against 0.89).
- **The exclamation mark leans right and barely tapers.** Its centre drifts
  from +0.111 at the apex to −0.169 at the dot, about 16°, and the stem holds a
  near-constant width (0.255 → 0.236) rather than narrowing to a point. Its dot
  is slightly narrower than the stem.

### Checking a change

Silhouettes are compared by intersection-over-union against the matching
reference frame, after normalising both to a common bounding box:

| Form | IoU |
| --- | --- |
| Circle | 0.925 |
| Egg | 0.946 |
| Hexagon | 0.953 |
| Triangle | 0.951 |
| Exclamation | 0.870 |
| Teardrop | 0.885 |
| Sleeping dot | 0.986 |
| **Mean** | **0.931** |

The residual is mostly edge antialiasing and the fact that the reference frames
are single moments of a continuously breathing body — the reference circle
measures 0.529 × 0.536, i.e. mid-bob, while Nala's is exactly round at rest.

The window is sized `1.89 ×` the body diameter (`1 / 0.529`) so forms that
reach past the idle circle — the exclamation mark, the orbit arcs — are not
clipped.

## Face

**The eyes are features on a sphere, not decals on a flat face.** This is the
single most important thing about them, and getting it wrong makes her look
dead no matter how the rest is tuned.

Measured across 169 frames of the idle sections, the pair's separation falls
away as the gaze swings sideways:

| \|pair centre x\| | 0.00–0.12 | 0.12–0.25 | 0.25–0.38 | 0.38–0.75 |
| --- | --- | --- | --- | --- |
| measured gap | 0.560 | 0.519 | 0.468 | 0.432 |
| sphere model | 0.555 | 0.540 | 0.483 | 0.426 |

Fitting `(x/s)² + (gap/g₀)² = 1` by least squares over the frames where the
eyes are normally open gives a sphere radius of **s = 0.654 R** with a
separation at centre of **g₀ = 0.559 R**, i.e. a half-gap angle of 0.442 rad.

| Property | Reference | Nala |
| --- | --- | --- |
| Gap looking straight ahead | 0.556 R | 0.561 R |
| Eye width / height (ahead) | 0.324 / 0.499 R | 0.326 / 0.507 R |
| Slit aspect (h/w) | 1.54 | 1.56 |
| Pair travel, horizontal | ±0.63 R | ±0.48 R |
| Pair travel, vertical | −0.50 to +0.36 R | ±0.45 R |
| Tilt at a diagonal glance | up to +0.185 | +0.178 |

Three consequences fall out of the sphere and are all visible in the reference:

- **A glance carries the eyes a long way.** Clamping the travel to ±0.34 R, as
  a flat translation invites, reads as the eyes barely moving.
- **The pair tilts** once the gaze is both sideways and up or down. Applying
  yaw before pitch produces this for free; the other order keeps the pair
  stubbornly level. Only 55% of the geometric tilt is used, or the extremes
  overshoot what the reference does.
- **The slits lean** with the sphere's local "up" — right when she looks up,
  left when she looks down.

Foreshortening is **uniform**, not a true tangent-plane projection: the
reference holds the height/width ratio at about 1.5 whichever way she looks
(1.54 near centre, 1.48 far to the side), so both axes shrink together.

Only the circle, egg, hexagon and triangle carry a face. The exclamation mark,
the teardrop, the "..." run and the sleeping dot are featureless, so the eyes
fade out across the morph rather than riding along on a shape that has none.

## Secondary motion

The things that keep her from looking like a cutout, all measured rather than
invented.

**She breathes.** During idle her height/width ratio swells by about 1.2% at
**0.31 Hz**, and she drifts vertically at that same frequency with an amplitude
of 0.0136 of her body. The shared frequency is the point: the two are coupled,
so she sinks as she widens, and that reads as breathing rather than as jitter.
Nala runs at 1.40% and 0.0135.

**The exclamation mark does not wobble.** This is the easy one to get wrong --
an impact wants a decaying shake. Frame by frame the reference simply swings
into its lean and stops:

| t (s) | 5.95 | 6.02 | 6.05 | 6.10 | 6.15 | 6.30 |
| --- | --- | --- | --- | --- | --- | --- |
| lean | −6.6° | −9.5° | −11.6° | −14.6° | −15.9° | −17.0° |

It arrives 11° short of its settled −17.4° and eases in over about 350 ms,
monotonically, with no overshoot. Across the whole held stretch afterwards the
total swing is 3.5°, decaying to nothing.

**Nothing overshoots in size.** Both of the reference's re-inflations rise to
full over 583 ms and 650 ms with an overshoot of exactly 0.00%, so the scale
spring is damped past critical. The elasticity lives in the squash, not in the
size.

## The extras

**The notification badge pops.** Measured over its one clean appearance in the
reference (t = 8.90-10.08 s):

| Property | Value |
| --- | --- |
| Settled radius | 0.142 R |
| Centre | 1.008 R from her middle, at 41.8 deg |
| Growth | peaks about 330 ms in |
| Overshoot | **+19.5%** before settling |

So it needs a spring rather than an approach. Nala renders at 0.142 R, 1.008 R
and 41.7 deg, peaking 18% over at 300 ms.

Her eyes go wide with it: 0.442 x 0.495 R against the 0.324 x 0.499 R of her
resting slits.

**The orbit arcs are sharply asymmetric.** They reach full in **117 ms** and
take **683 ms** to fade -- one rate cannot serve both. Their stroke is 0.059 R,
about twice what it looks like by eye, and between them they cover
0.68 +- 0.18 R^2 (range 0.35-1.04 across the thinking stretch). That last
figure is the one that matters: near-complete orbits overshoot it by more than
double, so the arcs are partial. They read as arcs rather than as scribbles
because the stroke barely tapers, not because they are long.

**She tumbles in three dimensions while thinking, not flat.** Measured over
the stretch where she is a triangle throughout:

| | Reference | Nala |
| --- | --- | --- |
| Apparent spin | 1.46 rad/s | 1.46 |
| Silhouette area swing | 42% | 41% |
| Bounding-box fill | 0.444 to 0.661 | 0.511 to 0.589 |
| Width / height, low end | 0.905 | 0.875 |

A flat spin holds all of those constant. Losing 42% of your area as you turn
means the shape is foreshortening, so she is modelled as a flat plate that
spins in the screen plane while tilting away from face-on. A plate tilted by
`tilt` about an axis lying in the screen plane compresses along the
perpendicular to that axis by `cos(tilt)`, which is both the projection and,
conveniently, the projected area.

Two things that are easy to get wrong here:

- **The tilt axis has to turn too.** Pinned to one direction she does not
  tumble, she just looks squashed, and no amount of tuning the tilt fixes it.
- **The spin has to live outside the roll.** The roll settles towards a
  target, so a spin folded into it is pulled straight back out every frame and
  she never turns at all. That bug had her spinning at 0.04 rad/s against the
  reference's 1.46.

She also **unfolds back to a circle while the rings are still up**, rather than
at the moment they start to fade. The reference does the two in sequence;
doing them together reads as one abrupt change.

A warning about measuring this. The orbit arcs are coloured, and at their
saturation blue sits at luminance 136. A "dark pixels are the body" threshold
loose enough to include it swallows the arcs into the silhouette, balloons the
bounding box, and reports a fill of 0.32 where the truth is 0.55. Mask on
saturation as well as luminance.

## Timing

Measured over all 1845 frames: 34 blinks, and 25 stretches where the silhouette
is changing.

| Behaviour | Reference | Nala |
| --- | --- | --- |
| Blink, full cycle | 243 ms | 250 ms |
| ... closing | 121 ms | slower than opening |
| ... fully shut | 71 ms | yes, shuts completely |
| ... opening | 84 ms | faster than closing |
| Blink interval | 0.6–6.4 s, mean 2.85 s | 0.7–5.2 s |
| Form morph | mean 200 ms, never over 333 ms | 233 ms |
| Idle flourish interval | — | 9–18 s |
| Sleep after | — | 75 s idle |
| "..." pulse | — | ~4.4 rad/s, one dot at a time |

Two of these are worth calling out because the obvious guess is wrong:

- **A blink closes more slowly than it opens** (121 ms against 84 ms), and
  holds shut for about 70 ms in between. The instinct to snap shut and ease
  open is backwards.
- **The lids shut completely.** The eyes vanish at the bottom of a blink rather
  than flattening to a slit.

The `--self-test` run prints its own measured blink cycle and morph duration
against these figures, so a change in easing shows up immediately.

## Winking, scattering and dashing

Three behaviours that are not just another silhouette.

**A wink is not a blink.** The reference squeezes the *right* eye to a clear
horizontal dash while the left stays a full slit, holds it for two or three
seconds, then blinks out of it. The dash matters: a blink shuts the eyes away
completely, a wink leaves one visible and flattened, so the lid stops at 0.86
rather than 1.0 and the squeezed eye spreads a little sideways. Lids are
therefore per eye rather than a single shared height.

**Scattering** collapses her to a speck while throwing off five or six
droplets: one or two proper blobs among several specks, drifting outward,
slowing against drag and fading over the back half of their lives.

**The dash** is the one behaviour that moves her window rather than her
silhouette. She tucks into a speck and streaks off, bouncing off the edges of
the screen and arriving with a bounce. The trail is a slender blade of about
six ribbons, all bowing the same way and bundled close -- not a fan opening to
both sides -- tapering to points at each end. She sits about a third of the
way along it, so it straddles her rather than trailing from her edge. It is
about one body-diameter long, which is why it fits inside the window she
already has instead of needing to be drawn across the desktop.

## Watching it in motion

Stills only go so far. `nala --film <dir>` records a scripted sequence at
exactly 60 fps, one PNG per frame, so her motion can be laid against the
reference's frame by frame. Two things showed up that no single frame could.

**Leaving rest and returning to it are not mirror images.** Measured on the
normalised silhouette width across the opening sequence:

| | Reference | Nala before | Nala now |
| --- | --- | --- | --- |
| Collapse into "..." | 67 ms | 83 ms | 83 ms |
| Return to the circle | **233 ms** | 100 ms | 233 ms |

The curve *shape* was already right; it was simply compressed. Reacting is
snappy and relaxing is gentle, so settling back to rest has its own duration
(`Mascot::kSettleBack`) rather than reusing the reaction one.

**The emphasised dot swells, it does not only darken.** As a fraction of the
run's own span the reference's dots go from 0.158-0.166 when quiet to 0.212
when loud, and their darkness from about 0.5 to 0.85. Nala now runs 0.161 to
0.209 and 0.54 to 0.9. The quiet dots had been far too faint at a third
darkness, which made the whole run read as grey rather than as one dot leading.

A note on measuring this: normalise by something intrinsic to her, like the
span between the outer dots. Normalising by the image width silently rescales
everything when the window manager gives the preview a different size, which
looked at first like the dots had halved.

## The "..." run

The three dots are not a separate shape. They are the circle with three
parameters animated — core radius, side radius and separation — so that at rest
the form *is* the idle circle. That is what makes the collapse read as one
continuous motion instead of a cross-fade between two drawings.

The quiet dots are faded rather than greyed. The reference draws them grey on a
near-white page; on a desktop overlay a fixed grey reads as a smudge over dark
wallpaper, while lower opacity reads correctly over anything.
