#include "memory.h"
#include "policy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <bit>

namespace {

constexpr QFileDevice::Permissions kPrivateDir =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
constexpr QFileDevice::Permissions kPrivateFile =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner;

qint64 ms(const QDateTime &t) { return t.toMSecsSinceEpoch(); }
QDateTime at(qint64 v) { return QDateTime::fromMSecsSinceEpoch(v); }

const char *kColumns =
    "id, started, last_seen, frames, app, title, activity, summary, keywords, "
    "urls, shot_path, shot_bytes, hash, pinned, source";

MemoryRecord readRecord(const QSqlQuery &q) {
  MemoryRecord r;
  r.id = q.value(0).toLongLong();
  r.started = at(q.value(1).toLongLong());
  r.lastSeen = at(q.value(2).toLongLong());
  r.frames = q.value(3).toInt();
  r.app = q.value(4).toString();
  r.title = q.value(5).toString();
  r.activity = q.value(6).toString();
  r.summary = q.value(7).toString();
  r.keywords = q.value(8).toString();
  r.urls = q.value(9).toString();
  r.shotPath = q.value(10).toString();
  r.shotBytes = q.value(11).toLongLong();
  r.hash = quint64(q.value(12).toLongLong());
  r.pinned = q.value(13).toBool();
  r.source = q.value(14).toString();
  return r;
}

Artifact readArtifact(const QSqlQuery &q) {
  Artifact a;
  a.id = q.value(0).toLongLong();
  a.kind = q.value(1).toString();
  a.value = q.value(2).toString();
  a.title = q.value(3).toString();
  a.firstSeen = at(q.value(4).toLongLong());
  a.lastSeen = at(q.value(5).toLongLong());
  a.memoryId = q.value(6).toLongLong();
  return a;
}

// Words that carry no search meaning in "what was that thing I looked at".
QStringList contentWords(const QString &text) {
  static const QSet<QString> stop = {
      "a",        "an",      "the",     "i",       "me",       "my",
      "mine",     "you",     "your",    "we",      "our",      "it",
      "its",      "is",      "was",     "were",    "are",      "be",
      "been",     "am",      "what",    "which",   "who",      "where",
      "when",     "why",     "how",     "that",    "this",     "those",
      "these",    "there",   "did",     "do",      "does",     "done",
      "didn't",   "don't",   "have",    "had",     "has",      "at",
      "on",       "in",      "of",      "for",     "to",       "from",
      "with",     "about",   "into",    "and",     "or",       "but",
      "look",     "looked",  "looking", "see",     "saw",      "seen",
      "open",     "opened",  "working", "work",    "worked",   "doing",
      "show",     "find",    "remember", "again",  "already",  "thing",
      "things",   "something", "stuff",  "nala",   "hey",      "please",
      "can",      "could",   "would",   "should",  "will",     "just",
      "any",      "some",    "all",     "one",     "up",       "then",
      "earlier",  "ago",     "recently", "time",   "last",     "yesterday",
      "today",    "tell",    "anything", "if",     "so",       "get",
      "got",      "use",     "used",    "using",   "made",     "make",
      "back",     "go",      "went",    "going",   "did",      "on"};
  static const QRegularExpression split(QStringLiteral(R"([^\p{L}\p{N}_.+#-]+)"));
  QStringList words;
  for (QString word : text.toLower().split(split, Qt::SkipEmptyParts)) {
    while (word.endsWith('.') || word.endsWith('-'))
      word.chop(1);
    if (word.size() >= 2 && !stop.contains(word) && !words.contains(word))
      words << word;
  }
  return words.mid(0, 12);
}

// An FTS5 query that cannot be abused: every term quoted, prefix matched.
QString ftsQuery(const QStringList &words) {
  QStringList terms;
  for (QString word : words) {
    word.remove(QRegularExpression(QStringLiteral(R"([^\p{L}\p{N}_]+)")));
    if (!word.isEmpty())
      terms << QStringLiteral("\"%1\"*").arg(word);
  }
  return terms.join(QStringLiteral(" OR "));
}

} // namespace

