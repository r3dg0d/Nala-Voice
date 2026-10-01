#pragma once
#include "sentencestream.h"
#include "tts.h"
#include <QElapsedTimer>
#include <QTimer>
// Silent backend benchmark. Output-device underruns are deliberately
// unavailable here; normal speech measures them on Speaker. No microphone or
// screen access.
class TtsBenchmark : public QObject {
public:
  TtsBenchmark(QVector<TextToSpeech *> engines,
               std::function<void(QString)> progress,
               std::function<void()> done, QObject *parent);
  void start();
  void cancel();
  void configureCommitment(SentenceStream::Options options, int maxDelayMs) {
    m_commitment.configure(options);
    m_maxDelayMs = maxDelayMs;
  }

private:
  void runNext();
  void finishCase(QString error = {});
  TtsChain m_chain;
  QTimer m_input, m_timeout;
  QElapsedTimer m_clock;
  SentenceStream m_commitment{SentenceStream::Options{24, 24, 220, true}};
  std::function<void(QString)> m_progress;
  std::function<void()> m_done;
  QStringList m_names, m_texts;
  int m_case = -1, m_position = 0;
  qint64 m_first = -1, m_bytes = 0;
  qint64 m_firstCommit = -1, m_lastCommit = 0;
  int m_rate = 24000, m_frameBytes = 2;
  bool m_running = false;
  int m_maxDelayMs = 350, m_segments = 0, m_carried = 0;
};
