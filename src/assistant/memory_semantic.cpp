#include "memory.h"
#include "policy.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <algorithm>
#include <cmath>

int MemoryStore::schemaVersion() const {
  QSqlQuery q(QSqlDatabase::database(m_connection));
  return q.exec("PRAGMA user_version") && q.next() ? q.value(0).toInt() : -1;
}
bool MemoryStore::migrate(QString *error) {
  auto db = QSqlDatabase::database(m_connection);
  const int version = schemaVersion();
  if (version > 1) {
    if (error)
      *error = "Memory database is newer than this Nala version";
    return false;
  }
  if (version == 1)
    return true;
  if (!db.transaction()) {
    if (error)
      *error = db.lastError().text();
    return false;
  }
  const QStringList statements = {
      "CREATE TABLE memory_embeddings(memory_id INTEGER PRIMARY KEY REFERENCES "
      "memories(id) ON DELETE CASCADE, model TEXT NOT NULL, dimensions INTEGER "
      "NOT NULL, version INTEGER NOT NULL DEFAULT 1, content_hash TEXT NOT "
      "NULL, vector BLOB NOT NULL)",
      "CREATE INDEX embeddings_space ON memory_embeddings(model,dimensions)",
      "CREATE TABLE embedding_jobs(memory_id INTEGER PRIMARY KEY REFERENCES "
      "memories(id) ON DELETE CASCADE, attempts INTEGER NOT NULL DEFAULT 0, "
      "next_attempt INTEGER NOT NULL DEFAULT 0)",
      "CREATE TABLE entities(id INTEGER PRIMARY KEY,kind TEXT NOT NULL,name "
      "TEXT NOT NULL,UNIQUE(kind,name))",
      "CREATE TABLE entity_aliases(entity_id INTEGER REFERENCES entities(id) "
      "ON DELETE CASCADE,alias TEXT NOT NULL,PRIMARY KEY(entity_id,alias))",
      "CREATE TABLE memory_entities(memory_id INTEGER REFERENCES memories(id) "
      "ON DELETE CASCADE,entity_id INTEGER REFERENCES entities(id) ON DELETE "
      "CASCADE,PRIMARY KEY(memory_id,entity_id))",
      "CREATE INDEX entity_memories ON memory_entities(entity_id,memory_id)",
      "CREATE TABLE entity_relations(a INTEGER REFERENCES entities(id) ON "
      "DELETE CASCADE,b INTEGER REFERENCES entities(id) ON DELETE "
      "CASCADE,memory_id INTEGER REFERENCES memories(id) ON DELETE "
      "CASCADE,kind TEXT NOT NULL,PRIMARY KEY(a,b,memory_id,kind))",
      "CREATE TABLE durable_facts(id INTEGER PRIMARY KEY,subject TEXT NOT "
      "NULL,category TEXT NOT NULL,text TEXT NOT NULL,confidence REAL NOT "
      "NULL,importance REAL NOT NULL DEFAULT 0.8,status TEXT NOT NULL DEFAULT "
      "'active' CHECK(status "
      "IN('active','superseded','historical','contradicted')),created_at "
      "INTEGER NOT NULL,last_confirmed INTEGER NOT NULL)",
      "CREATE INDEX facts_subject ON durable_facts(subject,status)",
      "CREATE TABLE fact_sources(fact_id INTEGER REFERENCES durable_facts(id) "
      "ON DELETE CASCADE,memory_id INTEGER REFERENCES memories(id) ON DELETE "
      "CASCADE,PRIMARY KEY(fact_id,memory_id))",
      "CREATE INDEX source_facts ON fact_sources(memory_id,fact_id)",
      "CREATE TABLE fact_relations(fact_id INTEGER REFERENCES "
      "durable_facts(id) ON DELETE CASCADE,depends_on INTEGER REFERENCES "
      "durable_facts(id) ON DELETE CASCADE,PRIMARY "
      "KEY(fact_id,depends_on),CHECK(fact_id!=depends_on))",
      "CREATE TRIGGER semantic_insert AFTER INSERT ON memories BEGIN INSERT OR "
      "IGNORE INTO embedding_jobs(memory_id) VALUES(new.id); END",
      "CREATE TRIGGER semantic_update AFTER UPDATE OF "
      "title,app,activity,summary,keywords,urls ON memories BEGIN DELETE FROM "
      "memory_embeddings WHERE memory_id=new.id; INSERT INTO "
      "embedding_jobs(memory_id) VALUES(new.id) ON CONFLICT(memory_id) DO "
      "UPDATE SET attempts=0,next_attempt=0; END",
      "CREATE TRIGGER semantic_forget AFTER DELETE ON memories BEGIN DELETE "
      "FROM durable_facts WHERE NOT EXISTS(SELECT 1 FROM fact_sources s WHERE "
      "s.fact_id=durable_facts.id); DELETE FROM entities WHERE NOT "
      "EXISTS(SELECT 1 FROM memory_entities m WHERE m.entity_id=entities.id); "
      "END",
      "INSERT OR IGNORE INTO embedding_jobs(memory_id) SELECT id FROM memories",
      "CREATE TRIGGER artifact_semantic_insert AFTER INSERT ON artifacts WHEN "
      "new.memory_id>0 BEGIN DELETE FROM memory_embeddings WHERE "
      "memory_id=new.memory_id; INSERT OR IGNORE INTO "
      "embedding_jobs(memory_id) SELECT id FROM memories WHERE "
      "id=new.memory_id; END",
      "CREATE TRIGGER artifact_semantic_update AFTER UPDATE ON artifacts BEGIN "
      "DELETE FROM memory_embeddings WHERE memory_id "
      "IN(old.memory_id,new.memory_id); INSERT OR IGNORE INTO "
      "embedding_jobs(memory_id) SELECT id FROM memories WHERE id "
      "IN(old.memory_id,new.memory_id); END",
      "CREATE TRIGGER artifact_semantic_delete AFTER DELETE ON artifacts BEGIN "
      "DELETE FROM memory_embeddings WHERE memory_id=old.memory_id; INSERT OR "
      "IGNORE INTO embedding_jobs(memory_id) SELECT id FROM memories WHERE "
      "id=old.memory_id; END",
      "CREATE TRIGGER fact_forget BEFORE DELETE ON durable_facts BEGIN UPDATE "
      "durable_facts SET status='historical' WHERE id IN(SELECT fact_id FROM "
      "fact_relations WHERE depends_on=old.id); END",
      "INSERT OR IGNORE INTO entities(kind,name) SELECT 'application',app FROM "
      "memories WHERE app IS NOT NULL AND app!=''",
      "INSERT OR IGNORE INTO entities(kind,name) SELECT kind,value FROM "
      "artifacts WHERE memory_id IN(SELECT id FROM memories)",
      "INSERT OR IGNORE INTO memory_entities(memory_id,entity_id) SELECT "
      "m.id,e.id FROM memories m JOIN entities e ON e.kind='application' AND "
      "e.name=m.app",
      "INSERT OR IGNORE INTO memory_entities(memory_id,entity_id) SELECT "
      "a.memory_id,e.id FROM artifacts a JOIN entities e ON e.kind=a.kind AND "
      "e.name=a.value WHERE a.memory_id IN(SELECT id FROM memories)",
      "INSERT INTO "
      "durable_facts(subject,category,text,confidence,importance,created_at,"
      "last_confirmed) SELECT 'legacy-note:'||id,'reference',summary,0.9,CASE "
      "WHEN pinned=1 THEN 1 ELSE 0.8 END,started,last_seen FROM memories WHERE "
      "(source='note' OR pinned=1) AND summary IS NOT NULL AND summary!=''",
      "INSERT INTO fact_sources(fact_id,memory_id) SELECT f.id,m.id FROM "
      "durable_facts f JOIN memories m ON f.subject='legacy-note:'||m.id",
      "PRAGMA user_version=1"};
  QSqlQuery q(db);
  for (const auto &sql : statements)
    if (!q.exec(sql)) {
      if (error)
        *error = q.lastError().text();
      db.rollback();
      return false;
    }
  if (!db.commit()) {
    if (error)
      *error = db.lastError().text();
    db.rollback();
    return false;
  }
  return true;
}
QString MemoryStore::embeddingText(qint64 id) const {
  const auto r = get(id);
  if (!r.id)
    return {};
  QString text =
      QStringLiteral("Application: %1\nWindow: %2\nActivity: %3\nSummary: "
                     "%4\nKeywords: %5\nArtifacts: %6")
          .arg(r.app, r.title, r.activity, r.summary, r.keywords, r.urls);
  for (const auto &a : artifactsFor(id))
    text += '\n' + a.kind + ": " + a.value;
  return text.left(12000);
}
QVector<float> MemoryStore::cachedEmbedding(const QString &model,
                                            const QString &hash) const {
  if (!m_open)
    return {};
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare("SELECT vector FROM memory_embeddings WHERE model=? AND "
            "content_hash=? AND version=1 LIMIT 1");
  q.addBindValue(model);
  q.addBindValue(hash);
  return q.exec() && q.next() ? semantic::decode(q.value(0).toByteArray())
                              : QVector<float>();
}
QVector<qint64> MemoryStore::pendingEmbeddings(const QString &model,
                                               int dimensions, int limit) {
  if (!m_open)
    return {};
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare("INSERT OR IGNORE INTO embedding_jobs(memory_id) SELECT m.id FROM "
            "memories m LEFT JOIN memory_embeddings e ON e.memory_id=m.id "
            "WHERE e.memory_id IS NULL OR e.model!=? OR (? > 0 AND "
            "e.dimensions!=?) OR e.version!=1");
  q.addBindValue(model);
  q.addBindValue(dimensions);
  q.addBindValue(dimensions);
  q.exec();
  q.prepare("SELECT memory_id FROM embedding_jobs WHERE next_attempt<=? ORDER "
            "BY attempts,memory_id LIMIT ?");
  q.addBindValue(QDateTime::currentMSecsSinceEpoch());
  q.addBindValue(limit);
  QVector<qint64> ids;
  if (q.exec())
    while (q.next())
      ids << q.value(0).toLongLong();
  return ids;
}
bool MemoryStore::storeEmbedding(qint64 id, const QString &model,
                                 const QString &hash,
                                 const QVector<float> &vector) {
  const auto bytes = semantic::encode(vector);
  if (!m_open || model.isEmpty() || bytes.isEmpty() || !get(id).id ||
      semantic::hash(embeddingText(id)) != hash)
    return false;
  auto db = QSqlDatabase::database(m_connection);
  if (!db.transaction())
    return false;
  QSqlQuery q(db);
  q.prepare("INSERT INTO "
            "memory_embeddings(memory_id,model,dimensions,content_hash,vector) "
            "VALUES(?,?,?,?,?) ON CONFLICT(memory_id) DO UPDATE SET "
            "model=excluded.model,dimensions=excluded.dimensions,content_hash="
            "excluded.content_hash,vector=excluded.vector,version=1");
  q.addBindValue(id);
  q.addBindValue(model);
  q.addBindValue(vector.size());
  q.addBindValue(hash);
  q.addBindValue(bytes);
  if (!q.exec()) {
    db.rollback();
    return false;
  }
  q.prepare("DELETE FROM embedding_jobs WHERE memory_id=?");
  q.addBindValue(id);
  if (!q.exec()) {
    db.rollback();
    return false;
  }
  return db.commit();
}
void MemoryStore::embeddingFailed(qint64 id) {
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare(
      "UPDATE embedding_jobs SET attempts=min(attempts+1,10),next_attempt=? + "
      "min(3600000,30000*(1<<min(attempts,7))) WHERE memory_id=?");
  q.addBindValue(QDateTime::currentMSecsSinceEpoch());
  q.addBindValue(id);
  q.exec();
}
QVector<RetrievalHit>
MemoryStore::hybrid(const QString &query, const QDateTime &now,
                    const QVector<float> &vector, const QString &model,
                    int lexicalK, int denseK, int rrfK, int finalK) const {
  if (!m_open)
    return {};
  lexicalK = std::clamp(lexicalK, 0, 200);
  denseK = std::clamp(denseK, 0, 200);
  finalK = std::clamp(finalK, 1, 200);
  const auto range = parseTimeRange(query, now);
  const auto within = [&](const MemoryRecord &r) {
    return r.id &&
           (!range.valid || (r.lastSeen >= range.from && r.started < range.to));
  };
  QVector<qint64> lexical, dense, artifact, entity;
  QMap<qint64, double> similarities;
  for (const auto &r : search(query, now, lexicalK))
    lexical << r.id;
  if (!vector.isEmpty() && denseK > 0) {
    QSqlQuery q(QSqlDatabase::database(m_connection));
    QString sql = "SELECT e.memory_id,e.vector,e.content_hash FROM "
                  "memory_embeddings e JOIN memories m ON m.id=e.memory_id "
                  "WHERE e.model=? AND e.dimensions=? AND e.version=1";
    if (range.valid)
      sql += " AND m.last_seen>=? AND m.started<?";
    q.prepare(sql);
    q.addBindValue(model);
    q.addBindValue(vector.size());
    if (range.valid) {
      q.addBindValue(range.from.toMSecsSinceEpoch());
      q.addBindValue(range.to.toMSecsSinceEpoch());
    }
    QVector<QPair<qint64, double>> scored;
    if (q.exec())
      while (q.next()) {
        const qint64 id = q.value(0).toLongLong();
        if (q.value(2).toString() != semantic::hash(embeddingText(id)))
          continue;
        const double score = semantic::cosine(
            vector, semantic::decode(q.value(1).toByteArray()));
        if (score > 0) {
          scored << qMakePair(id, score);
          similarities[id] = score;
        }
      }
    std::sort(scored.begin(), scored.end(), [](auto a, auto b) {
      return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    for (int i = 0; i < std::min(denseK, int(scored.size())); ++i)
      dense << scored[i].first;
  }
  for (const auto &a :
       artifacts(range.rest, range.valid ? range.from : QDateTime(),
                 range.valid ? range.to : QDateTime(), 30))
    if (within(get(a.memoryId)) && !artifact.contains(a.memoryId))
      artifact << a.memoryId;
  const auto words = range.rest.toLower().split(
      QRegularExpression("[^\\p{L}\\p{N}_.-]+"), Qt::SkipEmptyParts);
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare("SELECT DISTINCT me.memory_id,e.name,a.alias FROM memory_entities "
            "me JOIN entities e ON e.id=me.entity_id LEFT JOIN entity_aliases "
            "a ON a.entity_id=e.id");
  if (q.exec())
    while (q.next()) {
      const auto id = q.value(0).toLongLong();
      if (!within(get(id)) || entity.contains(id))
        continue;
      for (const auto &word : words)
        if (word.size() > 2 &&
            (q.value(1).toString().contains(word, Qt::CaseInsensitive) ||
             q.value(2).toString().contains(word, Qt::CaseInsensitive))) {
          entity << id;
          break;
        }
    }
  QVector<qint64> durable;
  if (m_durable) {
    for (const auto &fact : facts()) {
      const auto item = fact.toObject();
      bool matches = false;
      for (const auto &word : words)
        if (word.size() > 2 &&
            item.value("text").toString().contains(word, Qt::CaseInsensitive))
          matches = true;
      if (!matches)
        continue;
      for (const auto &source : item.value("sources").toArray()) {
        const auto id = source.toString().section(':', 1).toLongLong();
        if (within(get(id)) && !durable.contains(id))
          durable << id;
      }
    }
  }
  const auto scores =
      semantic::fuse({lexical, dense, artifact, entity, durable}, rrfK);
  QVector<RetrievalHit> hits;
  for (auto it = scores.begin(); it != scores.end(); ++it) {
    const auto rank = [](const auto &ids, qint64 id) {
      const int i = ids.indexOf(id);
      return i < 0 ? 0 : i + 1;
    };
    hits << RetrievalHit{it.key(),
                         rank(lexical, it.key()),
                         rank(dense, it.key()),
                         rank(artifact, it.key()),
                         rank(entity, it.key()),
                         it.value(),
                         similarities.value(it.key())};
  }
  std::sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) {
    return a.score != b.score ? a.score > b.score : a.id > b.id;
  });
  if (hits.size() > finalK)
    hits.resize(finalK);
  return hits;
}
QJsonObject MemoryStore::evidence(const RetrievalHit &hit, bool debug) const {
  const auto r = get(hit.id);
  QJsonArray refs, ids;
  ids.append("memory:" + QString::number(r.id));
  for (const auto &a : artifactsFor(r.id)) {
    refs.append(QJsonObject::fromVariantMap(a.toVariant()));
    ids.append("artifact:" + QString::number(a.id));
  }
  QJsonArray durable;
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare("SELECT f.id,f.text,f.status,f.confidence FROM durable_facts f "
            "JOIN fact_sources s ON s.fact_id=f.id WHERE s.memory_id=?");
  q.addBindValue(r.id);
  if (q.exec())
    while (q.next()) {
      durable.append(QJsonObject{{"id", q.value(0).toLongLong()},
                                 {"text", q.value(1).toString()},
                                 {"status", q.value(2).toString()},
                                 {"confidence", q.value(3).toDouble()}});
      ids.append("fact:" + q.value(0).toString());
    }
  QJsonObject out{{"memory_id", r.id},
                  {"source", r.source},
                  {"timestamp", r.started.toString(Qt::ISODateWithMs)},
                  {"app", r.app},
                  {"title", r.title},
                  {"summary", r.summary},
                  {"artifacts", refs},
                  {"facts", durable},
                  {"entities", entitiesFor(r.id)},
                  {"evidence", ids}};
  if (debug) {
    out.insert("lexical_rank", hit.lexicalRank);
    out.insert("dense_rank", hit.denseRank);
    out.insert("artifact_rank", hit.artifactRank);
    out.insert("entity_rank", hit.entityRank);
    out.insert("rrf_score", hit.score);
    out.insert("cosine", hit.similarity);
  }
  return out;
}
qint64 MemoryStore::rememberFact(qint64 memoryId, const QString &subject,
                                 const QString &category, const QString &text,
                                 double confidence) {
  if (!m_durable || !m_open || !get(memoryId).id ||
      subject.trimmed().isEmpty() || text.trimmed().isEmpty() ||
      !std::isfinite(confidence) || confidence < 0 || confidence > 1)
    return 0;
  auto db = QSqlDatabase::database(m_connection);
  if (!db.transaction())
    return 0;
  QSqlQuery q(db);
  const auto key = subject.simplified().toLower().left(200);
  const auto value = text.simplified().left(2000);
  q.prepare("SELECT id,text,confidence FROM durable_facts WHERE subject=? AND "
            "status='active' ORDER BY last_confirmed DESC");
  q.addBindValue(key);
  qint64 id = 0;
  bool weaker = false;
  QVector<qint64> previous;
  if (q.exec())
    while (q.next()) {
      if (q.value(1).toString().compare(value, Qt::CaseInsensitive) == 0)
        id = q.value(0).toLongLong();
      else {
        previous << q.value(0).toLongLong();
        if (q.value(2).toDouble() > confidence + 0.1)
          weaker = true;
      }
    }
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (!id) {
    // Derived facts depending on an old decision become historical, not current
    // truth.
    if (!weaker)
      for (qint64 old : previous) {
        q.prepare("UPDATE durable_facts SET status='historical' WHERE id "
                  "IN(SELECT fact_id FROM fact_relations WHERE depends_on=?)");
        q.addBindValue(old);
        if (!q.exec()) {
          db.rollback();
          return 0;
        }
        q.prepare("UPDATE durable_facts SET status='superseded' WHERE id=?");
        q.addBindValue(old);
        if (!q.exec()) {
          db.rollback();
          return 0;
        }
      }
    q.prepare("INSERT INTO "
              "durable_facts(subject,category,text,confidence,importance,"
              "status,created_at,last_confirmed) VALUES(?,?,?,?,?,?,?,?)");
    q.addBindValue(key);
    q.addBindValue(category.left(80));
    q.addBindValue(value);
    q.addBindValue(confidence);
    q.addBindValue(get(memoryId).pinned     ? 1.0
                   : category == "decision" ? 0.9
                                            : 0.8);
    q.addBindValue(weaker ? "contradicted" : "active");
    q.addBindValue(now);
    q.addBindValue(now);
    if (!q.exec()) {
      db.rollback();
      return 0;
    }
    id = q.lastInsertId().toLongLong();
  } else {
    q.prepare("UPDATE durable_facts SET "
              "last_confirmed=?,importance=min(1,importance+0.05),confidence="
              "max(confidence,?) WHERE id=?");
    q.addBindValue(now);
    q.addBindValue(confidence);
    q.addBindValue(id);
    if (!q.exec()) {
      db.rollback();
      return 0;
    }
  }
  q.prepare(
      "INSERT OR IGNORE INTO fact_sources(fact_id,memory_id) VALUES(?,?)");
  q.addBindValue(id);
  q.addBindValue(memoryId);
  if (!q.exec() || !db.commit()) {
    db.rollback();
    return 0;
  }
  return id;
}
QJsonArray MemoryStore::facts(const QString &query, bool history) const {
  QJsonArray out;
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare(
      "SELECT "
      "id,subject,category,text,confidence,status,created_at,last_confirmed "
      "FROM durable_facts WHERE (? OR status='active') AND (text LIKE ? OR "
      "subject LIKE ?) ORDER BY last_confirmed DESC LIMIT 100");
  q.addBindValue(history);
  q.addBindValue("%" + query + "%");
  q.addBindValue("%" + query + "%");
  if (q.exec())
    while (q.next()) {
      QJsonArray sources;
      QSqlQuery s(QSqlDatabase::database(m_connection));
      s.prepare("SELECT memory_id FROM fact_sources WHERE fact_id=?");
      s.addBindValue(q.value(0));
      if (s.exec())
        while (s.next())
          sources.append("memory:" + s.value(0).toString());
      out.append(QJsonObject{{"id", q.value(0).toLongLong()},
                             {"subject", q.value(1).toString()},
                             {"category", q.value(2).toString()},
                             {"text", q.value(3).toString()},
                             {"confidence", q.value(4).toDouble()},
                             {"status", q.value(5).toString()},
                             {"created_at", q.value(6).toLongLong()},
                             {"last_confirmed", q.value(7).toLongLong()},
                             {"sources", sources}});
    }
  return out;
}
void MemoryStore::associate(qint64 memoryId, const QString &kind,
                            const QString &name) {
  if (!m_entities || !get(memoryId).id || name.trimmed().isEmpty())
    return;
  auto db = QSqlDatabase::database(m_connection);
  if (!db.transaction())
    return;
  QSqlQuery q(db);
  q.prepare("INSERT OR IGNORE INTO entities(kind,name) VALUES(?,?)");
  q.addBindValue(kind.left(80));
  q.addBindValue(name.trimmed().left(2000));
  if (!q.exec()) {
    db.rollback();
    return;
  }
  q.prepare("INSERT OR IGNORE INTO memory_entities(memory_id,entity_id) SELECT "
            "?,id FROM entities WHERE kind=? AND name=?");
  q.addBindValue(memoryId);
  q.addBindValue(kind.left(80));
  q.addBindValue(name.trimmed().left(2000));
  if (!q.exec()) {
    db.rollback();
    return;
  }
  q.prepare("INSERT OR IGNORE INTO entity_relations(a,b,memory_id,kind) SELECT "
            "x.entity_id,y.entity_id,?,'co-occurs' FROM memory_entities x JOIN "
            "memory_entities y ON x.memory_id=y.memory_id WHERE x.memory_id=? "
            "AND x.entity_id<y.entity_id");
  q.addBindValue(memoryId);
  q.addBindValue(memoryId);
  if (!q.exec()) {
    db.rollback();
    return;
  }
  db.commit();
}
QJsonArray MemoryStore::entitiesFor(qint64 memoryId) const {
  QJsonArray out;
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare("SELECT e.id,e.kind,e.name FROM entities e JOIN memory_entities m "
            "ON m.entity_id=e.id WHERE m.memory_id=? ORDER BY e.id");
  q.addBindValue(memoryId);
  if (q.exec())
    while (q.next())
      out.append(QJsonObject{{"id", q.value(0).toLongLong()},
                             {"kind", q.value(1).toString()},
                             {"name", q.value(2).toString()}});
  return out;
}