QVariantMap MemoryRecord::toVariant() const {
  return {{"id", id},
          {"started", started},
          {"lastSeen", lastSeen},
          {"frames", frames},
          {"app", app},
          {"title", title},
          {"activity", activity},
          {"summary", summary},
          {"keywords", keywords},
          {"urls", urls.split('\n', Qt::SkipEmptyParts)},
          {"shot", shotPath},
          {"hasShot", !shotPath.isEmpty()},
          {"pinned", pinned},
          {"source", source}};
}

QVariantMap Artifact::toVariant() const {
  return {{"id", id},       {"kind", kind},           {"value", value},
          {"title", title}, {"firstSeen", firstSeen}, {"lastSeen", lastSeen},
          {"memoryId", memoryId}};
}

// --- hashing -------------------------------------------------------------------

quint64 differenceHash(const QImage &image) {
  if (image.isNull())
    return 0;
  // 9x8 greyscale: each bit says whether a cell is brighter than the one to
  // its right.
  const QImage small = image.convertToFormat(QImage::Format_Grayscale8)
                           .scaled(9, 8, Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation);
  quint64 hash = 0;
  int bit = 0;
  for (int y = 0; y < 8; ++y) {
    const uchar *row = small.constScanLine(y);
    for (int x = 0; x < 8; ++x, ++bit)
      // A step has to be visible to count: in a flat area, compression
      // noise would otherwise flip bits at random.
      if (row[x] > row[x + 1] + 2)
        hash |= quint64(1) << bit;
  }
  return hash;
}

int hammingDistance(quint64 a, quint64 b) { return std::popcount(a ^ b); }

QVector<Artifact> findArtifacts(const QString &text) {
  QVector<Artifact> found;
  QSet<QString> seen;
  const auto add = [&](const QString &kind, QString value) {
    while (!value.isEmpty() && QStringLiteral(".,;:)]}'\"").contains(value.back()))
      value.chop(1);
    if (value.isEmpty() || seen.contains(kind + value))
      return;
    seen.insert(kind + value);
    Artifact artifact;
    artifact.kind = kind;
    artifact.value = value;
    found.append(artifact);
  };

  static const QRegularExpression url(
      QStringLiteral(R"(\bhttps?://[^\s<>"'`]+)"),
      QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression repo(QStringLiteral(
      R"(\b(?:https?://)?github\.com/([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+))"));
  // Bare "owner/repo" as GitHub puts it in its own page titles.
  static const QRegularExpression githubTitle(QStringLiteral(
      R"(\b(?:GitHub - )([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)(?::| · |$))"));
  static const QRegularExpression path(QStringLiteral(
      R"((?:^|[\s"'(\[])((?:~|/home/[^/\s]+|/tmp|/etc|/nix|/opt|/srv|/mnt|/media)(?:/[^\s"'<>|:*?]+)+))"));

  for (auto it = url.globalMatch(text); it.hasNext();)
    add(QStringLiteral("url"), it.next().captured(0));
  for (auto it = repo.globalMatch(text); it.hasNext();) {
    QString name = it.next().captured(1);
    if (name.endsWith(QStringLiteral(".git")))
      name.chop(4);
    add(QStringLiteral("repo"), QStringLiteral("https://github.com/") + name);
  }
  for (auto it = githubTitle.globalMatch(text); it.hasNext();)
    add(QStringLiteral("repo"),
        QStringLiteral("https://github.com/") + it.next().captured(1));
  for (auto it = path.globalMatch(text); it.hasNext();)
    add(QStringLiteral("file"), it.next().captured(1));
  return found;
}

// --- store ---------------------------------------------------------------------

