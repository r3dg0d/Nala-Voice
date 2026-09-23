#pragma once
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

// Everything the assistant can be told, kept apart from preferences.json so
// the companion's own settings keep their shape.
//
// Keys are dotted ("llm.endpoint") and declared once, with their default and
// what counts as a valid value, in settings.cpp. Anything not declared is
// refused, and a value of the wrong type or out of range is refused rather
// than clamped -- the same rule Backend::configure() already follows.
class AssistantSettings : public QObject {
  Q_OBJECT
  // The whole table, for QML to bind against.
  Q_PROPERTY(QVariantMap values READ values NOTIFY changed)

public:
  explicit AssistantSettings(QString path, bool persistent = true,
                             QObject *parent = nullptr);

  QVariantMap values() const { return m_values; }

  Q_INVOKABLE QVariant value(const QString &key) const;
  // False if the key is unknown or the value is not acceptable for it.
  Q_INVOKABLE bool set(const QString &key, const QVariant &value);
  Q_INVOKABLE void reset(const QString &key);

  QString string(const QString &key) const { return value(key).toString(); }
  bool flag(const QString &key) const { return value(key).toBool(); }
  int integer(const QString &key) const { return value(key).toInt(); }
  double number(const QString &key) const { return value(key).toDouble(); }
  QStringList list(const QString &key) const {
    return value(key).toStringList();
  }

  static QVariantMap defaults();
  QString path() const { return m_path; }

  // Write now rather than after the debounce. Used before exit, and by the
  // privacy switch, which must survive a crash straight after it is pressed.
  void flush();

signals:
  void changed(const QString &key);

private:
  void load();
  void save();

  QString m_path;
  bool m_persistent = true;
  QVariantMap m_values;
  QTimer m_saveTimer;
};
