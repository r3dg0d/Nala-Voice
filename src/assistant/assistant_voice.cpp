// The assistant's ears beyond the recogniser: the wake word, teaching it a
// phrase, the setup flow, and assistant profiles.
#include "assistant.h"
#include "audio.h"
#include "audioutil.h"
#include "eventlog.h"
#include "settings.h"
#include "wakecli.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QSaveFile>
#include <QtConcurrent/QtConcurrentRun>
#include <QTimer>
#include <cmath>
#include <memory>

namespace {

constexpr int kArmMs = 8000; // after the wake word, time to start talking

QString featureDir() {
  const QString env = qEnvironmentVariable("NALA_WAKE_FEATURES");
  return env.isEmpty() ? wake::Features::defaultDir() : env;
}

bool writeWav(const QString &path, const wake::Clip &clip) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  file.write(audio::wav(QByteArray(reinterpret_cast<const char *>(clip.constData()),
                                   clip.size() * 2),
                        wake::kRate));
  if (!file.commit())
    return false;
  // A recording of someone's voice is theirs alone.
  QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return true;
}

} // namespace

// --- the detector ---------------------------------------------------------------

void Assistant::setWakeBackend(wake::WakeWordBackend *backend, bool ready) {
  if (m_wake)
    m_wake->disconnect(this);
  m_wake = backend;
  m_wakeReady = false;
  if (!m_wake)
    return;
  connect(m_wake, &wake::WakeWordBackend::detected, this, &Assistant::onWake);
  connect(m_wake, &wake::WakeWordBackend::scored, this,
          [this](const QString &, float score, float threshold) {
            m_wakeScore = score;
            m_wakeThreshold = threshold;
            emit wakeScored();
          });
  connect(m_wake, &wake::WakeWordBackend::failed, this, [this](const QString &why) {
    m_log->record("error", "wakeword", {{"reason", why}});
  });
  if (ready) {
    m_wakeReady = true;
    m_wake->start();
    m_wakeStatus = tr("listening for %1").arg(m_wake->wakewords().join(", "));
  }
  emit wakeChanged();
  emit stateChanged();
}

bool Assistant::wakeModelsPresent() const {
  return wake::Features::present(featureDir()) &&
         QFileInfo::exists(featureDir() + "/" + wake::NegativeBank::fileName());
}

void Assistant::reloadWake() {
  auto *neural = dynamic_cast<wake::NeuralBackend *>(m_wake);
  if (!neural) {
    emit wakeChanged(); // a test backend: left as it was set up
    return;
  }
  if (m_testing) {
    // Tests never read the real user's recordings or models.
    m_wakeReady = false;
    m_wakeStatus = tr("off while testing");
    emit wakeChanged();
    return;
  }
  const bool wanted = m_settings->string("stt.activation") == "wake";
  QString error;
  if (!neural->initialize(&error)) {
    m_wakeReady = false;
    m_wakeStatus = wanted ? tr("%1; listening by transcription instead").arg(error)
                          : error;
    m_log->record("wake", "unavailable", {{"reason", error}});
    emit wakeChanged();
    emit stateChanged();
    return;
  }
  for (const QString &phrase : neural->wakewords())
    neural->removeWakeword(phrase);
  QStringList untrained;
  for (const QString &phrase : m_identity.wakePhrases()) {
    const wake::Model model = wake::loadModel(phrase);
    if (!model.valid() || !neural->registerWakeword(model))
      untrained << phrase;
  }
  neural->setSensitivity(m_settings->number("wake.sensitivity"));
  neural->setConfirmation(1, m_settings->integer("wake.cooldownMs"));
  m_wakeReady = neural->ready();
  if (m_wakeReady) {
    neural->start();
    m_wakeStatus = tr("listening for %1").arg(neural->wakewords().join(", "));
    if (!untrained.isEmpty())
      m_wakeStatus += tr(" (not trained yet: %1)").arg(untrained.join(", "));
  } else {
    neural->stop();
    m_wakeStatus = untrained.isEmpty()
                       ? tr("no wake phrase enabled")
                       : tr("\"%1\" is not trained yet; listening by "
                            "transcription instead")
                             .arg(untrained.join("\", \""));
  }
  m_log->record("wake", "loaded", {{"phrases", neural->wakewords().join(", ")},
                                   {"ready", m_wakeReady}});
  emit wakeChanged();
  emit stateChanged();
}

