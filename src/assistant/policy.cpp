#include "policy.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

// --- actions ----------------------------------------------------------------

Decision decide(Risk risk, const QString &mode) {
  switch (risk) {
  case Risk::Safe:
    return Decision::Allow;
  case Risk::Low:
    return mode == "everything" ? Decision::Confirm : Decision::Allow;
  case Risk::Medium:
    return mode == "high" ? Decision::Allow : Decision::Confirm;
  case Risk::High:
    return Decision::Confirm;
  }
  return Decision::Deny;
}

QString riskName(Risk risk) {
  switch (risk) {
  case Risk::Safe:
    return QStringLiteral("safe");
  case Risk::Low:
    return QStringLiteral("low");
  case Risk::Medium:
    return QStringLiteral("medium");
  case Risk::High:
    return QStringLiteral("high");
  }
  return {};
}

// --- files ------------------------------------------------------------------

PathPolicy::PathPolicy(QStringList roots, QString home)
    : m_home(QDir::cleanPath(std::move(home))) {
  if (roots.isEmpty())
    roots << m_home;
  for (const QString &root : roots) {
    QString expanded = root;
    if (expanded == "~" || expanded.startsWith("~/"))
      expanded = m_home + expanded.mid(1);
    const QString canonical = QFileInfo(expanded).canonicalFilePath();
    if (!canonical.isEmpty())
      m_roots << canonical;
  }
}

bool PathPolicy::sensitive(const QString &canonical, const QString &home) {
  // Credentials, keys, browser profiles (cookies and saved logins), and
  // Nala's own memory and settings, which the agent reaches through its own
  // tools rather than as files.
  static const QStringList homeDirs = {
      ".ssh",          ".gnupg",          ".password-store",
      ".pki",          ".aws",            ".kube",
      ".docker",       ".mozilla",        ".thunderbird",
      ".librewolf",    ".zen",            ".config/chromium",
      ".config/google-chrome",            ".config/BraveSoftware",
      ".config/vivaldi",                  ".config/microsoft-edge",
      ".config/Bitwarden",                ".config/keepassxc",
      ".config/1Password",                ".config/gh",
      ".config/nala",  ".local/share/keyrings",
      ".local/share/nala",                ".local/state/nala",
      ".cache/nala",
  };
  static const QStringList homeFiles = {".netrc", ".git-credentials",
                                        ".pgpass", ".my.cnf", ".npmrc",
                                        ".pypirc", ".bash_history",
                                        ".zsh_history", ".histfile"};
  for (const QString &dir : homeDirs) {
    const QString full = home + "/" + dir;
    if (canonical == full || canonical.startsWith(full + "/"))
      return true;
  }
  for (const QString &file : homeFiles)
    if (canonical == home + "/" + file)
      return true;

  static const QRegularExpression names(QStringLiteral(
      R"((^|/)(\.env(\..*)?|id_(rsa|dsa|ecdsa|ed25519)(\.pub)?|.*\.(pem|key|p12|pfx|kdbx|keystore|jks|gpg|asc)|)"
      R"(credentials(\.json)?|secrets?(\.(json|ya?ml|toml))?|shadow|gshadow)$)"),
      QRegularExpression::CaseInsensitiveOption);
  return names.match(canonical).hasMatch() ||
         canonical.startsWith(QStringLiteral("/etc/ssl/private")) ||
         canonical.startsWith(QStringLiteral("/proc/")) ||
         canonical.startsWith(QStringLiteral("/sys/")) ||
         canonical.startsWith(QStringLiteral("/dev/"));
}

PathCheck PathPolicy::check(const QString &requested, bool forWrite) const {
  PathCheck result;
  QString path = requested.trimmed();
  if (path.isEmpty() || path.contains(QChar(0))) {
    result.reason = QStringLiteral("no path given");
    return result;
  }
  if (path == "~" || path.startsWith("~/"))
    path = m_home + path.mid(1);
  else if (QDir::isRelativePath(path))
    path = m_home + "/" + path;

  // Resolve symlinks, so a link inside an allowed root cannot point the agent
  // somewhere else. A file that does not exist yet (a write) is resolved
  // through its directory, which must exist.
  const QFileInfo info(path);
  QString canonical = info.canonicalFilePath();
  if (canonical.isEmpty()) {
    if (!forWrite) {
      result.reason = QStringLiteral("does not exist");
      return result;
    }
    const QString name = info.fileName();
    if (name.isEmpty() || name == "." || name == "..") {
      result.reason = QStringLiteral("not a file name");
      return result;
    }
    const QString parent = QFileInfo(info.absolutePath()).canonicalFilePath();
    if (parent.isEmpty()) {
      result.reason = QStringLiteral("its folder does not exist");
      return result;
    }
    canonical = parent + "/" + name;
  }

  bool inside = false;
  for (const QString &root : m_roots)
    if (canonical == root || canonical.startsWith(root + "/"))
      inside = true;
  if (!inside) {
    result.reason = QStringLiteral("outside the folders Nala may use");
    return result;
  }
  if (sensitive(canonical, m_home)) {
    result.reason = QStringLiteral("holds credentials or private data");
    return result;
  }
  result.allowed = true;
  result.path = canonical;
  return result;
}

