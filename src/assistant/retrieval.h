#pragma once
#include "llm.h"
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <functional>
class MemoryStore;
class AssistantSettings;
class QNetworkAccessManager;
class Retrieval : public QObject {
public:
  using Done = std::function<void(QJsonObject)>;
  Retrieval(MemoryStore *store, AssistantSettings *settings,
            QNetworkAccessManager *network, QObject *parent = nullptr);
  void startIndexing();
  void search(QString query, QString context, LlmClient::Config fast,
              bool debug, Done done);
  QJsonObject searchSync(QString query, LlmClient::Config fast, bool debug);
  void embed(QString text, std::function<void(QVector<float>, QString)> done);

private:
  void post(QUrl url, QJsonObject body, Done done);
  void retrieve(QString original, QString rewritten, bool debug, Done done);
  void indexNext();
  QString space() const;
  bool paused() const;
  MemoryStore *m_store;
  AssistantSettings *m_settings;
  QNetworkAccessManager *m_network;
  QTimer m_timer;
  bool m_indexing = false;
};
