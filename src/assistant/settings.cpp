#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>
#include <cmath>

namespace {

enum class Kind { Bool, String, Url, Int, Real, Choice, List };

struct Spec {
  const char *key;
  Kind kind;
  QVariant fallback;
  double lo = 0.0, hi = 0.0;
  QStringList choices = {};
};

// Password managers and friends. Matched against the window class, case
// folded, as a substring -- so "org.keepassxc.KeePassXC" is caught by
// "keepassxc".
const QStringList kExcludedApps = {
    "bitwarden",  "keepassxc", "keepass",   "1password", "enpass",
    "seahorse",   "kwalletmanager",         "proton-pass", "authenticator",
    "gnome-keyring", "polkit",  "pinentry", "nala"};

const QVector<Spec> &specs() {
  static const QVector<Spec> table = {
      // Language model: any OpenAI-compatible server. Ollama by default,
      // because it is the one most people already have running.
      {"llm.enabled", Kind::Bool, true},
      {"llm.endpoint", Kind::Url, QStringLiteral("http://127.0.0.1:11434/v1")},
      // Empty means "whatever the server lists first", so nothing here
      // presumes a particular release.
      {"llm.model", Kind::String, QString()},
      {"llm.apiKey", Kind::String, QString()},
      {"llm.temperature", Kind::Real, 0.6, 0.0, 2.0},
      {"llm.maxTokens", Kind::Int, 800, 16, 32768},
      {"llm.contextTurns", Kind::Int, 8, 0, 50},
      {"llm.timeoutSec", Kind::Int, 90, 5, 600},
      {"llm.vision", Kind::Bool, false},
      {"llm.toolCalling", Kind::Bool, true},
      {"llm.systemPrompt", Kind::String, QString()},

      // Speech recognition: whisper.cpp, as a server or a one-shot binary.
      {"stt.enabled", Kind::Bool, true},
      {"stt.mode", Kind::Choice, QStringLiteral("auto"), 0, 0,
       {"auto", "server", "cli"}},
      // Not whisper-server's own default of 8080: that is Fish Speech's.
      {"stt.serverUrl", Kind::Url, QStringLiteral("http://127.0.0.1:8178")},
      {"stt.binary", Kind::String, QStringLiteral("whisper-cli")},
      {"stt.model", Kind::String, QString()},
      {"stt.language", Kind::String, QStringLiteral("en")},
      {"stt.device", Kind::String, QString()},
      {"stt.activation", Kind::Choice, QStringLiteral("push"), 0, 0,
       {"push", "wake", "always"}},
      {"stt.wakePhrases", Kind::List,
       QStringList{"hey nala", "okay nala", "ok nala", "hi nala", "nala"}},
      // Primes the recogniser with her name and the commands that matter.
      {"stt.prompt", Kind::String,
       QStringLiteral("Hey Nala. Pause screen memory. Resume screen memory. "
                      "Stop listening. Open settings.")},
      {"stt.vadThresholdDb", Kind::Real, 9.0, 3.0, 30.0},
      {"stt.silenceMs", Kind::Int, 700, 200, 3000},
      {"stt.maxUtteranceSec", Kind::Int, 20, 3, 60},

      // Voice: Fish Speech's local API server.
      {"tts.enabled", Kind::Bool, true},
      {"tts.engine", Kind::Choice, QStringLiteral("fish"), 0, 0,
       {"fish", "none"}},
      {"tts.endpoint", Kind::Url, QStringLiteral("http://127.0.0.1:8080")},
      {"tts.referenceId", Kind::String, QString()},
      {"tts.streaming", Kind::Bool, true},
      {"tts.device", Kind::String, QString()},
      {"tts.stylePrefix", Kind::String, QString()},
      {"tts.muted", Kind::Bool, false},
      {"tts.volume", Kind::Real, 0.9, 0.0, 1.0},

      // Computer use.
      {"agent.enabled", Kind::Bool, true},
      {"agent.confirm", Kind::Choice, QStringLiteral("risky"), 0, 0,
       {"everything", "risky", "high"}},
      {"agent.input", Kind::Bool, true},
      {"agent.files", Kind::Bool, true},
      {"agent.fileRoots", Kind::List, QStringList()},
      {"agent.browser", Kind::Bool, true},
      {"agent.shell", Kind::Bool, false},
      {"agent.maxSteps", Kind::Int, 10, 1, 30},

      // Screen memory. Off until asked for.
      {"memory.enabled", Kind::Bool, false},
      {"memory.paused", Kind::Bool, false},
      {"memory.pausedUntil", Kind::Real, 0.0, 0.0, 1e15},
      {"memory.intervalSec", Kind::Int, 30, 5, 3600},
      {"memory.scope", Kind::Choice, QStringLiteral("window"), 0, 0,
       {"window", "monitor"}},
      {"memory.dedupe", Kind::Bool, true},
      {"memory.dedupeDistance", Kind::Int, 6, 0, 32},
      {"memory.screenshotDays", Kind::Int, 7, 0, 3650},
      {"memory.semanticDays", Kind::Int, 0, 0, 3650},
      {"memory.maxStorageMB", Kind::Int, 5120, 50, 1000000},
      {"memory.jpegQuality", Kind::Int, 70, 30, 95},
      {"memory.maxWidth", Kind::Int, 1280, 480, 3840},
      {"memory.describe", Kind::Bool, false},
      {"memory.requireWindowInfo", Kind::Bool, true},

      // Privacy.
      {"privacy.blockSensitive", Kind::Bool, true},
      {"privacy.visionFilter", Kind::Bool, false},
      {"privacy.excludedApps", Kind::List, kExcludedApps},
      {"privacy.excludedTitles", Kind::List, QStringList()},

      {"ui.speechBubbles", Kind::Bool, true},
      {"developer.debug", Kind::Bool, false},
  };
  return table;
}

const Spec *find(const QString &key) {
  for (const Spec &spec : specs())
    if (key == QLatin1String(spec.key))
      return &spec;
  return nullptr;
}

// The value as it should be stored, or an invalid QVariant if it will not do.
QVariant accept(const Spec &spec, const QVariant &value) {
  switch (spec.kind) {
  case Kind::Bool:
    if (value.typeId() == QMetaType::Bool)
      return value;
    return {};
  case Kind::String:
    if (value.canConvert<QString>() && value.typeId() != QMetaType::Bool)
      return value.toString().left(8000);
    return {};
  case Kind::Url: {
    const QUrl url(value.toString().trimmed(), QUrl::StrictMode);
    if (url.isValid() && (url.scheme() == "http" || url.scheme() == "https") &&
        !url.host().isEmpty())
      return url.toString(QUrl::StripTrailingSlash);
    return {};
  }
  case Kind::Int: {
    bool ok = false;
    const double number = value.toDouble(&ok);
    if (!ok || number != std::floor(number) || number < spec.lo ||
        number > spec.hi)
      return {};
    return int(number);
  }
  case Kind::Real: {
    bool ok = false;
    const double number = value.toDouble(&ok);
    if (!ok || !std::isfinite(number) || number < spec.lo || number > spec.hi)
      return {};
    return number;
  }
  case Kind::Choice:
    if (spec.choices.contains(value.toString()))
      return value.toString();
    return {};
  case Kind::List: {
    if (value.typeId() != QMetaType::QStringList &&
        value.typeId() != QMetaType::QVariantList)
      return {};
    QStringList items;
    for (const QString &item : value.toStringList()) {
      const QString trimmed = item.trimmed();
      if (!trimmed.isEmpty() && !items.contains(trimmed))
        items << trimmed.left(500);
    }
    return items.mid(0, 500);
  }
  }
  return {};
}

} // namespace

