#include "llm.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrlQuery>

LlmClient::LlmClient(QNetworkAccessManager *network, QObject *parent)
    : QObject(parent), m_network(network) {}

void LlmClient::configure(const Config &config) {
  if (config.endpoint != m_config.endpoint || config.model != m_config.model ||
      config.preferred != m_config.preferred ||
      config.provider != m_config.provider) {
    m_model = config.model; // re-resolve on the next request if empty
    m_capabilitiesKnown = false;
    m_capabilities.clear();
    m_extrasRejected = false;
    if (config.endpoint != m_config.endpoint ||
        config.provider != m_config.provider)
      m_server = Server::Unknown;
  }
  m_config = config;
}

QUrl LlmClient::root() const {
  QUrl url = m_config.endpoint;
  QString path = url.path();
  if (path.endsWith(QLatin1String("/v1")))
    path.chop(3);
  url.setPath(path);
  return url;
}

QString LlmClient::pickModel(const QStringList &available,
                             const QStringList &preferred) {
  const auto squash = [](QString s) {
    return s.toLower().remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
  };
  for (const QString &want : preferred) {
    const QString w = squash(want);
    if (w.isEmpty())
      continue;
    for (const QString &id : available)
      if (squash(id).contains(w))
        return id;
  }
  return available.value(0);
}

QStringList LlmClient::guessCapabilities(const QString &model) {
  QStringList caps{QStringLiteral("tools")};
  static const QRegularExpression vision(
      QStringLiteral("vl|vision|omni|llava|flash-next|gemma-?3|pixtral|"
                     "minicpm-v|moondream|qvq"),
      QRegularExpression::CaseInsensitiveOption);
  if (vision.match(model).hasMatch())
    caps << QStringLiteral("vision");
  return caps;
}

QString LlmClient::explain(const QString &error) {
  static const QRegularExpression oom(
      QStringLiteral("out of memory|CUDA error|cudaMalloc|insufficient memory|"
                     "not enough memory|failed to allocate|OOM"),
      QRegularExpression::CaseInsensitiveOption);
  if (oom.match(error).hasMatch())
    return QStringLiteral("the model ran out of GPU memory -- try a smaller "
                          "quantisation or context, or close whatever else "
                          "is using the GPU");
  if (error.contains(QLatin1String("Connection refused")) ||
      error.contains(QLatin1String("Host not found")))
    return QStringLiteral("no model server is answering at the configured "
                          "address");
  return error;
}

void LlmClient::detect(std::function<void()> done) {
  if (m_server != Server::Unknown) {
    done();
    return;
  }
  // The provider setting settles it without a round trip.
  if (m_config.provider == QLatin1String("llamacpp")) {
    m_server = Server::Other;
    done();
    return;
  }
  // Ollama answers /api/version; nothing else does.
  QUrl url = root();
  url.setPath(url.path() + "/api/version");
  QNetworkRequest req(url);
  req.setTransferTimeout(3000);
  QNetworkReply *reply = m_network->get(req);
  connect(reply, &QNetworkReply::finished, this, [this, reply, done] {
    reply->deleteLater();
    const QJsonObject json = QJsonDocument::fromJson(reply->readAll()).object();
    m_server = reply->error() == QNetworkReply::NoError && json.contains("version")
                   ? Server::Ollama
                   : Server::Other;
    done();
  });
}

void LlmClient::probe(std::function<void(QString)> done) {
  const auto finish = [done](const QString &error) {
    if (done)
      done(error);
  };
  detect([this, finish] {
    const auto withModel = [this, finish] {
      if (m_capabilitiesKnown) {
        finish({});
        return;
      }
      if (m_server != Server::Ollama) {
        m_capabilities = guessCapabilities(m_model);
        m_capabilitiesKnown = true;
        finish({});
        return;
      }
      QUrl url = root();
      url.setPath(url.path() + "/api/show");
      QNetworkRequest req(url);
      req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
      req.setTransferTimeout(5000);
      QNetworkReply *reply = m_network->post(
          req, QJsonDocument(QJsonObject{{"model", m_model}}).toJson());
      connect(reply, &QNetworkReply::finished, this, [this, reply, finish] {
        reply->deleteLater();
        const QJsonArray caps =
            QJsonDocument::fromJson(reply->readAll()).object().value("capabilities").toArray();
        m_capabilities.clear();
        for (const QJsonValue &c : caps)
          m_capabilities << c.toString();
        if (m_capabilities.isEmpty())
          m_capabilities = guessCapabilities(m_model);
        m_capabilitiesKnown = true;
        finish({});
      });
    };
    if (!m_model.isEmpty()) {
      withModel();
      return;
    }
    listModels([this, finish, withModel](const QStringList &ids, const QString &error) {
      if (ids.isEmpty()) {
        finish(error);
        return;
      }
      m_model = pickModel(ids, m_config.preferred);
      withModel();
    });
  });
}

