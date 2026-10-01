#include "windowtarget.h"
#include <QRegularExpression>
#include <algorithm>
namespace {
QString normalize(QString s) {
  s = s.toCaseFolded();
  s.replace(QRegularExpression("[^\\p{L}\\p{N}]+"), " ");
  return s.simplified();
}
bool appMatch(QString app, QString token) {
  app = normalize(app);
  if (app.contains(token))
    return true;
  const QStringList browsers{"firefox", "chromium", "chrome",
                             "brave",   "vivaldi",  "zen"};
  const QStringList terminals{"kitty",   "foot",    "alacritty",
                              "konsole", "wezterm", "ghostty"};
  for (const auto &alias : token == "browser"    ? browsers
                           : token == "terminal" ? terminals
                                                 : QStringList{})
    if (app.contains(alias))
      return true;
  return false;
}
} // namespace
namespace windowtarget {
QVector<Match> rank(const QVector<WindowInfo> &windows, QString query,
                    QString app, QString title, int workspace) {
  query = normalize(query);
  app = normalize(app);
  title = normalize(title);
  QStringList tokens = query.split(' ', Qt::SkipEmptyParts);
  const QStringList filler{"the",  "my",     "window", "app",    "application",
                           "with", "called", "titled", "showing"};
  for (const auto &word : filler)
    tokens.removeAll(word);
  if (tokens.isEmpty() && app.isEmpty() && title.isEmpty())
    return {};
  QVector<Match> result;
  for (const auto &w : windows) {
    if (!w.valid || (workspace && w.workspace != workspace))
      continue;
    const auto t = normalize(w.title),
               c = normalize(w.appClass + " " + w.initialClass);
    if ((!app.isEmpty() && !appMatch(c, app)) ||
        (!title.isEmpty() && !t.contains(title)))
      continue;
    bool matched = true;
    for (const auto &token : tokens)
      if (!t.contains(token) && !appMatch(c, token)) {
        matched = false;
        break;
      }
    if (!matched)
      continue;
    const auto phrase = tokens.join(' ');
    int score = 50;
    if (!phrase.isEmpty() && t == phrase)
      score = 100;
    else if (!phrase.isEmpty() && (normalize(w.appClass) == phrase ||
                                   normalize(w.initialClass) == phrase))
      score = 90;
    else if (!phrase.isEmpty() && t.contains(phrase))
      score = 80;
    if (!title.isEmpty())
      score += t == title ? 30 : 20;
    if (!app.isEmpty())
      score += 10;
    result.append({w, score});
  }
  std::stable_sort(result.begin(), result.end(),
                   [](auto a, auto b) { return a.score > b.score; });
  return result;
}
bool ambiguous(const QVector<Match> &matches) {
  return matches.size() > 1 && matches[0].score - matches[1].score < 15;
}
} // namespace windowtarget
