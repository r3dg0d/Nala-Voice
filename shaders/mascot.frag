#version 440

// Nala's body. Everything the mascot is made of -- silhouette, eyes and the
// notification badge -- is a signed distance field evaluated per pixel, so the
// shape stays crisp at any size and morphs between forms by blending distances
// rather than by interpolating vertices.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;

    vec4 bodyColor;
    vec4 eyeColor;
    vec4 badgeColor;

    // Silhouette: blend `formMix` of the way from `formA` to `formB`.
    float formA;
    float formB;
    float formMix;

    // Whole-body deformation.
    float squashX;
    float squashY;
    float bodyScale;
    // Inverse of the 2x2 that projects her onto the screen, as
    // (m00, m01, m10, m11). Carries both the in-plane lean and the tumble,
    // which is a rotation in three dimensions: a flat plate turning away from
    // the viewer foreshortens, and an in-plane rotation alone cannot.
    vec4 bodyTransform;

    // Eyes, in units of the body radius, relative to the body centre. Each
    // carries its own scale because the two sit at different places on the
    // sphere and so foreshorten by different amounts.
    vec2 eyeLeft;
    vec2 eyeRight;
    vec2 eyeLeftScale;
    vec2 eyeRightScale;
    float eyeLeftAngle;
    float eyeRightAngle;
    float eyeWidth;
    float eyeHeight;
    float eyeRound;

    // Extras.
    float badge;      // notification dot, 0..1
    float dotsSpread; // "..." separation, 0..1
    float dotsShrink; // how far the core has collapsed into the "..." run
    float dotsPhase;  // travelling emphasis around the "..." run
};

// Every constant below is a half-extent in the item's [-1,1] space, measured
// from the reference animation frame by frame. See docs/animation.md.
const float kRadius = 0.529; // idle body radius
const float kDotGap = 0.285; // "..." dot spacing
const float kDotSide = 0.092;
const float kDotCore = 0.092; // all three are the same size when quiet

float sdCircle(vec2 p, float r) { return length(p) - r; }

float sdEllipse(vec2 p, vec2 r) {
    // Cheap bounded approximation: exact enough for a silhouette this smooth.
    float k = length(p / r);
    return (k - 1.0) * min(r.x, r.y);
}

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float sdSegment(vec2 p, vec2 a, vec2 b, float r) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h) - r;
}

float sdHexagon(vec2 p, float r) {
    const vec3 k = vec3(-0.866025404, 0.5, 0.577350269);
    p = abs(p);
    p -= 2.0 * min(dot(k.xy, p), 0.0) * k.xy;
    p -= vec2(clamp(p.x, -k.z * r, k.z * r), r);
    return length(p) * sign(p.y);
}

float sdTriangle(vec2 p, float r) {
    const float k = 1.7320508;
    p.x = abs(p.x) - r;
    p.y = p.y + r / k;
    if (p.x + k * p.y > 0.0)
        p = vec2(p.x - k * p.y, -k * p.x - p.y) * 0.5;
    p.x -= clamp(p.x, -2.0 * r, 0.0);
    return -length(p) * sign(p.y);
}

vec2 rotate(vec2 p, float a) {
    float c = cos(a), s = sin(a);
    return vec2(c * p.x - s * p.y, s * p.x + c * p.y);
}