AssistantSettings::AssistantSettings(QString path, bool persistent,
                                     QObject *parent)
    : QObject(parent), m_path(std::move(path)), m_persistent(persistent),
      m_values(defaults()) {
  m_saveTimer.setSingleShot(true);
  m_saveTimer.setInterval(400);
  connect(&m_saveTimer, &QTimer::timeout, this, &AssistantSettings::save);
  load();
}

QVariantMap AssistantSettings::defaults() {
  QVariantMap map;
  for (const Spec &spec : specs())
    map.insert(QString::fromLatin1(spec.key), spec.fallback);
  return map;
}

QVariant AssistantSettings::value(const QString &key) const {
  return m_values.value(key);
}

bool AssistantSettings::set(const QString &key, const QVariant &value) {
  const Spec *spec = find(key);
  if (!spec)
    return false;
  const QVariant accepted = accept(*spec, value);
  if (!accepted.isValid())
    return false;
  if (m_values.value(key) == accepted)
    return true;
  m_values.insert(key, accepted);
  emit changed(key);
  m_saveTimer.start();
  return true;
}

void AssistantSettings::reset(const QString &key) {
  if (const Spec *spec = find(key))
    set(key, spec->fallback);
}

void AssistantSettings::load() {
  QFile file(m_path);
  if (!file.open(QIODevice::ReadOnly))
    return;
  const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
  // One bad entry costs that entry, not the file.
  for (auto it = json.begin(); it != json.end(); ++it)
    if (const Spec *spec = find(it.key())) {
      const QVariant accepted = accept(*spec, it.value().toVariant());
      if (accepted.isValid())
        m_values.insert(it.key(), accepted);
    }
}

void AssistantSettings::save() {
  if (!m_persistent)
    return;
  QDir().mkpath(QFileInfo(m_path).absolutePath());
  QSaveFile file(m_path);
  if (!file.open(QIODevice::WriteOnly))
    return;
  // It may hold an API key, so it is the user's alone.
  file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  file.write(QJsonDocument(QJsonObject::fromVariantMap(m_values))
                 .toJson(QJsonDocument::Indented));
  file.commit();
}

void AssistantSettings::flush() {
  m_saveTimer.stop();
  save();
}
