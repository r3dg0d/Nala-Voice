#include "commandrouter.h"

#include <QHash>
#include <algorithm>

namespace {

// Minutes in one of a spoken unit.
int unitMinutes(const QString &unit) {
  if (unit.startsWith("minute"))
    return 1;
  if (unit.startsWith("hour"))
    return 60;
  if (unit.startsWith("day"))
    return 24 * 60;
  if (unit.startsWith("week"))
    return 7 * 24 * 60;
  if (unit.startsWith("month"))
    return 30 * 24 * 60;
  return 0;
}

// "N units" as minutes, N defaulting to one ("the last hour").
QVariantMap duration(const QString &count, const QString &unit) {
  const int n = count.isEmpty() ? 1 : count.toInt();
  const int minutes = n * unitMinutes(unit);
  if (minutes <= 0)
    return {};
  return {{"minutes", minutes}};
}

const QString kScreenThing = QStringLiteral(
    "(?:the\\s+)?(?:screen\\s+(?:recording|recorder|memory|memories|capture|"
    "capturing)|recording(?:\\s+(?:my|the)\\s+screen)?|"
    "remembering(?:\\s+(?:my|the)\\s+screen|\\s+things|\\s+this)?|"
    "(?:visual\\s+)?memory|memories|watching(?:\\s+my\\s+screen)?)");

const QString kUnit =
    QStringLiteral("(minute|minutes|hour|hours|day|days|week|weeks)");

} // namespace

CommandRouter::CommandRouter(const QString &name) { setName(name); }

void CommandRouter::setName(const QString &name) {
  m_name = normalise(name);
  if (m_name.isEmpty())
    m_name = QStringLiteral("nala");
  m_patterns.clear();
  build();
}

