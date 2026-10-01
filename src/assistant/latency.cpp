#include "latency.h"

void LatencyTrace::reset() {
  m_underruns = 0;
  for (qint64 &v : m_ms)
    v = -1;
}

void LatencyTrace::set(Stage stage, qint64 ms) {
  if (stage >= 0 && stage < StageCount)
    m_ms[stage] = std::max<qint64>(0, ms);
}

QString LatencyTrace::stageName(Stage stage) {
  switch (stage) {
  case Wake: return QStringLiteral("Wake word");
  case Vad: return QStringLiteral("VAD");
  case Stt: return QStringLiteral("STT");
  case Route: return QStringLiteral("Routing");
  case LlmFirstToken: return QStringLiteral("LLM first token");
  case LlmDone: return QStringLiteral("LLM full response");
  case TtsFirstAudio: return QStringLiteral("TTS first audio");
  case Action: return QStringLiteral("Command execution");
  case FirstCommit:
    return QStringLiteral("First text commit (turn elapsed)");
  case TtsSessionStart:
    return QStringLiteral("TTS session start (turn elapsed)");
  case FirstPcm:
    return QStringLiteral("First PCM submitted to output (turn elapsed)");
  case TtsDone:
    return QStringLiteral("Synthesis complete (turn elapsed)");
  case StageCount: break;
  }
  return {};
}

qint64 LatencyTrace::perceived() const {
  if (has(FirstPcm))
    return ms(FirstPcm);
  // Wake detection happens before the utterance; it is not part of the wait
  // after speaking. A command replaces the whole model-and-voice path.
  qint64 total = 0;
  for (Stage s : {Stt, Route, LlmFirstToken, TtsFirstAudio, Action})
    if (has(s))
      total += m_ms[s];
  return total;
}

QString LatencyTrace::report() const {
  QString out = QStringLiteral("Nala latency\n\n");
  for (int s = 0; s < StageCount; ++s)
    if (has(Stage(s)))
      out += QStringLiteral("%1: %2 ms\n").arg(stageName(Stage(s))).arg(m_ms[s]);
  out += QStringLiteral(
             "Output buffer underruns: %1\n\nTotal perceived latency: %2 ms")
             .arg(m_underruns)
             .arg(perceived());
  return out;
}

// Keys are short and never contain "token": the event log redacts any key
// that looks like a credential, and a timing is not one.
static const char *jsonKey(int stage) {
  static const char *keys[] = {"wake_ms",      "vad_ms",        "stt_ms",
                               "route_ms",     "llm_first_ms",  "llm_done_ms",
                               "tts_first_ms", "action_ms",     "commit_ms",
                               "tts_start_ms", "pcm_output_ms", "tts_done_ms"};
  return keys[stage];
}

QJsonObject LatencyTrace::toJson() const {
  QJsonObject o;
  for (int s = 0; s < StageCount; ++s)
    if (has(Stage(s)))
      o.insert(QLatin1String(jsonKey(s)), double(m_ms[s]));
  o.insert("perceived_ms", double(perceived()));
  o.insert("output_underruns", m_underruns);
  return o;
}