// --- screen memory ------------------------------------------------------------

PrivacyCheck privacyGate(const WindowInfo &window,
                         const QStringList &excludedApps,
                         const QStringList &excludedTitles,
                         bool blockSensitive, bool requireWindowInfo) {
  PrivacyCheck check;
  if (!window.valid) {
    // Without knowing what is on screen there is nothing to check it against.
    check.allowed = !requireWindowInfo;
    check.reason = requireWindowInfo ? QStringLiteral("unknown window")
                                     : QString();
    return check;
  }

  const QString klass = window.appClass.toLower();
  const QString initial = window.initialClass.toLower();
  for (const QString &app : excludedApps) {
    const QString needle = app.trimmed().toLower();
    if (!needle.isEmpty() &&
        (klass.contains(needle) || initial.contains(needle))) {
      check.reason = QStringLiteral("excluded application");
      return check;
    }
  }

  const QString title = window.title.toLower();
  for (const QString &word : excludedTitles) {
    const QString needle = word.trimmed().toLower();
    if (!needle.isEmpty() && title.contains(needle)) {
      check.reason = QStringLiteral("excluded window");
      return check;
    }
  }

  if (blockSensitive) {
    struct Category {
      const char *name;
      QRegularExpression pattern;
    };
    static const QVector<Category> categories = {
        {"private browsing",
         QRegularExpression(QStringLiteral(
             R"(\b(incognito|private browsing|inprivate|private window)\b)"))},
        {"credentials",
         QRegularExpression(QStringLiteral(
             R"(\b(passwords?|passwort|passcode|pass phrase|passphrase|)"
             R"(sign[ -]?in|log[ -]?in|signin|login|2fa|mfa|two[- ]factor|)"
             R"(two[- ]step|authenticator|verification code|security code|)"
             R"(recovery codes?|backup codes?|one[- ]time (code|password)|otp|)"
             R"(seed phrase|recovery phrase|mnemonic|private key)\b)"))},
        {"banking and payment",
         QRegularExpression(QStringLiteral(
             R"(\b(bank|banking|credit card|debit card|card number|checkout|)"
             R"(payment|pay now|billing|paypal|venmo|cash app|zelle|)"
             R"(wise\.com|revolut|coinbase|kraken|binance|metamask|wallet)\b)"))},
        {"adult content",
         QRegularExpression(QStringLiteral(
             R"(\b(nsfw|porn\w*|xxx|onlyfans|fansly|hentai|rule ?34|nudes?|)"
             R"(xvideos|xhamster|redtube|youporn|chaturbate|e621|r/gonewild)\b)"))},
    };
    for (const Category &category : categories)
      if (category.pattern.match(title).hasMatch()) {
        check.reason = QString::fromLatin1(category.name);
        return check;
      }
  }

  check.allowed = true;
  return check;
}

// --- asking about the past ------------------------------------------------------

namespace {

QDateTime startOfDay(const QDate &date) { return QDateTime(date, QTime(0, 0)); }

int weekday(const QString &name) {
  static const QStringList days = {"monday", "tuesday",  "wednesday",
                                   "thursday", "friday", "saturday",
                                   "sunday"};
  return int(days.indexOf(name)) + 1; // Qt::Monday == 1
}

} // namespace

