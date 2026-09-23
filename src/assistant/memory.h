#pragma once
#include <QDateTime>
#include <QImage>
#include <QString>
#include <QVariantMap>
#include <QVector>

// Episodic memory: what was on screen, when, in which app, and -- more
// usefully than any screenshot -- the files, pages and repositories involved.
//
// One row per kept moment. A row can outlive its screenshot: images expire on
// their own schedule while the lightweight description stays searchable.
struct MemoryRecord {
  qint64 id = 0;
  QDateTime started;
  QDateTime lastSeen;
  int frames = 1;          // near-identical captures folded into this one
  QString app;             // window class
  QString title;
  QString activity;        // "coding", "browsing"... from description, if any
  QString summary;
  QString keywords;
  QString urls;            // newline separated
  QString shotPath;        // empty once the image has expired
  qint64 shotBytes = 0;
  quint64 hash = 0;        // perceptual hash of the image
  bool pinned = false;
  QString source = QStringLiteral("screen"); // "screen", "agent" or "note"

  QVariantMap toVariant() const;
};

struct Artifact {
  qint64 id = 0;
  QString kind;  // "url", "file", "repo"
  QString value; // the URL or absolute path
  QString title;
  QDateTime firstSeen, lastSeen;
  qint64 memoryId = 0;
  QVariantMap toVariant() const;
};

// Perceptual difference hash: 64 bits, robust to scaling and recompression,
// sensitive to the layout of the picture changing. Two frames a few bits apart
// are the same moment.
quint64 differenceHash(const QImage &image);
int hammingDistance(quint64 a, quint64 b);

// URLs, file paths and GitHub repositories named in a window title or a
// description.
QVector<Artifact> findArtifacts(const QString &text);

class MemoryStore {
public:
  MemoryStore();
  ~MemoryStore();
  MemoryStore(const MemoryStore &) = delete;
  MemoryStore &operator=(const MemoryStore &) = delete;

  // `dir` holds memory.db and shots/. Created private if missing.
  bool open(const QString &dir, QString *error = nullptr);
  bool isOpen() const { return m_open; }
  bool fullText() const { return m_fts; }
  QString dir() const { return m_dir; }
  QString shotsDir() const { return m_dir + "/shots"; }

  qint64 insert(MemoryRecord record);
  void touch(qint64 id, const QDateTime &seen);
  void describe(qint64 id, const QString &activity, const QString &summary,
                const QString &keywords, const QString &urls);
  bool setPinned(qint64 id, bool pinned);
  MemoryRecord get(qint64 id) const;
  MemoryRecord latest() const;

  // Forget everything in [from, to): rows, screenshots, artifacts. Pinned
  // memories included -- this is an explicit request to forget.
  int forget(const QDateTime &from, const QDateTime &to);
  int forgetOne(qint64 id);

  // Delete screenshot images (not the rows) from before `before`, sparing
  // pinned memories.
  int dropScreenshotsBefore(const QDateTime &before);

  struct Sweep {
    int screenshots = 0;
    int rows = 0;
    qint64 bytes = 0;
  };
  // Apply retention: images older than `shotDays`, rows older than
  // `rowDays` (0 = keep forever), then the oldest unpinned images until the
  // total is under `maxBytes`.
  Sweep enforce(int shotDays, int rowDays, qint64 maxBytes,
                const QDateTime &now);

  qint64 storageBytes() const;
  int count() const;

  // Free-text and time search: "github repo last week".
  QVector<MemoryRecord> search(const QString &query, const QDateTime &now,
                               int limit = 12) const;
  // For the timeline: newest first, optionally filtered.
  QVector<MemoryRecord> list(const QDateTime &from, const QDateTime &to,
                             const QString &app, const QString &text,
                             int limit = 500) const;
  QStringList apps() const;

  qint64 addArtifact(const Artifact &artifact);
  QVector<Artifact> artifacts(const QString &text, const QDateTime &from,
                              const QDateTime &to, int limit = 20) const;
  QVector<Artifact> artifactsFor(qint64 memoryId) const;

private:
  void removeShot(const QString &path);
  QString m_dir;
  QString m_connection;
  bool m_open = false;
  bool m_fts = false;
};
