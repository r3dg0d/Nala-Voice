#include "speech.h"
#include "audioutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>

// --- whisper-server ----------------------------------------------------------

WhisperServer::WhisperServer(QNetworkAccessManager *network, QObject *parent)
    : SpeechToText(parent), m_network(network) {}

void WhisperServer::transcribe(const QByteArray &pcm16k,
                               const QString &language) {
  cancel();
  auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
  const auto field = [multi](const QByteArray &name, const QByteArray &value) {
    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                   "form-data; name=\"" + name + "\"");
    part.setBody(value);
    multi->append(part);
  };
  QHttpPart file;
  file.setHeader(QNetworkRequest::ContentTypeHeader, "audio/wav");
  file.setHeader(QNetworkRequest::ContentDispositionHeader,
                 "form-data; name=\"file\"; filename=\"speech.wav\"");
  file.setBody(audio::wav(pcm16k, audio::kSttRate));
  multi->append(file);
  field("response_format", "json");
  field("temperature", "0.0");
  field("language", language.isEmpty() ? "en" : language.toUtf8());
  if (!m_prompt.isEmpty())
    field("prompt", m_prompt.toUtf8());

  QUrl url = m_url;
  url.setPath(url.path() + "/inference");
  QNetworkRequest request(url);
  request.setTransferTimeout(60000);
  m_clock.start();
  m_reply = m_network->post(request, multi);
  multi->setParent(m_reply);
  QNetworkReply *reply = m_reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    reply->deleteLater();
    if (reply != m_reply)
      return; // cancelled, or superseded
    m_reply = nullptr;
    if (reply->error() != QNetworkReply::NoError) {
      emit failed(QStringLiteral("whisper-server: %1").arg(reply->errorString()));
      return;
    }
    const QJsonObject json = QJsonDocument::fromJson(reply->readAll()).object();
    if (!json.contains("text")) {
      emit failed(QStringLiteral("whisper-server sent no text."));
      return;
    }
    emit transcribed(json.value("text").toString().trimmed(),
                     m_clock.elapsed());
  });
}

void WhisperServer::cancel() {
  if (QNetworkReply *reply = m_reply) {
    m_reply = nullptr;
    reply->abort();
  }
}

// --- whisper-cli -------------------------------------------------------------

WhisperCli::WhisperCli(QObject *parent) : SpeechToText(parent) {}
WhisperCli::~WhisperCli() { cancel(); }

QString WhisperCli::defaultModel() {
  // The first ggml model in Nala's data directory, preferring the small
  // English ones that answer quickly.
  const QString dir =
      QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
      "/nala/whisper";
  for (const char *name : {"ggml-base.en.bin", "ggml-small.en.bin",
                              "ggml-base.bin", "ggml-small.bin",
                              "ggml-tiny.en.bin", "ggml-tiny.bin"})
    if (QFileInfo::exists(dir + "/" + name))
      return dir + "/" + name;
  const QStringList any =
      QDir(dir).entryList({"ggml-*.bin"}, QDir::Files, QDir::Name);
  return any.isEmpty() ? dir + "/ggml-base.en.bin" : dir + "/" + any.first();
}

void WhisperCli::configure(const QString &binary, const QString &model) {
  m_binary = binary.isEmpty() ? QStringLiteral("whisper-cli") : binary;
  m_model = model.isEmpty() ? defaultModel() : model;
}

void WhisperCli::transcribe(const QByteArray &pcm16k, const QString &language) {
  cancel();
  if (!QFileInfo::exists(m_model)) {
    emit failed(QStringLiteral("No whisper model at %1.").arg(m_model));
    return;
  }
  const QString program = QStandardPaths::findExecutable(m_binary).isEmpty()
                              ? m_binary
                              : QStandardPaths::findExecutable(m_binary);

  // The recording lives in a private directory only for as long as the
  // recogniser needs it.
  m_dir = new QTemporaryDir();
  if (!m_dir->isValid()) {
    delete m_dir;
    m_dir = nullptr;
    emit failed(QStringLiteral("Could not create a temporary directory."));
    return;
  }
  QFile::setPermissions(m_dir->path(), QFileDevice::ReadOwner |
                                           QFileDevice::WriteOwner |
                                           QFileDevice::ExeOwner);
  const QString wavPath = m_dir->filePath("speech.wav");
  QFile wav(wavPath);
  if (!wav.open(QIODevice::WriteOnly) ||
      wav.write(audio::wav(pcm16k, audio::kSttRate)) < 0) {
    delete m_dir;
    m_dir = nullptr;
    emit failed(QStringLiteral("Could not write the recording."));
    return;
  }
  wav.close();

  const int threads = std::clamp(QThread::idealThreadCount() / 2, 1, 8);
  // Arguments as a list, never a shell string: nothing here is interpreted.
  QStringList args = {"-m",  m_model,
                            "-f",  wavPath,
                            "-l",  language.isEmpty() ? "en" : language,
                            "-t",  QString::number(threads),
                            "-nt", "-np"};
  if (!m_prompt.isEmpty())
    args << "--prompt" << m_prompt;
  if (!m_gpu)
    args << "-ng";
  auto *process = new QProcess(this);
  m_process = process;
  m_clock.start();
  connect(process, &QProcess::finished, this,
          [this, process](int code, QProcess::ExitStatus status) {
            process->deleteLater();
            if (process != m_process)
              return;
            m_process = nullptr;
            delete m_dir;
            m_dir = nullptr;
            if (status != QProcess::NormalExit || code != 0) {
              emit failed(QStringLiteral("whisper-cli exited with %1: %2")
                              .arg(code)
                              .arg(QString::fromUtf8(
                                       process->readAllStandardError())
                                       .trimmed()
                                       .right(300)));
              return;
            }
            // One line per segment; timestamps are already off.
            const QString text = QString::fromUtf8(
                process->readAllStandardOutput()).simplified();
            emit transcribed(text, m_clock.elapsed());
          });
  connect(process, &QProcess::errorOccurred, this,
          [this, process](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || process != m_process)
              return;
            m_process = nullptr;
            process->deleteLater();
            delete m_dir;
            m_dir = nullptr;
            emit failed(QStringLiteral("%1 is not installed.").arg(m_binary));
          });
  process->start(program, args);
}