TimeRange parseTimeRange(const QString &query, const QDateTime &now) {
  TimeRange range;
  QString text = query.toLower().simplified();
  const QDate today = now.date();

  const auto take = [&](const QRegularExpressionMatch &match, QDateTime from,
                        QDateTime to) {
    range.valid = true;
    range.from = from;
    range.to = std::min(to, now);
    text.remove(match.capturedStart(), match.capturedLength());
  };

  static const QRegularExpression ago(QStringLiteral(
      R"(\b(\d+|a|an|one|two|three|a few|few|a couple of)\s+(minute|hour|day|week)s?\s+ago\b)"));
  static const QRegularExpression pastN(QStringLiteral(
      R"(\b(?:in\s+)?(?:the\s+)?(?:last|past)\s+(\d+|few|couple of)?\s*(minute|hour|day|week|month)s?\b)"));
  static const QRegularExpression named(QStringLiteral(
      R"(\b(earlier today|today|this morning|this afternoon|this evening|tonight|)"
      R"(last night|yesterday morning|yesterday afternoon|yesterday evening|)"
      R"(yesterday|day before yesterday|this week|last week|this month|last month)\b)"));
  static const QRegularExpression day(QStringLiteral(
      R"(\b(?:(last|on|this)\s+)?(monday|tuesday|wednesday|thursday|friday|saturday|sunday)\b)"));

  const auto count = [](const QString &word) {
    if (word.isEmpty() || word == "a" || word == "an" || word == "one")
      return 1;
    if (word.contains("couple") || word == "two")
      return 2;
    if (word.contains("few") || word == "three")
      return 3;
    return std::max(1, word.toInt());
  };
  const auto seconds = [](const QString &unit) -> qint64 {
    if (unit == "minute")
      return 60;
    if (unit == "hour")
      return 3600;
    if (unit == "day")
      return 86400;
    if (unit == "week")
      return 7 * 86400;
    return 30 * 86400;
  };

  if (auto m = named.match(text); m.hasMatch()) {
    const QString what = m.captured(1);
    const QDate yesterday = today.addDays(-1);
    const QDate monday = today.addDays(1 - today.dayOfWeek());
    if (what == "today" || what == "earlier today")
      take(m, startOfDay(today), now);
    else if (what == "this morning")
      take(m, startOfDay(today), QDateTime(today, QTime(12, 0)));
    else if (what == "this afternoon")
      take(m, QDateTime(today, QTime(12, 0)), QDateTime(today, QTime(18, 0)));
    else if (what == "this evening" || what == "tonight")
      take(m, QDateTime(today, QTime(17, 0)), now);
    else if (what == "last night")
      take(m, QDateTime(yesterday, QTime(18, 0)), QDateTime(today, QTime(6, 0)));
    else if (what == "yesterday morning")
      take(m, startOfDay(yesterday), QDateTime(yesterday, QTime(12, 0)));
    else if (what == "yesterday afternoon")
      take(m, QDateTime(yesterday, QTime(12, 0)),
           QDateTime(yesterday, QTime(18, 0)));
    else if (what == "yesterday evening")
      take(m, QDateTime(yesterday, QTime(17, 0)), startOfDay(today));
    else if (what == "yesterday")
      take(m, startOfDay(yesterday), startOfDay(today));
    else if (what == "day before yesterday")
      take(m, startOfDay(today.addDays(-2)), startOfDay(yesterday));
    else if (what == "this week")
      take(m, startOfDay(monday), now);
    else if (what == "last week")
      take(m, startOfDay(monday.addDays(-7)), startOfDay(monday));
    else if (what == "this month")
      take(m, startOfDay(QDate(today.year(), today.month(), 1)), now);
    else if (what == "last month") {
      const QDate first(today.year(), today.month(), 1);
      take(m, startOfDay(first.addMonths(-1)), startOfDay(first));
    }
  } else if (auto m = ago.match(text); m.hasMatch()) {
    // "3 days ago" is a day, not a span up to now; smaller units get a margin.
    const qint64 span = count(m.captured(1)) * seconds(m.captured(2));
    const QDateTime at = now.addSecs(-span);
    if (m.captured(2) == "day" || m.captured(2) == "week")
      take(m, startOfDay(at.date()), startOfDay(at.date().addDays(1)));
    else
      take(m, at.addSecs(-span / 2 - 300), at.addSecs(span / 2 + 300));
  } else if (auto m = pastN.match(text); m.hasMatch()) {
    const qint64 span = count(m.captured(1)) * seconds(m.captured(2));
    take(m, now.addSecs(-span), now);
  } else if (auto m = day.match(text); m.hasMatch()) {
    const int wanted = weekday(m.captured(2));
    int back = (today.dayOfWeek() - wanted + 7) % 7;
    // "last monday" on a Monday means a week ago; "monday" on a Monday means
    // today.
    if (back == 0 && m.captured(1) == "last")
      back = 7;
    const QDate date = today.addDays(-back);
    take(m, startOfDay(date), startOfDay(date.addDays(1)));
  }

  range.rest = text.simplified();
  return range;
}
