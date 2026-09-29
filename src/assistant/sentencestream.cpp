#include "sentencestream.h"

#include <QRegularExpression>
#include <algorithm>

namespace {

// Words whose trailing full stop does not end a sentence.
bool abbreviation(const QString &before) {
  static const QStringList known = {"mr",  "mrs", "ms",  "dr",  "prof", "sr",
                                    "jr",  "st",  "vs",  "etc", "e.g", "i.e",
                                    "approx", "no", "fig", "inc", "ltd"};
  int start = before.size();
  while (start > 0 && (before.at(start - 1).isLetter() || before.at(start - 1) == '.'))
    --start;
  const QString word = before.mid(start).toLower();
  return known.contains(word);
}

// Index just past the first sentence boundary in `text` that is certain, or -1.
// A terminator counts only once whitespace follows it (so "3.5" and "v1.2" are
// safe) unless the stream is over.
int boundary(const QString &text, bool final) {
  for (int i = 0; i < text.size(); ++i) {
    const QChar c = text.at(i);
    if (c == '\n') {
      if (i > 0) return i + 1;
      continue;
    }
    if (c != '.' && c != '!' && c != '?' && c != QChar(0x2026))
      continue;
    int end = i;
    while (end + 1 < text.size() &&
           (text.at(end + 1) == '.' || text.at(end + 1) == '!' ||
            text.at(end + 1) == '?' || text.at(end + 1) == '"' ||
            text.at(end + 1) == '\'' || text.at(end + 1) == ')'))
      ++end;
    if (end + 1 >= text.size()) {
      if (final) return text.size();
      return -1; // need the next character to be sure
    }
    if (!text.at(end + 1).isSpace())
      continue;
    if (c == '.' && abbreviation(text.left(i)))
      continue;
    return end + 1;
  }
  return -1;
}

} // namespace

QString SentenceStream::speakable(const QString &input) {
  QString text = input;
  // Fenced code is not read aloud.
  static const QRegularExpression fence(QStringLiteral("```[\\s\\S]*?(```|$)"));
  text.replace(fence, QStringLiteral(" (code omitted) "));
  static const QRegularExpression inlineCode(QStringLiteral("`([^`]*)`"));
  text.replace(inlineCode, QStringLiteral("\\1"));
  static const QRegularExpression link(QStringLiteral("\\[([^\\]]*)\\]\\([^)]*\\)"));
  text.replace(link, QStringLiteral("\\1"));
  static const QRegularExpression url(QStringLiteral("https?://\\S+"));
  text.replace(url, QStringLiteral("a link"));
  static const QRegularExpression heading(QStringLiteral("(?m)^\\s{0,3}#{1,6}\\s*"));
  text.remove(heading);
  static const QRegularExpression bullet(QStringLiteral("(?m)^\\s*[-*+]\\s+"));
  text.remove(bullet);
  static const QRegularExpression emphasis(QStringLiteral("[*_]{1,3}"));
  text.remove(emphasis);
  return text.simplified();
}

QStringList SentenceStream::feed(const QString &delta) {
  m_buffer += delta;
  return drain(false);
}

QString SentenceStream::flush() {
  const QStringList rest = drain(true);
  QString tail = rest.join(' ');
  const QString left = speakable(m_buffer);
  m_buffer.clear();
  if (!left.isEmpty()) {
    tail = tail.isEmpty() ? left : tail + ' ' + left;
  }
  m_emitted += int(tail.size());
  return tail;
}

void SentenceStream::reset() {
  m_buffer.clear();
  m_emitted = 0;
  m_first = true;
}

QStringList SentenceStream::drain(bool final) {
  QStringList out;
  QString pending; // short sentences held to be joined with the next
  for (;;) {
    // A fence still open must not be cut in the middle.
    const int fences = m_buffer.count(QLatin1String("```"));
    if (fences % 2 == 1 && !final)
      break;
    int cut = boundary(m_buffer, final);
    if (cut < 0 && m_buffer.size() > m_options.maxChars) {
      cut = m_buffer.lastIndexOf(',', m_options.maxChars);
      if (cut < m_options.maxChars / 3)
        cut = m_buffer.lastIndexOf(' ', m_options.maxChars);
      if (cut < m_options.maxChars / 3)
        cut = m_options.maxChars;
      ++cut;
    }
    if (cut < 0)
      break;
    const QString sentence = speakable(m_buffer.left(cut));
    m_buffer.remove(0, cut);
    if (sentence.isEmpty())
      continue;
    pending = pending.isEmpty() ? sentence : pending + ' ' + sentence;
    const int want = m_first ? m_options.firstMinChars : m_options.minChars;
    if (pending.size() >= want) {
      out << pending;
      m_emitted += int(pending.size());
      pending.clear();
      m_first = false;
    }
  }
  // Too short to speak alone: put it back in front, so it is joined with what
  // follows (or flushed at the end).
  if (!pending.isEmpty()) {
    if (final) {
      out << pending;
      m_emitted += int(pending.size());
    } else {
      m_buffer.prepend(pending + ' ');
    }
  }
  return out;
}

// --- ThinkFilter ---------------------------------------------------------------

QString ThinkFilter::feed(const QString &delta) {
  static const QString open = QStringLiteral("<think>");
  static const QString close = QStringLiteral("</think>");
  m_pending += delta;
  QString visible;
  for (;;) {
    const QString &tag = m_inThought ? close : open;
    const int at = m_pending.indexOf(tag, 0, Qt::CaseInsensitive);
    if (at >= 0) {
      if (!m_inThought)
        visible += m_pending.left(at);
      m_pending.remove(0, at + tag.size());
      m_inThought = !m_inThought;
      continue;
    }
    // No whole tag: everything but a possible tag prefix at the end is safe.
    int keep = 0;
    for (int n = std::min<int>(tag.size() - 1, m_pending.size()); n > 0; --n)
      if (tag.startsWith(m_pending.right(n), Qt::CaseInsensitive)) {
        keep = n;
        break;
      }
    const int safe = m_pending.size() - keep;
    if (!m_inThought)
      visible += m_pending.left(safe);
    m_pending.remove(0, safe);
    break;
  }
  return visible;
}

QString ThinkFilter::flush() {
  const QString rest = m_inThought ? QString() : m_pending;
  m_pending.clear();
  return rest;
}

void ThinkFilter::reset() {
  m_pending.clear();
  m_inThought = false;
}
