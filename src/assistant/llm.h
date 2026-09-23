#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVector>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

struct ToolCall {
  QString id;
  QString name;
  QJsonObject arguments;
  bool argumentsValid = true; // false when the model sent malformed JSON
};

struct LlmReply {
  QString content;           // what she should say, reasoning removed
  QVector<ToolCall> toolCalls;
  QString finishReason;
  QJsonObject message;       // the assistant message, to append to history
};

// Any OpenAI-compatible chat endpoint: Ollama, llama.cpp's llama-server,
// vLLM, LM Studio, or a custom one. Nothing here assumes a particular model;
// if none is configured it uses whatever the server lists first.
class LlmClient : public QObject {
  Q_OBJECT

public:
  struct Config {
    QUrl endpoint;          // up to and including /v1
    QString model;
    QString apiKey;
    double temperature = 0.6;
    int maxTokens = 800;
    int timeoutSec = 90;
  };

  LlmClient(QNetworkAccessManager *network, QObject *parent = nullptr);

  void configure(const Config &config);
  const Config &config() const { return m_config; }
  // The model actually in use, once known.
  QString model() const { return m_model; }

  // One round trip. Only one is in flight; a new one cancels the last.
  void chat(const QJsonArray &messages, const QJsonArray &tools = {});
  void cancel();
  bool busy() const { return m_reply != nullptr; }

  // GET /models. Calls back with the ids, or an error.
  void listModels(std::function<void(QStringList, QString)> done);

  // Pure: turn a /chat/completions response into a reply.
  static LlmReply parse(const QJsonObject &response, QString *error = nullptr);
  // Remove <think>…</think> reasoning some models (Qwen among them) emit.
  static QString stripReasoning(const QString &text);

signals:
  void replied(const LlmReply &reply);
  void failed(const QString &reason);

private:
  QNetworkRequest request(const QString &path) const;
  void send(const QJsonArray &messages, const QJsonArray &tools);

  QNetworkAccessManager *m_network;
  Config m_config;
  QString m_model;
  QPointer<QNetworkReply> m_reply;
  // Bumped by every chat() and cancel(), so a model lookup that finishes
  // after either one does not send a stale request.
  int m_generation = 0;
};
