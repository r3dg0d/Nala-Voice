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
// vLLM, SGLang, LM Studio, or a custom one. With no model named it picks the
// first of `preferred` the server offers (Qwen3.8-Flash-Next first), else
// whatever it lists first. Servers differ in how thinking is switched off and
// in what they say about a model's abilities; those differences live here.
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
    QStringList preferred;
    QString thinking = QStringLiteral("off"); // "off", "on", "server"
  };
  enum class Server { Unknown, Ollama, Other };

  LlmClient(QNetworkAccessManager *network, QObject *parent = nullptr);

  void configure(const Config &config);
  const Config &config() const { return m_config; }
  // The model actually in use, once known.
  QString model() const { return m_model; }
  Server server() const { return m_server; }
  // What the server says the model can do ("vision", "tools", "thinking"),
  // or a guess from its name where the server cannot say.
  QStringList capabilities() const { return m_capabilities; }
  bool capabilitiesKnown() const { return m_capabilitiesKnown; }
  // Resolve the model, the server and the capabilities now.
  void probe(std::function<void(QString error)> done = {});
  // Ask Ollama to release the model's memory. No-op elsewhere.
  void unload();

  // Pure: the first preferred model the server has, matching loosely
  // ("qwen3.8-flash-next" finds "hf.co/unsloth/Qwen3.8-Flash-Next-GGUF:Q2").
  static QString pickModel(const QStringList &available,
                           const QStringList &preferred);
  // Pure: what a model can probably do, from its name alone.
  static QStringList guessCapabilities(const QString &model);
  // Pure: a failure message a person can act on.
  static QString explain(const QString &error);

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
  QUrl root() const; // the endpoint without its /v1
  void send(const QJsonArray &messages, const QJsonArray &tools,
            bool extras = true);
  void detect(std::function<void()> done);

  QNetworkAccessManager *m_network;
  Config m_config;
  QString m_model;
  Server m_server = Server::Unknown;
  QStringList m_capabilities;
  bool m_capabilitiesKnown = false;
  bool m_extrasRejected = false; // the server refused our thinking switch
  QPointer<QNetworkReply> m_reply;
  // Bumped by every chat() and cancel(), so a model lookup that finishes
  // after either one does not send a stale request.
  int m_generation = 0;
};
