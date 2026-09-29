#include "contextbudget.h"

#include <algorithm>

namespace ctx {

int estimateTokens(const QString &text) {
  return int((text.size() + 3) / 4);
}

int estimateTokens(const QJsonObject &message) {
  int chars = 8; // role and framing
  const QJsonValue content = message.value("content");
  if (content.isString()) {
    chars += content.toString().size();
  } else if (content.isArray()) {
    for (const QJsonValue &part : content.toArray()) {
      const QJsonObject o = part.toObject();
      if (o.value("type").toString() == "image_url")
        chars += 4 * 700; // an image costs about as much as a page
      else
        chars += o.value("text").toString().size();
    }
  }
  return (chars + 3) / 4;
}

int estimateTokens(const QJsonArray &messages) {
  int total = 0;
  for (const QJsonValue &m : messages)
    total += estimateTokens(m.toObject());
  return total;
}

QString clip(const QString &text, int maxChars) {
  if (maxChars <= 0 || text.size() <= maxChars)
    return text;
  const int marker = 60;
  const int head = std::max(0, (maxChars - marker) * 2 / 3);
  const int tail = std::max(0, maxChars - marker - head);
  return text.left(head) +
         QStringLiteral("\n[… %1 characters omitted …]\n").arg(text.size() - head - tail) +
         text.right(tail);
}

Fit fit(const QJsonObject &system, const QString &summary,
        const QJsonArray &history, int maxRecentTurns, int maxTokens,
        int reserveForReply) {
  Fit out;
  const int budget = std::max(256, maxTokens - std::max(0, reserveForReply));
  int fixed = estimateTokens(system);
  if (!summary.isEmpty())
    fixed += estimateTokens(summary) + 12;

  // Recent window first: N exchanges is 2N messages.
  qsizetype first = std::max<qsizetype>(0, history.size() - maxRecentTurns * 2);
  // Never start on an assistant message; a reply without its question misleads.
  while (first < history.size() &&
         history.at(first).toObject().value("role").toString() == "assistant")
    ++first;
  for (qsizetype i = 0; i < first; ++i)
    out.dropped.append(history.at(i));

  // Then the token budget: shed the oldest pair until it fits (always keep the
  // newest exchange, however large -- the request itself must be answerable).
  int used = fixed;
  for (qsizetype i = first; i < history.size(); ++i)
    used += estimateTokens(history.at(i).toObject());
  while (used > budget && history.size() - first > 2) {
    used -= estimateTokens(history.at(first).toObject());
    out.dropped.append(history.at(first));
    ++first;
    if (first < history.size() &&
        history.at(first).toObject().value("role").toString() == "assistant") {
      used -= estimateTokens(history.at(first).toObject());
      out.dropped.append(history.at(first));
      ++first;
    }
  }
  for (qsizetype i = first; i < history.size(); ++i)
    out.history.append(history.at(i));
  out.tokens = used;
  return out;
}

QJsonArray summaryRequest(const QString &previousSummary,
                          const QJsonArray &dropped) {
  QString transcript;
  for (const QJsonValue &v : dropped) {
    const QJsonObject m = v.toObject();
    const QString role = m.value("role").toString();
    if (role != "user" && role != "assistant")
      continue;
    const QString text = clip(m.value("content").toString(), 600);
    transcript += (role == "user" ? QStringLiteral("User: ") : QStringLiteral("Assistant: ")) +
                  text + '\n';
  }
  const QString instruction = QStringLiteral(
      "You maintain a short running summary of a voice conversation between a "
      "user and their desktop assistant. Merge the new exchanges into the "
      "summary. Keep facts, decisions, names, preferences and unfinished tasks; "
      "drop pleasantries and tool noise. Reply with the summary only, under "
      "120 words, plain text.");
  const QString body =
      (previousSummary.isEmpty() ? QString() : QStringLiteral("Summary so far:\n%1\n\n").arg(previousSummary)) +
      QStringLiteral("New exchanges:\n%1").arg(transcript);
  return QJsonArray{QJsonObject{{"role", "system"}, {"content", instruction}},
                    QJsonObject{{"role", "user"}, {"content", body}}};
}

QString capSummary(const QString &summary, int maxChars) {
  QString s = summary.simplified();
  if (s.size() > maxChars)
    s = s.left(maxChars).trimmed() + QStringLiteral("…");
  return s;
}

} // namespace ctx
