#pragma once
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <functional>

// The fast path. Simple spoken commands are matched here, deterministically
// and in microseconds, so "stop", "pause screen memory" or "open settings"
// never wait on -- or depend on -- a language model. Anything not matched
// goes to the model.
//
// The privacy switch lives here on purpose: pausing screen memory must work
// when the model is slow, wrong, or not running at all.
struct Route {
  bool matched = false;
  QString action;    // e.g. "memory.pause"; empty when not matched
  QVariantMap args;  // e.g. {"minutes": 60}
  QString text;      // the normalised utterance, wake phrase removed
  bool addressed = false; // it began with a wake phrase
};

class CommandRouter {
public:
  CommandRouter();

  // Lower-case, strip punctuation and filler, spell numbers as digits.
  static QString normalise(const QString &utterance);

  // Removes a leading wake phrase; `addressed` says whether there was one.
  static QString stripWake(const QString &normalised,
                           const QStringList &phrases, bool *addressed);

  Route route(const QString &utterance,
              const QStringList &wakePhrases = {}) const;

  // Every action the router can produce, for documentation and tests.
  QStringList actions() const;

private:
  struct Pattern {
    QRegularExpression re;
    QString action;
    std::function<QVariantMap(const QRegularExpressionMatch &)> args;
  };
  void add(const QString &pattern, const QString &action,
           std::function<QVariantMap(const QRegularExpressionMatch &)> args =
               nullptr);

  QVector<Pattern> m_patterns;
};
