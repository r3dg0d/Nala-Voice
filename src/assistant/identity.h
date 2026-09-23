#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

class AssistantSettings;

// Who the assistant is: the name she answers to, the phrases that wake her,
// how she talks, and her voice. The desktop pet is always Nala the project;
// the assistant living in her can be called anything, and nothing in the AI
// side of the code writes the name down itself -- it asks this.
struct Identity {
  QString name = QStringLiteral("Nala");
  QStringList phrases{QStringLiteral("hey nala")}; // enabled wake phrases
  bool acceptName = false;   // the bare name wakes her too
  QString personality = QStringLiteral("friendly");
  QString customPersonality;
  QString responseLength = QStringLiteral("normal");
  QString expressiveness = QStringLiteral("normal");
  QString voice;             // Fish Speech reference id
  QString stylePrefix;

  static Identity from(const AssistantSettings &settings);

  // The phrases that should wake her, lower case, de-duplicated.
  QStringList wakePhrases() const;
  // Every way of addressing her that should be stripped from a request
  // before it is routed: the wake phrases plus "hey <name>", "<name>" and
  // friends, whether or not those wake her.
  QStringList addressForms() const;
  // Words whisper.cpp is primed with, so it hears her name and the commands
  // that matter.
  QString recognitionPrompt() const;
  // The model's system prompt: who she is, how to talk, what is true now.
  QString systemPrompt(const QDateTime &now, const QString &memoryStatus,
                       bool vision, const QString &custom = {}) const;
  // Pet expressiveness as a multiplier on her reactions.
  double expressiveScale() const;
};

// A shareable assistant profile: name, wake phrases, personality, voice and
// interface preferences. Never keys, endpoints' secrets, memories or
// recordings.
namespace profile {
QJsonObject exportProfile(const AssistantSettings &settings);
// Applies what it recognises; returns the keys it set.
QStringList importProfile(AssistantSettings &settings, const QJsonObject &json,
                          QString *error = nullptr);
QStringList keys();
} // namespace profile
