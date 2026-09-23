#include "eventlog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace {

constexpr int kRing = 300;
constexpr qint64 kMaxBytes = 4 * 1024 * 1024;

// Keys whose values are secret whatever they contain.
bool secretKey(const QString &key) {
  static const QRegularExpression re(
      // Whole names or suffixes, so access_token is caught and maxTokens is
      // not.
      QStringLiteral("(^|[_\\-.])(pass(word|phrase)?|secret|token|"
                     "api[_-]?key|authori[sz]ation|cookie|credentials?|"
                     "private[_-]?key|otp)$"),
      QRegularExpression::CaseInsensitiveOption);
  return re.match(key).hasMatch();
}

} // namespace

EventLog::EventLog(QString path, QObject *parent)
    : QObject(parent), m_path(std::move(path)) {}

void EventLog::setDebug(bool debug) {
  if (m_debug == debug)
    return;
  m_debug = debug;
  emit debugChanged();
}

QString EventLog::redact(const QString &text) {
  // Recognisable token shapes, then "password is hunter2"-style phrases.
  static const QList<QRegularExpression> patterns = {
      QRegularExpression(QStringLiteral(
          R"((sk-[A-Za-z0-9_\-]{16,}|gh[pousr]_[A-Za-z0-9]{20,}|)"
          R"(github_pat_[A-Za-z0-9_]{20,}|xox[abprs]-[A-Za-z0-9\-]{10,}|)"
          R"(AKIA[0-9A-Z]{16}|AIza[0-9A-Za-z_\-]{30,}|)"
          R"(eyJ[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]+))")),
      QRegularExpression(
          QStringLiteral(R"((bearer|basic)\s+[A-Za-z0-9._~+/=\-]{8,})"),
          QRegularExpression::CaseInsensitiveOption),
      QRegularExpression(
          QStringLiteral(R"(-----BEGIN [A-Z ]*PRIVATE KEY-----[\s\S]*?)"
                         R"(-----END [A-Z ]*PRIVATE KEY-----)")),
  };
  static const QRegularExpression phrase(
      QStringLiteral(R"(\b(password|passphrase|passcode|pin|api key|token|)"
                     R"(secret)(\s*(is|was|=|:)\s*)(\S+))"),
      QRegularExpression::CaseInsensitiveOption);

  QString out = text;
  for (const QRegularExpression &re : patterns)
    out.replace(re, QStringLiteral("[redacted]"));
  out.replace(phrase, QStringLiteral("\\1\\2[redacted]"));
  return out;
}

QJsonObject EventLog::redact(const QJsonObject &object) {
  QJsonObject out;
  for (auto it = object.begin(); it != object.end(); ++it) {
    const QJsonValue value = it.value();
    if (secretKey(it.key()))
      out.insert(it.key(), QStringLiteral("[redacted]"));
    else if (value.isString())
      out.insert(it.key(), redact(value.toString()));
    else if (value.isObject())
      out.insert(it.key(), redact(value.toObject()));
    else if (value.isArray()) {
      QJsonArray items;
      for (const QJsonValue &item : value.toArray())
        items.append(item.isString()   ? QJsonValue(redact(item.toString()))
                     : item.isObject() ? QJsonValue(redact(item.toObject()))
                                       : item);
      out.insert(it.key(), items);
    } else
      out.insert(it.key(), value);
  }
  return out;
}

void EventLog::record(const QString &category, const QString &event,
                      const QJsonObject &fields) {
  QJsonObject entry = redact(fields);
  entry.insert("t", QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
  entry.insert("cat", category);
  entry.insert("event", event);
  write(entry);
}

void EventLog::trace(const QString &category, const QString &event,
                     const QJsonObject &fields) {
  if (m_debug)
    record(category, event, fields);
}

void EventLog::write(const QJsonObject &entry) {
  m_ring.append(entry);
  while (m_ring.size() > kRing)
    m_ring.removeFirst();
  emit appended();

  if (m_path.isEmpty())
    return;
  QDir().mkpath(QFileInfo(m_path).absolutePath());
  // Keep one generation of history rather than growing without end.
  if (QFileInfo(m_path).size() > kMaxBytes) {
    QFile::remove(m_path + ".1");
    QFile::rename(m_path, m_path + ".1");
  }
  QFile file(m_path);
  if (!file.open(QIODevice::Append | QIODevice::WriteOnly))
    return;
  file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  file.write(QJsonDocument(entry).toJson(QJsonDocument::Compact) + "\n");
}

QVariantList EventLog::recent() const {
  QVariantList list;
  for (auto it = m_ring.crbegin(); it != m_ring.crend(); ++it)
    list << it->toVariantMap();
  return list;
}
