#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

// Keeps the prompt inside a token budget without dumping unlimited history
// into the model. Pure: no network, no settings object.
//
// What the model sees, in priority order:
//   1. the system prompt (always)
//   2. a running summary of older turns (short, replaced not appended)
//   3. the most recent turns, newest kept longest
//   4. the request itself
// Tool results are clipped before they are stored, so one huge command output
// cannot crowd out the conversation for the rest of the session.
namespace ctx {

// tokens ~ characters / 4: a budget guide, not a tokenizer.
int estimateTokens(const QString &text);
int estimateTokens(const QJsonObject &message);
int estimateTokens(const QJsonArray &messages);

// Keeps the start and end of `text` and says what was cut, so the model knows
// the output was longer. A no-op under the limit.
QString clip(const QString &text, int maxChars);

struct Fit {
  QJsonArray history;  // the messages to send after the system prompt
  QJsonArray dropped;  // older messages that no longer fit, oldest first
  int tokens = 0;      // estimate of system + summary + history
};

// `history` is user/assistant messages, oldest first. `summary`, when
// non-empty, is added as a system note ahead of them. At most `maxRecentTurns`
// exchanges are kept (a turn is a user + assistant pair) and the whole prompt
// is held under `maxTokens - reserveForReply`.
Fit fit(const QJsonObject &system, const QString &summary,
        const QJsonArray &history, int maxRecentTurns, int maxTokens,
        int reserveForReply);

// The request that folds dropped messages into the running summary. The
// caller sends it to a fast model and stores the reply with capSummary().
QJsonArray summaryRequest(const QString &previousSummary,
                          const QJsonArray &dropped);
QString capSummary(const QString &summary, int maxChars = 1500);

} // namespace ctx
