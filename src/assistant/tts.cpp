#include "tts.h"

#include "audioutil.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <algorithm>

// --- Qwen3-TTS -------------------------------------------------------------------

QwenTts::QwenTts(QNetworkAccessManager *network, QObject *parent)
    : TextToSpeech(parent), m_network(network) {}

QJsonObject QwenTts::requestBody(const QString &text) const {
  QJsonObject body{
      {"model", m_config.model},
      {"input", text},
      {"voice", m_config.voice},
      // Raw PCM when streaming: no header to wait for, so the first sample
      // plays as soon as the model has produced it.
      {"response_format", m_config.streaming ? "pcm" : "wav"},
      {"stream", m_config.streaming},
  };
  if (!m_config.instruct.isEmpty())
    body.insert("instruct", m_config.instruct);
  return body;
}

void QwenTts::synthesize(const QString &text) {
  stop();
  QUrl url = withApiPath(m_config.endpoint, QStringLiteral("/v1/audio/speech"));
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  request.setTransferTimeout(120000);
  if (!m_config.apiKey.isEmpty())
    request.setRawHeader("Authorization", "Bearer " + m_config.apiKey.toUtf8());
  m_header.clear();
  m_carry.clear();
  m_formatSent = false;
  m_wav = false;
  m_reply = m_network->post(request, QJsonDocument(requestBody(text)).toJson(QJsonDocument::Compact));
  QNetworkReply *reply = m_reply;
  connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
    if (reply == m_reply)
      readMore();
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    reply->deleteLater();
    if (reply != m_reply)
      return;
    m_reply = nullptr;
    if (reply->error() != QNetworkReply::NoError) {
      emit failed(QStringLiteral("Qwen3-TTS: %1").arg(reply->errorString()));
      return;
    }
    // A short reply may arrive whole, without a readyRead of its own.
    m_reply = reply;
    readMore();
    m_reply = nullptr;
    if (!m_formatSent) {
      emit failed(QStringLiteral("Qwen3-TTS sent no playable audio."));
      return;
    }
    // A trailing half sample is dropped rather than played as a click.
    emit done();
  });
}

void QwenTts::readMore() {
  if (!m_reply)
    return;
  const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  // JSON/HTML error bodies must not start audio and disable the fallback.
  if (status < 200 || status >= 300)
    return;
  const QByteArray bytes = m_reply->readAll();
  if (!m_formatSent) {
    m_header.append(bytes);
    if (m_header.size() < 4)
      return;
    if (m_header.startsWith("RIFF")) {
      const audio::WavInfo info = audio::parseWav(m_header);
      if (!info.ok) {
        if (m_header.size() > 4096) {
          QNetworkReply *reply = m_reply;
          m_reply = nullptr;
          reply->abort();
          emit failed(QStringLiteral("Qwen3-TTS sent a broken WAV header."));
        }
        return;
      }
      m_wav = true;
      m_frameBytes = std::max(1, info.channels * info.bitsPerSample / 8);
      m_formatSent = true;
      emit format(info.sampleRate, info.channels, info.bitsPerSample);
      m_carry = m_header.mid(info.dataOffset);
    } else {
      // Raw 16-bit mono PCM at the configured rate.
      m_frameBytes = 2;
      m_formatSent = true;
      emit format(m_config.sampleRate, 1, 16);
      m_carry = m_header;
    }
    m_header.clear();
  } else {
    m_carry.append(bytes);
  }
  const qsizetype whole = m_carry.size() - m_carry.size() % m_frameBytes;
  if (whole > 0) {
    emit audio(m_carry.left(whole));
    m_carry.remove(0, whole);
  }
}

void QwenTts::stop() {
  if (QNetworkReply *reply = m_reply) {
    m_reply = nullptr;
    reply->abort();
  }
}

void QwenTts::check(std::function<void(QString)> done) {
  QUrl url = withApiPath(m_config.endpoint, QStringLiteral("/v1/models"));
  QNetworkRequest request(url);
  request.setTransferTimeout(3000);
  QNetworkReply *reply = m_network->get(request);
  connect(reply, &QNetworkReply::finished, this, [reply, done] {
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 200 && status < 300)
      done({});
    else
      done(reply->errorString());
  });
}

// --- the chain -------------------------------------------------------------------

