# Grounding desktop actions

Use “click the export button” (`nala ask click the export button`). A single
named click takes the command fast path into `computer.locate_and_click`; it
needs no conversational model call to choose coordinates. App launches and
window switches still use the application index and Hyprland metadata first.
More complex requests can select the same typed tool through the model.

The separate `GuiGrounder` observes the focused monitor, predicts a coarse
point, crops around it, overlays numbered landmarks, and asks the dedicated
`llm.visionModel` to correct the point. It translates image/crop coordinates
into logical desktop coordinates, including negative monitor origins and
scaling. The last marker must converge within four source-image pixels by default,
with confidence at least 0.90 and an explicit `ready` response. Those are
model estimates, not calibrated probabilities. No reasoning trace is stored.

The loop allows four refinements by default, up to two retries, and a
60-second deadline. It holds the original window, monitor geometry and image
size; changes stop the operation. A fresh observation just before the click
also checks for a layout change. Afterward it compares the monitor and a local
region around the target. If an `expected` state was supplied, the vision
model must additionally confirm that state. Without `expected`, success means
an observed visual change, not proof that a business operation completed.
Animation or a blinking caret can still fool a visual-change check.

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
intersecting windows; private windows block a monitor observation. Hyprland,
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