void Assistant::onFrames(const QVector<int16_t> &samples) {
  const QString &recording = m_training.recording;
  if (recording == QLatin1String("speech") || recording == QLatin1String("noise")) {
    m_training.buffer += samples;
    const int limit = wake::kRate * (recording == QLatin1String("speech") ? 25 : 4);
    if (m_training.buffer.size() >= limit)
      stopRecording();
    return;
  }
  if (!recording.isEmpty() || m_micTest || !m_wake)
    return;
  // Small buffers in, a verdict out; nothing is kept.
  if (wakeMode() || m_wakeTest.isActive())
    m_wake->processAudio(samples.constData(), int(samples.size()));
}

void Assistant::hearAudio(const QVector<int16_t> &samples16k) {
  m_mic->inject(samples16k);
}

QString Assistant::hearFile(const QString &path) {
  if (!m_settings->flag("developer.debug"))
    return tr("only in developer mode");
  wake::Clip clip;
  QString error;
  if (!readClip(path, &clip, &error))
    return error;
  // A second of quiet either side, fed in 80 ms blocks at the pace of speech.
  wake::Clip audio = wake::augment::pinkNoise(wake::kRate, 0.001, 1);
  audio += clip;
  audio += wake::augment::pinkNoise(wake::kRate * 2, 0.001, 2);
  auto *timer = new QTimer(this);
  auto at = std::make_shared<int>(0);
  connect(timer, &QTimer::timeout, this, [this, timer, audio, at] {
    const int n = std::min(int(wake::kChunk), int(audio.size()) - *at);
    if (n <= 0) {
      timer->deleteLater();
      return;
    }
    hearAudio(audio.mid(*at, n));
    *at += n;
  });
  timer->start(80);
  m_log->record("developer", "hear-file", {{"seconds", clip.size() / double(wake::kRate)}});
  return {};
}

void Assistant::onWake(const wake::Detection &detection) {
  m_log->record("wake", "detected", {{"phrase", detection.phrase},
                                     {"score", double(detection.score)},
                                     {"ms", detection.latencyMs}});
  if (m_wakeTest.isActive()) {
    // Testing: light up, do nothing.
    emit wakeDetected(detection.phrase, detection.score);
    return;
  }
  if (!m_training.recording.isEmpty() || m_micTest)
    return;
  if (m_speaking) {
    if (!m_settings->flag("wake.bargeIn"))
      return; // she should not have been listening; ignore it anyway
    stop();
  } else if (m_thinking || m_transcribing) {
    // Called again mid-answer: they have something new to say.
    stop();
  }
  // The reaction comes from here, straight away -- not from the recogniser
  // or the model.
  if (m_settings->flag("wake.visual"))
    emit wakeDetected(detection.phrase, detection.score);
  if (m_settings->flag("wake.chime"))
    chime();
  arm(kArmMs);
}

void Assistant::arm(int ms) {
  m_armed = true;
  m_armTimer.start(ms);
  m_followUp.stop();
  if (m_asleep) {
    m_asleep = false;
    if (m_companion)
      m_companion("wake");
  }
  settle();
  emit stateChanged();
}

void Assistant::disarm() {
  if (!m_armed)
    return;
  m_armed = false;
  m_armTimer.stop();
  settle();
  emit stateChanged();
}

void Assistant::chime() {
  if (m_testing || m_speaking)
    return;
  // Two soft rising notes, a fifth apart, about a fifth of a second.
  constexpr int rate = 22050;
  QByteArray pcm;
  const auto note = [&](double hz, int ms) {
    const int n = rate * ms / 1000;
    for (int i = 0; i < n; ++i) {
      const double t = double(i) / rate;
      const double envelope =
          std::min(1.0, t / 0.005) * std::exp(-t * 14.0);
      const auto v = int16_t(std::lround(
          std::sin(2.0 * M_PI * hz * t) * envelope * 0.14 * 32767.0));
      pcm.append(reinterpret_cast<const char *>(&v), 2);
    }
  };
  note(880.0, 90);
  note(1318.5, 140);
  if (Speaker::devices().isEmpty())
    return;
  m_chiming = true;
  if (!m_speaker->begin(rate, 1, 16, m_settings->string("tts.device"),
                        m_settings->number("tts.volume"))) {
    m_chiming = false;
    return;
  }
  m_speaker->append(pcm);
  m_speaker->finish();
}

