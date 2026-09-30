# Local hybrid retrieval

FTS5/BM25 stays in place, with the existing LIKE fallback. `Retrieval` adds a
configurable embedding client, background indexing and optional reranking.
SQLite is the only database; no vector extension or new ML runtime is needed.

Set these keys in the existing assistant settings file while Nala is stopped
(or through `AssistantSettings`):

```json
{
  "memory.embedding.enabled": true,
  "memory.embedding.provider": "ollama",
  "memory.embedding.endpoint": "http://127.0.0.1:11434",
  "memory.embedding.model": "qwen3-embedding:0.6b"
}
```

Install that small configurable local model with
`ollama pull qwen3-embedding:0.6b`. See the
[model publisher](https://github.com/QwenLM/Qwen3-Embedding) and
[Ollama API example](https://ollama.com/library/qwen3-embedding:0.6b).
This does not enable screen recording. Embeddings default off until configured;
semantic search defaults on but falls back to local lexical retrieval.

The `ollama` provider posts to `/api/embed`. The `openai` provider posts to
`<endpoint>/embeddings` (normally endpoint ends in `/v1`). No vendor is
required. Local loopback endpoints are defaults; a remote endpoint is an
explicit user configuration and receives memory text. Redirects are refused.
The endpoint, provider and model identify a vector space. Dimensions are
validated but may remain 0 for server-selected size. The stored vector format
is version 1, little-endian float32; non-finite, zero and malformed vectors are
rejected. Screenshot bytes are never embedded.

A database trigger queues inserted/edited records immediately. The background
worker indexes one record every five seconds, independently of conversation
calls, and stops new writes while memory is paused. Failed jobs retain the
memory and retry with bounded exponential backoff. A content hash prevents an
old response from indexing edited or forgotten text. Model/endpoint/dimension
changes requeue incompatible records. Artifact changes invalidate the index. Identical text can reuse a vector in the
same space without merging or deleting episodic records. This is exact text
deduplication, not a near-duplicate classifier.

On a memory request:

1. Optionally rewrite a contextual question using the configured fast model;
   no context or a failed rewrite uses the original query.
2. Apply the original time constraint to candidates. The existing time parser
   handles yesterday, earlier today, last week and other supported phrases.
3. Collect BM25 and cosine ranks, artifact matches, entity matches and active
   durable-fact sources.
4. Fuse ranks as `sum(1 / (k + rank))`, with ranks starting at 1 and duplicates
   counted once per channel. BM25/cosine scores are not normalized together.
5. Optionally call a local `/rerank` backend on the smaller fused candidate
   set. It accepts model/query/documents/top_n and returns complete indexed
   `results` with `relevance_score`. Missing, partial or invalid responses
   retain the RRF order. No reranking server/model is automatically installed.

| Setting | Default |
| --- | --- |
| `memory.semanticSearch`, `memory.queryRewrite` | true |
| `memory.embedding.enabled` | false |
| `memory.embedding.provider` | ollama |
| `memory.embedding.endpoint` | http://127.0.0.1:11434 |
| `memory.embedding.model` | qwen3-embedding:0.6b |
| `memory.embedding.dimensions` | 0 |
| `memory.backendTimeoutMs` | 3000, total deadline per backend call |
| `memory.hybrid.lexicalK`, `denseK` | 30 |
| `memory.hybrid.rrfK` | 60 |
| `memory.hybrid.finalK` | 10 |
| `memory.rerank.enabled` | false |
| `memory.rerank.endpoint` | http://127.0.0.1:8081/v1 |
| `memory.rerank.model`, `topK` | bge-reranker-v2-m3, 30 |

```bash
nala memory search "Rust search engine yesterday"
nala memory search --debug "Rust search engine yesterday"
```

The CLI emits JSON evidence. Debug adds channel ranks, cosine/RRF scores and
backend status; normal results retain IDs, origin, timestamps, artifacts,
facts and entity links. CLI searches have no conversation context, so rewriting
is skipped. Retrieval currently scores vectors in-process on the GUI thread;
this suits a personal corpus, not millions of records. Backend timeouts and FTS
fallback remain usable without an embedding server. Large corpora need further
profiling and background scoring, rather than a heavyweight database by default.

The pipeline adapts rewrite/hybrid/RRF/rerank ideas from
[Caraman at SemEval-2026 Task 8](https://arxiv.org/abs/2605.12028).
There is no reproduction claim or learned query-rewrite adapter.