void LlmClient::unload() { unload(m_model); }

void LlmClient::unload(const QString &model, std::function<void()> done) {
  if (m_server != Server::Ollama || model.isEmpty()) {
    if (done)
      done();
    return;
  }
  QUrl url = root();
  url.setPath(url.path() + "/api/generate");
  QNetworkRequest req(url);
  req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  req.setTransferTimeout(10000);
  QNetworkReply *reply = m_network->post(
      req, QJsonDocument(QJsonObject{{"model", model}, {"keep_alive", 0}}).toJson());
  connect(reply, &QNetworkReply::finished, this, [reply, done] {
    reply->deleteLater();
    if (done)
      done();
  });
}

void LlmClient::useModel(const QString &model) {
  if (model.isEmpty() || model == m_model)
    return;
  m_model = model;
  m_capabilitiesKnown = false;
  m_capabilities.clear();
  // Learn what this one can do (vision, tools) without holding up the request.
  if (m_server != Server::Unknown)
    probe();
}

void LlmClient::installedModels(
    std::function<void(QVector<catalog::Installed>, QString)> done) {
  detect([this, done] {
    if (m_server == Server::Ollama) {
      QUrl url = root();
      url.setPath(url.path() + "/api/tags");
      QNetworkRequest req(url);
      req.setTransferTimeout(4000);
      QNetworkReply *reply = m_network->get(req);
      connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
          done({}, explain(reply->errorString()));
          return;
        }
        done(catalog::parseTags(QJsonDocument::fromJson(reply->readAll()).object()), {});
      });
      return;
    }
    listModels([done](const QStringList &ids, const QString &error) {
      QVector<catalog::Installed> out;
      for (const QString &id : ids) {
        catalog::Installed i;
        i.name = id;
        out.append(i);
      }
      done(out, ids.isEmpty() ? error : QString());
    });
  });
}

void LlmClient::loadedModels(
    std::function<void(QVector<catalog::Loaded>, QString)> done) {
  detect([this, done] {
    if (m_server != Server::Ollama) {
      done({}, {});
      return;
    }
    QUrl url = root();
    url.setPath(url.path() + "/api/ps");
    QNetworkRequest req(url);
    req.setTransferTimeout(4000);
    QNetworkReply *reply = m_network->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
      reply->deleteLater();
      if (reply->error() != QNetworkReply::NoError) {
        done({}, explain(reply->errorString()));
        return;
      }
      done(catalog::parsePs(QJsonDocument::fromJson(reply->readAll()).object()), {});
    });
  });
}

void LlmClient::health(std::function<void(QString, QString)> done) {
  detect([this, done] {
    if (m_server == Server::Ollama) {
      QUrl url = root();
      url.setPath(url.path() + "/api/version");
      QNetworkRequest req(url);
      req.setTransferTimeout(3000);
      QNetworkReply *reply = m_network->get(req);
      connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
          done({}, explain(reply->errorString()));
          return;
        }
        done(QJsonDocument::fromJson(reply->readAll()).object().value("version").toString(), {});
      });
      return;
    }
    listModels([done](const QStringList &ids, const QString &error) {
      done(ids.isEmpty() ? QString() : QStringLiteral("OpenAI-compatible server"),
           ids.isEmpty() ? error : QString());
    });
  });
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
  m_streaming = false;
  start(messages, tools);
}

void LlmClient::chatStream(const QJsonArray &messages, const QJsonArray &tools) {
  m_streaming = true;
  start(messages, tools);
}

