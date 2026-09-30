# Nala 1.4.0 engineering report

## Repository baseline and scope

Started on clean `main` at `25f081870d40dfa2f53e6ba3f9aaf060bf37b4ef`.
Inspected the existing architecture, tools, policy, memory, models, desktop,
settings, tests, packaging and recent Git history. Changes extend the C++20 / Qt 6
assistant and retain its voice/UI, deterministic command paths, SQLite timeline,
privacy gate, typed tools, permission categories and Nix/Wayland helpers.
No new library dependency, external database, AT-SPI service, cloud model or
research training pipeline was added. Version metadata agrees across CMake,
flake and PKGBUILD; changelog updated.

## Research ideas adopted

- [See, Point, Refine](https://arxiv.org/abs/2604.13019): bounded multi-turn grounding with numbered cursor feedback.
- [Learning GUI Grounding](https://arxiv.org/abs/2509.21552): spatial corrections from marked observations.
- [GUI-Eyes](https://arxiv.org/abs/2601.09770): coarse full-frame perception followed by cropped detail.
- [Screenshots or Tools?](https://arxiv.org/abs/2608.03327): existing structured tools complement visual interaction.
- [Caraman at SemEval-2026](https://arxiv.org/abs/2605.12028): contextual rewrite, separate lexical/dense ranks, RRF and optional reranking.
- [Agent Zero Memory](https://arxiv.org/abs/2608.29606): complementary episodic/associative/durable views with evidence provenance and history.

These are inference-time adaptations, not reproductions of training procedures
or paper benchmark results.

## Computer-use architecture

Named single clicks take the fast command route directly into the high-risk
`computer.locate_and_click` tool. Existing app launch/window/media commands
continue using deterministic or structured paths. Complex requests can use the
typed tools through the conversational model. `computer.locate_and_type` adds
verified field focus and non-secret text entry. Existing coordinate primitives
remain available for debugging.

`GuiGrounder` injects capture/predict/point/click/retry ports, enabling offline
fixtures without a live desktop. It downsizes a coarse frame to at most 1280px,
points without clicking, crops around the estimate, draws numbered landmarks,
and refines until explicit readiness, confidence >=0.90 and movement <=4
source-image pixels. Normalized model coordinates are converted before image,
crop, scaling and logical-desktop transforms. Negative monitor origins work.
Qwen-VL emitted normalized coordinates even under the earlier pixel prompt;
using an explicit normalized protocol fixed that observed failure.

The loop holds window identity, monitor geometry and frame size; checks fresh
privacy/focus/layout before clicking; and has four refinements, two separately
confirmed retries and a 60-second deadline by default. It compares whole-frame
and local-region visual change after action; an optional expected state also
requires model confirmation. Every named action goes through the existing
high-risk confirmation policy. Turning off access while awaiting confirmation
prevents the action. Typing rechecks privacy/focus and never blindly retypes an
unverified result. Screen bytes remain in memory.

## Semantic retrieval

`Retrieval` asynchronously calls configurable Ollama `/api/embed` or compatible
`/embeddings`, rejects redirects/malformed/zero/non-finite vectors, and bounds
HTTP response size and total call time. Insert/update triggers queue jobs; one
record is indexed every five seconds while enabled and unpaused. Errors retain
records and retry with capped exponential backoff. Content hashes and live IDs
reject stale replies. Identical source text can reuse a same-space vector while
preserving separate events. Stored provider/endpoint/model, dimensions, version
and source hash prevent mixing incompatible embeddings.

Memory questions alone pass through the intent gate and optional fast contextual
rewrite. Time limits are applied before candidate ranking. BM25, dense cosine,
artifact matches, entity matches and active fact sources contribute separate
ranks; RRF sums `1/(k+rank)` and deduplicates each channel. Optional `/rerank`
requires complete valid indexed scores, sorts descending, and falls back to
RRF if unavailable or invalid. Results carry source, timestamp, memory/fact/
artifact IDs and debug channel scores. CLI `nala memory search --debug <query>`
works without a running companion. Simple commands and normal explanations do
not pay semantic-search latency.

## Memory and migration

A transactional schema-version-1 migration adds `memory_embeddings`,
`embedding_jobs`, `entities`, `entity_aliases`, `memory_entities`,
`entity_relations`, `durable_facts`, `fact_sources` and `fact_relations`, with
foreign keys, indexes and invalidation/forget triggers. Existing IDs/events are
retained, legacy notes/pins become evidence-backed facts, app/artifact associations
are backfilled and missing FTS indexes are rebuilt. Failed migrations roll back;
newer database versions are refused before mutations. The installed database
was backed up through SQLite's backup API before migration and passed integrity
checks afterward. It had zero timeline rows, so legacy nonempty migration is
verified by offline fixtures rather than by this live database.

Explicit notes/pins and `memory.remember` create durable facts with stable subject,
category, importance, confidence and source IDs. Repeated facts merge sources;
updates preserve superseded history. Substantially weaker conflicts do not replace
stronger current facts. Direct dependent facts become historical; dependency
cycles are rejected. App/artifact links and explicit model mentions provide a
small associative view. Optional heuristic user-turn extraction defaults off.
Forgetting removes vectors/jobs/source links, orphan facts/entities and owned
screenshots, while facts supported by another retained source survive.

Memory answers are buffered until citations can be checked against retrieved
IDs. Unknown IDs or uncited memory answers abstain; normal speech strips citation
tokens while developer output retains them. This validates provenance IDs,
not sentence-level entailment.

## Local models and installed configuration

- [Gemma4 E2B](https://ollama.com/library/gemma4:e2b): 4.6GB Q4 model for fast chat/tool selection, thinking off.
- [Qwen3-VL 8B Instruct](https://huggingface.co/Qwen/Qwen3-VL-8B-Instruct): local 6.1GB Q4_K_M model for grounding.
- [Qwen3-Embedding 0.6B](https://github.com/QwenLM/Qwen3-Embedding): local 639MB embedding model.

Models run through existing Ollama on the workstation's RTX 4090. Qwen-VL and
embedding downloads completed. Dedicated vision selection leaves the fast chat
model small. Local embedding indexing is enabled on this workstation; public
settings keep embeddings opt-in. Screen recording and automatic durable
extraction remain disabled. Existing permissions and voice configuration remain.
NixOS's `/run/ydotoold/socket` is supplied to ydotool when the environment does
not specify one. The installed doctor reports Hyprland, screenshot/typing/click
helpers and the native local model server available; a zero-displacement ydotool
probe succeeded. Live arbitrary-window clicks have not been certified.

## Settings

GUI: maxRefinements=4, maxRetries=2, tolerancePixels=4, coordinateSpace=
normalized_1000 (pixels optional), cropZoom=true, verifyActions=true.

Memory: semanticSearch=true, queryRewrite=true, durable.enabled=true,
durable.autoExtract=false, entities.enabled=true. Embedding defaults disabled,
provider=ollama, endpoint=http://127.0.0.1:11434, model=qwen3-embedding:0.6b,
dimensions=0. Backend timeout=3000ms. Hybrid lexicalK/denseK=30, rrfK=60,
finalK=10. Reranker defaults disabled, endpoint=http://127.0.0.1:8081/v1,
model=bge-reranker-v2-m3, topK=30. Existing settings validation is used.
See [computer use](computer-use.md), [search](semantic-search.md), and
[memory](memory.md) for behavior and limits.

## Validation and benchmarks

All five CTest targets passed: self-test, install smoke, assistant tests, model
tests and new agent tests (34.07 seconds total). The complete Nix package built
successfully and independently ran 105 assistant checks (3 environment skips),
154 model checks, 46 agent checks, and the application self-test. No additional
sanitizer configuration was present. Diff whitespace checks and Gitleaks scans
of the current tree and staged changes passed.

New offline tests cover transforms/crops, convergence/readiness/limits, markers,
image differences, action retries/refusal/cancellation/focus races, normalized
coordinates, high-risk tool schemas, vectors/cosine/RRF/ranks/time/artifacts,
model/hash invalidation, HTTP providers/fallbacks/dimensions, reranker sorting,
rewrite fallback, intent/citation gates, migrations/rollback/newer schemas,
fact conflicts/history/dependencies/aliases/shared provenance/forget cascades,
and a real CLI debug-search process under isolated XDG directories.

The local 20-record/eight-query benchmark with actual Qwen embeddings measured:
FTS Recall@5 0.583, MRR 1.000, nDCG@5 0.675; dense 0.833/0.917/0.814;
hybrid 0.833/1.000/0.852. Query embedding took 5–13ms; indexing took 1729ms.
CPU retrieval was about 0.40ms FTS, 2.14ms dense, 1.67ms hybrid on this run.
No reranker was installed, so its comparison is explicitly an unchanged fallback.

Actual Qwen-VL predictions hit 3/3 synthetic 120×44px Export buttons, with simulated
pointer/clicks, 1–2 refinements and 1052–1736ms total. Initial errors were
4.47/2.00/1.41px; final errors 3.16/1.00/3.16px. One case's refinement worsened
center error. Six additional scripted fixtures check loop mechanics. These tiny
project-local fixtures do not establish general computer-use reliability.
Raw outputs and rerun instructions are in [benchmarks](../benchmarks/README.md).

## Known limitations

- No AT-SPI bus was available on the workstation; no accessibility/DOM integration
  was added. Existing structured desktop/app/browser tools remain first choices.
- Confidence is self-reported. A ready point can still be wrong; animation/carets
  can imitate successful visual change. Expected-state checks improve evidence
  but are not a business-operation guarantee. Keyboard focus can race a helper.
- Whole-monitor privacy checks are conservative and may block overlapping private
  or other-workspace windows. Arbitrary target descriptions cannot ensure intent.
- Dense scanning and metadata queries currently run in-process on the GUI thread;
  a large corpus requires profiling and moving CPU ranking off that thread.
- Exact vector reuse does not implement near-duplicate semantic consolidation.
  Automatic durable extraction is heuristic and opt-in. Free-form facts need
  stable subjects for reliable conflict updates; invalidation is direct dependency
  only, not a recursive regeneration engine. Entity extraction is deliberately small.
- Evidence-ID checks do not prove factual entailment. Historical conflicts require
  model care. Optional rewriting/reranking may add bounded network latency.
- Qwen3-TTS and Fish Speech services were unavailable on this workstation; the
  existing voice fallback behavior remains, and these services were not reconfigured.
- Benchmarks use synthetic content and simulated GUI actions, not research datasets
  or private live application clicks. No parity claim with hosted agents is made.

## Files

Added:

- `benchmarks/README.md`
- `benchmarks/agent_bench.cpp`
- `benchmarks/results/local-qwen-embedding.json`
- `benchmarks/results/local-qwen-vision.json`
- `docs/computer-use.md`
- `docs/memory.md`
- `docs/semantic-search.md`
- `src/assistant/assistant_gui.cpp`
- `src/assistant/guigrounder.cpp`
- `src/assistant/guigrounder.h`
- `src/assistant/memory_semantic.cpp`
- `src/assistant/retrieval.cpp`
- `src/assistant/retrieval.h`
- `src/assistant/semantic.cpp`
- `src/assistant/semantic.h`
- `tests/agent_tests.cpp`

Modified:

- `CHANGELOG.md`
- `CMakeLists.txt`
- `PKGBUILD`
- `README.md`
- `docs/ARCHITECTURE.md`
- `docs/models.md`
- `docs/tools.md`
- `flake.nix`
- `src/assistant/assistant.cpp`
- `src/assistant/assistant.h`
- `src/assistant/assistant_models.cpp`
- `src/assistant/commandrouter.cpp`
- `src/assistant/desktop.cpp`
- `src/assistant/identity.cpp`
- `src/assistant/llm.cpp`
- `src/assistant/memory.cpp`
- `src/assistant/memory.h`
- `src/assistant/settings.cpp`
- `src/main.cpp`

## Git and delivery

Implementation, tests, benchmarks and documentation commit:
`fb120c85801ec99c282afe251f254d6a9f275674` on `main`. A subsequent documentation commit records this report.
No force push, release or tag is used. Final push/remote/CI status is recorded
in the delivery message after verification; it is not asserted in this file
before the push happens. The user installation is Nala 1.4.0, pinned by a Nix
GC root with a user launcher and desktop entry; the original system package
remains available as a rollback path.