void CommandRouter::build() {
  // Her name, as it may appear in a command: "open nova's settings".
  const QString own = QStringLiteral("(?:your\\s+|the\\s+|%1's\\s+|%1\\s+)?")
                          .arg(QRegularExpression::escape(m_name));
  // Order matters: the first pattern that matches the whole utterance wins,
  // so the specific ones come before the general "open <app>".

  // Interruptions. These also cancel whatever she is part-way through.
  add("(?:stop|stop it|stop talking|stop that|quiet|be quiet|hush|shush|"
      "shut up|never ?mind|cancel|cancel that|forget it|that's enough|"
      "enough|abort)",
      "assistant.stop");

  // Answers to "should I…?". Only acted on while a question is open.
  add("(?:yes|yeah|yep|yup|sure|do it|go ahead|go for it|confirm|confirmed|"
      "yes please|yes do it|okay do it|ok do it|submit it|send it|approved)",
      "confirm.yes");
  add("(?:no|nope|nah|don't|do not|no thanks|no thank you|don't do it|"
      "cancel it|deny|denied)",
      "confirm.no");

  // The privacy switch, in all the ways people say it.
  add("(?:turn off|switch off|stop|pause|disable|kill|halt)\\s+" + kScreenThing +
          "(?:\\s+for\\s+(\\d+)?\\s*" + kUnit + ")?",
      "memory.pause", [](const QRegularExpressionMatch &m) {
        return m.captured(2).isEmpty() ? QVariantMap()
                                       : duration(m.captured(1), m.captured(2));
      });
  add("(?:stop remembering|don't remember|do not remember)\\s+"
      "(?:my\\s+screen|anything|this|what i'm doing)",
      "memory.pause");
  add("(?:privacy mode|go private|privacy please|private mode|"
      "stop looking at my screen|look away)",
      "memory.pause");
  add("(?:turn on|switch on|resume|restart|start|enable|unpause)\\s+" +
          kScreenThing + "(?:\\s+again)?",
      "memory.resume");
  add("turn\\s+" + kScreenThing + "\\s+back\\s+on", "memory.resume");
  add("(?:you can\\s+)?(?:start\\s+)?remember(?:ing)?\\s+(?:my\\s+screen\\s+)?"
      "again",
      "memory.resume");

  // Forgetting.
  add("(?:forget|delete|erase|wipe|remove)\\s+(?:everything\\s+from\\s+)?"
      "(?:the\\s+)?(?:last|past|previous)\\s+(\\d+)?\\s*" +
          kUnit + "(?:\\s+of\\s+memor(?:y|ies))?",
      "memory.forget", [](const QRegularExpressionMatch &m) {
        return duration(m.captured(1), m.captured(2));
      });
  add("(?:forget|delete|erase|wipe)\\s+(?:everything\\s+from\\s+|all\\s+of\\s+)?"
      "today",
      "memory.forget", [](const QRegularExpressionMatch &) {
        return QVariantMap{{"range", "today"}};
      });
  add("(?:forget|delete|erase|wipe)\\s+(?:everything|all\\s+(?:your\\s+|my\\s+)?"
      "memories|all\\s+of\\s+your\\s+memories)",
      "memory.forget", [](const QRegularExpressionMatch &) {
        return QVariantMap{{"range", "all"}};
      });
  add("(?:delete|remove|clear|forget)\\s+(?:all\\s+|the\\s+)?"
      "(?:screenshots|screen\\s+memories|memories)\\s+older\\s+than\\s+"
      "(\\d+)?\\s*(day|days|week|weeks|month|months)",
      "memory.deleteOlder", [](const QRegularExpressionMatch &m) {
        const QVariantMap span = duration(m.captured(1), m.captured(2));
        return QVariantMap{{"days", span.value("minutes").toInt() / (24 * 60)}};
      });

  // Keeping.
  add("(?:keep|pin|save|bookmark)\\s+(?:this|that)(?:\\s+memory)?"
      "(?:\\s+(?:forever|permanently))?",
      "memory.pin");
  add("remember\\s+(?:this|that)(?:\\s+(?:forever|permanently|for\\s+good))?",
      "memory.pin");
  add("(?:never|don't|do not)\\s+(?:record|remember|capture|watch|screenshot)\\s+"
      "(?:this|that)(?:\\s+(?:app|application|window|program))?"
      "(?:\\s+again|\\s+ever)?",
      "memory.excludeCurrent");
  add("how\\s+much\\s+(?:storage|space|disk|disk\\s+space|room)\\b.*",
      "memory.status");
  add("(?:are\\s+you|is\\s+(?:screen\\s+)?(?:memory|recording))\\s*"
      "(?:recording|remembering|watching|on|enabled|paused)?"
      "(?:\\s+my\\s+screen)?",
      "memory.status");
  add("(?:open|show)(?:\\s+me)?\\s+(?:your\\s+|my\\s+|the\\s+)?"
      "(?:memories|memory|timeline|memory\\s+timeline|history)",
      "memory.timeline");

  // Her own windows.
  add("(?:open|show)(?:\\s+me)?\\s+" + own +
          "(?:settings|preferences|options|config|configuration)",
      "settings.open");
  add("(?:close|hide)\\s+" + own +
          "(?:settings|preferences|options|config|configuration)",
      "settings.close");

  // Ears and voice.
  add("(?:stop listening|stop listening to me|go deaf|turn off (?:the\\s+|your\\s+)?"
      "(?:microphone|mic)|mute (?:the\\s+|your\\s+)?(?:microphone|mic))",
      "listen.stop");
  add("(?:start listening|listen(?:\\s+to\\s+me)?|turn on (?:the\\s+|your\\s+)?"
      "(?:microphone|mic))",
      "listen.start");
  add("(?:mute|silence)\\s+(?:yourself|your\\s+voice)|"
      "(?:stop|quit)\\s+(?:speaking|talking)\\s+(?:out\\s+)?loud|"
      "(?:text|type|bubbles?)\\s+only",
      "tts.mute");
  add("(?:unmute|un-mute)\\s+(?:yourself|your\\s+voice)|"
      "(?:you\\s+can\\s+)?(?:talk|speak)\\s+(?:to\\s+me\\s+)?(?:again|out\\s+loud)",
      "tts.unmute");

  // Her.
  add("(?:go\\s+to\\s+sleep|sleep|take\\s+a\\s+nap|nap\\s+time|"
      "good\\s*night|go\\s+to\\s+bed)",
      "nala.sleep");
  add("(?:wake\\s+up|wakey\\s+wakey|good\\s+morning|rise\\s+and\\s+shine)",
      "nala.wake");
  add("wink(?:\\s+at\\s+me)?", "nala.wink");
  add("(?:show\\s+me\\s+everything|do\\s+a\\s+demo|show\\s+off)", "nala.demo");

  // Windows and applications.
  add("close\\s+(?:all|every)\\s+(?:of\\s+)?(?:the\\s+|my\\s+)?"
      "(?:windows?|apps?|applications?|programs?)",
      "window.closeAll");
  add("(?:close|quit|exit|kill)\\s+(?:the\\s+|my\\s+)?"
      "((?!all\\b|every\\b)[a-z0-9][a-z0-9.+\\-]*(?:\\s+[a-z0-9.+\\-]+){0,2})",
      "apps.close", [](const QRegularExpressionMatch &m) {
        return QVariantMap{{"name", m.captured(1)}};
      });
  add("(?:switch\\s+to|focus(?:\\s+on)?|bring\\s+up)\\s+(?:the\\s+|my\\s+)?"
      "([a-z0-9][a-z0-9.+\\-]*(?:\\s+[a-z0-9.+\\-]+){0,2})",
      "apps.focus", [](const QRegularExpressionMatch &m) {
        return QVariantMap{{"name", m.captured(1)}};
      });
  // Deliberately narrow: at most three words and no conjunctions, so "open my
  // X feed and doomscroll" is left for the model rather than half-understood.
  add("(?:open|launch|start|run|fire\\s+up)\\s+(?:up\\s+)?(?:the\\s+|my\\s+|a\\s+|a\\s+new\\s+)?"
      "((?!.*\\b(?:and|then|to|with|for|on|in|feed|page|tab|website|site|"
      "file|folder)\\b)[a-z0-9][a-z0-9.+\\-]*(?:\\s+[a-z0-9.+\\-]+){0,2})",
      "apps.launch", [](const QRegularExpressionMatch &m) {
        return QVariantMap{{"name", m.captured(1)}};
      });
}

