# Project-local benchmarks

Run from the repository's Nix development shell, after building:

```bash
QT_QPA_PLATFORM=offscreen ./build/nala-agent-bench
QT_QPA_PLATFORM=offscreen ./build/nala-agent-bench --live
QT_QPA_PLATFORM=offscreen ./build/nala-agent-bench --vision qwen3-vl:8b-instruct-q4_K_M
```

The default run uses controlled one-hot embeddings and scripted GUI predictions;
it checks mechanics rather than model quality. `--live` uses local Ollama
qwen3-embedding:0.6b on 20 synthetic records and eight labeled queries.
App/entity/fact extraction is disabled for this corpus to isolate lexical and
dense retrieval; separate unit tests cover those channels. Relevance labels
are manually assigned topic groups and this tiny dataset is not representative
of all user memories.

Measured locally on an RTX 4090 on 2026-09-30:

| Retrieval mode | Recall@5 | MRR | nDCG@5 | Mean CPU retrieval ms |
| --- | ---: | ---: | ---: | ---: |
| FTS | 0.583 | 1.000 | 0.675 | 0.401 |
| dense | 0.833 | 0.917 | 0.814 | 2.140 |
| hybrid | 0.833 | 1.000 | 0.852 | 1.665 |
| hybrid_rerank_unavailable_fallback | 0.833 | 1.000 | 0.852 | 1.977 |

Indexing took 1729 ms for 20 documents; query embedding took
5–13 ms per query. CPU timings exclude embedding/network
and optional rewrite latency. No reranker was installed: the fourth row
reports the unchanged hybrid fallback, not a measured reranker benefit.
Offline fake-server tests exercise valid score ordering and unavailable/invalid
reranker responses. Raw results: [embedding](results/local-qwen-embedding.json).

`--vision` draws three 800×500 synthetic Export dialogs, runs the actual local
Qwen3-VL 8B Q4_K_M model, and simulates pointer movement/clicks on those images.
It never clicks the live desktop. All three targets were hit, using 2–3 model
requests and 1–2 refinements in 1052–1736 ms. Initial center errors were
4.47, 2.00 and 1.41 px; final errors were 3.16, 1.00 and 3.16 px. Refinement
worsened the third center error despite a successful button hit. These results
do not establish reliability on dense real applications or parity with hosted
computer-use agents. A normalized 0–1000 coordinate protocol is essential for
this model; treating those outputs as pixels caused failed earlier trials.
Raw predictions and outcomes: [vision](results/local-qwen-vision.json).

The GUI section of the default/retrieval run instead uses six scripted
fixtures: 50 px initial error to zero after two refinements. It verifies the
loop and transforms, not the VLM. Benchmark output contains no user screenshots
or private memories. Timing varies with warm-up, resident models and GPU load.
