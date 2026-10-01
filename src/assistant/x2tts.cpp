#include "x2tts.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QWebSocket>
#include <utility>

X2Tts::X2Tts(QObject *parent) : TextToSpeech(parent) {
  m_timeout.setSingleShot(true);
  connect(&m_timeout, &QTimer::timeout, this,
          [this] { fail("X2 streaming timed out."); });
}

void X2Tts::configure(Config config) {
  if (config.endpoint != m_config.endpoint || config.voice != m_config.voice ||
      config.reuse != m_config.reuse ||
      config.inputMode != m_config.inputMode) {
    m_active = true;
    stop(); // configuration changes retire even an idle transport
  }
  m_config = std::move(config);
}

void X2Tts::send(const QJsonObject &message) {
  if (m_socket)
    m_socket->sendTextMessage(QString::fromUtf8(
        QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void X2Tts::connectSocket() {
  if (m_socket)
    return;
  auto *socket =
      new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
  m_socket = socket;
  socket->setMaxAllowedIncomingFrameSize(1024 * 1024);
  socket->setMaxAllowedIncomingMessageSize(1024 * 1024);
  connect(socket, &QWebSocket::connected, this, [this, socket] {
    if (socket != m_socket)
      return;
    emit diagnostic("transport ready");
    if (m_active)
      sendStart();
  });
  connect(socket, &QWebSocket::disconnected, this, [this, socket] {
    if (socket != m_socket)
      return;
    m_socket = nullptr;
    socket->deleteLater();
    if (m_active)
      fail("X2 server disconnected.");
  });
  connect(socket, &QWebSocket::errorOccurred, this,
          [this, socket](QAbstractSocket::SocketError) {
            if (socket == m_socket && m_active)
              fail("X2 server connection failed.");
            else if (socket == m_socket) {
              m_socket = nullptr;
              socket->abort();
              socket->deleteLater();
            }
          });
  connect(socket, &QWebSocket::textMessageReceived, this,
          [this, socket](const QString &text) {
            if (socket != m_socket || !m_active)
              return;
            const auto frame = QJsonDocument::fromJson(text.toUtf8()).object();
            if (frame.value("type") != "event")
              return;
            const auto event = frame.value("event").toObject();
            if (event.value("session_id").toString() != m_session)
              return;
            const QString type = event.value("type").toString();
            if (type == "start") {
              const auto audio = event.value("audio").toObject();
              if (audio.value("encoding") != "pcm_s16le" ||
                  audio.value("sample_rate").toInt() != 24000 ||
                  audio.value("channels").toInt() != 1) {
                fail("X2 returned an unsupported PCM format.");
                return;
              }
              if (!m_format) {
                m_format = true;
                emit format(24000, 1, 16);
              }
            } else if (type == "segment_end") {
              const auto value = event.value("meta").toObject().value(
                  "extension_continuity_carried");
              emit segmentFinished(
                  value.toBool() ||
                  value.toString().compare("true", Qt::CaseInsensitive) == 0);
            } else if (type == "error") {
              // Do not expose server text: it may contain generated speech or
              // private input.
              fail("X2 synthesis failed.");
            } else if (type == "done") {
              if (!m_format || !m_received || !m_carry.isEmpty()) {
                fail("X2 returned incomplete audio.");
                return;
              }
              m_timeout.stop();
              m_active = false;
              if (!m_config.reuse) {
                m_active = true;
                stop();
              }
              emit done();
            }
          });
  connect(socket, &QWebSocket::binaryMessageReceived, this,
          [this, socket](const QByteArray &pcm) {
            if (socket != m_socket || !m_active)
              return;
            if (!m_format) {
              fail("X2 sent audio before declaring its format.");
              return;
            }
            m_timeout.start(30000);
            m_carry += pcm;
            const auto whole = m_carry.size() - m_carry.size() % 2;
            if (whole) {
              m_received = true;
              const auto chunk = m_carry.left(whole);
              m_carry.remove(0, whole);
              emit audio(chunk);
            }
          });
  socket->open(m_config.endpoint);
}

void X2Tts::prewarm() {
  if (!m_active)
    connectSocket();
}

void X2Tts::sendStart() {
  if (!m_active || m_started)
    return;
  m_started = true;
  send(
      {{"type", "start"},
       {"session_id", m_session},
       {"config", QJsonObject{{"task_type", "custom_voice"},
                              {"language", "english"},
                              {"speaker", m_config.voice},
                              {"input_mode", m_config.inputMode},
                              {"group_policy",
                               m_config.inputMode == "token" ? "none" : "auto"},
                              {"audio", QJsonObject{{"encoding", "pcm_s16le"},
                                                    {"sample_rate", 24000},
                                                    {"channels", 1}}}}}});
  emit sessionStarted();
  for (const auto &text : std::exchange(m_pending, {}))
    pushText(text);
  if (m_ended)
    send({{"type", "end"}});
}

void X2Tts::beginStream() {
  if (m_active)
    stop();
  m_session = QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_active = true;
  m_started = m_ended = m_format = m_received = false;
  m_seq = 0;
  m_carry.clear();
  m_pending.clear();
  m_timeout.start(30000);
  if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState)
    sendStart();
  else
    connectSocket();
}

void X2Tts::pushText(const QString &text) {
  if (!m_active || text.isEmpty())
    return;
  if (!m_started) {
    m_pending.append(text);
    return;
  }
  send({{"type", "text"}, {"text", text}, {"seq_no", ++m_seq}});
  m_timeout.start(30000);
}

void X2Tts::finishStream() {
  if (!m_active || m_ended)
    return;
  m_ended = true;
  if (m_started)
    send({{"type", "end"}});
}

void X2Tts::synthesize(const QString &text) {
  beginStream();
  pushText(text);
  finishStream();
}

void X2Tts::stop() {
  const bool retire = m_active || !m_config.reuse;
  if (m_active && m_started)
    send({{"type", "cancel"}});
  m_active = false;
  m_timeout.stop();
  m_pending.clear();
  m_carry.clear();
  if (retire)
    if (auto *socket = std::exchange(m_socket, nullptr)) {
      socket->disconnect(this);
      socket->abort();
      socket->deleteLater();
    }
}

void X2Tts::fail(const QString &reason) {
  const bool active = m_active;
  stop();
  if (active)
    emit failed(reason);
}

QUrl X2Tts::healthUrl(QUrl endpoint) {
  endpoint.setScheme(endpoint.scheme() == "wss" ? "https" : "http");
  endpoint.setPath("/readyz");
  endpoint.setQuery(QString());
  endpoint.setFragment(QString());
  return endpoint;
}
