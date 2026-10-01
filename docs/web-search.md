# Key-free live web evidence

Enable **Web search (no API key)** under Computer use settings. Search is a
separate `agent.web` permission, off by default in public installations.
It is enabled on the workstation where this update was requested. It does not
require a paid API, account or search model download.

Examples:

- “What is the latest NixOS release?”
- “Search the web for Qt 6 release notes.”
- “Look up recent Qwen model announcements.”

With `web.autoSearch` on, explicit web requests and clearly current questions
retrieve public snippets before the local model answers. The completed search
is represented as tool evidence; duplicate searching is disabled for that
automatic request while exact source reads remain available. Ordinary questions,
clock/media/app commands, and personal memory questions keep their existing
paths. Queries mentioning “my”, “our”, screen, clipboard or memories are not
automatically sent out. Automatic search sends only the current user request,
never conversation summaries, screenshots or private memory records.

The default provider reads [DuckDuckGo Lite](https://duckduckgo.com/duckduckgo-help-pages/features/non-javascript).
It is a public HTML interface, not a guaranteed machine API: markup changes,
bot challenges, rate limits and outages can interrupt it. Nala reports failure
instead of silently answering a fresh question from stale model knowledge.
No CAPTCHA bypass, rotating proxy or paid scraper is used.

For a server you operate or trust, select SearXNG and configure
`web.searxng.endpoint` (default `http://127.0.0.1:8080/search`). Its
[search API](https://docs.searxng.org/dev/search_api.html) needs JSON enabled in
`search.formats`; many public instances disable JSON. Nala does not install or
pick a third-party instance automatically. Neither backend needs an API key.

`web.search` returns title, URL, snippet, retrieval timestamp and evidence ID.
`web.fetch` can read an exact public URL returned by search in the current turn,
with a 6000-character text limit. It strips scripts/styles, uses no browser
cookies, executes no JavaScript and refuses redirects, credentials, private
addresses and nonstandard ports. DNS addresses are checked before fetching;
this is not a transport-pinned defense against malicious DNS rebinding.
JavaScript-only pages, PDFs, login walls and redirects require a normal browser.
Fetch DNS and HTTP stages each have a deadline. Responses are capped at 512 KiB.

This is a small retrieval-augmented answer flow: search → optional source read →
untrusted evidence → local model → evidence-ID validation. It does not build a
permanent web vector index. Answers use `[web:N]` internally, with clickable
source buttons on the speech bubble. URLs are not read aloud. Unknown or missing
citations cause abstention. Citation validation proves the source was retrieved,
not that every claim is entailed or that a search snippet is up to date; the
model should fetch primary sources when snippets are insufficient.

A subsequent search after reading untrusted tool data requires confirmation,
even with `agent.confirm = high`, to reduce query-based exfiltration. Source
fetching cannot append private data or visit URLs invented by web content.
Search query text reaches the provider, and visited sources see a request from
your IP; key-free does not mean offline or anonymous. Recognizable secrets are
rejected. Existing tool logs may retain the query locally; debug/model history
can retain retrieved context, but web evidence is not written into durable
memory automatically.

| Setting | Default |
| --- | --- |
| `agent.web` | false |
| `web.autoSearch` | true, when web access is enabled |
| `web.provider` | duckduckgo; searxng optional |
| `web.duckduckgo.endpoint` | https://lite.duckduckgo.com/lite/ |
| `web.searxng.endpoint` | http://127.0.0.1:8080/search |
| `web.maxResults` | 5 (1–8) |
| `web.timeoutMs` | 10000 (1000–30000) per stage |