MemoryStore::MemoryStore()
    : m_connection(QStringLiteral("nala-memory-") +
                   QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

MemoryStore::~MemoryStore() {
  {
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (db.isOpen())
      db.close();
  }
  if (QSqlDatabase::contains(m_connection))
    QSqlDatabase::removeDatabase(m_connection);
}

bool MemoryStore::open(const QString &dir, QString *error) {
  m_dir = dir;
  if (!QDir().mkpath(shotsDir())) {
    if (error)
      *error = QStringLiteral("cannot create %1").arg(dir);
    return false;
  }
  QFile::setPermissions(m_dir, kPrivateDir);
  QFile::setPermissions(shotsDir(), kPrivateDir);

  QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                              m_connection);
  db.setDatabaseName(m_dir + "/memory.db");
  if (!db.open()) {
    if (error)
      *error = db.lastError().text();
    return false;
  }
  QFile::setPermissions(m_dir + "/memory.db", kPrivateFile);

  QSqlQuery q(db);
  const QStringList schema = {
      "PRAGMA journal_mode=WAL",
      "PRAGMA foreign_keys=ON",
      "CREATE TABLE IF NOT EXISTS memories ("
      " id INTEGER PRIMARY KEY, started INTEGER NOT NULL,"
      " last_seen INTEGER NOT NULL, frames INTEGER NOT NULL DEFAULT 1,"
      " app TEXT, title TEXT, activity TEXT, summary TEXT, keywords TEXT,"
      " urls TEXT, shot_path TEXT, shot_bytes INTEGER NOT NULL DEFAULT 0,"
      " hash INTEGER, pinned INTEGER NOT NULL DEFAULT 0, source TEXT)",
      "CREATE INDEX IF NOT EXISTS memories_started ON memories(started)",
      "CREATE TABLE IF NOT EXISTS artifacts ("
      " id INTEGER PRIMARY KEY, kind TEXT NOT NULL, value TEXT NOT NULL,"
      " title TEXT, first_seen INTEGER, last_seen INTEGER, memory_id INTEGER,"
      " UNIQUE(kind, value))",
      "CREATE INDEX IF NOT EXISTS artifacts_seen ON artifacts(last_seen)",
  };
  for (const QString &statement : schema)
    if (!q.exec(statement)) {
      if (error)
        *error = q.lastError().text();
      return false;
    }

  // Full-text search where SQLite was built with it; plain LIKE otherwise.
  m_fts = q.exec(
      "CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts USING fts5("
      " title, app, activity, summary, keywords, urls,"
      " content='memories', content_rowid='id',"
      " tokenize='unicode61 remove_diacritics 2')");
  if (m_fts) {
    const QString cols = "title, app, activity, summary, keywords, urls";
    const QString news =
        "new.title, new.app, new.activity, new.summary, new.keywords, new.urls";
    const QString olds =
        "old.title, old.app, old.activity, old.summary, old.keywords, old.urls";
    q.exec(QStringLiteral("CREATE TRIGGER IF NOT EXISTS memories_ai AFTER "
                          "INSERT ON memories BEGIN INSERT INTO memories_fts("
                          "rowid, %1) VALUES (new.id, %2); END")
               .arg(cols, news));
    q.exec(QStringLiteral("CREATE TRIGGER IF NOT EXISTS memories_ad AFTER "
                          "DELETE ON memories BEGIN INSERT INTO memories_fts("
                          "memories_fts, rowid, %1) VALUES ('delete', old.id, "
                          "%2); END")
               .arg(cols, olds));
    q.exec(QStringLiteral(
               "CREATE TRIGGER IF NOT EXISTS memories_au AFTER UPDATE OF "
               "title, app, activity, summary, keywords, urls ON memories "
               "BEGIN INSERT INTO memories_fts(memories_fts, rowid, %1) "
               "VALUES ('delete', old.id, %2); INSERT INTO memories_fts("
               "rowid, %1) VALUES (new.id, %3); END")
               .arg(cols, olds, news));
  }
  m_open = true;
  return true;
}

