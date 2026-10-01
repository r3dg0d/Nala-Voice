#include "websearch.h"
#include "settings.h"
#include <QDateTime>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QTextDocument>
#include <QTimer>
#include <QUrlQuery>
#include <memory>
namespace {
QJsonObject fail(QString error) { return {{"ok", false}, {"error", error}}; }
QJsonObject result(QJsonArray rows) {
  return {
      {"ok", !rows.isEmpty()},
      {"results", rows},
      {"retrieved_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
      {"error", rows.isEmpty()
                    ? "No usable web results (provider may be rate limited)"
                    : ""}};
}
} // namespace
WebSearch::WebSearch(QNetworkAccessManager *n, AssistantSettings *s, QObject *p)
    : QObject(p), m_network(n), m_settings(s) {}
QString WebSearch::plain(QString html) {
  html.remove(QRegularExpression(
      "<(script|style|noscript)\\b[^>]*>[\\s\\S]*?</\\1\\s*>",
      QRegularExpression::CaseInsensitiveOption));
  QTextDocument doc;
  doc.setHtml(html);
  return doc.toPlainText().simplified();
}
bool WebSearch::publicAddress(QString value) {
  QHostAddress a(value);
  if (a.isNull() || a.isLoopback() || a.isMulticast())
    return false;
  bool v4 = false;
  const quint32 ip = a.toIPv4Address(&v4);
  if (v4) {
    const auto mask = [&](quint32 base, quint32 bits) {
      return (ip & bits) == base;
    };
    return !(mask(0, 0xff000000) || mask(0x0a000000, 0xff000000) ||
             mask(0x64400000, 0xffc00000) || mask(0x7f000000, 0xff000000) ||
             mask(0xa9fe0000, 0xffff0000) || mask(0xac100000, 0xfff00000) ||
             mask(0xc0a80000, 0xffff0000) || mask(0xc0000000, 0xffffff00) ||
             mask(0xc6120000, 0xfffe0000) || (ip >> 24) >= 224);
  }
  // Only globally routed IPv6 unicast (2000::/3), not
  // ULA/link-local/unspecified.
  const auto bytes = a.toIPv6Address();
  return (bytes[0] & 0xe0) == 0x20;
}
bool WebSearch::publicUrl(const QUrl &u) {
  if (!u.isValid() || (u.scheme() != "https" && u.scheme() != "http") ||
      u.host().isEmpty() || !u.userInfo().isEmpty() ||
      (u.port(-1) != -1 && u.port() != 80 && u.port() != 443))
    return false;
  const QString host = u.host().toLower();
  QHostAddress ip(host);
  if (!ip.isNull())
    return publicAddress(host);
  return host.contains('.') && host != "localhost" &&
         !host.endsWith(".localhost") && !host.endsWith(".local") &&
         !host.endsWith(".internal") && !host.endsWith(".test") &&
         !host.endsWith(".invalid");
}
QJsonObject WebSearch::parseDuck(QString html, int limit) {
  if (html.contains("anomaly.js") || html.contains("anomaly-modal"))
    return fail(
        "DuckDuckGo returned a bot challenge; try later or configure SearXNG");
  const QRegularExpression links("<a\\b([^>]*)>([\\s\\S]*?)</a>",
                                 QRegularExpression::CaseInsensitiveOption);
  const QRegularExpression href("href\\s*=\\s*[\"']([^\"']+)[\"']",
                                QRegularExpression::CaseInsensitiveOption);
  const QRegularExpression snippet("<td\\b[^>]*class\\s*=\\s*[\"']result-"
                                   "snippet[\"'][^>]*>([\\s\\S]*?)</td>",
                                   QRegularExpression::CaseInsensitiveOption);
  auto matches = links.globalMatch(html);
  QJsonArray rows;
  QSet<QString> seen;
  while (matches.hasNext() && rows.size() < limit) {
    auto link = matches.next();
    if (!link.captured(1).contains("result-link"))
      continue;
    auto h = href.match(link.captured(1));
    if (!h.hasMatch())
      continue;
    QUrl url(plain(h.captured(1)));
    if (url.isRelative())
      url = QUrl("https://lite.duckduckgo.com/").resolved(url);
    if (url.host() == "duckduckgo.com" && url.path() == "/l/")
      url = QUrl(QUrlQuery(url).queryItemValue("uddg", QUrl::FullyDecoded));
    url.setFragment({});
    if (!publicUrl(url) || seen.contains(url.toString()))
      continue;
    seen.insert(url.toString());
    const auto end = matches.hasNext() ? html.indexOf("class='result-link'",
                                                      link.capturedEnd())
                                       : -1;
    auto sn = snippet.match(
        html.mid(link.capturedEnd(), end < 0 ? -1 : end - link.capturedEnd()));
    rows.append(QJsonObject{{"title", plain(link.captured(2)).left(240)},
                            {"url", url.toString()},
                            {"snippet", plain(sn.captured(1)).left(1200)}});
  }
  return result(rows);
}
QJsonObject WebSearch::parseSearx(QJsonObject json, int limit) {
  QJsonArray rows;
  QSet<QString> seen;
  for (auto v : json.value("results").toArray()) {
    auto o = v.toObject();
    QUrl url(o.value("url").toString());
    url.setFragment({});
    if (!publicUrl(url) || seen.contains(url.toString()))
      continue;
    seen.insert(url.toString());
    rows.append(QJsonObject{
        {"title", plain(o.value("title").toString()).left(240)},
        {"url", url.toString()},
        {"snippet", plain(o.value("content").toString()).left(1200)},
        {"published", o.value("publishedDate")}});
    if (rows.size() >= limit)
      break;
  }
  return result(rows);
}
void WebSearch::get(QUrl url,
                    std::function<void(QByteArray, QString, QString)> done) {
  if (!url.isValid() || !url.userInfo().isEmpty() || url.host().isEmpty() ||
      (url.scheme() != "http" && url.scheme() != "https")) {
    done({}, {}, "Invalid web endpoint");
    return;
  }
  QNetworkRequest request(url);
  request.setRawHeader("User-Agent",
                       "Nala/1.4 (local voice assistant; key-free search)");
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                       QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                       QNetworkRequest::Manual);
  auto *reply = m_network->get(request);
  m_active.insert(reply);
  reply->setReadBufferSize(513 * 1024);
  auto bytes = std::make_shared<QByteArray>();
  auto overflow = std::make_shared<bool>(false);
  auto *timer = new QTimer(reply);
  timer->setSingleShot(true);
  QObject::connect(timer, &QTimer::timeout, reply, [reply] { reply->abort(); });
  timer->start(m_settings->integer("web.timeoutMs"));
  QObject::connect(reply, &QNetworkReply::readyRead, reply,
                   [reply, bytes, overflow] {
                     bytes->append(reply->readAll());
                     if (bytes->size() > 512 * 1024) {
                       *overflow = true;
                       reply->abort();
                     }
                   });
  QObject::connect(
      reply, &QNetworkReply::finished, this,
      [this, reply, bytes, overflow, timer, done] {
        timer->stop();
        m_active.remove(reply);
        if (reply->isOpen())
          bytes->append(reply->readAll());
        QString error;
        if (*overflow || bytes->size() > 512 * 1024)
          error = "Web response exceeded 512 KiB";
        else if (reply->error() != QNetworkReply::NoError)
          error = "Web request failed or timed out";
        else if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                     .toInt() != 200)
          error = "Web endpoint refused the request or redirected";
        const QString type =
            reply->header(QNetworkRequest::ContentTypeHeader).toString();
        reply->deleteLater();
        done(error.isEmpty() ? *bytes : QByteArray{}, type, error);
      });
}
void WebSearch::cancel() {
  ++m_generation;
  const auto replies = m_active;
  for (auto *r : replies)
    r->abort();
}
void WebSearch::search(QString query, QString freshness, Done done) {
  const bool searx = m_settings->string("web.provider") == "searxng";
  QUrl url(m_settings->string(searx ? "web.searxng.endpoint"
                                    : "web.duckduckgo.endpoint"));
  QUrlQuery params;
  params.addQueryItem("q", query);
  if (searx) {
    params.addQueryItem("format", "json");
    if (!freshness.isEmpty())
      params.addQueryItem("time_range", freshness);
  } else if (!freshness.isEmpty())
    params.addQueryItem("df", freshness == "day"     ? "d"
                              : freshness == "month" ? "m"
                                                     : "y");
  url.setQuery(params);
  const int limit = m_settings->integer("web.maxResults");
  get(url, [searx, limit, done](QByteArray bytes, QString, QString error) {
    if (!error.isEmpty()) {
      done(fail(error));
      return;
    }
    done(searx ? parseSearx(QJsonDocument::fromJson(bytes).object(), limit)
               : parseDuck(QString::fromUtf8(bytes), limit));
  });
}
void WebSearch::fetch(QUrl url, Done done) {
  if (!publicUrl(url)) {
    done(fail("Only public HTTP(S) sources can be fetched"));
    return;
  }
  const int generation = m_generation;
  auto completed = std::make_shared<bool>(false);
  auto *deadline = new QTimer(this);
  deadline->setSingleShot(true);
  const auto finish = [completed, deadline, done](QJsonObject result) {
    if (*completed)
      return;
    *completed = true;
    deadline->stop();
    deadline->deleteLater();
    done(result);
  };
  const int lookup = QHostInfo::lookupHost(
      url.host(), this,
      [this, url, finish, generation, completed,
       deadline](const QHostInfo &host) {
        if (*completed)
          return;
        deadline->stop();
        if (generation != m_generation) {
          finish(fail("Fetch cancelled"));
          return;
        }
        if (host.error() != QHostInfo::NoError || host.addresses().isEmpty()) {
          finish(fail("Source DNS lookup failed"));
          return;
        }
        for (const auto &ip : host.addresses())
          if (!publicAddress(ip.toString())) {
            finish(fail("Source resolves to a private address"));
            return;
          }
        get(url, [url, finish](QByteArray bytes, QString type, QString error) {
          if (!error.isEmpty()) {
            finish(fail(error));
            return;
          }
          if (!type.startsWith("text/html") && !type.startsWith("text/plain")) {
            finish(fail("Source is not HTML or plain text"));
            return;
          }
          finish({{"ok", true},
                  {"url", url.toString()},
                  {"text", plain(QString::fromUtf8(bytes)).left(6000)},
                  {"retrieved_at",
                   QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                  {"truncated", bytes.size() > 6000}});
        });
      });
  QObject::connect(deadline, &QTimer::timeout, this, [lookup, finish] {
    QHostInfo::abortHostLookup(lookup);
    finish(fail("Source DNS lookup timed out"));
  });
  deadline->start(m_settings->integer("web.timeoutMs"));
}
bool WebSearch::needsFreshInfo(QString query) {
  query = query.toLower().trimmed();
  if (QRegularExpression("\\b(my|our|screen|clipboard|remember|memories)\\b")
          .match(query)
          .hasMatch())
    return false;
  return QRegularExpression("^(search (?:the )?web|search online|look up)\\b")
             .match(query)
             .hasMatch() ||
         (QRegularExpression(
              "^(what|which|who|when|where|how|tell me|give me|show me)\\b")
              .match(query)
              .hasMatch() &&
          QRegularExpression("\\b(latest|current|currently|today|right "
                             "now|recent news|real time)\\b")
              .match(query)
              .hasMatch());
}
