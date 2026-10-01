# Closed-loop computer use

Use native application/window/filesystem tools first. Named window focus uses
Hyprland metadata and checks the active window; `computer.perform` is for the
remaining controls. The model supplies a semantic goal and expected effect,
not coordinates requested from the user. Existing low-level tools remain.

## Controller

`GuiGrounder` owns the observation/action lifecycle. The production ports in
`assistant_gui.cpp` enforce turn generation, privacy, focused-window identity,
computer-input permission and cursor position on every consequential operation.
Each retry goes through the existing confirmation system. Stop cancels models,
captures, stabilization callbacks and future inputs.

1. Capture the focused window with its logical desktop bounds and application.
2. Try exact, unique AT-SPI labels and roles; use fresh bounds immediately before
   input. Missing accessibility falls back to vision; duplicates stop safely.
3. Ground on an overview (at most 1280 pixels wide), then a native 640×480 crop.
   Track numbered rings without obscuring control centers. Translate crop,
   image and logical desktop coordinates explicitly, including negative origins.
4. Refine absolute coordinates or bounded relative image offsets. Confidence
   alone is insufficient: require convergence and an independent clean-crop
   clickable-bounds check. Reject changes around the target before clicking.
5. Sample at 80 ms until three consecutive visual samples settle, bounded by
   1600 ms. A timeout stops rather than causing more input.
6. Measure native-resolution changed tiles. Show localized before/after evidence
   with highlighted changes. Describe observable changes without the intended
   goal, then separately compare those facts with the expected effect.
7. Return verified success or a reason to replan. No visible response permits
   bounded, newly authorized re-grounding (default two retries). A changed but
   incorrect outcome, failed typing or changed application is not blindly retried.

A popup can be observed and verified across window/layout changes only when it
belongs to the same application and an expected effect was supplied. No further
input is injected into the newly focused dialog by that action.

## Accessibility and keyboard

The optional read-only Qt D-Bus AT-SPI client restricts traversal to the active
window's process, visible/sensitive/enabled interactive elements, 256 nodes and
1500 ms. It never enables global accessibility or reads unrelated application
labels. Availability depends on the application's accessibility bridge. Browser
controls can benefit from AT-SPI; no CDP endpoint, injected extension or separate
DOM automation service is installed.

`computer.perform` accepts explicit strategies `browser_address_bar`, `find`,
`next_field`, `previous_field`, `next_item`, `previous_item`, or `click`.
These use Ctrl+L, Ctrl+F, Tab, Shift+Tab, Down, Up, or grounding. Ctrl+L is
restricted to recognized browsers. Keyboard actions still require permissions
and visible verification. Text entry verifies field focus before typing and
checks the resulting literal text afterward; it never automatically types twice.

## Optional semantic memory

`agent.gui.workflowMemory` defaults to **false**. Both it and ordinary memory
recording must be enabled and unpaused. Trajectories retain redacted text,
application, timestamp, observation hash, method, confidence, expected/observed
effect, actions, refinements, retries and success. No screenshots are stored by
this subsystem, and entered text is not retained as a replayable step.

A successful same-app turn with two to eight verified semantic clicks can save
a procedure. `computer.workflows` retrieves matching procedures as untrusted
data; `computer.perform` with a workflow ID re-grounds and verifies each step
through `Assistant::callTool`, including its own confirmation. Raw coordinates,
arbitrary tool bodies and cross-app replay are rejected. Existing forget,
retention, pause and screen-clear controls govern these records.

## Settings and diagnostics

Defaults: `agent.gui.enabled`, `accessibility`, `cropZoom`, `verifyActions`,
`visualDiff`, `waitForStable` are true; `maxRefinements=4`, `maxRetries=2`,
`tolerancePixels=4`, `stableIntervalMs=80`, `stableSamples=3`,
`stableTimeoutMs=1600`. Bounds are validated by the settings schema.

`developer.debug` logs the controller's structured result: actions, screenshots,
model calls, refinements, retries, elapsed time, grounding method, confidence,
observation hash and observed effect. These stay out of normal conversation.
Confidence is a model signal, not a calibrated probability of success.

Offline tests inject all desktop/model ports. `nala-agent-bench --vision MODEL`
uses real local vision predictions on six synthetic export dialogs, with simulated
inputs and independent target rectangles. It never moves the real pointer.
Fixture success does not establish live desktop success or general GUI accuracy.

## Local fixture results, 2026-09-30

`qwen3-vl:8b-instruct-q4_K_M` hit and verified all six export-dialog fixtures.
Final distance from the independent target center was 1–3.61 pixels. Warm
fixtures took 3.2–4.2 s; the first fixture took 10.6 s including model startup.
Grounding, clean target confirmation and evidence-first verification used real
local model calls; pointer/clicks were simulated. This does not measure live
AT-SPI support, real desktop success, scrolling reliability or general accuracy.
[Raw results](../benchmarks/results/2026-09-30-gui-vision.json) include screenshot,
model-call, refinement and timing counts. Offline controller tests additionally
cover duplicate/disabled labels, stale target bounds, cancellation, unstable UI,
relative offsets, popup windows and verified typing/keyboard actions.
