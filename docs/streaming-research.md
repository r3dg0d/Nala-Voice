# Research used for the 1.5 streaming/controller upgrade

The implementation adapts mechanisms to Nala's existing Qt, policy and privacy
boundaries. It does not reproduce complete research systems or reuse reported
paper scores as local results.

| Primary source | Mechanism applied |
| --- | --- |
| [X2Streaming-TTS paper](https://arxiv.org/html/2608.18661v1), [official implementation](https://github.com/X-Square-Robot/X2Streaming-TTS) | Deterministic causal commitment, adaptive segment capacity, Code2Wav state and causal Talker hidden-tail inheritance through verified upstream hooks. Separate local inference service. |
| [See, Point, Refine](https://arxiv.org/html/2604.13019v1) | Reobserve, point, crop and refine before consequential input. |
| [GUI-Cursor](https://arxiv.org/html/2509.21552v1) | Marker-relative corrections with explicit coordinate frames. This source is from 2025. |
| [GUI-Eyes](https://arxiv.org/html/2601.09770v1) | Active perception and higher-resolution local observations. |
| [DesktopDelta](https://arxiv.org/abs/2607.26041) | Action-induced differences to localize verification evidence; examined abstract, full HTML unavailable. |
| [Evidence-first reflection](https://arxiv.org/html/2608.24015v1) | Separate intention-free change description from success judgment. |
| [The Art of Building Verifiers for Computer Use Agents](https://arxiv.org/html/2604.06240v1) | Explicit expected effects and independent outcome checks. |
| [Executable Agentic Memory](https://arxiv.org/html/2605.12294v1) | Optional semantic procedures with per-step grounding, validation and permissions. |
| [Agent S3 source](https://github.com/simular-ai/Agent-S/blob/main/gui_agents/s3/agents/agent_s.py) | Separate planner/operator/observer responsibilities without replacing Nala's orchestrator. |
| [UI-TARS Desktop](https://github.com/bytedance/UI-TARS-desktop) | Observation/action iteration, cancellation and bounded recovery. |
| [AT-SPI interface definitions](https://github.com/GNOME/at-spi2-core/tree/main/xml) | Exact semantic labels, states and screen extents before pixel grounding. |

Both the standard English Qwen CustomVoice checkpoint and the streaming-trained
X2 checkpoint require local quality checks:
transport-level completion is not proof that all text was spoken. Very small
prefixes and engine sampling precision can affect omissions and continuity.
Nala exposes benchmark audio artifacts so its own synthetic fixtures can be
transcribed or listened to independently of latency measurements.
