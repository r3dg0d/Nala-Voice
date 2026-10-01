#include "accessibility.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QQueue>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <QtConcurrent>

namespace {
struct Ref {
  QString bus, path;
};
QVector<Ref> children(const QDBusMessage &reply) {
  QVector<Ref> out;
  if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
    return out;
  const auto a = qvariant_cast<QDBusArgument>(reply.arguments().first());
  a.beginArray();
  while (!a.atEnd() && out.size() < 256) {
    QString bus;
    QDBusObjectPath path;
    a.beginStructure();
    a >> bus >> path;
    a.endStructure();
    if (!bus.isEmpty() && path.path() != "/org/a11y/atspi/null")
      out << Ref{bus, path.path()};
  }
  a.endArray();
  return out;
}
QDBusMessage call(const QDBusConnection &bus, Ref ref, QString iface,
                  QString method, QVariantList args = {}) {
  auto message =
      QDBusMessage::createMethodCall(ref.bus, ref.path, iface, method);
  message.setArguments(args);
  return bus.call(message, QDBus::Block, 80);
}
QVector<accessibility::Element> readTree(int pid) {
  QVector<accessibility::Element> out;
  if (pid <= 0)
    return out;
  const auto address =
      call(QDBusConnection::sessionBus(), {"org.a11y.Bus", "/org/a11y/bus"},
           "org.a11y.Bus", "GetAddress");
  if (address.type() != QDBusMessage::ReplyMessage ||
      address.arguments().isEmpty())
    return out;
  const auto connection = QUuid::createUuid().toString();
  {
    const auto bus = QDBusConnection::connectToBus(
        address.arguments().first().toString(), connection);
    QElapsedTimer deadline;
    deadline.start();
    const QString accessible = "org.a11y.atspi.Accessible";
    QQueue<Ref> queue;
    const auto roots = children(call(
        bus, {"org.a11y.atspi.Registry", "/org/a11y/atspi/accessible/root"},
        accessible, "GetChildren"));
    for (auto root : roots) {
      const auto process = call(
          bus, {"org.freedesktop.DBus", "/org/freedesktop/DBus"},
          "org.freedesktop.DBus", "GetConnectionUnixProcessID", {root.bus});
      if (!process.arguments().isEmpty() &&
          process.arguments().first().toInt() == pid)
        queue.enqueue(root);
      if (deadline.elapsed() > 1500)
        break;
    }
    QSet<QString> seen;
    while (!queue.isEmpty() && seen.size() < 256 && deadline.elapsed() < 1500) {
      const auto ref = queue.dequeue();
      const auto key = ref.bus + ref.path;
      if (seen.contains(key))
        continue;
      seen.insert(key);
      const auto properties = call(bus, ref, "org.freedesktop.DBus.Properties",
                                   "GetAll", {accessible});
      const auto props =
          properties.arguments().isEmpty()
              ? QVariantMap{}
              : qdbus_cast<QVariantMap>(properties.arguments().first());
      const auto name = props.value("Name").toString();
      if (!name.isEmpty() && name.size() <= 300) {
        const auto state = call(bus, ref, accessible, "GetState");
        const auto words =
            state.arguments().isEmpty()
                ? QList<uint>{}
                : qdbus_cast<QList<uint>>(state.arguments().first());
        const uint bits = words.value(0);
        const bool usable = (bits & (1u << 8)) && (bits & (1u << 24)) &&
                            (bits & (1u << 25)) && (bits & (1u << 30)) &&
                            !(bits & ((1u << 6) | (1u << 27)));
        if (usable) {
          const auto extents = call(bus, ref, "org.a11y.atspi.Component",
                                    "GetExtents", {uint(0)});
          const auto role = call(bus, ref, accessible, "GetRoleName");
          if (extents.type() == QDBusMessage::ReplyMessage &&
              !extents.arguments().isEmpty()) {
            const auto a =
                qvariant_cast<QDBusArgument>(extents.arguments().first());
            int x = 0, y = 0, w = 0, h = 0;
            a.beginStructure();
            a >> x >> y >> w >> h;
            a.endStructure();
            out << accessibility::Element{name,
                                          role.arguments().value(0).toString(),
                                          ref.bus,
                                          ref.path,
                                          QRect(x, y, w, h),
                                          true,
                                          true};
          }
        }
      }
      for (auto child : children(call(bus, ref, accessible, "GetChildren")))
        if (child.bus == ref.bus && queue.size() < 256)
          queue.enqueue(child);
    }
  }
  QDBusConnection::disconnectFromBus(connection);
  return out;
}
} // namespace

accessibility::Match accessibility::select(const QVector<Element> &elements,
                                           QString label, QRect window) {
  label = label.simplified().toCaseFolded();
  static const QRegularExpression suffix(
      "\\s+(button|field|menu item|checkbox|toggle)$");
  label.remove(suffix);
  Match match;
  int count = 0;
  for (const auto &e : elements) {
    if (!e.enabled || !e.showing || e.bounds.width() < 4 ||
        e.bounds.height() < 4 || !window.contains(e.bounds) ||
        e.name.simplified().toCaseFolded() != label)
      continue;
    const auto role = e.role.toLower();
    if (!(role.contains("button") || role.contains("entry") ||
          role.contains("text") || role.contains("menu item") ||
          role.contains("check box") || role.contains("combo box")))
      continue;
    match.bounds = e.bounds;
    ++count;
  }
  if (count > 1) {
    match.bounds = {};
    match.error =
        "Accessibility label is ambiguous; specify the control's context";
  }
  return match;
}

void accessibility::locate(QObject *owner, int pid, QRect window, QString label,
                           std::function<void(Match)> done) {
  auto *watcher = new QFutureWatcher<QVector<Element>>(owner);
  QObject::connect(watcher, &QFutureWatcher<QVector<Element>>::finished, owner,
                   [watcher, window, label, done] {
                     const auto result =
                         select(watcher->result(), label, window);
                     watcher->deleteLater();
                     done(result);
                   });
  watcher->setFuture(QtConcurrent::run([pid] { return readTree(pid); }));
}