void CommandRouter::add(
    const QString &pattern, const QString &action,
    std::function<QVariantMap(const QRegularExpressionMatch &)> args) {
  QRegularExpression re(QStringLiteral("^(?:") + pattern +
                        QStringLiteral(")$"));
  re.optimize();
  Q_ASSERT_X(re.isValid(), "CommandRouter", qPrintable(re.errorString()));
  m_patterns.append({re, action, std::move(args)});
}

QString CommandRouter::normalise(const QString &utterance) {
  QString text = utterance.toLower();
  text.replace(QChar(0x2019), QChar('\'')); // typographic apostrophe
  text.replace(QChar(0x2018), QChar('\''));

  // Whisper's markers for non-speech: "[BLANK_AUDIO]", "(music)".
  static const QRegularExpression noise(
      QStringLiteral(R"(\[[^\]]*\]|\([^)]*\)|\*[^*]*\*)"));
  text.remove(noise);

  // Punctuation goes, except where it is part of a word: "github.com",
  // "don't", "c++", "wi-fi".
  static const QRegularExpression punct(
      QStringLiteral(R"([,!?;:"“”…]|(?<![a-z0-9])['.\-]|['.\-](?![a-z0-9+]))"));
  text.replace(punct, QStringLiteral(" "));
  text = text.simplified();

  static const QHash<QString, QString> numbers = {
      {"a", "1"},      {"an", "1"},      {"one", "1"},      {"two", "2"},
      {"three", "3"},  {"four", "4"},    {"five", "5"},     {"six", "6"},
      {"seven", "7"},  {"eight", "8"},   {"nine", "9"},     {"ten", "10"},
      {"eleven", "11"}, {"twelve", "12"}, {"fifteen", "15"}, {"twenty", "20"},
      {"thirty", "30"}, {"forty", "40"},  {"forty-five", "45"},
      {"sixty", "60"},  {"ninety", "90"}, {"couple", "2"},   {"few", "3"},
      {"several", "5"}};
  static const QRegularExpression unitAfter(QStringLiteral(
      R"(^(minute|minutes|hour|hours|day|days|week|weeks|month|months)$)"));
  text.replace(QStringLiteral("half an hour"), QStringLiteral("30 minutes"));
  text.replace(QStringLiteral("a couple of"), QStringLiteral("couple"));
  text.replace(QStringLiteral("a few"), QStringLiteral("few"));
  QStringList words = text.split(' ', Qt::SkipEmptyParts);
  for (int i = 0; i + 1 < words.size(); ++i)
    if (numbers.contains(words[i]) && unitAfter.match(words[i + 1]).hasMatch())
      words[i] = numbers.value(words[i]);
  text = words.join(' ');

  // Politeness and filler that carry no instruction.
  static const QRegularExpression lead(QStringLiteral(
      R"(^(?:(?:please|um+|uh+|so|okay so|and|hey)\s+|(?:can|could|would|will)\s+you\s+(?:please\s+)?|i\s+(?:want|need|would like)\s+you\s+to\s+|i'd\s+like\s+you\s+to\s+)+)"));
  static const QRegularExpression tail(QStringLiteral(
      R"((?:\s+(?:please|for me|now|right now|thanks|thank you|for me please))+$)"));
  // The fillers only come off the ends, and only while something is left.
  for (int pass = 0; pass < 3; ++pass) {
    const QString before = text;
    const QString stripped = QString(text).remove(lead).remove(tail).trimmed();
    if (!stripped.isEmpty())
      text = stripped;
    if (text == before)
      break;
  }
  return text;
}