void TtsChain::setEngines(const QVector<TextToSpeech *> &engines) {
  if (engines == m_engines)
    return; // unchanged: keep the connections and the failure memory
  stop();
  for (TextToSpeech *old : m_engines)
    old->disconnect(this);
  m_engines = engines;
  m_downUntil = QVector<qint64>(engines.size(), 0);
  for (int i = 0; i < engines.size(); ++i) {
    TextToSpeech *engine = engines.at(i);
    connect(engine, &TextToSpeech::format, this, [this, i](int r, int c, int b) {
      if (i != m_current || m_stopped)
        return;
      emit format(r, c, b);
    });
    connect(engine, &TextToSpeech::audio, this,
            [this, i](const QByteArray &pcm) {
              if (i == m_current && !m_stopped && !pcm.isEmpty()) {
                m_audioStarted = true;
                emit audio(pcm);
              }
            });
    connect(engine, &TextToSpeech::done, this, [this, i] {
      if (i != m_current || m_stopped)
        return;
      m_lastEngine = m_engines.at(i)->name();
      m_stopped = true;
      emit done();
    });
    connect(engine, &TextToSpeech::failed, this, [this, i](const QString &why) {
      if (i != m_current || m_stopped)
        return;
      m_errors << why;
      m_downUntil[i] = QDateTime::currentMSecsSinceEpoch() + m_cooldownMs;
      if (m_audioStarted) {
        // Half a sentence has been heard; starting it again elsewhere would
        // repeat it. Report the failure.
        m_stopped = true;
        emit failed(m_errors.join("; "));
        return;
      }
      tryFrom(i + 1);
    });
  }
}

QString TtsChain::name() const {
  QStringList names;
  for (const TextToSpeech *e : m_engines)
    names << e->name();
  return names.join(" -> ");
}

QStringList TtsChain::downEngines() const {
  QStringList down;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (int i = 0; i < m_engines.size(); ++i)
    if (m_downUntil.value(i) > now)
      down << m_engines.at(i)->name();
  return down;
}

void TtsChain::synthesize(const QString &text) {
  stop();
  m_streaming = false;
  m_text = text;
  m_errors.clear();
  m_stopped = false;
  m_audioStarted = false;
  // Prefer engines that are not cooling down; if every one is, try them all
  // rather than staying silent.
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  bool anyUp = false;
  for (int i = 0; i < m_engines.size(); ++i)
    anyUp = anyUp || m_downUntil.value(i) <= now;
  if (!anyUp)
    m_downUntil.fill(0);
  tryFrom(0);
}

void TtsChain::tryFrom(int index) {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (int i = index; i < m_engines.size(); ++i) {
    if (m_downUntil.value(i) > now)
      continue;
    m_current = i;
    auto *engine = m_engines.at(i);
    if (m_streaming && engine->incremental()) {
      engine->beginStream();
      if (m_stopped || m_current != i)
        return; // synchronous failure may select fallback
      if (!m_text.isEmpty())
        engine->pushText(m_text);
      if (m_inputDone)
        engine->finishStream();
    } else if (!m_streaming || m_inputDone) {
      engine->synthesize(m_text);
    }
    return;
  }
  m_stopped = true;
  emit failed(m_errors.isEmpty() ? QStringLiteral("No voice engine is available.")
                                 : m_errors.join("; "));
}

void TtsChain::stop() {
  m_stopped = true;
  for (TextToSpeech *engine : m_engines)
    engine->stop();
}

bool TtsChain::incremental() const {
  return !m_engines.isEmpty() && m_engines.first()->incremental();
}

void TtsChain::beginStream() {
  stop();
  m_text.clear();
  m_errors.clear();
  m_streaming = true;
  m_inputDone = m_audioStarted = false;
  m_stopped = false;
  tryFrom(0);
}

void TtsChain::pushText(const QString &text) {
  if (m_stopped || m_inputDone || text.isEmpty())
    return;
  m_text += text;
  if (m_text.size() > 64000) {
    stop();
    emit failed("Speech text exceeded the session limit.");
    return;
  }
  if (m_current >= 0 && m_engines.at(m_current)->incremental())
    m_engines.at(m_current)->pushText(text);
}

void TtsChain::finishStream() {
  if (m_stopped || m_inputDone)
    return;
  m_inputDone = true;
  if (m_current >= 0) {
    auto *engine = m_engines.at(m_current);
    if (engine->incremental())
      engine->finishStream();
    else if (!m_text.isEmpty())
      engine->synthesize(m_text);
    else {
      m_stopped = true;
      emit done();
    }
  }
}
