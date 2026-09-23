# Screen memory

Off by default. When on, Nala keeps an occasional picture of the window you
are using so you can later ask "what was I working on yesterday?" — and, more
usefully, remembers the files, pages and repositories involved.

It is not a recorder. Most frames are never kept.

## The pipeline

```
every memory.intervalSec (default 30 s)
  paused or disabled?            → nothing is captured at all
  active window (Hyprland IPC)
  privacy gate                   → excluded / sensitive: nothing is captured
  grim → JPEG in memory          (never written to disk yet)
  same window and title still focused?   → otherwise discarded
  downscale to memory.maxWidth, perceptual hash (off the UI thread)
  paused meanwhile?              → discarded
  same window, same title, hash within memory.dedupeDistance of the last kept
                                 → folded into that memory (frames += 1)
  [privacy.visionFilter] ask the model SAFE/SENSITIVE → anything but SAFE: discarded
  write shots/YYYY-MM-DD/<ms>-<hash>.jpg (0600) and a row in memory.db
  extract URLs, file paths, GitHub repositories from the title → artifacts
  [memory.describe] ask the model for activity, summary, keywords, URLs
```

Storage lives in `~/.local/share/nala/memory/` (0700): `memory.db`
(SQLite, WAL, full-text search with FTS5 when SQLite has it) and `shots/`.

## What a memory holds

Time started and last seen, how many near-identical frames it absorbed, app,
window title, activity, summary, keywords, URLs, screenshot path and size,
perceptual hash, pinned flag, and source (`screen`, `note`, or `agent`).
Artifacts (`url`, `file`, `repo`) are kept in their own table with first and
last sighting, so "where is that resume?" can be answered with a path long
after the screenshot has expired. Files the agent writes and pages it opens
are recorded as artifacts too.

## Retention

Applied every 30 minutes and after every 20 new memories:

1. Screenshots older than `memory.screenshotDays` (default 7) are deleted;
   their rows — the description — stay.
2. Rows older than `memory.semanticDays` (default forever) are deleted.
3. While the total is over `memory.maxStorageMB` (default 5 GB), the oldest
   unpinned screenshots are deleted, only as many as needed.

Pinned memories are never removed by retention. Explicit forgetting
("forget the last hour", Delete in the timeline) removes pinned ones too —
that is what was asked for. Deletion only ever touches files inside
`shots/`, whatever a row says.

## Searching

`memory.search` (the model's tool) and the timeline search combine:

- a time phrase — today, yesterday, this morning, last week, on Monday,
  3 days ago, in the last 2 hours… — turned into a range;
- the remaining content words, matched with FTS5 (each word quoted and
  prefix-matched, so search text cannot inject query syntax) or LIKE;
- artifacts matching the same words and range.

Embedding (vector) search is planned, not implemented.

## The timeline

`nala timeline`, the tray's *Memories…*, or "show me your memories". Memories
are grouped by day and into episodes (same app, gaps under ten minutes).
Selecting one shows its screenshot (if still kept), time, app, title,
summary, artifacts and what happened around the same time, with Pin and
Delete. Filters: text, app, and today / 7 days / 30 days / everything.

## Voice controls

| Say | Does |
| --- | --- |
| "pause screen memory", "turn off screen recording", "stop remembering my screen", "privacy mode" | pause until resumed |
| "pause memory for one hour" | timed pause; resumes by itself when the time is up |
| "resume screen memory", "turn screen recording back on" | resume |
| "forget the last five minutes / hour", "forget today" | delete that span |
| "forget everything" | asks first, then deletes everything |
| "delete screenshots older than seven days" | drop images, keep descriptions |
| "keep this memory", "remember this permanently" | pin the latest |
| "never record this app" | add the focused app to the exclusions |
| "how much storage are your memories using?" | status |
