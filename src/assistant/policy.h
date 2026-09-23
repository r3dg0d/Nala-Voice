#pragma once
#include <QDateTime>
#include <QRect>
#include <QString>
#include <QStringList>

// The rules, as pure functions: what the agent may touch, what needs asking
// first, and what screen memory must never keep. Everything here is
// deterministic so it can be tested exhaustively, and none of it depends on a
// model behaving itself.

// --- actions ----------------------------------------------------------------

enum class Risk {
  Safe,   // reads only: listing windows, searching memory
  Low,    // reversible and local: opening an app, scrolling, focusing
  Medium, // can lose state: closing windows, typing, clicking
  High,   // consequential: writing files, running commands, submitting
};

enum class Decision { Allow, Confirm, Deny };

// `mode` is agent.confirm: "everything", "risky" or "high". High-risk actions
// always need a yes -- no setting turns that off.
Decision decide(Risk risk, const QString &mode);
QString riskName(Risk risk);

// --- files ------------------------------------------------------------------

struct PathCheck {
  bool allowed = false;
  QString path;   // canonical, symlinks resolved
  QString reason; // why not, when not
};

class PathPolicy {
public:
  // `roots` empty means the home directory.
  PathPolicy(QStringList roots, QString home);

  PathCheck check(const QString &requested, bool forWrite) const;
  static bool sensitive(const QString &canonical, const QString &home);

private:
  QStringList m_roots;
  QString m_home;
};

// --- screen memory ------------------------------------------------------------

struct WindowInfo {
  bool valid = false;
  QString address;
  QString appClass;
  QString initialClass;
  QString title;
  QString monitorName;
  QRect geometry;
  bool fullscreen = false;
  bool xwayland = false;
  int pid = 0;
};

struct PrivacyCheck {
  bool allowed = false;
  QString reason; // a category, never the title itself
};

// Whether a frame of this window may be kept. Errs towards not.
PrivacyCheck privacyGate(const WindowInfo &window,
                         const QStringList &excludedApps,
                         const QStringList &excludedTitles,
                         bool blockSensitive, bool requireWindowInfo);

// --- asking about the past ------------------------------------------------------

struct TimeRange {
  bool valid = false;
  QDateTime from, to;
  QString rest; // the query with the time phrase taken out
};

// "yesterday", "last week", "on monday", "3 days ago", "this morning"...
TimeRange parseTimeRange(const QString &query, const QDateTime &now);
