#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVector>
#include <functional>

#include "modelcatalog.h"
#include "sentencestream.h"

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
    // "off", "on", "server" (leave it to the model). "auto" is resolved per
    // request by the assistant; here it behaves as "off".
    QString thinking = QStringLiteral("off");
    // "auto" asks the server who it is; "ollama" and "llamacpp" say so.
    QString provider = QStringLiteral("auto");
    // Ollama only. Its OpenAI-compatible endpoint cannot set the context
    // window (a model whose Modelfile says 131072 loads at 131072, spills onto
    // system RAM and answers slowly), thinking precisely, or keep_alive; its
    // native /api/chat can. Off means use the OpenAI-compatible endpoint.
    bool native = true;
    int numCtx = 0;      // 0: whatever the model or server says
    QString keepAlive;   // "30m", "-1" (forever); empty: the server's default
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
  void unload(const QString &model, std::function<void()> done = {});

  // Switch the model for the next request, keeping everything else. A model
  // that differs from the last one has its abilities looked up again.
  void useModel(const QString &model);
  // "on" / "off" for the next requests; empty follows Config::thinking.
  void setThinkingOverride(const QString &mode) { m_thinkOverride = mode; }

  // What the server has and what it holds in memory right now. Ollama answers
  // from /api/tags and /api/ps; other servers list ids and report nothing loaded.
  void installedModels(std::function<void(QVector<catalog::Installed>, QString)> done);
  void loadedModels(std::function<void(QVector<catalog::Loaded>, QString)> done);
  // Server version for status ("0.34.3"), or an error.
  void health(std::function<void(QString version, QString error)> done);

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
  // The same, but the answer arrives as it is written: delta() carries the
  // visible text (reasoning stripped), replied() still ends the turn with the
  // whole reply and any tool calls.
  void chatStream(const QJsonArray &messages, const QJsonArray &tools = {});
  void cancel();
  bool busy() const { return m_reply != nullptr; }

  // What listModels reports for a server that answered with an empty list.
  static QString noModelsMessage();
  // GET /models. Calls back with the ids, or an error.
  void listModels(std::function<void(QStringList, QString)> done);

  // Pure: the messages in Ollama's native shape. Images become a list of
  // base64 strings, tool-call arguments become objects, tool results name the
  // tool they answer (the id the model was given is looked up).
  static QJsonArray ollamaMessages(const QJsonArray &openai);
  // Pure: a native /api/chat response as an OpenAI-shaped one, for parse().
  static QJsonObject fromNativeResponse(const QJsonObject &native);

  // Pure: turn a /chat/completions response into a reply.
  static LlmReply parse(const QJsonObject &response, QString *error = nullptr);
  // Remove <think>…</think> reasoning some models (Qwen among them) emit.
  static QString stripReasoning(const QString &text);

signals:
  void replied(const LlmReply &reply);
  void failed(const QString &reason);
  // Streaming only. `text` is answer text, never reasoning.
  void delta(const QString &text);
  // Milliseconds from the request to the first token of any kind (prefill),
  // and to the first visible answer token.
  void prefillDone(qint64 ms);
  void firstToken(qint64 ms);
  // Native Ollama only, at the end of a reply: what the server measured.
  void stats(int promptTokens, int outputTokens, qint64 loadMs, qint64 promptMs,
             qint64 outputMs);
  // Reasoning tokens seen this reply, for the log and the benchmark.
  void usage(int completionTokens, int reasoningTokens);

private:
  QNetworkRequest request(const QString &path) const;
  QUrl root() const; // the endpoint without its /v1
  void send(const QJsonArray &messages, const QJsonArray &tools,
            bool extras = true);
  void start(const QJsonArray &messages, const QJsonArray &tools);
  void takeStream(QNetworkReply *reply);
  void takeNativeStream(QNetworkReply *reply);
  QJsonObject openAiBody(const QJsonArray &messages, const QJsonArray &tools,
                         bool streaming, bool extras) const;
  QJsonObject nativeBody(const QJsonArray &messages, const QJsonArray &tools,
                         bool streaming, bool extras) const;
  QNetworkRequest nativeRequest(const QString &path) const;
  QJsonObject streamedResponse() const;
  QString effectiveThinking() const;
  void detect(std::function<void()> done);

  QNetworkAccessManager *m_network;
  Config m_config;
  QString m_model;
  Server m_server = Server::Unknown;
  QStringList m_capabilities;
  bool m_capabilitiesKnown = false;
  bool m_capabilitiesFromServer = false; // not just guessed from the name
  bool m_extrasRejected = false; // the server refused our thinking switch
  QPointer<QNetworkReply> m_reply;
  // Bumped by every chat() and cancel(), so a model lookup that finishes
  // after either one does not send a stale request.
  int m_generation = 0;
  QString m_thinkOverride;

  // The reply being streamed.
  bool m_streaming = false;
  QByteArray m_sse;
  QString m_acc;
  QString m_finish;
  struct PartialCall { QString id, name, args; };
  QMap<int, PartialCall> m_calls;
  ThinkFilter m_filter;
  QElapsedTimer m_clock;
  bool m_sawToken = false, m_sawVisible = false;
  int m_tokens = 0, m_reasoningTokens = 0;
};
