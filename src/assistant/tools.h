#pragma once
#include "policy.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

// The only way the model touches the machine. Each tool is typed (a JSON
// schema the arguments are checked against before anything runs), carries a
// risk that decides whether she must ask first, and belongs to a category the
// user can switch off wholesale.
struct Tool {
  using Done = std::function<void(QJsonObject result)>;

  QString name;        // "apps.launch"
  QString description; // for the model
  QJsonObject parameters; // JSON schema, type "object"
  Risk risk = Risk::Low;
  QString category;    // "computer", "window", "apps", "files", "browser",
                       // "shell", "memory", "nala"
  // How to ask about it: "Close Firefox?". Defaults to the name.
  std::function<QString(const QJsonObject &)> summary;
  // Some tools are riskier with some arguments (overwriting versus creating).
  std::function<Risk(const QJsonObject &)> riskFor;
  std::function<void(const QJsonObject &, Done)> run;
};

class ToolRegistry {
public:
  void add(Tool tool);
  const Tool *find(const QString &name) const;
  const QVector<Tool> &tools() const { return m_tools; }

  // The OpenAI "tools" array for the categories that are switched on. Tool
  // names are sent with dots swapped for underscores, which some servers
  // insist on; wireName() and fromWire() convert.
  QJsonArray schema(const QSet<QString> &categories) const;
  static QString wireName(const QString &name);
  QString fromWire(const QString &wire) const;

  // Empty if the arguments satisfy the schema, otherwise what is wrong.
  static QString validate(const QJsonObject &schema, const QJsonObject &args);

private:
  QVector<Tool> m_tools;
};

// Schema-building shorthand.
namespace schema {
QJsonObject object(const QJsonObject &properties,
                   const QStringList &required = {});
QJsonObject string(const QString &description, int maxLength = 2000);
QJsonObject integer(const QString &description, int minimum, int maximum);
QJsonObject boolean(const QString &description);
QJsonObject oneOf(const QString &description, const QStringList &values);
} // namespace schema