void Assistant::testWake() {
  if (!m_mic->active())
    openMicrophone();
  if (m_wake && !m_wakeReady) {
    // Still worth running: the meter shows what the detector hears.
    reloadWake();
  }
  if (m_wake)
    m_wake->start();
  m_wakeTest.start();
  emit wakeChanged();
}

void Assistant::startMicTest() {
  m_micTest = true;
  if (!m_mic->active())
    openMicrophone();
  emit stateChanged();
}

void Assistant::stopMicTest() {
  m_micTest = false;
  if (!continuous() && !m_pushToTalk)
    m_mic->stop();
  emit stateChanged();
}

void Assistant::testVoice() {
  say(tr("Hi! I'm %1.").arg(m_identity.name));
}

void Assistant::setSpeakingForTest(bool speaking) {
  if (speaking) {
    m_speaking = true;
    if (m_wake && !m_settings->flag("wake.bargeIn"))
      m_wake->pause();
    settle();
  } else {
    finishSpeaking();
  }
}

// --- teaching it a phrase ---------------------------------------------------------

void Assistant::startTraining(const QString &phrase) {
  const QString p = CommandRouter::normalise(phrase);
  if (p.isEmpty() || m_training.busy)
    return;
  const bool micWasOpen = m_mic->active();
  m_training = Training{};
  m_training.phrase = p;
  m_training.micWasOpen = micWasOpen;
  if (!m_mic->active())
    openMicrophone();
  m_log->record("wake", "training-started", {{"phrase", p}});
  emit trainingChanged();
}

void Assistant::recordSample(const QString &kind) {
  if (m_training.phrase.isEmpty() || m_training.busy ||
      !QStringList{"phrase", "speech", "noise"}.contains(kind))
    return;
  if (!m_mic->active())
    openMicrophone();
  m_training.recording = kind;
  m_training.buffer.clear();
  // A phrase ends when the voice-activity detector says so; the others run
  // for a set time, or until stopped.
  m_recordTimer.start(kind == QLatin1String("phrase") ? 6000
                      : kind == QLatin1String("speech") ? 26000
                                                        : 4500);
  emit trainingChanged();
  emit stateChanged();
  settle();
}

void Assistant::stopRecording() {
  const QString kind = m_training.recording;
  if (kind.isEmpty())
    return;
  m_training.recording.clear();
  m_recordTimer.stop();
  if (kind == QLatin1String("phrase")) {
    emit trainingSample(kind, int(m_training.positives.size()), false,
                        tr("I didn't hear anything -- try again."));
  } else if (kind == QLatin1String("speech")) {
    const bool ok = m_training.buffer.size() >= wake::kRate * 5;
    if (ok)
      m_training.speech = m_training.buffer;
    emit trainingSample(kind, 0, ok,
                        ok ? tr("Thanks -- that helps me tell your voice apart "
                                "from the phrase.")
                           : tr("That was a bit short; about fifteen seconds "
                                "of talking works best."));
  } else {
    m_training.noise = m_training.buffer;
    emit trainingSample(kind, 0, true, tr("Got the room."));
  }
  m_training.buffer.clear();
  emit trainingChanged();
  emit stateChanged();
  settle();
}

void Assistant::cancelTraining() {
  const bool micWasOpen = m_training.micWasOpen;
  if (m_training.busy)
    return;
  m_recordTimer.stop();
  m_training = Training{};
  if (!micWasOpen && !continuous() && !m_pushToTalk)
    m_mic->stop();
  emit trainingChanged();
  emit stateChanged();
  settle();
}

