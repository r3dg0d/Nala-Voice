# Focusing the right window

Use app and title descriptions, without screenshots or coordinates:

```bash
nala ask focus the browser with Nala
nala ask focus the terminal with build
nala ask switch to Firefox on workspace 2
```

Single focus requests use the deterministic command path. The matcher combines
app class/initial class and title words, with browser/terminal aliases. Exact
titles outrank app-only or partial title matches. Explicit app, title and
workspace filters are available through `window.focus_target`; an exact address
from `window.list` can resolve any ambiguity. The list includes workspace and
monitor metadata. Hyprland's focus dispatcher handles switching workspaces.

If similarly ranked windows match, Nala returns candidate titles/apps/workspaces
and asks for a more specific description, without choosing the first window.
Private titles are masked in candidate output. Focus is reversible, low risk,
and obeys the existing confirmation mode. Both `apps.focus` and the existing
address-based `window.focus` use the new verified focus operation.

After dispatch, Nala checks the compositor's active-window address at 50ms
intervals, for at most ten attempts. A closed window, disabled permission,
replaced turn or failed verification returns an error. This verifies window
focus, not the caret inside a particular text field. Named visual typing still
uses its own focus and privacy checks. Window titles are untrusted metadata;
fuzzy matching cannot always infer which of several similar windows you mean.

On Lua-configured Hyprland 0.55+, legacy dispatch commands are rejected. Nala falls back to the [Lua dispatchers](https://wiki.hypr.land/configuring/core/dispatchers/) only after that explicit parse rejection, retaining older configurations. This applies to focus, close and cursor movement, so visual grounding can actually point on current Hyprland. Address inputs remain hex-validated; titles are never interpolated into Lua.