void WhisperCli::cancel() {
  if (QProcess *process = m_process) {
    m_process = nullptr;
    process->disconnect(this);
    process->kill();
    process->waitForFinished(500);
    process->deleteLater();
  }
  delete m_dir;
  m_dir = nullptr;
}

// --- Fish Speech ---------------------------------------------------------------

FishSpeech::FishSpeech(QNetworkAccessManager *network, QObject *parent)
    : TextToSpeech(parent), m_network(network) {}

void FishSpeech::configure(const QUrl &endpoint, const QString &referenceId,
                           bool streaming, const QString &stylePrefix) {
  m_endpoint = endpoint;
  m_referenceId = referenceId;
  m_streaming = streaming;
  m_stylePrefix = stylePrefix;
}

void FishSpeech::synthesize(const QString &text) {
  stop();
  QJsonObject body{
      {"text", m_stylePrefix.isEmpty() ? text : m_stylePrefix + " " + text},
      {"format", "wav"},
      {"streaming", m_streaming},
      {"normalize", true},
      {"latency", "balanced"},
  };
  if (!m_referenceId.isEmpty())
    body.insert("reference_id", m_referenceId);

  QUrl url = m_endpoint;
  url.setPath(url.path() + "/v1/tts");
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  request.setTransferTimeout(120000);
  m_header.clear();
  m_carry.clear();
  m_formatSent = false;
  m_reply = m_network->post(request, QJsonDocument(body).toJson());
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
      emit failed(QStringLiteral("Fish Speech: %1").arg(reply->errorString()));
      return;
    }
    if (!m_formatSent) {
      emit failed(QStringLiteral("Fish Speech sent no playable audio."));
      return;
    }
    emit done();
  });
}

void FishSpeech::readMore() {
  const QByteArray bytes = m_reply->readAll();
  if (!m_formatSent) {
    m_header.append(bytes);
    const audio::WavInfo info = audio::parseWav(m_header);
    if (!info.ok) {
      // Keep waiting for the header unless it is clearly not coming.
      if (m_header.size() > 4096) {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->abort();
        emit failed(QStringLiteral("Fish Speech did not send WAV audio."));
      }
      return;
    }
    m_formatSent = true;
    m_frameBytes = std::max(1, info.channels * info.bitsPerSample / 8);
    emit format(info.sampleRate, info.channels, info.bitsPerSample);
    m_carry = m_header.mid(info.dataOffset);
    m_header.clear();
  } else {
    m_carry.append(bytes);
  }
  // Hand over whole frames only; a sample split across two network reads
  // would otherwise come out as a click.
  const qsizetype whole = m_carry.size() - m_carry.size() % m_frameBytes;
  if (whole > 0) {
    emit audio(m_carry.left(whole));
    m_carry.remove(0, whole);
  }
}

void FishSpeech::stop() {
  if (QNetworkReply *reply = m_reply) {
    m_reply = nullptr;
    reply->abort();
  }
}

QStringList splitSentences(const QString &text, int maxChars) {
  static const QRegularExpression end(
      QStringLiteral(R"((?<=[.!?…])\s+|\n+)"));
  QStringList out;
  QString current;
  for (const QString &piece : text.split(end, Qt::SkipEmptyParts)) {
    const QString part = piece.trimmed();
    if (part.isEmpty())
      continue;
    // Short sentences are joined so she does not pause after every "Okay."
    if (!current.isEmpty() && current.size() + part.size() + 1 <= maxChars &&
        current.size() < 40) {
      current += ' ' + part;
      continue;
    }
    if (!current.isEmpty())
      out << current;
    current = part;
    // An enormous run-on sentence is broken at a comma, or failing that a
    // space, rather than sent as one long wait.
    while (current.size() > maxChars) {
      int cut = current.lastIndexOf(',', maxChars);
      if (cut < maxChars / 3)
        cut = current.lastIndexOf(' ', maxChars);
      if (cut < maxChars / 3)
        cut = maxChars;
      out << current.left(cut + 1).trimmed();
      current = current.mid(cut + 1).trimmed();
    }
  }
  if (!current.isEmpty())
    out << current;
  return out;
}