void LlmClient::start(const QJsonArray &messages, const QJsonArray &tools) {
  cancel();
  if (!m_model.isEmpty() && m_server != Server::Unknown) {
    send(messages, tools);
    return;
  }
  // First request: find out who we are talking to, and pick the model.
  const int generation = m_generation;
  probe([this, messages, tools, generation](const QString &error) {
    if (generation != m_generation)
      return;
    if (m_model.isEmpty()) {
      emit failed(QStringLiteral("No language model available: %1")
                      .arg(explain(error)));
      return;
    }
    send(messages, tools);
  });
}

QString LlmClient::effectiveThinking() const {
  const QString mode = m_thinkOverride.isEmpty() ? m_config.thinking : m_thinkOverride;
  return mode == QLatin1String("auto") ? QStringLiteral("off") : mode;
}

void LlmClient::send(const QJsonArray &messages, const QJsonArray &tools,
                     bool extras) {
  const bool streaming = m_streaming;
  QJsonObject body{
      {"model", m_model},
      {"messages", messages},
      {"temperature", m_config.temperature},
      {"max_tokens", m_config.maxTokens},
      {"stream", streaming},
  };
  if (!tools.isEmpty()) {
    body.insert("tools", tools);
    body.insert("tool_choice", "auto");
  }
  // Switching thinking off is spelt differently by each server: Ollama
  // honours reasoning_effort "none" (measured: 24 s -> 0.5 s for a one-word
  // answer), and ignores chat_template_kwargs; vLLM, SGLang and llama.cpp
  // pass chat_template_kwargs to the model's template, which is how Qwen
  // switches it off. If a server rejects either, it is asked again without.
  // gpt-oss cannot switch reasoning off at all: its floor is "low".
  const QString thinking = effectiveThinking();
  const bool useExtras = extras && !m_extrasRejected;
  if (useExtras && thinking != QLatin1String("server")) {
    const bool think = thinking == QLatin1String("on");
    const bool gptOss = m_model.contains(QLatin1String("gpt-oss"), Qt::CaseInsensitive);
    if (m_server == Server::Ollama) {
      if (gptOss)
        body.insert("reasoning_effort", think ? "medium" : "low");
      else if (!think)
        body.insert("reasoning_effort", "none");
    } else if (!gptOss) {
      body.insert("chat_template_kwargs",
                  QJsonObject{{"enable_thinking", think}});
    }
  }
  m_sse.clear();
  m_acc.clear();
  m_finish.clear();
  m_calls.clear();
  m_filter.reset();
  m_sawToken = m_sawVisible = false;
  m_tokens = m_reasoningTokens = 0;
  m_clock.start();
  m_reply = m_network->post(request("/chat/completions"),
                            QJsonDocument(body).toJson(QJsonDocument::Compact));
  QNetworkReply *reply = m_reply;
  if (streaming)
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
      if (reply == m_reply)
        takeStream(reply);
    });
  connect(reply, &QNetworkReply::finished, this, [this, reply, useExtras, body, messages, tools, streaming] {
    reply->deleteLater();
    if (reply != m_reply)
      return; // cancelled or superseded: say nothing
    m_reply = nullptr;
    if (streaming)
      takeStream(reply); // whatever is left in the buffer
    const QByteArray bytes = streaming ? QByteArray() : reply->readAll();
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 400 && useExtras && !m_extrasRejected &&
        (body.contains("reasoning_effort") ||
         body.contains("chat_template_kwargs"))) {
      m_extrasRejected = true;
      send(messages, tools, false);
      return;
    }
    if (reply->error() != QNetworkReply::NoError) {
      // Servers put the useful part of an error in the body.
      const QByteArray errBody = streaming ? m_sse : bytes;
      const QString detail = QJsonDocument::fromJson(errBody)
                                 .object()
                                 .value("error")
                                 .toObject()
                                 .value("message")
                                 .toString();
      emit failed(explain(detail.isEmpty() ? reply->errorString() : detail));
      return;
    }
    QString error;
    const LlmReply parsed = parse(
        streaming ? streamedResponse() : QJsonDocument::fromJson(bytes).object(),
        &error);
    if (!error.isEmpty()) {
      emit failed(error);
      return;
    }
    if (streaming) {
      // Text still held back waiting to see whether it was a tag.
      const QString rest = m_filter.flush();
      if (!rest.isEmpty())
        emit delta(rest);
      emit usage(m_tokens, m_reasoningTokens);
    }
    emit replied(parsed);
  });
}

