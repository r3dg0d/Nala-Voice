#pragma once
#include <QElapsedTimer>
#include <QJsonObject>
#include <QString>

// Where the time went between "you stopped talking" and "she did something".
// Each stage records its own duration; the caller owns the clocks. Nothing here
// is shown unless developer.debug (or `nala latency`) asks for it.
class LatencyTrace {
public:
  enum Stage {
    Wake,          // wake-word detection (from the detector, not sequential)
    Vad,           // end-of-speech detection window
    Stt,           // whisper.cpp
    Route,         // command router / model router
    LlmFirstToken, // prefill + first token
    LlmDone,       // full response
    TtsFirstAudio, // first audio out of the voice
    Action,        // a deterministic command, end to end
    StageCount
  };

  void reset();
  void set(Stage stage, qint64 ms);
  bool has(Stage stage) const { return m_ms[stage] >= 0; }
  qint64 ms(Stage stage) const { return m_ms[stage]; }

  // What was felt: the sequential stages up to the first output.
  qint64 perceived() const;
  // Multi-line text in the format of docs/voice-pipeline.md.
  QString report() const;
  QJsonObject toJson() const;

  static QString stageName(Stage stage);

private:
  qint64 m_ms[StageCount] = {-1, -1, -1, -1, -1, -1, -1, -1};
};
