#pragma once
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QUrl>
#include <functional>
class QNetworkAccessManager;
class QNetworkReply;
class AssistantSettings;
class WebSearch : public QObject {
public:
  using Done = std::function<void(QJsonObject)>;
  WebSearch(QNetworkAccessManager *, AssistantSettings *,
            QObject *parent = nullptr);
  void search(QString query, QString freshness, Done);
  void fetch(QUrl, Done);
  void cancel();
  static QJsonObject parseDuck(QString html, int limit);
  static QJsonObject parseSearx(QJsonObject json, int limit);
  static QString plain(QString html);
  static bool publicUrl(const QUrl &);
  static bool publicAddress(QString);
  static bool needsFreshInfo(QString);

private:
  void get(QUrl, std::function<void(QByteArray, QString, QString)>);
  QNetworkAccessManager *m_network;
  AssistantSettings *m_settings;
  QSet<QNetworkReply *> m_active;
  int m_generation = 0;
};