qint64 MemoryStore::insert(MemoryRecord r) {
  if (!m_open)
    return 0;
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare("INSERT INTO memories (started, last_seen, frames, app, title, "
            "activity, summary, keywords, urls, shot_path, shot_bytes, hash, "
            "pinned, source) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  q.addBindValue(ms(r.started));
  q.addBindValue(ms(r.lastSeen.isValid() ? r.lastSeen : r.started));
  q.addBindValue(r.frames);
  q.addBindValue(r.app);
  q.addBindValue(r.title);
  q.addBindValue(r.activity);
  q.addBindValue(r.summary);
  q.addBindValue(r.keywords);
  q.addBindValue(r.urls);
  q.addBindValue(r.shotPath);
  q.addBindValue(r.shotBytes);
  q.addBindValue(qint64(r.hash));
  q.addBindValue(r.pinned ? 1 : 0);
  q.addBindValue(r.source);
  if (!q.exec())
    return 0;
  return q.lastInsertId().toLongLong();
}

void MemoryStore::touch(qint64 id, const QDateTime &seen) {
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare("UPDATE memories SET last_seen = ?, frames = frames + 1 "
            "WHERE id = ?");
  q.addBindValue(ms(seen));
  q.addBindValue(id);
  q.exec();
}

void MemoryStore::describe(qint64 id, const QString &activity,
                           const QString &summary, const QString &keywords,
                           const QString &urls) {
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare("UPDATE memories SET activity = ?, summary = ?, keywords = ?, "
            "urls = ? WHERE id = ?");
  q.addBindValue(activity.left(80));
  q.addBindValue(summary.left(2000));
  q.addBindValue(keywords.left(500));
  q.addBindValue(urls.left(4000));
  q.addBindValue(id);
  q.exec();
}

bool MemoryStore::setPinned(qint64 id, bool pinned) {
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare("UPDATE memories SET pinned = ? WHERE id = ?");
  q.addBindValue(pinned ? 1 : 0);
  q.addBindValue(id);
  return q.exec() && q.numRowsAffected() > 0;
}

MemoryRecord MemoryStore::get(qint64 id) const {
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare(QStringLiteral("SELECT %1 FROM memories WHERE id = ?")
                .arg(QLatin1String(kColumns)));
  q.addBindValue(id);
  if (q.exec() && q.next())
    return readRecord(q);
  return {};
}

MemoryRecord MemoryStore::latest() const {
  QSqlQuery q(QSqlDatabase::database(m_connection));
  if (q.exec(QStringLiteral("SELECT %1 FROM memories ORDER BY last_seen DESC "
                            "LIMIT 1")
                 .arg(QLatin1String(kColumns))) &&
      q.next())
    return readRecord(q);
  return {};
}

void MemoryStore::removeShot(const QString &path) {
  // Only ever delete inside our own directory, whatever the row says.
  if (path.isEmpty())
    return;
  const QString canonical = QFileInfo(path).canonicalFilePath();
  const QString root = QFileInfo(shotsDir()).canonicalFilePath();
  if (!canonical.isEmpty() && !root.isEmpty() &&
      canonical.startsWith(root + "/"))
    QFile::remove(canonical);
}

int MemoryStore::forget(const QDateTime &from, const QDateTime &to) {
  if (!m_open)
    return 0;
  QSqlDatabase db = QSqlDatabase::database(m_connection);
  QSqlQuery q(db);
  q.prepare("SELECT id, shot_path FROM memories WHERE last_seen >= ? AND "
            "started < ?");
  q.addBindValue(ms(from));
  q.addBindValue(ms(to));
  QVector<qint64> ids;
  if (q.exec())
    while (q.next()) {
      ids << q.value(0).toLongLong();
      removeShot(q.value(1).toString());
    }
  db.transaction();
  QSqlQuery del(db);
  del.prepare("DELETE FROM memories WHERE id = ?");
  QSqlQuery art(db);
  art.prepare("DELETE FROM artifacts WHERE memory_id = ?");
  for (qint64 id : ids) {
    del.addBindValue(id);
    del.exec();
    art.addBindValue(id);
    art.exec();
  }
  // Anything last seen in that window goes too, even if it was first seen
  // before it.
  QSqlQuery span(db);
  span.prepare("DELETE FROM artifacts WHERE last_seen >= ? AND last_seen < ?");
  span.addBindValue(ms(from));
  span.addBindValue(ms(to));
  span.exec();
  db.commit();
  return int(ids.size());
}

int MemoryStore::forgetOne(qint64 id) {
  const MemoryRecord record = get(id);
  if (record.id == 0)
    return 0;
  removeShot(record.shotPath);
  QSqlDatabase db = QSqlDatabase::database(m_connection);
  QSqlQuery q(db);
  q.prepare("DELETE FROM artifacts WHERE memory_id = ?");
  q.addBindValue(id);
  q.exec();
  q.prepare("DELETE FROM memories WHERE id = ?");
  q.addBindValue(id);
  return q.exec() ? q.numRowsAffected() : 0;
}

int MemoryStore::dropScreenshotsBefore(const QDateTime &before) {
  QSqlDatabase db = QSqlDatabase::database(m_connection);
  QSqlQuery q(db);
  q.prepare("SELECT id, shot_path FROM memories WHERE shot_path != '' AND "
            "pinned = 0 AND last_seen < ?");
  q.addBindValue(ms(before));
  QVector<qint64> ids;
  if (q.exec())
    while (q.next()) {
      ids << q.value(0).toLongLong();
      removeShot(q.value(1).toString());
    }
  QSqlQuery clear(db);
  clear.prepare("UPDATE memories SET shot_path = '', shot_bytes = 0 "
                "WHERE id = ?");
  db.transaction();
  for (qint64 id : ids) {
    clear.addBindValue(id);
    clear.exec();
  }
  db.commit();
  return int(ids.size());
}

MemoryStore::Sweep MemoryStore::enforce(int shotDays, int rowDays,
                                        qint64 maxBytes, const QDateTime &now) {
  Sweep sweep;
  if (!m_open)
    return sweep;
  const qint64 before = storageBytes();
  if (shotDays > 0)
    sweep.screenshots += dropScreenshotsBefore(now.addDays(-shotDays));

  QSqlDatabase db = QSqlDatabase::database(m_connection);
  if (rowDays > 0) {
    QSqlQuery q(db);
    q.prepare("SELECT id FROM memories WHERE pinned = 0 AND last_seen < ?");
    q.addBindValue(ms(now.addDays(-rowDays)));
    QVector<qint64> ids;
    if (q.exec())
      while (q.next())
        ids << q.value(0).toLongLong();
    for (qint64 id : ids)
      sweep.rows += forgetOne(id);
  }

  // Over the cap: the oldest unpinned images, and only as many as it takes.
  const qint64 excess = maxBytes > 0 ? storageBytes() - maxBytes : 0;
  if (excess > 0) {
    QSqlQuery q(db);
    q.prepare("SELECT id, shot_path, shot_bytes FROM memories WHERE "
              "shot_path != '' AND pinned = 0 ORDER BY last_seen ASC");
    QVector<qint64> ids;
    qint64 freed = 0;
    if (q.exec())
      while (freed < excess && q.next()) {
        ids << q.value(0).toLongLong();
        removeShot(q.value(1).toString());
        freed += q.value(2).toLongLong();
      }
    // Whatever is left over the cap is pinned; that is not ours to remove.
    QSqlQuery clear(db);
    clear.prepare("UPDATE memories SET shot_path = '', shot_bytes = 0 "
                  "WHERE id = ?");
    db.transaction();
    for (qint64 id : ids) {
      clear.addBindValue(id);
      clear.exec();
    }
    db.commit();
    sweep.screenshots += int(ids.size());
  }
  sweep.bytes = std::max<qint64>(0, before - storageBytes());
  return sweep;
}

qint64 MemoryStore::storageBytes() const {
  if (!m_open)
    return 0;
  qint64 total = 0;
  QSqlQuery q(QSqlDatabase::database(m_connection));
  if (q.exec("SELECT COALESCE(SUM(shot_bytes), 0) FROM memories") && q.next())
    total += q.value(0).toLongLong();
  for (const char *file : {"/memory.db", "/memory.db-wal"})
    total += QFileInfo(m_dir + file).size();
  return total;
}

int MemoryStore::count() const {
  QSqlQuery q(QSqlDatabase::database(m_connection));
  if (q.exec("SELECT COUNT(*) FROM memories") && q.next())
    return q.value(0).toInt();
  return 0;
}

QVector<MemoryRecord> MemoryStore::search(const QString &query,
                                          const QDateTime &now,
                                          int limit) const {
  QVector<MemoryRecord> out;
  if (!m_open)
    return out;
  const TimeRange range = parseTimeRange(query, now);
  const QStringList words = contentWords(range.rest);
  QSqlDatabase db = QSqlDatabase::database(m_connection);

  if (words.isEmpty())
    return list(range.valid ? range.from : QDateTime(),
                range.valid ? range.to : QDateTime(), {}, {}, limit);

  QSqlQuery q(db);
  QString when;
  if (range.valid)
    when = QStringLiteral(" AND m.last_seen >= ? AND m.started < ?");
  if (m_fts) {
    q.prepare(QStringLiteral("SELECT %1 FROM memories_fts f JOIN memories m "
                             "ON m.id = f.rowid WHERE memories_fts MATCH ?%2 "
                             "ORDER BY bm25(memories_fts), m.last_seen DESC "
                             "LIMIT ?")
                  .arg(QString::fromLatin1(kColumns)
                           .replace(QRegularExpression(QStringLiteral(
                                        R"(\b(\w+)\b)")),
                                    QStringLiteral("m.\\1")),
                       when));
    q.addBindValue(ftsQuery(words));
  } else {
    QStringList likes;
    for (int i = 0; i < words.size(); ++i)
      likes << QStringLiteral("(m.title LIKE ? OR m.summary LIKE ? OR "
                              "m.keywords LIKE ? OR m.urls LIKE ? OR "
                              "m.app LIKE ?)");
    q.prepare(QStringLiteral("SELECT %1 FROM memories m WHERE (%2)%3 "
                             "ORDER BY m.last_seen DESC LIMIT ?")
                  .arg(QString::fromLatin1(kColumns)
                           .replace(QRegularExpression(QStringLiteral(
                                        R"(\b(\w+)\b)")),
                                    QStringLiteral("m.\\1")),
                       likes.join(" OR "), when));
    for (const QString &word : words)
      for (int i = 0; i < 5; ++i)
        q.addBindValue("%" + word + "%");
  }
  if (range.valid) {
    q.addBindValue(ms(range.from));
    q.addBindValue(ms(range.to));
  }
  q.addBindValue(limit);
  if (q.exec())
    while (q.next())
      out << readRecord(q);
  return out;
}

QVector<MemoryRecord> MemoryStore::list(const QDateTime &from,
                                        const QDateTime &to, const QString &app,
                                        const QString &text, int limit) const {
  QVector<MemoryRecord> out;
  if (!m_open)
    return out;
  QStringList where;
  QVariantList binds;
  if (from.isValid()) {
    where << "last_seen >= ?";
    binds << ms(from);
  }
  if (to.isValid()) {
    where << "started < ?";
    binds << ms(to);
  }
  if (!app.isEmpty()) {
    where << "app = ?";
    binds << app;
  }
  if (!text.trimmed().isEmpty()) {
    where << "(title LIKE ? OR summary LIKE ? OR keywords LIKE ? OR urls LIKE ?)";
    for (int i = 0; i < 4; ++i)
      binds << "%" + text.trimmed() + "%";
  }
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare(QStringLiteral("SELECT %1 FROM memories%2 ORDER BY started DESC "
                           "LIMIT ?")
                .arg(QLatin1String(kColumns),
                     where.isEmpty() ? QString()
                                     : " WHERE " + where.join(" AND ")));
  for (const QVariant &bind : binds)
    q.addBindValue(bind);
  q.addBindValue(limit);
  if (q.exec())
    while (q.next())
      out << readRecord(q);
  return out;
}

QStringList MemoryStore::apps() const {
  QStringList out;
  QSqlQuery q(QSqlDatabase::database(m_connection));
  if (q.exec("SELECT app, COUNT(*) c FROM memories WHERE app != '' "
             "GROUP BY app ORDER BY c DESC LIMIT 100"))
    while (q.next())
      out << q.value(0).toString();
  return out;
}

qint64 MemoryStore::addArtifact(const Artifact &a) {
  if (!m_open || a.value.isEmpty())
    return 0;
  const qint64 seen =
      ms(a.lastSeen.isValid() ? a.lastSeen : QDateTime::currentDateTime());
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare("INSERT INTO artifacts (kind, value, title, first_seen, last_seen, "
            "memory_id) VALUES (?,?,?,?,?,?) ON CONFLICT(kind, value) DO "
            "UPDATE SET last_seen = excluded.last_seen, memory_id = "
            "excluded.memory_id, title = COALESCE(NULLIF(excluded.title, ''), "
            "title)");
  q.addBindValue(a.kind);
  q.addBindValue(a.value.left(2000));
  q.addBindValue(a.title.left(300));
  q.addBindValue(a.firstSeen.isValid() ? ms(a.firstSeen) : seen);
  q.addBindValue(seen);
  q.addBindValue(a.memoryId);
  return q.exec() ? q.lastInsertId().toLongLong() : 0;
}

QVector<Artifact> MemoryStore::artifacts(const QString &text,
                                         const QDateTime &from,
                                         const QDateTime &to,
                                         int limit) const {
  QVector<Artifact> out;
  if (!m_open)
    return out;
  QStringList where;
  QVariantList binds;
  const QStringList words = contentWords(text);
  if (!words.isEmpty()) {
    QStringList any;
    for (const QString &word : words) {
      any << "(value LIKE ? OR title LIKE ? OR kind = ?)";
      binds << "%" + word + "%" << "%" + word + "%" << word;
    }
    where << "(" + any.join(" OR ") + ")";
  }
  if (from.isValid()) {
    where << "last_seen >= ?";
    binds << ms(from);
  }
  if (to.isValid()) {
    where << "first_seen < ?";
    binds << ms(to);
  }
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare(QStringLiteral("SELECT id, kind, value, title, first_seen, "
                           "last_seen, memory_id FROM artifacts%1 ORDER BY "
                           "last_seen DESC LIMIT ?")
                .arg(where.isEmpty() ? QString()
                                     : " WHERE " + where.join(" AND ")));
  for (const QVariant &bind : binds)
    q.addBindValue(bind);
  q.addBindValue(limit);
  if (q.exec())
    while (q.next())
      out << readArtifact(q);
  return out;
}

QVector<Artifact> MemoryStore::artifactsFor(qint64 memoryId) const {
  QVector<Artifact> out;
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare("SELECT id, kind, value, title, first_seen, last_seen, memory_id "
            "FROM artifacts WHERE memory_id = ? ORDER BY last_seen DESC");
  q.addBindValue(memoryId);
  if (q.exec())
    while (q.next())
      out << readArtifact(q);
  return out;
}