void Assistant::finishTraining() {
  if (m_training.busy)
    return;
  if (m_training.positives.size() < 3) {
    emit trainingFinished(false, tr("Say the phrase at least three times -- "
                                    "six is better."));
    return;
  }
  const QString dir = wake::phraseDir(m_training.phrase) + "/samples";
  // The recordings are kept, so the phrase can be retrained later, and
  // until the user deletes them -- nowhere but here.
  QDir(dir).removeRecursively();
  QDir().mkpath(dir);
  for (const QString &d : {wake::phraseDir(m_training.phrase), dir})
    QFile::setPermissions(d, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ExeOwner);
  for (int i = 0; i < m_training.positives.size(); ++i)
    writeWav(QStringLiteral("%1/phrase-%2.wav").arg(dir).arg(i + 1),
             m_training.positives[i]);
  QVector<wake::Clip> negatives;
  if (!m_training.speech.isEmpty()) {
    writeWav(dir + "/speech.wav", m_training.speech);
    negatives << m_training.speech;
  }
  if (!m_training.noise.isEmpty()) {
    writeWav(dir + "/room.wav", m_training.noise);
    negatives << m_training.noise;
  }
  trainOn(m_training.phrase, m_training.positives, negatives);
}

void Assistant::retrain(const QString &phrase) {
  const QString dir = wake::phraseDir(phrase) + "/samples";
  QVector<wake::Clip> positives, negatives;
  for (const QFileInfo &file : QDir(dir).entryInfoList({"*.wav"}, QDir::Files)) {
    wake::Clip clip;
    QString error;
    if (!readClip(file.filePath(), &clip, &error))
      continue;
    (file.fileName().startsWith("phrase-") ? positives : negatives) << clip;
  }
  if (positives.size() < 3) {
    emit trainingFinished(false, tr("There are no recordings of \"%1\" to "
                                    "train from; record it first.")
                                     .arg(phrase));
    return;
  }
  m_training = Training{};
  m_training.phrase = CommandRouter::normalise(phrase);
  m_training.micWasOpen = true;
  trainOn(m_training.phrase, positives, negatives);
}

void Assistant::trainOn(const QString &phrase,
                        const QVector<wake::Clip> &positives,
                        const QVector<wake::Clip> &negatives) {
  m_training.busy = true;
  emit trainingChanged();
  struct Outcome {
    wake::Model model;
    wake::TrainReport report;
    QString error;
  };
  const QString dir = featureDir();
  auto *watcher = new QFutureWatcher<Outcome>(this);
  connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher, phrase] {
    watcher->deleteLater();
    const Outcome outcome = watcher->result();
    m_training.busy = false;
    const bool ok = outcome.error.isEmpty() && outcome.report.ok;
    QString message;
    if (ok) {
      QStringList phrases = m_settings->list("wake.phrases");
      if (!phrases.contains(phrase))
        m_settings->set("wake.phrases", phrases << phrase);
      reloadWake();
      message = tr("Learned \"%1\" from %2 recordings.")
                    .arg(phrase)
                    .arg(outcome.model.stats.value("recordings").toInt());
      if (outcome.report.falseAcceptsPerHour >= 0.0)
        message += tr(" Tested on %1 hours of other people talking, it would "
                      "have woken %2 times an hour.")
                       .arg(outcome.report.benchmarkHours, 0, 'f', 1)
                       .arg(outcome.report.falseAcceptsPerHour, 0, 'f', 1);
    } else {
      message = outcome.error.isEmpty() ? outcome.report.error : outcome.error;
    }
    m_log->record("wake", ok ? "trained" : "training-failed",
                  {{"phrase", phrase},
                   {"ms", outcome.report.milliseconds},
                   {"falseAcceptsPerHour", outcome.report.falseAcceptsPerHour},
                   {"reason", ok ? QString() : message}});
    if (!m_training.micWasOpen && !continuous() && !m_pushToTalk)
      m_mic->stop();
    m_training = Training{};
    emit trainingFinished(ok, message);
    emit trainingChanged();
    emit wakeChanged();
  });
  watcher->setFuture(QtConcurrent::run([phrase, positives, negatives, dir] {
    Outcome out;
    // Its own feature extractor: the live one belongs to the UI thread.
    wake::Features features;
    if (!features.load(dir, &out.error))
      return out;
    wake::NegativeBank bank;
    bank.load(dir); // better with it; still works without
    wake::Trainer trainer(&features, &bank);
    out.model = trainer.train(phrase, positives, negatives, &out.report);
    if (out.report.ok && !wake::saveModel(out.model, &out.error))
      out.report.ok = false;
    return out;
  }));
}

