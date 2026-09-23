#include "tools.h"

#include <cmath>

void ToolRegistry::add(Tool tool) {
  Q_ASSERT(!find(tool.name));
  m_tools.append(std::move(tool));
}

const Tool *ToolRegistry::find(const QString &name) const {
  for (const Tool &tool : m_tools)
    if (tool.name == name)
      return &tool;
  return nullptr;
}

QString ToolRegistry::wireName(const QString &name) {
  return QString(name).replace('.', '_');
}

QString ToolRegistry::fromWire(const QString &wire) const {
  for (const Tool &tool : m_tools)
    if (tool.name == wire || wireName(tool.name) == wire)
      return tool.name;
  return {};
}

QJsonArray ToolRegistry::schema(const QSet<QString> &categories) const {
  QJsonArray out;
  for (const Tool &tool : m_tools) {
    if (!categories.contains(tool.category))
      continue;
    out.append(QJsonObject{
        {"type", "function"},
        {"function", QJsonObject{{"name", wireName(tool.name)},
                                 {"description", tool.description},
                                 {"parameters", tool.parameters}}}});
  }
  return out;
}

QString ToolRegistry::validate(const QJsonObject &schema,
                               const QJsonObject &args) {
  const QJsonObject properties = schema.value("properties").toObject();
  for (const QJsonValue &name : schema.value("required").toArray())
    if (!args.contains(name.toString()))
      return QStringLiteral("missing \"%1\"").arg(name.toString());

  for (auto it = args.begin(); it != args.end(); ++it) {
    if (!properties.contains(it.key()))
      return QStringLiteral("unexpected \"%1\"").arg(it.key());
    const QJsonObject spec = properties.value(it.key()).toObject();
    const QString type = spec.value("type").toString();
    const QJsonValue value = it.value();
    const QString where = QStringLiteral("\"%1\"").arg(it.key());

    if (type == "string") {
      if (!value.isString())
        return where + " must be text";
      const int max = spec.value("maxLength").toInt(0);
      if (max > 0 && value.toString().size() > max)
        return where + QStringLiteral(" is longer than %1").arg(max);
      const QJsonArray allowed = spec.value("enum").toArray();
      if (!allowed.isEmpty() && !allowed.contains(value))
        return where + " is not one of the allowed values";
    } else if (type == "integer") {
      const double number = value.toDouble(NAN);
      if (!value.isDouble() || number != std::floor(number))
        return where + " must be a whole number";
      if (spec.contains("minimum") && number < spec.value("minimum").toDouble())
        return where + " is too small";
      if (spec.contains("maximum") && number > spec.value("maximum").toDouble())
        return where + " is too large";
    } else if (type == "number") {
      if (!value.isDouble())
        return where + " must be a number";
    } else if (type == "boolean") {
      if (!value.isBool())
        return where + " must be true or false";
    } else if (type == "array") {
      if (!value.isArray())
        return where + " must be a list";
    }
  }
  return {};
}

namespace schema {

QJsonObject object(const QJsonObject &properties, const QStringList &required) {
  QJsonObject out{{"type", "object"}, {"properties", properties}};
  if (!required.isEmpty())
    out.insert("required", QJsonArray::fromStringList(required));
  return out;
}

QJsonObject string(const QString &description, int maxLength) {
  return {{"type", "string"},
          {"description", description},
          {"maxLength", maxLength}};
}

QJsonObject integer(const QString &description, int minimum, int maximum) {
  return {{"type", "integer"},
          {"description", description},
          {"minimum", minimum},
          {"maximum", maximum}};
}

QJsonObject boolean(const QString &description) {
  return {{"type", "boolean"}, {"description", description}};
}

QJsonObject oneOf(const QString &description, const QStringList &values) {
  return {{"type", "string"},
          {"description", description},
          {"enum", QJsonArray::fromStringList(values)}};
}

} // namespace schema