// Smooth union -- lets separate lumps fuse into one another the way the real
// mascot's parts do when it reassembles.
float smoothUnion(float a, float b, float k) {
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

// The "..." run. At spread 0 with a full core this is exactly the idle circle,
// which is what makes the collapse into three dots one continuous motion.
// How emphasised dot `i` is right now, 0..1.
float dotPulse(int i) {
    float w = max(0.0, cos(dotsPhase - float(i) * 2.0943951));
    return w * w;
}

// The emphasised dot swells as well as darkening: measured across the
// reference's run it goes from 0.046 R when quiet to 0.058-0.061 R when loud.
// Gated on the spread so the swell cannot distort the collapse itself.
float dotScale(int i) {
    return mix(1.0, 1.0 + 0.30 * dotPulse(i),
               smoothstep(0.55, 1.0, dotsSpread));
}

// Opacity. The quiet dots sit around half darkness in the reference (0.49 to
// 0.58) rather than a third, against the loud one's 0.64 to 0.88.
float dotWave(int i) { return 0.55 + 0.40 * dotPulse(i); }

float dotCore() { return mix(kRadius, kDotCore, dotsShrink) * dotScale(1); }
float dotSide(int i) { return kDotSide * dotsSpread * dotScale(i); }
float dotGap() { return kDotGap * dotsSpread; }

float sdDots(vec2 p, float spread, float shrink) {
    float gap = kDotGap * spread;
    float d = sdCircle(p, mix(kRadius, kDotCore, shrink) * dotScale(1));
    d = min(d, sdCircle(p - vec2(gap, 0.0), kDotSide * spread * dotScale(2)));
    d = min(d, sdCircle(p + vec2(gap, 0.0), kDotSide * spread * dotScale(0)));
    return d;
}

// In the reference the three dots pulse in turn, so the run reads as a
// progress indicator rather than as punctuation.
//
// A pixel takes the emphasis of the dot it actually belongs to -- picked by
// smallest distance field, not nearest centre. While the core is still most of
// her body the two side dots sit inside it, and going by centres would hand
// half the body to them and grey it out on the way in.
float dotsEmphasis(vec2 p) {
    float core = dotCore(), gap = dotGap();

    // The core only joins the pulse once it is genuinely a dot; until then it
    // is still her body, and her body is solid.
    float formed = smoothstep(0.80, 1.0, dotsShrink);

    float best = sdCircle(p, core);
    float emphasis = mix(1.0, dotWave(1), formed);

    float dLeft = sdCircle(p + vec2(gap, 0.0), dotSide(0));
    if (dLeft < best) { best = dLeft; emphasis = dotWave(0); }

    float dRight = sdCircle(p - vec2(gap, 0.0), dotSide(2));
    if (dRight < best) { best = dRight; emphasis = dotWave(2); }

    return emphasis;
}

// The stem holds a constant width rather than tapering: sampled down the
// reference it reads 0.250, 0.255, 0.255, 0.255, 0.255, 0.255, 0.252, 0.250.
// Tapering it, which is the intuitive thing to do, costs about 0.02 of IoU.
float sdExclaim(vec2 p) {
    p = rotate(p, 0.27);
    float stem = sdSegment(p, vec2(0.0, 0.217), vec2(0.0, -0.030), 0.068);
    float point = sdCircle(p - vec2(0.0, -0.215), 0.062);
    return min(stem, point);
}

// Round at the top, drawn down to a point at the bottom. The reference is
// widest 40% of the way down, which is where the ball centre sits.
float sdTeardrop(vec2 p) {
    p = rotate(p, 0.07); // the tip drifts left, as the reference's does
    float ball = sdCircle(p - vec2(0.0, 0.060), 0.225);
    float tip = sdSegment(p, vec2(0.0, 0.060), vec2(0.0, -0.272), 0.026);
    return smoothUnion(ball, tip, 0.15);
}

// Wider at the bottom than the top, rather than a plain ellipse.
float sdEgg(vec2 p) {
    p.y += 0.025;
    float taper = 1.0 - 0.15 * clamp(p.y / 0.50, -1.0, 1.0);
    return sdEllipse(vec2(p.x / taper, p.y), vec2(0.410, 0.50)) * taper;
}

float form(vec2 p, float which) {
    int f = int(which + 0.5);
    if (f == 1) return sdEgg(p);
    // Pointy-top, and rounded hard: the reference hexagon is much softer than
    // a textbook one, and measures taller than it is wide.
    if (f == 2) return sdHexagon(rotate(p, 1.5707963), 0.33) - 0.16;
    if (f == 3) return sdTriangle(p, 0.35) - 0.175;
    if (f == 4) return sdExclaim(p);
    if (f == 5) return sdTeardrop(p);
    if (f == 6) return sdDots(p, dotsSpread, dotsShrink);
    if (f == 7) return sdCircle(p, 0.083);
    return sdCircle(p, kRadius);
}

float isDots(float which) { return int(which + 0.5) == 6 ? 1.0 : 0.0; }

// Which forms carry a face. In the reference the exclamation mark, the "..."
// run, the teardrop and the sleeping dot are all featureless, so the eyes fade
// out across the morph instead of riding along on a shape that has no face.
float faceWeight(float which) {
    int f = int(which + 0.5);
    return (f == 0 || f == 1 || f == 2 || f == 3) ? 1.0 : 0.0;
}

// A single eye: a rounded vertical slit that can flatten to a blink dash or
// open out into a near-circle.
float sdEye(vec2 p, vec2 centre, vec2 squash, float lean) {
    vec2 extent = vec2(eyeWidth, eyeHeight) * squash * kRadius;
    float r = min(extent.x, extent.y) * eyeRound;
    return sdRoundBox(rotate(p - centre * kRadius, lean), extent, r);
}

void main() {
    vec2 p = (qt_TexCoord0 - 0.5) * 2.0;

    // Undo the body transform so the SDFs stay in their own tidy space.
    p /= max(bodyScale, 0.0001);
    p = vec2(bodyTransform.x * p.x + bodyTransform.y * p.y,
             bodyTransform.z * p.x + bodyTransform.w * p.y);
    p /= vec2(max(squashX, 0.0001), max(squashY, 0.0001));
    p.y = -p.y; // screen y grows downward; the shapes are authored y-up

    float a = form(p, formA);
    float b = form(p, formB);
    float body = mix(a, b, formMix);

    // One pixel, measured in the same space the distances live in.
    float px = fwidth(p.x) * 1.15;

    float bodyMask = clamp(0.5 - body / px, 0.0, 1.0);

    float eyes = min(
        sdEye(p, vec2(eyeLeft.x, -eyeLeft.y), eyeLeftScale, eyeLeftAngle),
        sdEye(p, vec2(eyeRight.x, -eyeRight.y), eyeRightScale, eyeRightAngle));
    // Eyes only exist where there is body to carve them out of, and only on
    // the forms that have a face at all.
    float face = mix(faceWeight(formA), faceWeight(formB), formMix);
    float eyeMask = clamp(0.5 - eyes / px, 0.0, 1.0) * bodyMask * face;

    // Measured on the reference: radius 0.142 R, centred 1.008 R out from her
    // middle at 41.8 degrees -- so it sits right on the rim.
    float badgeD = sdCircle(p - vec2(0.397, 0.354), 0.0751 * badge);
    float badgeMask = badge > 0.001 ? clamp(0.5 - badgeD / px, 0.0, 1.0) : 0.0;

    // The badge sits on the rim and pokes outside the silhouette, so it
    // contributes coverage of its own rather than only recolouring the body.
    float alpha = max(bodyMask * bodyColor.a, badgeMask);

    // Fade the quiet dots rather than greying them: on a dark desktop a fixed
    // grey would read as a smudge, while lower opacity reads correctly on any
    // wallpaper.
    // Gate on how far the core has actually collapsed, not on the morph: while
    // the body is still a body it must stay solid, or the whole silhouette
    // turns translucent on the way into the run.
    float dotsWeight = mix(isDots(formA), isDots(formB), formMix) *
                       smoothstep(0.15, 0.55, dotsSpread);
    if (dotsWeight > 0.001) {
        // On the way in only the emerging side dots are quiet; the core is
        // still her body, and her body is solid. Once the core has become a
        // dot in its own right it joins the pulse. Testing against the side
        // dots' own fields -- rather than against the blended silhouette,
        // which reaches past them -- is what keeps the body from dimming.
        float gap = dotGap();
        float dSide = min(sdCircle(p + vec2(gap, 0.0), dotSide(0)),
                          sdCircle(p - vec2(gap, 0.0), dotSide(2)));
        float inSide = 1.0 - step(0.0, dSide);
        float formed = smoothstep(0.80, 1.0, dotsShrink);
        float apply = dotsWeight * max(formed, inSide);
        alpha *= mix(1.0, dotsEmphasis(p), apply);
    }

    vec3 rgb = bodyColor.rgb;
    rgb = mix(rgb, eyeColor.rgb, eyeMask);
    rgb = mix(rgb, badgeColor.rgb, badgeMask);

    fragColor = vec4(rgb * alpha, alpha) * qt_Opacity;
}