// Server-sent events: `data: {json}` lines, a blank line between events,
// `data: [DONE]` at the end. Content arrives in choices[0].delta.content;
// reasoning in .reasoning or .reasoning_content (Ollama, vLLM, llama.cpp
// differ); tool calls in .tool_calls[] keyed by index, arguments in pieces.
void LlmClient::takeStream(QNetworkReply *reply) {
  const QByteArray chunk = reply->readAll();
  // An error body is not an event stream; keep it for failed().
  if (reply->error() != QNetworkReply::NoError ||
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() >= 400) {
    m_sse += chunk;
    return;
  }
  m_sse += chunk;
  for (;;) {
    const int nl = m_sse.indexOf('\n');
    if (nl < 0)
      break;
    const QByteArray line = m_sse.left(nl).trimmed();
    m_sse.remove(0, nl + 1);
    if (!line.startsWith("data:"))
      continue;
    const QByteArray data = line.mid(5).trimmed();
    if (data.isEmpty() || data == "[DONE]")
      continue;
    const QJsonObject event = QJsonDocument::fromJson(data).object();
    if (event.contains("error")) {
      m_finish = QStringLiteral("error");
      continue;
    }
    const QJsonArray choices = event.value("choices").toArray();
    if (choices.isEmpty())
      continue;
    const QJsonObject choice = choices.first().toObject();
    const QJsonObject d = choice.value("delta").toObject();
    if (!choice.value("finish_reason").isNull() &&
        !choice.value("finish_reason").toString().isEmpty())
      m_finish = choice.value("finish_reason").toString();

    const QString reasoning = d.value("reasoning").toString() +
                              d.value("reasoning_content").toString();
    const QString content = d.value("content").toString();
    if (!reasoning.isEmpty() || !content.isEmpty() ||
        d.contains("tool_calls")) {
      if (!m_sawToken) {
        m_sawToken = true;
        emit prefillDone(m_clock.elapsed());
      }
    }
    if (!reasoning.isEmpty())
      ++m_reasoningTokens;
    if (!content.isEmpty()) {
      ++m_tokens;
      m_acc += content;
      const QString visible = m_filter.feed(content);
      if (!visible.isEmpty()) {
        if (!m_sawVisible) {
          m_sawVisible = true;
          emit firstToken(m_clock.elapsed());
        }
        emit delta(visible);
      }
    }
    for (const QJsonValue &v : d.value("tool_calls").toArray()) {
      const QJsonObject call = v.toObject();
      PartialCall &partial = m_calls[call.value("index").toInt(int(m_calls.size()))];
      if (call.contains("id"))
        partial.id = call.value("id").toString();
      const QJsonObject fn = call.value("function").toObject();
      if (fn.contains("name"))
        partial.name += fn.value("name").toString();
      const QJsonValue args = fn.value("arguments");
      if (args.isString())
        partial.args += args.toString();
      else if (args.isObject())
        partial.args = QString::fromUtf8(QJsonDocument(args.toObject()).toJson(QJsonDocument::Compact));
    }
  }
}

// The streamed pieces, put back into the shape of a non-streaming response so
// one parser handles both.
QJsonObject LlmClient::streamedResponse() const {
  QJsonObject message{{"role", "assistant"}, {"content", m_acc}};
  if (!m_calls.isEmpty()) {
    QJsonArray calls;
    for (auto it = m_calls.constBegin(); it != m_calls.constEnd(); ++it)
      calls.append(QJsonObject{{"id", it->id},
                               {"type", "function"},
                               {"function", QJsonObject{{"name", it->name},
                                                        {"arguments", it->args}}}});
    message.insert("tool_calls", calls);
  }
  const QString finish = m_finish.isEmpty() ? QStringLiteral("stop") : m_finish;
  if (finish == QLatin1String("error"))
    return {}; // parse() reports "The model sent no answer."
  return {{"choices", QJsonArray{QJsonObject{{"message", message},
                                             {"finish_reason", finish}}}}};
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
