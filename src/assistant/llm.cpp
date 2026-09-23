#include "llm.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

LlmClient::LlmClient(QNetworkAccessManager *network, QObject *parent)
    : QObject(parent), m_network(network) {}

void LlmClient::configure(const Config &config) {
  if (config.endpoint != m_config.endpoint || config.model != m_config.model)
    m_model = config.model; // re-resolve on the next request if empty
  m_config = config;
}

QNetworkRequest LlmClient::request(const QString &path) const {
  QUrl url = m_config.endpoint;
  url.setPath(url.path() + path);
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  request.setTransferTimeout(m_config.timeoutSec * 1000);
  if (!m_config.apiKey.isEmpty())
    request.setRawHeader("Authorization", "Bearer " + m_config.apiKey.toUtf8());
  return request;
}

void LlmClient::listModels(std::function<void(QStringList, QString)> done) {
  QNetworkRequest req = request("/models");
  req.setTransferTimeout(5000);
  QNetworkReply *reply = m_network->get(req);
  connect(reply, &QNetworkReply::finished, this, [reply, done] {
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
      done({}, reply->errorString());
      return;
    }
    QStringList ids;
    const QJsonArray data =
        QJsonDocument::fromJson(reply->readAll()).object().value("data").toArray();
    for (const QJsonValue &model : data)
      ids << model.toObject().value("id").toString();
    ids.removeAll(QString());
    done(ids, ids.isEmpty() ? QStringLiteral("The server lists no models.")
                            : QString());
  });
}

void LlmClient::chat(const QJsonArray &messages, const QJsonArray &tools) {
  cancel();
  if (!m_model.isEmpty()) {
    send(messages, tools);
    return;
  }
  // No model named: take the server's first, once.
  const int generation = m_generation;
  listModels([this, messages, tools, generation](const QStringList &ids,
                                                 const QString &error) {
    if (generation != m_generation)
      return;
    if (ids.isEmpty()) {
      emit failed(QStringLiteral("No language model available: %1").arg(error));
      return;
    }
    m_model = ids.first();
    send(messages, tools);
  });
}

void LlmClient::send(const QJsonArray &messages, const QJsonArray &tools) {
  QJsonObject body{
      {"model", m_model},
      {"messages", messages},
      {"temperature", m_config.temperature},
      {"max_tokens", m_config.maxTokens},
      {"stream", false},
  };
  if (!tools.isEmpty()) {
    body.insert("tools", tools);
    body.insert("tool_choice", "auto");
  }
  m_reply = m_network->post(request("/chat/completions"),
                            QJsonDocument(body).toJson(QJsonDocument::Compact));
  QNetworkReply *reply = m_reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    reply->deleteLater();
    if (reply != m_reply)
      return; // cancelled or superseded: say nothing
    m_reply = nullptr;
    const QByteArray bytes = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
      // Servers put the useful part of an error in the body.
      const QString detail = QJsonDocument::fromJson(bytes)
                                 .object()
                                 .value("error")
                                 .toObject()
                                 .value("message")
                                 .toString();
      emit failed(detail.isEmpty() ? reply->errorString() : detail);
      return;
    }
    QString error;
    const LlmReply parsed =
        parse(QJsonDocument::fromJson(bytes).object(), &error);
    if (!error.isEmpty()) {
      emit failed(error);
      return;
    }
    emit replied(parsed);
  });
}

void LlmClient::cancel() {
  ++m_generation;
  if (QNetworkReply *reply = m_reply) {
    m_reply = nullptr;
    reply->abort();
  }
}

QString LlmClient::stripReasoning(const QString &text) {
  static const QRegularExpression think(
      QStringLiteral(R"(<think>[\s\S]*?(</think>|$))"),
      QRegularExpression::CaseInsensitiveOption);
  QString out = text;
  out.remove(think);
  // A server that splits the reasoning off may still leave a lone closer.
  const int closer = out.indexOf(QStringLiteral("</think>"));
  if (closer >= 0)
    out = out.mid(closer + 8);
  return out.trimmed();
}

LlmReply LlmClient::parse(const QJsonObject &response, QString *error) {
  LlmReply reply;
  const QJsonArray choices = response.value("choices").toArray();
  if (choices.isEmpty()) {
    if (error)
      *error = QStringLiteral("The model sent no answer.");
    return reply;
  }
  const QJsonObject choice = choices.first().toObject();
  const QJsonObject message = choice.value("message").toObject();
  reply.finishReason = choice.value("finish_reason").toString();
  reply.content = stripReasoning(message.value("content").toString());

  for (const QJsonValue &value : message.value("tool_calls").toArray()) {
    const QJsonObject call = value.toObject();
    const QJsonObject function = call.value("function").toObject();
    ToolCall tool;
    tool.id = call.value("id").toString();
    tool.name = function.value("name").toString();
    // OpenAI sends the arguments as a JSON string; some servers send the
    // object itself.
    const QJsonValue args = function.value("arguments");
    if (args.isObject()) {
      tool.arguments = args.toObject();
    } else if (args.isString()) {
      const QString raw = args.toString().trimmed();
      if (!raw.isEmpty()) {
        QJsonParseError parseError;
        const QJsonDocument doc =
            QJsonDocument::fromJson(raw.toUtf8(), &parseError);
        tool.argumentsValid =
            parseError.error == QJsonParseError::NoError && doc.isObject();
        tool.arguments = doc.object();
      }
    } else if (!args.isUndefined() && !args.isNull()) {
      tool.argumentsValid = false;
    }
    if (tool.id.isEmpty())
      tool.id = QStringLiteral("call_%1").arg(reply.toolCalls.size());
    if (!tool.name.isEmpty())
      reply.toolCalls.append(tool);
  }

  // Keep what goes back into history in the canonical shape, with the ids
  // filled in so the tool results can refer to them.
  QJsonObject history{{"role", "assistant"}, {"content", reply.content}};
  if (!reply.toolCalls.isEmpty()) {
    QJsonArray calls;
    for (const ToolCall &tool : reply.toolCalls)
      calls.append(QJsonObject{
          {"id", tool.id},
          {"type", "function"},
          {"function",
           QJsonObject{{"name", tool.name},
                       {"arguments",
                        QString::fromUtf8(QJsonDocument(tool.arguments)
                                              .toJson(QJsonDocument::Compact))}}}});
    history.insert("tool_calls", calls);
  }
  reply.message = history;
  return reply;
}