bool MemoryStore::addAlias(qint64 entityId, const QString &alias) {
  if (!m_entities || alias.trimmed().isEmpty())
    return false;
  QSqlQuery q(QSqlDatabase::database(m_connection));
  q.prepare(
      "INSERT OR IGNORE INTO entity_aliases(entity_id,alias) VALUES(?,?)");
  q.addBindValue(entityId);
  q.addBindValue(alias.trimmed().left(300));
  return q.exec();
}
bool MemoryStore::addFactDependency(qint64 factId, qint64 dependsOn) {
  if (!m_durable || factId == dependsOn)
    return false;
  QSqlQuery q(QSqlDatabase::database(m_connection));
  // Reject cycles: derived summaries must have an acyclic provenance
  // dependency.
  q.prepare(
      "WITH RECURSIVE ancestors(id) AS (SELECT depends_on FROM fact_relations "
      "WHERE fact_id=? UNION SELECT r.depends_on FROM fact_relations r JOIN "
      "ancestors a ON r.fact_id=a.id) SELECT 1 FROM ancestors WHERE id=?");
  q.addBindValue(dependsOn);
  q.addBindValue(factId);
  if (!q.exec() || q.next())
    return false;
  q.prepare(
      "INSERT OR IGNORE INTO fact_relations(fact_id,depends_on) VALUES(?,?)");
  q.addBindValue(factId);
  q.addBindValue(dependsOn);
  return q.exec();
}