QString CommandRouter::stripWake(const QString &normalised,
                                 const QStringList &phrases, bool *addressed) {
  QStringList sorted = phrases;
  for (QString &phrase : sorted)
    phrase = normalise(phrase);
  // Longest first, so "hey nala" is taken whole rather than as "nala".
  std::sort(sorted.begin(), sorted.end(),
            [](const QString &a, const QString &b) {
              return a.size() > b.size();
            });

  QString text = normalised;
  bool found = false;
  for (const QString &phrase : sorted) {
    if (phrase.isEmpty())
      continue;
    if (text == phrase) {
      text.clear();
      found = true;
      break;
    }
    if (text.startsWith(phrase + ' ')) {
      text = text.mid(phrase.size() + 1);
      found = true;
      break;
    }
  }
  // "stop, Nala" addresses her just as well.
  for (const QString &phrase : sorted) {
    if (!phrase.isEmpty() && text.endsWith(' ' + phrase)) {
      text.chop(phrase.size() + 1);
      found = true;
      break;
    }
  }
  if (addressed)
    *addressed = found;
  return normalise(text);
}

Route CommandRouter::route(const QString &utterance,
                           const QStringList &wakePhrases) const {
  Route route;
  route.text = stripWake(normalise(utterance), wakePhrases, &route.addressed);
  if (route.text.isEmpty())
    return route;
  for (const Pattern &pattern : m_patterns) {
    const QRegularExpressionMatch match = pattern.re.match(route.text);
    if (!match.hasMatch())
      continue;
    route.matched = true;
    route.action = pattern.action;
    if (pattern.args)
      route.args = pattern.args(match);
    return route;
  }

  // The privacy switch must not hang on her name being heard right: "hey
  // Arlo, pause screen memory" still pauses it. Only for pausing, which is
  // harmless if it was meant for someone else.
  const QStringList words = route.text.split(' ');
  for (int skip = 1; skip <= 3 && skip < words.size(); ++skip) {
    const QString rest = words.mid(skip).join(' ');
    for (const Pattern &pattern : m_patterns) {
      if (pattern.action != QLatin1String("memory.pause"))
        continue;
      const QRegularExpressionMatch match = pattern.re.match(rest);
      if (!match.hasMatch())
        continue;
      route.matched = true;
      route.action = pattern.action;
      route.text = rest;
      if (pattern.args)
        route.args = pattern.args(match);
      return route;
    }
  }
  return route;
}

Route CommandRouter::routeAfterWake(const QString &utterance,
                                    const QStringList &wakePhrases) const {
  Route direct = route(utterance, wakePhrases);
  if (direct.matched || direct.addressed)
    return direct;
  // Skip no more words than a wake phrase has: "hit nala, don't open
  // discord" must not become "open discord".
  int longest = 1;
  for (const QString &phrase : wakePhrases)
    longest = std::max(longest, int(normalise(phrase).split(' ', Qt::SkipEmptyParts).size()));
  const QStringList words = direct.text.split(' ', Qt::SkipEmptyParts);
  for (int skip = 1; skip <= std::min(longest, 3) && skip < words.size(); ++skip) {
    Route r = route(words.mid(skip).join(' '), {});
    if (r.matched) {
      r.addressed = true;
      return r;
    }
  }
  direct.addressed = true; // the detector heard her name
  return direct;
}

QStringList CommandRouter::actions() const {
  QStringList names;
  for (const Pattern &pattern : m_patterns)
    if (!names.contains(pattern.action))
      names << pattern.action;
  return names;
}
