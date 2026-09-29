#include "latency.h"

void LatencyTrace::reset() {
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
  case StageCount: break;
  }
  return {};
}

qint64 LatencyTrace::perceived() const {
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
  out += QStringLiteral("\nTotal perceived latency: %1 ms").arg(perceived());
  return out;
}

QJsonObject LatencyTrace::toJson() const {
  QJsonObject o;
  for (int s = 0; s < StageCount; ++s)
    if (has(Stage(s)))
      o.insert(stageName(Stage(s)).toLower().replace(' ', '_'), double(m_ms[s]));
  o.insert("perceived_ms", double(perceived()));
  return o;
}
