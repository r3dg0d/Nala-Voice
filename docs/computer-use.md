# Grounding desktop actions

Use “click the export button” (`nala ask click the export button`). A single
named click takes the command fast path into `computer.locate_and_click`; it
needs no conversational model call to choose coordinates. App launches and
window switches still use the application index and Hyprland metadata first.
More complex requests can select the same typed tool through the model.

The separate `GuiGrounder` observes the focused window, predicts a coarse
point, crops around it, overlays numbered landmarks with unobscured centers, and asks the dedicated
`llm.visionModel` to correct the point. It translates image/crop coordinates
into logical desktop coordinates, including negative monitor origins and
scaling. The last marker must converge within four source-image pixels by default,
with confidence at least 0.90 and an explicit `ready` response. Those are
model estimates, not calibrated probabilities. No reasoning trace is stored.

The loop allows four refinements by default, up to two retries, and a
60-second deadline. Crops follow estimates that approach their edge. Before
clicking, a separate prediction on a clean crop returns the requested control's
bounding box. The proposed point must lie inside that box with an inset margin;
convergence alone cannot authorize a click. Nala also checks Hyprland's actual
cursor position after moving and immediately before clicking.

A fresh observation checks both the overall layout and a 64-pixel region around
the target. Focus, capture geometry, image size or local layout changes stop the
operation. After clicking, visual change alone is insufficient: the vision model
must confirm the supplied `expected` state, or visible activation of the requested
control. Rejected semantic verification does not automatically repeat the click.
These checks reduce false successes but cannot prove that an application completed
a business operation; a vision model can still misunderstand a control or result.

The tools accept `wholeMonitor: true` for taskbars/panels outside the focused
window. Use `window.focus_target` first to select another application's window.
Default window captures preserve small controls at higher resolution and avoid
unrelated content elsewhere on the monitor.

All named visual actions are conservatively **high risk**: an arbitrary button
may send, purchase, submit or delete. The existing confirmation card is used.
A retry requires another confirmation, re-observation and re-grounding;
refusing, stopping or disabling input prevents the next action. Existing
coordinate tools remain available for debugging with their existing risks.

`computer.locate_and_type` focuses a field, asks the vision model to check its
visible caret, checks window focus again, uses `wtype`, then captures the
result. It rejects secret/payment/code fields and recognizable secret strings.
It cannot prove DOM/accessibility focus or exact text correctness. Do not use
it for secrets. A visual-change failure after typing is reported without
retyping. Desktop focus can still change while a keyboard helper is running.

| Setting | Default |
| --- | --- |
| `agent.gui.maxRefinements` | 4 (1–8) |
| `agent.gui.maxRetries` | 2 (0–2; each requires confirmation) |
| `agent.gui.tolerancePixels` | 4 (1–16) |
| `agent.gui.coordinateSpace` | normalized_1000; pixels optional |
| `agent.gui.cropZoom` | true |
| `agent.gui.verifyActions` | true |
| `llm.visionModel` | empty: inherits the configured LLM |

Capture remains in memory. The deterministic privacy gate checks focused and
intersecting windows on the current workspace; private windows block the captured
region. Sticky/special-workspace windows are checked conservatively. Hyprland,
`grim`, `ydotoold` and `wtype` remain optional. NixOS's `/run/ydotoold/socket`
is detected and passed to `ydotool` when its environment variable is absent.
No AT-SPI, browser DOM bridge or trained grounding model is bundled.

The design adapts iterative marked feedback from
[See, Point, Refine](https://arxiv.org/abs/2604.13019), spatial feedback from
[Learning GUI Grounding](https://arxiv.org/abs/2509.21552), and coarse/fine
perception from [GUI-Eyes](https://arxiv.org/abs/2601.09770). Existing structured
tools complement screenshots, consistent with
[Screenshots or Tools?](https://arxiv.org/abs/2608.03327).
Nala implements inference-time orchestration; it does not reproduce their
training, datasets or reported benchmark scores.

The default coordinate protocol is `agent.gui.coordinateSpace = normalized_1000`: model coordinates are explicitly converted from a 0–1000 image grid before crop and desktop transforms. Pixel-speaking backends can select `pixels`. `agent.gui.tolerancePixels` defaults to 4 and accepts 1–16 source-image pixels. This threshold controls convergence; it is not a guarantee of target accuracy.
