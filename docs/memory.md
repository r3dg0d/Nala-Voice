# Episodic, associative and durable memory

The timeline, screenshot deduplication, excluded apps, sensitive-window gate,
pause, pinned records and retention remain in place. Screen capture defaults
off. Explicit notes can still be saved without enabling background capture.

SQLite schema version 1 adds:

- `memory_embeddings`, `embedding_jobs`: vectors and retryable indexing work.
- `entities`, `entity_aliases`, `memory_entities`, `entity_relations`: small
  normalized associations; co-occurrence edges carry their source memory.
- `durable_facts`, `fact_sources`, `fact_relations`: current/history facts,
  evidence sources and acyclic dependencies.

The new tables, indexes, triggers, legacy note/pin backfill and schema version
are created in one transaction. Existing event IDs and screenshots are retained;
old events are queued for embedding and existing app/artifact links backfilled.
A newly created FTS table is rebuilt from existing rows. A failed migration
rolls back additions. A newer schema version is rejected. Back up `memory.db`
with SQLite's backup API before upgrades; do not copy an active WAL database.

Durable writes use explicit notes, pins, or `memory.remember` with a stable
`subject`, `category` and user-supplied `text`. Repeating the same fact merges
source links and confirmation time. Updating a subject supersedes its old fact
without erasing history. Dependent summaries become historical. Confidence and
importance accompany facts; these are heuristic values, not calibrated scores.
A substantially lower-confidence conflicting update is marked contradicted
and does not replace the stronger current fact. Repetition increases importance.
App/artifact associations and explicit Qwen/Gemma/Llama/Whisper/Fish Speech
model mentions are extracted deterministically; arbitrary people or private
locations are not inferred.
Explicit notes/pins are the deterministic importance gate, avoiding a large
LLM extraction call after each interaction.

`memory.durable.enabled` and `memory.entities.enabled` default true.
`memory.durable.autoExtract` defaults **false**. When opted in, a cheap gate
recognizes direct preference/decision/configuration language in user turns and
stores that turn as evidence. It does not infer personal facts from arbitrary
screen text. Use stable subjects in `memory.remember` for deliberate updates;
free-form preferences cannot reliably infer which older fact they contradict.

A deterministic intent gate routes questions about past events, considered
models and preferences into retrieval. Clock, app launch, media controls and
ordinary explanations avoid embeddings and query rewriting. Search results
carry `memory:`, `artifact:` and `fact:` evidence IDs. Memory answers are
buffered until those citations can be checked: unknown IDs or an uncited
answer cause an abstention. Normal speech removes the internal citation tokens;
developer mode retains them and logs retrieved IDs. This enforces evidence ID
validity, **not** logical entailment of every sentence; a model can still
misinterpret an opened source. Conflicting historical facts remain labeled.

Explicit forgetting cascades through embeddings, jobs, source/entity links,
relations, and facts with no remaining source. Shared facts survive only while
another retained memory supports them. In-flight embeddings check existence
and content hashes before committing, so they cannot resurrect forgotten data.
Unreferenced entities are pruned. Screenshot files are removed within Nala's
own private directory. Dropping only old screenshots retains textual evidence
and vectors, just as screenshot retention previously retained timeline rows.
No rerank cache or separate hidden memory database exists.

This adapts the complementary memory views, source routing and provenance
principles of [Agent Zero Memory](https://arxiv.org/abs/2608.29606) to a small
local SQLite store. It does not implement their document hierarchy or agentic
search loops, and makes no claim to their evaluation results.
