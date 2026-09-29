#pragma once
#include <QString>
#include <QStringList>

// Sentence-aware streaming for speech. Tokens arrive a few characters at a
// time; the voice should start on the first *sentence*, not the first token,
// and never on a fragment like "Sure" that would come out as a clipped syllable.
//
//   SentenceStream s;
//   for each delta:  for (sentence : s.feed(delta)) speak(sentence);
//   at the end:      if (rest = s.flush(); !rest.isEmpty()) speak(rest);
class SentenceStream {
public:
  struct Options {
    int firstMinChars = 14; // the first chunk: short, so speech starts early
    int minChars = 28;      // later chunks are joined until they are this long
    int maxChars = 220;     // a run-on is cut at a comma or space
  };
  SentenceStream() = default;
  explicit SentenceStream(Options options) : m_options(options) {}

  QStringList feed(const QString &delta);
  QString flush();
  void reset();
  // Everything handed out so far, so the caller can tell what was already spoken.
  int emittedChars() const { return m_emitted; }

  // What a voice should read: no markdown decoration, links as "link",
  // code fences dropped. Pure.
  static QString speakable(const QString &text);

private:
  QStringList drain(bool final);

  Options m_options;
  QString m_buffer;
  int m_emitted = 0;
  bool m_first = true;
};

// Removes <think>…</think> reasoning from a token stream, even when a tag is
// split across chunks. Reasoning must never reach the voice.
class ThinkFilter {
public:
  // Returns the visible part of `delta`; reasoning is swallowed.
  QString feed(const QString &delta);
  // Whatever was held back waiting to see whether it was a tag.
  QString flush();
  bool inThought() const { return m_inThought; }
  void reset();

private:
  QString m_pending;
  bool m_inThought = false;
};
