#include "modelrouter.h"

#include <QRegularExpression>

namespace modelrouter {

QString roleName(Role role) {
  switch (role) {
  case Role::Main: return QStringLiteral("main");
  case Role::Fast: return QStringLiteral("fast");
  case Role::Speed: return QStringLiteral("speed");
  }
  return QStringLiteral("main");
}

bool parseRole(const QString &name, Role *role) {
  const QString n = name.trimmed().toLower();
  Role r;
  if (n == QLatin1String("main")) r = Role::Main;
  else if (n == QLatin1String("fast")) r = Role::Fast;
  else if (n == QLatin1String("speed")) r = Role::Speed;
  else return false;
  if (role) *role = r;
  return true;
}

QString taskName(Task task) {
  switch (task) {
  case Task::Conversation: return QStringLiteral("conversation");
  case Task::Tools: return QStringLiteral("tools");
  case Task::Coding: return QStringLiteral("coding");
  case Task::Summary: return QStringLiteral("summary");
  case Task::Classify: return QStringLiteral("classify");
  }
  return QStringLiteral("conversation");
}

namespace {

QString squash(const QString &s) {
  static const QRegularExpression junk(QStringLiteral("[^a-z0-9]"));
  return s.toLower().remove(junk);
}

int words(const QString &text) {
  return text.split(QRegularExpression(QStringLiteral("\\s+")),
                    Qt::SkipEmptyParts).size();
}

// Signs that a request needs the big model.
bool heavy(const QString &lower) {
  static const QRegularExpression re(QStringLiteral(
      "\\b(explain|why|how (does|do|can|would|should)|debug|analy[sz]e|"
      "diagnose|troubleshoot|compare|plan|design|architecture|refactor|"
      "implement|optimi[sz]e|review|investigate|step by step|in detail|"
      "reason|derive|prove|trade-?offs?|pros and cons|what.s wrong|"
      "root cause|migrate)\\b"));
  return re.match(lower).hasMatch();
}

bool codey(const QString &lower) {
  static const QRegularExpression re(QStringLiteral(
      "\\b(code|function|compile[rd]?|compiler|rust|python|c\\+\\+|"
      "javascript|typescript|nix|flake|stack ?trace|traceback|segfault|"
      "regex|script|bug|exception|panic|error:|cargo|gcc|clang|git|"
      "kubernetes|docker|sql)\\b"));
  return re.match(lower).hasMatch();
}

bool summary(const QString &lower) {
  static const QRegularExpression re(
      QStringLiteral("\\b(summari[sz]e|summary|tl;?dr|recap|condense)\\b"));
  return re.match(lower).hasMatch();
}

bool toolish(const QString &lower) {
  static const QRegularExpression re(QStringLiteral(
      "^(please )?(open|close|launch|start|quit|kill|set|turn|mute|unmute|"
      "pause|play|resume|skip|next|previous|take|screenshot|lock|show|hide|"
      "switch|focus|copy|paste|search|find|remind|what.s the (time|date))\\b"));
  return re.match(lower).hasMatch();
}

Role roleFor(const QString &name, Role fallback) {
  Role r;
  return parseRole(name, &r) ? r : fallback;
}

} // namespace

bool needsReasoning(const QString &text) {
  const QString lower = text.toLower().trimmed();
  return heavy(lower) || (codey(lower) && words(lower) > 8) || words(lower) > 30;
}

Task classifyTask(const QString &text) {
  const QString lower = text.toLower().trimmed();
  if (summary(lower)) return Task::Summary;
  if (codey(lower) && (heavy(lower) || words(lower) > 8)) return Task::Coding;
  if (toolish(lower)) return Task::Tools;
  return Task::Conversation;
}

Choice choose(const QString &text, const Options &options) {
  Choice c;
  c.deep = needsReasoning(text);
  Role forced;
  if (options.mode != QLatin1String("auto") && parseRole(options.mode, &forced)) {
    c.role = forced;
    c.reason = QStringLiteral("mode is %1").arg(options.mode);
    c.task = classifyTask(text);
    return c;
  }
  c.task = classifyTask(text);
  if (!options.autoRouting) {
    c.role = Role::Main;
    c.reason = QStringLiteral("automatic routing is off");
    return c;
  }

  const QString lower = text.toLower().trimmed();
  const int n = words(lower);
  const auto applyTask = [&](const QString &configured, Role natural,
                            const QString &why) {
    c.role = configured.isEmpty() ? natural : roleFor(configured, natural);
    c.reason = configured.isEmpty() ? why
                                    : QStringLiteral("%1 task set to %2")
                                          .arg(taskName(c.task), configured);
  };

  switch (c.task) {
  case Task::Coding:
    applyTask(options.coding, Role::Main, QStringLiteral("code or a technical fault"));
    break;
  case Task::Summary:
    applyTask(options.summary, Role::Fast, QStringLiteral("a summary"));
    break;
  case Task::Classify:
    applyTask(options.classify, Role::Fast, QStringLiteral("classification"));
    break;
  case Task::Tools:
    applyTask(options.tools, Role::Fast, QStringLiteral("a simple command"));
    break;
  case Task::Conversation:
    if (heavy(lower) || n > 30) {
      // Configured conversation role applies only to light chat.
      c.role = Role::Main;
      c.reason = n > 30 ? QStringLiteral("long request")
                        : QStringLiteral("needs reasoning");
    } else {
      applyTask(options.conversation, Role::Fast, QStringLiteral("short chat"));
    }
    break;
  }
  return c;
}

QList<Role> fallbackOrder(Role role, const QStringList &custom) {
  QList<Role> order;
  for (const QString &name : custom) {
    Role r;
    if (parseRole(name, &r) && !order.contains(r)) order << r;
  }
  if (!order.isEmpty()) {
    if (order.first() != role) order.prepend(role);
    // Whatever the list left out still follows, so nothing is unreachable.
    for (Role r : {Role::Main, Role::Fast, Role::Speed})
      if (!order.contains(r)) order << r;
    return order;
  }
  switch (role) {
  case Role::Main: return {Role::Main, Role::Fast, Role::Speed};
  case Role::Fast: return {Role::Fast, Role::Speed, Role::Main};
  case Role::Speed: return {Role::Speed, Role::Fast, Role::Main};
  }
  return {Role::Main, Role::Fast, Role::Speed};
}

QString findModel(const QString &configured, const QStringList &available) {
  const QString want = squash(configured);
  if (want.isEmpty()) return {};
  // Exact (after squashing) beats "contains", so "qwen3" cannot shadow
  // "qwen3-30b-a3b" when both are installed.
  for (const QString &id : available)
    if (squash(id) == want) return id;
  for (const QString &id : available)
    if (squash(id).contains(want)) return id;
  return {};
}

Resolved resolve(Role wanted, const QString names[3],
                 const QStringList &available, const QStringList &customOrder) {
  Resolved out;
  for (Role r : fallbackOrder(wanted, customOrder)) {
    const QString found = findModel(names[int(r)], available);
    if (!found.isEmpty()) {
      out.model = found;
      out.role = r;
      out.fellBack = r != wanted;
      return out;
    }
  }
  return out;
}

bool parseModelPhrase(const QString &text, ModelPhrase *out) {
  const QString t = text.toLower().trimmed();
  static const QRegularExpression back(QStringLiteral(
      "\\b(switch back to|go back to|return to|use|turn on|enable|back to)\\b"
      ".*\\b(auto|automatic)\\b.*\\b(model|models|selection|routing)\\b"));
  static const QRegularExpression pick(QStringLiteral(
      "\\b(use|switch to|change to|go to|try)\\b\\s+(?:the\\s+)?"
      "(fast|quick|speed|speedy|main|smart|big|best|smartest)\\s+model\\b"));
  ModelPhrase phrase;
  phrase.persistent = t.contains(QLatin1String("from now on")) ||
                      t.contains(QLatin1String("always")) ||
                      t.contains(QLatin1String("switch to")) ||
                      t.contains(QLatin1String("switch back")) ||
                      t.contains(QLatin1String("default"));
  if (t.contains(QLatin1String("for this")) ||
      t.contains(QLatin1String("for that")) ||
      t.contains(QLatin1String("just this")))
    phrase.persistent = false;

  if (back.match(t).hasMatch() ||
      t.contains(QLatin1String("automatic model")) ||
      t.contains(QLatin1String("auto model"))) {
    phrase.mode = QStringLiteral("auto");
    phrase.persistent = true; // returning to auto is a standing preference
    if (out) *out = phrase;
    return true;
  }
  const auto m = pick.match(t);
  if (!m.hasMatch()) return false;
  const QString w = m.captured(2);
  if (w == QLatin1String("fast") || w == QLatin1String("quick"))
    phrase.mode = QStringLiteral("fast");
  else if (w == QLatin1String("speed") || w == QLatin1String("speedy"))
    phrase.mode = QStringLiteral("speed");
  else
    phrase.mode = QStringLiteral("main");
  if (out) *out = phrase;
  return true;
}

} // namespace modelrouter
