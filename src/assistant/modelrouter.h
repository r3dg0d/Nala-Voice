#pragma once
#include <QString>
#include <QStringList>

// Which local model answers a request. Pure: no network, no settings object,
// so the rules are testable and the cost is microseconds -- the router must
// never add more latency than it saves.
//
// Order of preference, cheapest first: the CommandRouter's deterministic fast
// path (never reaches here), then this lightweight classifier, then a fast
// model, then the main model.
namespace modelrouter {

enum class Role { Main, Fast, Speed };

QString roleName(Role role);          // "main" / "fast" / "speed"
bool parseRole(const QString &name, Role *role);

// What the request is, for the per-task overrides.
enum class Task { Conversation, Tools, Coding, Summary, Classify };
QString taskName(Task task);          // "conversation" / "tools" / ...

struct Choice {
  Role role = Role::Main;
  Task task = Task::Conversation;
  QString reason; // one short line, for the log and `nala model status`
};

struct Options {
  QString mode = QStringLiteral("auto"); // auto, main, fast, speed
  bool autoRouting = true;
  // Which role handles each task when routing automatically. An empty entry
  // means "let the classifier decide".
  QString conversation; // e.g. "speed": light chat goes to the A3B model
  QString tools;
  QString coding;
  QString summary;
  QString classify;
};

// Classify by the words alone (no model call). Heavy work -- reasoning,
// debugging, code, analysis, planning, long input -- goes to Main; short
// chatter and simple lookups go to Fast.
Task classifyTask(const QString &text);
Choice choose(const QString &text, const Options &options);

// Roles to try, in order, when `role`'s model is missing. `custom` is the user's
// configured order for that role ("main,fast,speed"); empty uses the default.
QList<Role> fallbackOrder(Role role, const QStringList &custom = {});

// Loose match of a configured name ("qwen3-30b-a3b") against what the server
// lists ("qwen3:30b-a3b"): case and punctuation are ignored. Empty when none.
QString findModel(const QString &configured, const QStringList &available);

struct Resolved {
  QString model;        // empty: nothing usable
  Role role = Role::Main; // the role whose model it is
  bool fellBack = false;
};
// The first usable model for `wanted`, following the fallback order. `names`
// maps roles to configured names in the order Main, Fast, Speed.
Resolved resolve(Role wanted, const QString names[3],
                 const QStringList &available,
                 const QStringList &customOrder = {});

// "use the fast model", "switch back to automatic model selection", "use the
// smart model for this". `persistent` is set only when the wording asks for it
// ("from now on", "always", "switch to"), so a passing request never rewrites
// the user's defaults.
struct ModelPhrase {
  QString mode;      // auto / main / fast / speed
  bool persistent = false;
};
bool parseModelPhrase(const QString &text, ModelPhrase *out);

} // namespace modelrouter