bool Assistant::deleteWakeData(const QString &phrase) {
  const bool ok = wake::deletePhrase(phrase);
  m_log->record("privacy", "wake-data-deleted", {{"phrase", phrase}, {"ok", ok}});
  reloadWake();
  return ok;
}

int Assistant::deleteAllWakeData() {
  int n = 0;
  for (const QString &phrase : wake::trainedPhrases())
    n += wake::deletePhrase(phrase) ? 1 : 0;
  // Anything left over, recordings from a cancelled training included.
  QDir(QFileInfo(wake::phraseDir(QStringLiteral("x"))).absolutePath())
      .removeRecursively();
  m_log->record("privacy", "wake-data-deleted-all", {{"phrases", n}});
  reloadWake();
  return n;
}

void Assistant::setPhraseEnabled(const QString &phrase, bool enabled) {
  const QString p = CommandRouter::normalise(phrase);
  if (p.isEmpty())
    return;
  QStringList phrases = m_settings->list("wake.phrases");
  if (enabled && !phrases.contains(p))
    phrases << p;
  if (!enabled)
    phrases.removeAll(p);
  m_settings->set("wake.phrases", phrases);
}

QVariantList Assistant::wakePhraseList() const {
  QStringList all = m_identity.wakePhrases();
  for (const QString &p : wake::trainedPhrases())
    if (!all.contains(p))
      all << p;
  QVariantList out;
  const QStringList enabled = m_identity.wakePhrases();
  for (const QString &p : all) {
    const wake::Model model = wake::loadModel(p);
    const int samples = int(QDir(wake::phraseDir(p) + "/samples")
                                .entryList({"phrase-*.wav"}, QDir::Files)
                                .size());
    out << QVariantMap{
        {"phrase", p},
        {"enabled", enabled.contains(p)},
        {"trained", model.valid()},
        {"samples", samples},
        {"threshold", model.valid() ? double(model.threshold) : 0.0},
        {"falseWakesPerHour",
         model.stats.value("falseAcceptsPerHour").toDouble(-1.0)},
        {"trainedAt", model.trained}};
  }
  return out;
}

void Assistant::installWakeModels() {
  const QString dir = featureDir();
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
    watcher->deleteLater();
    const QString error = watcher->result();
    m_log->record("wake", error.isEmpty() ? "models-installed" : "models-failed",
                  {{"reason", error}});
    reloadWake();
    emit wakeModelsInstalled(error.isEmpty(),
                             error.isEmpty() ? tr("Wake-word models installed.")
                                             : error);
  });
  watcher->setFuture(QtConcurrent::run([dir] {
    QString error;
    installFeatureModels(dir, &error);
    return error;
  }));
}

void Assistant::finishSetup() {
  m_settings->set("identity.setupDone", true);
  m_settings->flush();
  emit identityChanged();
}

// --- profiles -----------------------------------------------------------------------

QString Assistant::exportProfile(const QString &path) const {
  QString target = path;
  if (target.startsWith("~/"))
    target = QDir::homePath() + target.mid(1);
  QSaveFile file(target);
  if (!file.open(QIODevice::WriteOnly))
    return file.errorString();
  file.write(QJsonDocument(profile::exportProfile(*m_settings))
                 .toJson(QJsonDocument::Indented));
  return file.commit() ? QString() : file.errorString();
}

QString Assistant::importProfile(const QString &path) {
  QString source = path;
  if (source.startsWith("~/"))
    source = QDir::homePath() + source.mid(1);
  QFile file(source);
  if (!file.open(QIODevice::ReadOnly))
    return file.errorString();
  if (file.size() > 1024 * 1024)
    return tr("that file is too large to be a profile");
  QString error;
  const QStringList applied = profile::importProfile(
      *m_settings, QJsonDocument::fromJson(file.readAll()).object(), &error);
  m_log->record("identity", "profile-imported", {{"keys", int(applied.size())}});
  if (!error.isEmpty())
    return error;
  return applied.isEmpty() ? tr("nothing in it could be used") : QString();
}
