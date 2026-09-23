#include "assistant.h"
#include "audio.h"
#include "eventlog.h"
#include "screenmemory.h"
#include "settings.h"
#include "speech.h"

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkReply>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <climits>
#include <memory>
#include <tuple>

namespace {

constexpr int kQuestionSeconds = 60;
constexpr int kFollowUpMs = 8000;
constexpr int kListenTimeoutMs = 10000;
constexpr int kMaxFileRead = 64 * 1024;
constexpr int kMaxShellOutput = 8 * 1024;

QJsonObject ok(const QJsonObject &fields = {}) {
  QJsonObject out = fields;
  out.insert("ok", true);
  return out;
}

QJsonObject fail(const QString &why) {
  return QJsonObject{{"ok", false}, {"error", why}};
}

QString when(const QDateTime &t) {
  const QDate today = QDate::currentDate();
  const QString time = QLocale().toString(t.time(), QLocale::ShortFormat);
  if (t.date() == today)
    return QStringLiteral("today %1").arg(time);
  if (t.date() == today.addDays(-1))
    return QStringLiteral("yesterday %1").arg(time);
  return QLocale().toString(t, QStringLiteral("ddd d MMM, ")) + time;
}

QString dayLabel(const QDate &date) {
  const QDate today = QDate::currentDate();
  if (date == today)
    return QStringLiteral("Today");
  if (date == today.addDays(-1))
    return QStringLiteral("Yesterday");
  if (date.daysTo(today) < 7)
    return QLocale().dayName(date.dayOfWeek());
  return QLocale().toString(date, QLocale::LongFormat);
}

// The one place the personality is written down.
const char *kPersona =
    "You are Nala, a small, friendly companion who lives on the user's Linux "
    "desktop (Hyprland) as an animated blob with big eyes. You talk the way a "
    "good friend does: warm, brief and direct. Your replies are spoken aloud, "
    "so use one to three short sentences, plain words, and no markdown, "
    "lists, code blocks or emoji unless asked.\n\n"
    "You can only affect the computer through the tools you are given. Never "
    "claim to have opened, closed, typed, written, sent or remembered "
    "anything unless a tool result says it succeeded. If a tool fails or the "
    "user declines, say so plainly and do not try to get around it. Some "
    "tools pause to ask the user for permission themselves; do not ask again "
    "in text before calling them. Never submit, send, post, buy or delete "
    "without the user having clearly asked for it.\n\n"
    "For questions about what the user did, saw or worked on before, search "
    "memory first and answer from what you find, saying when you found "
    "nothing. Prefer real references -- file paths, URLs, repositories -- "
    "over descriptions of screenshots.";

} // namespace

Assistant::Assistant(const Paths &paths, bool testing, QObject *parent)
    : QObject(parent), m_testing(testing) {
  m_settings = new AssistantSettings(paths.settings, !testing, this);
  m_log = new EventLog(testing ? QString() : paths.log, this);
  m_store = std::make_unique<MemoryStore>();
  QString error;
  if (!m_store->open(paths.memoryDir, &error))
    m_log->record("error", "memory-unavailable", {{"reason", error}});

  m_llm = new LlmClient(&m_network, this);
  m_memoryLlm = new LlmClient(&m_network, this);
  m_memory = new ScreenMemory(m_settings, m_store.get(), m_log, m_memoryLlm, this);
  if (testing)
    m_memory->setOffline(true);

  m_mic = new Microphone(this);
  connect(m_mic, &Microphone::utterance, this, &Assistant::onUtterance);
  connect(m_mic, &Microphone::levelChanged, this, [this](double level) {
    m_micLevel = level;
    emit levelsChanged();
  });
  connect(m_mic, &Microphone::speechStarted, this, [this] {
    m_listenTimeout.stop(); // she heard something; wait for the end of it
    m_log->trace("stt", "speech-started");
  });
  connect(m_mic, &Microphone::failed, this, [this](const QString &why) {
    m_log->record("error", "microphone", {{"reason", why}});
    m_pushToTalk = false;
    say(why, false);
    m_errorTimer.start();
    settle();
  });

  m_speaker = new Speaker(this);
  connect(m_speaker, &Speaker::levelChanged, this, [this](double level) {
    m_voiceLevel = level;
    emit levelsChanged();
  });
  connect(m_speaker, &Speaker::finished, this, [this] {
    if (!m_synthesizing)
      speakNext();
  });
  connect(m_speaker, &Speaker::failed, this, [this](const QString &why) {
    m_log->record("error", "speaker", {{"reason", why}});
    m_voiceBroken = true;
    m_speech.clear();
    m_fish->stop();
    m_synthesizing = false;
    finishSpeaking();
  });

  m_whisperServer = new WhisperServer(&m_network, this);
  m_whisperCli = new WhisperCli(this);
  for (SpeechToText *stt :
       std::initializer_list<SpeechToText *>{m_whisperServer, m_whisperCli}) {
    connect(stt, &SpeechToText::transcribed, this, &Assistant::onTranscript);
    connect(stt, &SpeechToText::failed, this, [this, stt](const QString &why) {
      // In auto mode a server that is not there is not an error: fall back
      // to the one-shot binary for this and later utterances.
      if (stt == m_whisperServer &&
          m_settings->string("stt.mode") == "auto" && !m_lastPcm.isEmpty()) {
        m_serverDead = true;
        m_log->record("stt", "server-unavailable", {{"reason", why}});
        m_whisperCli->transcribe(m_lastPcm, m_settings->string("stt.language"));
        return;
      }
      m_lastPcm.clear();
      m_transcribing = false;
      m_log->record("error", "stt", {{"reason", why}});
      say(QStringLiteral("I couldn't make that out: %1").arg(why), false);
      m_errorTimer.start();
      settle();
    });
  }

  m_fish = new FishSpeech(&m_network, this);
  connect(m_fish, &TextToSpeech::format, this,
          [this](int rate, int channels, int bits) {
            if (!m_speaker->begin(rate, channels, bits,
                                  m_settings->string("tts.device"),
                                  m_settings->number("tts.volume")))
              m_fish->stop();
          });
  connect(m_fish, &TextToSpeech::audio, m_speaker, &Speaker::append);
  connect(m_fish, &TextToSpeech::done, this, [this] {
    m_synthesizing = false;
    if (m_speaker->playing())
      m_speaker->finish();
    else
      speakNext();
  });
  connect(m_fish, &TextToSpeech::failed, this, [this](const QString &why) {
    // Without a voice she still answers, in the bubble.
    m_log->record("error", "tts", {{"reason", why}});
    m_voiceBroken = true;
    m_synthesizing = false;
    m_speech.clear();
    m_speaker->stop();
    finishSpeaking();
  });

  connect(m_llm, &LlmClient::replied, this, &Assistant::onModelReply);
  connect(m_llm, &LlmClient::failed, this, [this](const QString &why) {
    m_thinking = false;
    m_pendingCalls.clear();
    m_log->record("error", "llm", {{"reason", why}});
    say(QStringLiteral("I can't reach my brain right now (%1).").arg(why));
    m_errorTimer.start();
    settle();
  });

  m_bubbleTimer.setSingleShot(true);
  connect(&m_bubbleTimer, &QTimer::timeout, this, &Assistant::dismissBubble);
  m_errorTimer.setSingleShot(true);
  m_errorTimer.setInterval(3000);
  connect(&m_errorTimer, &QTimer::timeout, this, &Assistant::settle);
  m_listenTimeout.setSingleShot(true);
  m_listenTimeout.setInterval(kListenTimeoutMs);
  connect(&m_listenTimeout, &QTimer::timeout, this, [this] {
    if (m_pushToTalk)
      stopListening();
  });
  m_followUp.setSingleShot(true);
  m_followUp.setInterval(kFollowUpMs);
  connect(&m_followUp, &QTimer::timeout, this, &Assistant::settle);
  m_questionTimer.setSingleShot(true);
  m_questionTimer.setInterval(kQuestionSeconds * 1000);
  connect(&m_questionTimer, &QTimer::timeout, this, [this] {
    m_log->record("tool", "confirmation-expired");
    answer(false);
  });

  connect(m_settings, &AssistantSettings::changed, this,
          &Assistant::applySettings);
  connect(m_memory, &ScreenMemory::changed, this, &Assistant::stateChanged);
  applySettings();
  registerTools();

  if (!testing) {
    QTimer::singleShot(0, this, [this] {
      m_apps.scan();
      diagnose();
      if (continuous())
        openMicrophone();
    });
  }
}

Assistant::~Assistant() {
  m_settings->flush();
  m_mic->stop();
  m_speaker->stop();
}

// --- state -------------------------------------------------------------------

void Assistant::setState(const QString &state) {
  if (m_state == state)
    return;
  m_log->record("state", state);
  m_state = state;
  emit stateChanged();
}

void Assistant::settle() {
  if (m_errorTimer.isActive())
    setState(QStringLiteral("error"));
  else if (m_speaking)
    setState(QStringLiteral("speaking"));
  else if (m_acting)
    setState(QStringLiteral("acting"));
  else if (m_thinking || m_transcribing)
    setState(QStringLiteral("thinking"));
  else if (m_pushToTalk || m_followUp.isActive() || !m_question.isEmpty())
    setState(QStringLiteral("listening"));
  else if (m_asleep)
    setState(QStringLiteral("sleeping"));
  else
    setState(QStringLiteral("idle"));
}

void Assistant::applySettings(const QString &key) {
  m_log->setDebug(m_settings->flag("developer.debug"));

  LlmClient::Config llm;
  llm.endpoint = QUrl(m_settings->string("llm.endpoint"));
  llm.model = m_settings->string("llm.model");
  llm.apiKey = m_settings->string("llm.apiKey");
  llm.temperature = m_settings->number("llm.temperature");
  llm.maxTokens = m_settings->integer("llm.maxTokens");
  llm.timeoutSec = m_settings->integer("llm.timeoutSec");
  m_llm->configure(llm);
  m_memoryLlm->configure(llm);

  m_whisperServer->setUrl(QUrl(m_settings->string("stt.serverUrl")));
  m_whisperCli->configure(m_settings->string("stt.binary"),
                          m_settings->string("stt.model"));
  m_whisperServer->setPrompt(m_settings->string("stt.prompt"));
  m_whisperCli->setPrompt(m_settings->string("stt.prompt"));
  if (key.startsWith("stt."))
    m_serverDead = false; // worth another try with the new settings
  m_fish->configure(QUrl(m_settings->string("tts.endpoint")),
                    m_settings->string("tts.referenceId"),
                    m_settings->flag("tts.streaming"),
                    m_settings->string("tts.stylePrefix"));
  if (key.startsWith("tts."))
    m_voiceBroken = false;

  if (key == "stt.activation" || key == "stt.enabled" || key == "stt.device") {
    m_deaf = false;
    if (continuous())
      openMicrophone();
    else if (!m_pushToTalk)
      m_mic->stop();
    emit stateChanged();
  }
}

bool Assistant::micOpen() const { return m_mic->active(); }

QString Assistant::model() const { return m_llm->model(); }

QStringList Assistant::microphones() const { return Microphone::devices(); }
QStringList Assistant::speakers() const { return Speaker::devices(); }

// --- hearing -------------------------------------------------------------------

bool Assistant::continuous() const {
  return m_settings->flag("stt.enabled") && !m_deaf && !m_testing &&
         m_settings->string("stt.activation") != "push";
}

void Assistant::openMicrophone() {
  audio::VoiceActivity::Config vad;
  vad.thresholdDb = m_settings->number("stt.vadThresholdDb");
  vad.silenceMs = m_settings->integer("stt.silenceMs");
  vad.maxUtteranceMs = m_settings->integer("stt.maxUtteranceSec") * 1000;
  if (m_mic->start(m_settings->string("stt.device"), vad))
    m_log->record("stt", "microphone-open");
  emit stateChanged();
}

void Assistant::toggleListening() {
  if (m_pushToTalk)
    stopListening();
  else
    startListening();
}

void Assistant::startListening() {
  if (!m_settings->flag("stt.enabled")) {
    say(QStringLiteral("Speech recognition is switched off in my settings."),
        false);
    return;
  }
  // Talking over her means "listen to me instead".
  if (m_speaking)
    stop();
  m_deaf = false;
  m_pushToTalk = true;
  if (!m_mic->active())
    openMicrophone();
  if (!m_mic->active()) {
    m_pushToTalk = false;
    return;
  }
  m_listenTimeout.start();
  if (m_asleep) {
    m_asleep = false;
    if (m_companion)
      m_companion("wake");
  }
  settle();
}

void Assistant::stopListening() {
  m_pushToTalk = false;
  m_listenTimeout.stop();
  if (!continuous())
    m_mic->stop();
  settle();
}

void Assistant::setSpeechBackend(SpeechToText *stt) {
  m_sttOverride = stt;
  connect(stt, &SpeechToText::transcribed, this, &Assistant::onTranscript);
}

void Assistant::onUtterance(const QByteArray &pcm16k) {
  if (m_pushToTalk) {
    m_pushToTalk = false;
    m_listenTimeout.stop();
    if (!continuous())
      m_mic->stop();
  }
  // Very short blips are clicks and coughs, not words.
  if (pcm16k.size() < audio::kSttRate * 2 * 3 / 10) {
    settle();
    return;
  }
  m_transcribing = true;
  m_lastPcm = pcm16k;
  settle();
  const QString language = m_settings->string("stt.language");
  const QString mode = m_settings->string("stt.mode");
  if (m_sttOverride)
    m_sttOverride->transcribe(pcm16k, language);
  else if (mode == "server" || (mode == "auto" && !m_serverDead))
    m_whisperServer->transcribe(pcm16k, language);
  else
    m_whisperCli->transcribe(pcm16k, language);
}

void Assistant::onTranscript(const QString &text, qint64 ms) {
  m_transcribing = false;
  m_lastPcm.clear();
  m_log->record("stt", "transcribed",
                {{"ms", ms}, {"chars", int(text.size())}});
  m_log->trace("stt", "transcript", {{"text", text}});
  handle(text, true);
  settle();
}

// --- deciding ------------------------------------------------------------------

void Assistant::ask(const QString &text) { handle(text, false); }

void Assistant::handle(const QString &text, bool spoken) {
  const Route route =
      m_router.route(text, m_settings->list("stt.wakePhrases"));
  if (route.text.isEmpty() && !route.addressed)
    return; // silence, or whisper's "[BLANK_AUDIO]"

  if (spoken && continuous()) {
    // With the microphone always open she hears herself. While she is
    // talking, the only thing worth acting on is being told to stop.
    if (m_speaking && route.action != QLatin1String("assistant.stop"))
      return;
    const bool conversation = m_followUp.isActive() || !m_question.isEmpty();
    // Pausing screen memory is honoured whoever says it and however her name
    // came out: stopping is always the safe direction.
    if (m_settings->string("stt.activation") == "wake" && !route.addressed &&
        !conversation && route.action != QLatin1String("memory.pause")) {
      m_log->trace("stt", "not-addressed");
      return;
    }
  }
  if (route.addressed && route.text.isEmpty()) {
    // "Hey Nala" on its own: she is listening for the rest.
    m_followUp.start();
    m_heard.clear();
    say(QStringLiteral("Yes?"), false);
    settle();
    return;
  }
  m_followUp.stop();
  // A new request replaces the last answer rather than sitting under it.
  m_heard = route.text;
  m_bubble.clear();
  emit bubbleChanged();

  if (m_asleep && route.action != QLatin1String("nala.sleep")) {
    m_asleep = false;
    if (m_companion)
      m_companion("wake");
  }

  if (!m_question.isEmpty()) {
    if (route.action == QLatin1String("confirm.yes")) {
      answer(true);
      return;
    }
    if (route.action == QLatin1String("confirm.no") ||
        route.action == QLatin1String("assistant.stop")) {
      answer(false);
      return;
    }
    // Anything else means they have moved on.
    answer(false);
  }

  if (route.matched) {
    m_log->record("router", route.action,
                  QJsonObject::fromVariantMap(route.args));
    runFast(route);
    return;
  }
  think(route.text);
}

void Assistant::runFast(const Route &route) {
  const QString &a = route.action;
  const QVariantMap &args = route.args;
  const auto companion = [this](const QString &command) {
    if (m_companion)
      m_companion(command);
  };

  if (a == "assistant.stop") {
    stop();
    return;
  }
  if (a == "confirm.yes" || a == "confirm.no") {
    say(QStringLiteral("There's nothing waiting on an answer."), false);
    return;
  }
  if (a == "memory.pause") {
    const int minutes = args.value("minutes").toInt();
    m_memory->pause(minutes);
    say(minutes > 0 ? QStringLiteral("Screen memory paused for %1.")
                          .arg(minutes % 60 == 0 && minutes >= 60
                                   ? QStringLiteral("%1 hour%2")
                                         .arg(minutes / 60)
                                         .arg(minutes >= 120 ? "s" : "")
                                   : QStringLiteral("%1 minutes").arg(minutes))
                    : QStringLiteral("Screen memory paused. I'm not looking."));
    return;
  }
  if (a == "memory.resume") {
    if (!m_memory->enabled()) {
      m_memory->resume();
      say(QStringLiteral("Screen memory is switched off in my settings. "
                         "Turn it on there if you want me to remember."));
      return;
    }
    m_memory->resume();
    say(QStringLiteral("Screen memory is back on."));
    return;
  }
  if (a == "memory.forget") {
    const QString range = args.value("range").toString();
    if (range == "all") {
      confirm(QStringLiteral("Forget every memory I have, including pinned "
                             "ones?"),
              [this](bool yes) {
                if (!yes) {
                  say(QStringLiteral("Okay, I'll keep them."));
                  return;
                }
                const int n = m_store->forget(QDateTime::fromMSecsSinceEpoch(0),
                                              QDateTime::currentDateTime()
                                                  .addDays(1));
                m_log->record("memory", "forgot-all", {{"count", n}});
                say(QStringLiteral("Done. I've forgotten everything."));
              });
      return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime from =
        range == "today" ? QDateTime(now.date(), QTime(0, 0))
                         : now.addSecs(-qint64(args.value("minutes").toInt()) * 60);
    const int n = m_store->forget(from, now.addSecs(60));
    m_log->record("memory", "forgot", {{"count", n}});
    say(n == 0 ? QStringLiteral("There was nothing to forget.")
               : QStringLiteral("Forgotten. %1 memor%2 gone.")
                     .arg(n)
                     .arg(n == 1 ? "y" : "ies"));
    emit m_memory->stored(0);
    return;
  }
  if (a == "memory.deleteOlder") {
    const int days = std::max(1, args.value("days").toInt());
    const int n = m_store->dropScreenshotsBefore(
        QDateTime::currentDateTime().addDays(-days));
    m_log->record("memory", "dropped-screenshots", {{"count", n}, {"days", days}});
    say(QStringLiteral("Deleted %1 screenshot%2 older than %3 day%4. Pinned "
                       "ones are kept.")
            .arg(n)
            .arg(n == 1 ? "" : "s")
            .arg(days)
            .arg(days == 1 ? "" : "s"));
    emit m_memory->stored(0);
    return;
  }
  if (a == "memory.pin") {
    const MemoryRecord latest = m_store->latest();
    if (latest.id == 0) {
      say(QStringLiteral("I don't have a recent memory to keep."));
      return;
    }
    m_store->setPinned(latest.id, true);
    say(QStringLiteral("Pinned. I'll keep that one."));
    emit m_memory->stored(latest.id);
    return;
  }
  if (a == "memory.excludeCurrent") {
    const WindowInfo window = desktop::activeWindow();
    if (!window.valid || window.appClass.isEmpty()) {
      say(QStringLiteral("I can't tell which app that is."));
      return;
    }
    QStringList apps = m_settings->list("privacy.excludedApps");
    if (!apps.contains(window.appClass, Qt::CaseInsensitive))
      apps << window.appClass;
    m_settings->set("privacy.excludedApps", apps);
    m_settings->flush();
    m_log->record("privacy", "app-excluded", {{"app", window.appClass}});
    say(QStringLiteral("I won't remember %1 again.").arg(window.appClass));
    return;
  }
  if (a == "memory.status") {
    say(QStringLiteral("Screen memory is %1. I have %2 memor%3 using %4.")
            .arg(m_memory->status())
            .arg(m_store->count())
            .arg(m_store->count() == 1 ? "y" : "ies")
            .arg(formatBytes(m_store->storageBytes())));
    return;
  }
  if (a == "memory.timeline") {
    emit timelineRequested();
    return;
  }
  if (a == "settings.open") {
    emit settingsRequested();
    return;
  }
  if (a == "settings.close") {
    emit settingsCloseRequested();
    return;
  }
  if (a == "listen.stop") {
    m_pushToTalk = false;
    m_deaf = true;
    m_mic->stop();
    say(QStringLiteral("Okay, I've stopped listening. Use \"nala listen\" "
                       "when you need me."),
        false);
    settle();
    return;
  }
  if (a == "listen.start") {
    m_deaf = false;
    if (continuous())
      openMicrophone();
    else
      startListening();
    return;
  }
  if (a == "tts.mute" || a == "tts.unmute") {
    const bool mute = a == "tts.mute";
    m_settings->set("tts.muted", mute);
    if (mute) {
      m_fish->stop();
      m_speaker->stop();
    }
    say(mute ? QStringLiteral("Okay, bubbles only.")
             : QStringLiteral("I can talk again!"));
    return;
  }
  if (a == "nala.sleep") {
    stop();
    m_asleep = true;
    companion("sleep");
    settle();
    return;
  }
  if (a == "nala.wake") {
    m_asleep = false;
    companion("wake");
    say(QStringLiteral("I'm up!"), false);
    return;
  }
  if (a == "nala.wink" || a == "nala.demo") {
    companion(a.mid(5));
    return;
  }
  if (a == "window.closeAll") {
    callTool("window.close_all", {}, [this](const QJsonObject &result) {
      say(result.value("ok").toBool()
              ? QStringLiteral("Closed %1 window%2.")
                    .arg(result.value("closed").toInt())
                    .arg(result.value("closed").toInt() == 1 ? "" : "s")
              : QStringLiteral("I didn't close anything: %1")
                    .arg(result.value("error").toString()));
    });
    return;
  }
  if (a == "apps.launch" || a == "apps.close" || a == "apps.focus") {
    const QString name = args.value("name").toString();
    // Not an app she knows of: perhaps the model can make sense of it.
    if (a == "apps.launch" && !m_apps.find(name)) {
      if (m_settings->flag("llm.enabled"))
        think(route.text);
      else
        say(QStringLiteral("I couldn't find an app called %1.").arg(name));
      return;
    }
    const QString tool = a == "apps.launch" ? "apps.launch"
                         : a == "apps.close" ? "apps.close"
                                             : "apps.focus";
    callTool(tool, {{"name", name}}, [this, a, name, route](const QJsonObject &r) {
      if (r.value("ok").toBool()) {
        say(a == "apps.launch"  ? QStringLiteral("Opening %1.")
                                      .arg(r.value("app").toString(name))
            : a == "apps.close" ? QStringLiteral("Closed %1.").arg(name)
                                : QStringLiteral("There you go."),
            a != "apps.focus");
        return;
      }
      if (m_settings->flag("llm.enabled") && a != "apps.launch" &&
          r.value("error").toString().startsWith("no window"))
        think(route.text);
      else
        say(r.value("error").toString());
    });
    return;
  }
  // An action the router knows but nothing here handles would be a bug.
  m_log->record("error", "unhandled-action", {{"action", a}});
  think(route.text);
}

QSet<QString> Assistant::categories() const {
  QSet<QString> on = {"memory", "nala"};
  if (!m_settings->flag("agent.enabled"))
    return on;
  on << "window" << "apps";
  if (m_settings->flag("agent.input"))
    on << "computer";
  if (m_settings->flag("agent.files"))
    on << "files";
  if (m_settings->flag("agent.browser"))
    on << "browser";
  if (m_settings->flag("agent.shell"))
    on << "shell";
  return on;
}

QJsonObject Assistant::systemMessage() const {
  QString prompt = m_settings->string("llm.systemPrompt").trimmed();
  if (prompt.isEmpty())
    prompt = QString::fromLatin1(kPersona);
  prompt += QStringLiteral("\n\nIt is %1. Screen memory is %2. You %3 see "
                           "images.")
                .arg(QLocale().toString(QDateTime::currentDateTime(),
                                        QStringLiteral("dddd d MMMM yyyy, h:mm AP")),
                     m_memory->status(),
                     m_settings->flag("llm.vision") ? "can" : "cannot");
  return {{"role", "system"}, {"content", prompt}};
}

void Assistant::think(const QString &text) {
  if (!m_settings->flag("llm.enabled")) {
    say(QStringLiteral("I only know simple commands right now; my language "
                       "model is switched off."));
    return;
  }
  ++m_turn;
  m_steps = 0;
  m_pendingCalls.clear();
  m_turnText = text;
  m_turnTainted = false;
  m_turnMessages = QJsonArray{systemMessage()};
  // The last few exchanges, so "open it" knows what "it" was.
  const int keep = m_settings->integer("llm.contextTurns") * 2;
  for (qsizetype i = std::max<qsizetype>(0, m_history.size() - keep);
       i < m_history.size(); ++i)
    m_turnMessages.append(m_history.at(i));
  m_turnMessages.append(QJsonObject{{"role", "user"}, {"content", text}});

  m_thinking = true;
  settle();
  m_log->record("llm", "request",
                {{"model", m_llm->model()}, {"messages", int(m_turnMessages.size())}});
  m_log->trace("llm", "prompt", {{"text", text}});
  m_llm->chat(m_turnMessages, m_settings->flag("llm.toolCalling")
                                  ? m_tools.schema(categories())
                                  : QJsonArray());
}

void Assistant::injectModelReply(const LlmReply &reply) { onModelReply(reply); }

void Assistant::onModelReply(const LlmReply &reply) {
  m_thinking = false;
  m_turnMessages.append(reply.message);
  m_log->record("llm", "reply",
                {{"tools", int(reply.toolCalls.size())},
                 {"chars", int(reply.content.size())},
                 {"finish", reply.finishReason}});

  if (!reply.toolCalls.isEmpty() &&
      m_steps < m_settings->integer("agent.maxSteps")) {
    ++m_steps;
    for (const ToolCall &call : reply.toolCalls)
      m_pendingCalls.enqueue(call);
    runToolCalls();
    return;
  }

  QString text = reply.content;
  if (text.isEmpty())
    text = reply.toolCalls.isEmpty()
               ? QStringLiteral("Hmm, I don't have an answer for that.")
               : QStringLiteral("I stopped after %1 steps without finishing.")
                     .arg(m_steps);
  // Only the exchange itself is remembered, not the tool traffic: it keeps
  // the context small and stops stale results being trusted later.
  m_history.append(QJsonObject{{"role", "user"}, {"content", m_turnText}});
  m_history.append(QJsonObject{{"role", "assistant"}, {"content", text}});
  while (m_history.size() > 100)
    m_history.removeFirst();
  say(text);
}

void Assistant::runToolCalls() {
  if (m_pendingCalls.isEmpty()) {
    m_thinking = true;
    settle();
    m_llm->chat(m_turnMessages, m_settings->flag("llm.toolCalling")
                                    ? m_tools.schema(categories())
                                    : QJsonArray());
    return;
  }
  const ToolCall call = m_pendingCalls.dequeue();
  const int turn = m_turn;
  const auto finish = [this, call, turn](QJsonObject result) {
    if (turn != m_turn)
      return; // stopped meanwhile
    // Pictures go to the model as a picture, not as base64 in a string.
    const QString image = result.take("image").toString();
    m_turnMessages.append(QJsonObject{
        {"role", "tool"},
        {"tool_call_id", call.id},
        {"content", QString::fromUtf8(
                        QJsonDocument(result).toJson(QJsonDocument::Compact))
                        .left(16000)}});
    if (!image.isEmpty())
      m_turnMessages.append(QJsonObject{
          {"role", "user"},
          {"content",
           QJsonArray{QJsonObject{{"type", "text"},
                                  {"text", "The screenshot you asked for."}},
                      QJsonObject{{"type", "image_url"},
                                  {"image_url",
                                   QJsonObject{{"url", "data:image/jpeg;base64," +
                                                           image}}}}}}});
    runToolCalls();
  };
  const QString name = m_tools.fromWire(call.name);
  if (name.isEmpty()) {
    finish(fail(QStringLiteral("There is no tool called %1.").arg(call.name)));
    return;
  }
  if (!call.argumentsValid) {
    finish(fail(QStringLiteral("The arguments were not valid JSON.")));
    return;
  }
  callTool(name, call.arguments, finish);
}

void Assistant::callTool(const QString &name, const QJsonObject &args,
                         std::function<void(QJsonObject)> done) {
  const Tool *tool = m_tools.find(name);
  if (!tool) {
    done(fail(QStringLiteral("There is no tool called %1.").arg(name)));
    return;
  }
  const QString invalid = ToolRegistry::validate(tool->parameters, args);
  if (!invalid.isEmpty()) {
    m_log->record("tool", "rejected", {{"tool", name}, {"reason", invalid}});
    done(fail(QStringLiteral("Invalid arguments: %1").arg(invalid)));
    return;
  }
  if (!categories().contains(tool->category)) {
    m_log->record("tool", "disabled", {{"tool", name}});
    done(fail(QStringLiteral("The user has switched off %1 access.")
                  .arg(tool->category)));
    return;
  }
  const Risk risk = tool->riskFor ? tool->riskFor(args) : tool->risk;
  const Decision decision = decide(risk, m_settings->string("agent.confirm"));
  m_log->record("tool", "call",
                {{"tool", name},
                 {"args", args},
                 {"risk", riskName(risk)},
                 {"decision", decision == Decision::Allow     ? "allow"
                              : decision == Decision::Confirm ? "confirm"
                                                              : "deny"}});
  if (decision == Decision::Deny) {
    done(fail(QStringLiteral("Not allowed.")));
    return;
  }

  const auto runIt = [this, tool, name, args, done] {
    m_acting = true;
    settle();
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    tool->run(args, [this, name, done, clock](QJsonObject result) {
      m_acting = false;
      settle();
      m_log->record("tool", "result",
                    {{"tool", name},
                     {"ok", result.value("ok").toBool()},
                     {"ms", clock->elapsed()},
                     {"error", result.value("error").toString()}});
      done(result);
    });
  };
  if (decision == Decision::Allow) {
    runIt();
    return;
  }
  const QString summary = tool->summary ? tool->summary(args) : name;
  confirm(summary, [this, name, runIt, done](bool yes) {
    if (yes) {
      runIt();
      return;
    }
    m_log->record("tool", "declined", {{"tool", name}});
    done(fail(QStringLiteral("The user said no.")));
  });
}

// --- asking first --------------------------------------------------------------

void Assistant::confirm(const QString &question,
                        std::function<void(bool)> then) {
  if (m_onAnswer)
    answer(false); // one question at a time
  m_question = question.endsWith('?') ? question : question + '?';
  m_onAnswer = std::move(then);
  m_questionTimer.start();
  m_log->record("tool", "confirmation-asked");
  emit questionChanged();
  say(m_question);
  settle();
}

void Assistant::answer(bool yes) {
  if (!m_onAnswer)
    return;
  auto then = std::move(m_onAnswer);
  m_onAnswer = nullptr;
  m_question.clear();
  m_questionTimer.stop();
  m_log->record("tool", yes ? "confirmed" : "refused");
  emit questionChanged();
  settle();
  then(yes);
}

void Assistant::stop() {
  ++m_turn;
  m_llm->cancel();
  m_pendingCalls.clear();
  m_thinking = false;
  m_transcribing = false;
  m_lastPcm.clear();
  m_whisperServer->cancel();
  m_whisperCli->cancel();
  m_fish->stop();
  m_speaker->stop();
  m_speech.clear();
  m_synthesizing = false;
  m_speaking = false;
  m_followUp.stop();
  m_errorTimer.stop();
  if (m_onAnswer)
    answer(false);
  m_log->record("state", "stopped");
  dismissBubble();
  settle();
}

// --- speaking ------------------------------------------------------------------

void Assistant::say(const QString &text, bool speak) {
  m_bubble = text;
  emit bubbleChanged();
  emit said(text);
  m_log->trace("tts", "say", {{"text", text}});
  // Long enough to read, and longer than it takes to hear.
  m_bubbleTimer.start(std::clamp(int(text.size()) * 80, 4000, 20000));

  const bool voice = speak && !m_testing && m_settings->flag("tts.enabled") &&
                     !m_settings->flag("tts.muted") &&
                     m_settings->string("tts.engine") == "fish" &&
                     !m_voiceBroken;
  m_fish->stop();
  m_speaker->stop();
  m_speech.clear();
  m_synthesizing = false;
  if (!voice) {
    m_speaking = false;
    settle();
    return;
  }
  m_speech = splitSentences(text);
  m_speaking = true;
  settle();
  speakNext();
}

void Assistant::speakNext() {
  if (m_speech.isEmpty()) {
    finishSpeaking();
    return;
  }
  m_synthesizing = true;
  m_fish->synthesize(m_speech.takeFirst());
}

void Assistant::finishSpeaking() {
  const bool was = m_speaking;
  m_speaking = false;
  if (was)
    m_bubbleTimer.start(4000);
  settle();
  // A question asked out loud wants an answer out loud.
  if (was && !m_question.isEmpty() && !continuous() &&
      m_settings->flag("stt.enabled") && !m_testing)
    startListening();
}

void Assistant::dismissBubble() {
  if (m_speaking || !m_question.isEmpty())
    return;
  m_bubble.clear();
  m_heard.clear();
  emit bubbleChanged();
}

// --- memory for the UI -----------------------------------------------------------

QVariantList Assistant::timeline(const QString &text, const QString &app,
                                 int days) const {
  const QDateTime now = QDateTime::currentDateTime();
  const QVector<MemoryRecord> records =
      m_store->list(days > 0 ? QDateTime(now.date().addDays(1 - days), QTime(0, 0))
                             : QDateTime(),
                    QDateTime(), app, text, 2000);
  // Newest first, grouped by day, and within a day into episodes: the same
  // app with no gap longer than ten minutes.
  QVariantList result;
  QVariantMap day;
  QVariantList episodes;
  QVariantMap episode;
  QVariantList ids;
  QDateTime episodeEnd;
  const auto closeEpisode = [&] {
    if (ids.isEmpty())
      return;
    episode.insert("ids", ids);
    episode.insert("count", ids.size());
    episodes << episode;
    episode.clear();
    ids.clear();
  };
  const auto closeDay = [&] {
    closeEpisode();
    if (!episodes.isEmpty()) {
      day.insert("episodes", episodes);
      result << day;
    }
    episodes.clear();
    day.clear();
  };
  QDate currentDay;
  for (const MemoryRecord &r : records) {
    if (r.started.date() != currentDay) {
      closeDay();
      currentDay = r.started.date();
      day.insert("label", dayLabel(currentDay));
      day.insert("date", currentDay);
    }
    const bool sameEpisode = !ids.isEmpty() &&
                             episode.value("app").toString() == r.app &&
                             r.lastSeen.secsTo(episodeEnd) < 600;
    if (!sameEpisode) {
      closeEpisode();
      episode.insert("app", r.app);
      episode.insert("title", r.title);
      episode.insert("activity", r.activity);
      episode.insert("summary", r.summary);
      episode.insert("end", r.lastSeen);
      episode.insert("representative", r.id);
      episode.insert("pinned", false);
    }
    episode.insert("start", r.started);
    episode.insert("time", QLocale().toString(r.started.time(),
                                              QLocale::ShortFormat));
    if (r.pinned)
      episode.insert("pinned", true);
    if (episode.value("summary").toString().isEmpty() && !r.summary.isEmpty())
      episode.insert("summary", r.summary);
    episodeEnd = r.started;
    ids << r.id;
  }
  closeDay();
  return result;
}

QVariantMap Assistant::memoryDetail(qint64 id) const {
  const MemoryRecord record = m_store->get(id);
  if (record.id == 0)
    return {};
  QVariantMap out = record.toVariant();
  out.insert("when", when(record.started));
  if (!record.shotPath.isEmpty())
    out.insert("image", QUrl::fromLocalFile(record.shotPath).toString());
  QVariantList artifacts;
  for (const Artifact &artifact : m_store->artifactsFor(id))
    artifacts << artifact.toVariant();
  out.insert("artifacts", artifacts);
  // Neighbours in time, for "related".
  QVariantList related;
  for (const MemoryRecord &near :
       m_store->list(record.started.addSecs(-1800),
                     record.started.addSecs(1800), {}, {}, 8))
    if (near.id != id)
      related << QVariantMap{{"id", near.id},
                             {"app", near.app},
                             {"title", near.title},
                             {"when", when(near.started)}};
  out.insert("related", related);
  return out;
}

QStringList Assistant::memoryApps() const { return m_store->apps(); }

bool Assistant::pinMemory(qint64 id, bool pinned) {
  const bool done = m_store->setPinned(id, pinned);
  if (done) {
    m_log->record("memory", pinned ? "pinned" : "unpinned", {{"id", id}});
    emit m_memory->stored(id);
  }
  return done;
}

bool Assistant::forgetMemory(qint64 id) {
  const bool done = m_store->forgetOne(id) > 0;
  if (done) {
    m_log->record("memory", "deleted", {{"id", id}});
    emit m_memory->stored(0);
  }
  return done;
}

int Assistant::forgetMinutes(int minutes) {
  const QDateTime now = QDateTime::currentDateTime();
  const int n = m_store->forget(now.addSecs(-qint64(minutes) * 60),
                                now.addSecs(60));
  m_log->record("memory", "forgot", {{"count", n}, {"minutes", minutes}});
  emit m_memory->stored(0);
  return n;
}

QVariantList Assistant::searchMemory(const QString &query) const {
  QVariantList out;
  for (const MemoryRecord &r :
       m_store->search(query, QDateTime::currentDateTime(), 30)) {
    QVariantMap item = r.toVariant();
    item.insert("when", when(r.started));
    out << item;
  }
  return out;
}

QString Assistant::formatBytes(qint64 bytes) const {
  return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

// --- health ----------------------------------------------------------------------

void Assistant::diagnose() {
  diagnose([](const QString &) {});
}

void Assistant::diagnose(std::function<void(QString)> done) {
  struct Report {
    QVariantList items;
    int pending = 0;
    std::function<void(QString)> done;
  };
  auto report = std::make_shared<Report>();
  report->done = std::move(done);
  const auto add = [report](const QString &name, bool ok, const QString &detail,
                            bool optional = false) {
    report->items << QVariantMap{
        {"name", name}, {"ok", ok}, {"detail", detail}, {"optional", optional}};
  };
  const auto finish = [this, report] {
    if (--report->pending > 0)
      return;
    m_setup = report->items;
    emit setupChanged();
    QString text;
    for (const QVariant &v : report->items) {
      const QVariantMap item = v.toMap();
      text += QStringLiteral("%1 %2: %3\n")
                  .arg(item.value("ok").toBool()          ? "ok  "
                       : item.value("optional").toBool() ? "--  "
                                                          : "FAIL")
                  .arg(item.value("name").toString(),
                       item.value("detail").toString());
    }
    report->done(text);
  };
  // Asynchronous checks first, so the synchronous ones overlap with them.
  report->pending = 4;

  m_llm->listModels([this, add, finish](const QStringList &ids,
                                        const QString &error) {
    const QString using_ = m_settings->string("llm.model").isEmpty()
                               ? ids.value(0)
                               : m_settings->string("llm.model");
    add(QStringLiteral("language model"), !ids.isEmpty(),
        ids.isEmpty() ? QStringLiteral("%1 at %2")
                            .arg(error, m_settings->string("llm.endpoint"))
                      : QStringLiteral("%1 (%2 available at %3)")
                            .arg(using_)
                            .arg(ids.size())
                            .arg(m_settings->string("llm.endpoint")));
    finish();
  });

  const auto probe = [this](const QUrl &url,
                            std::function<void(bool, QString)> then) {
    QNetworkRequest request(url);
    request.setTransferTimeout(3000);
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, then] {
      reply->deleteLater();
      // Any HTTP answer at all means something is listening.
      const int status =
          reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      then(status > 0, status > 0 ? QString() : reply->errorString());
    });
  };

  const QString sttUrl = m_settings->string("stt.serverUrl");
  probe(QUrl(sttUrl), [this, add, finish, sttUrl](bool up, QString) {
    m_serverDead = !up;
    const QString cli =
        QStandardPaths::findExecutable(m_settings->string("stt.binary"));
    const QString model = m_settings->string("stt.model").isEmpty()
                              ? WhisperCli::defaultModel()
                              : m_settings->string("stt.model");
    const bool cliReady = !cli.isEmpty() && QFileInfo::exists(model);
    add(QStringLiteral("whisper-server"), up,
        up ? sttUrl : QStringLiteral("not running at %1").arg(sttUrl), cliReady);
    add(QStringLiteral("whisper-cli"), cliReady,
        cli.isEmpty() ? QStringLiteral("%1 not found")
                            .arg(m_settings->string("stt.binary"))
        : !QFileInfo::exists(model)
            ? QStringLiteral("no model at %1").arg(model)
            : QStringLiteral("%1 with %2").arg(cli, QFileInfo(model).fileName()),
        up);
    finish();
  });

  QUrl health(m_settings->string("tts.endpoint"));
  health.setPath(health.path() + "/v1/health");
  probe(health, [this, add, finish](bool up, QString error) {
    add(QStringLiteral("Fish Speech"), up,
        up ? m_settings->string("tts.endpoint")
           : QStringLiteral("%1 (%2) -- replies will be text only")
                 .arg(m_settings->string("tts.endpoint"), error),
        true);
    // Known to be missing: answer in the bubble straight away rather than
    // failing an attempt at every reply.
    m_voiceBroken = !up;
    finish();
  });

  const QStringList mics = Microphone::devices();
  add(QStringLiteral("microphone"), !mics.isEmpty(),
      mics.isEmpty() ? QStringLiteral("none found") : mics.join(", "));
  const QStringList outs = Speaker::devices();
  add(QStringLiteral("speakers"), !outs.isEmpty(),
      outs.isEmpty() ? QStringLiteral("none found") : outs.join(", "));

  const desktop::Tools tools = desktop::detectTools();
  add(QStringLiteral("Hyprland"), desktop::hyprlandAvailable(),
      desktop::hyprlandAvailable()
          ? QStringLiteral("window control available")
          : QStringLiteral("not running; window tools and screen memory "
                           "need it"));
  add(QStringLiteral("grim"), tools.grim,
      tools.grim ? QStringLiteral("screenshots available")
                 : QStringLiteral("missing; screenshots and screen memory "
                                  "need it"),
      true);
  add(QStringLiteral("wtype"), tools.wtype,
      tools.wtype ? QStringLiteral("typing available")
                  : QStringLiteral("missing; typing and key presses need it"),
      true);
  add(QStringLiteral("ydotool"), tools.ydotool && tools.ydotoold,
      !tools.ydotool    ? QStringLiteral("missing; clicking and scrolling need it")
      : !tools.ydotoold ? QStringLiteral("installed, but ydotoold is not running")
                        : QStringLiteral("clicking available"),
      true);
  add(QStringLiteral("screen memory"), m_store->isOpen(),
      QStringLiteral("%1, %2 memories, %3%4")
          .arg(m_memory->status())
          .arg(m_store->count())
          .arg(formatBytes(m_store->storageBytes()))
          .arg(m_store->fullText() ? QString()
                                   : QStringLiteral(", no full-text search")));
  finish();
}

// --- tools -----------------------------------------------------------------------

void Assistant::registerTools() {
  using namespace schema;
  const QString home = QDir::homePath();

  // --- screen and input ---
  m_tools.add(Tool{
      "computer.screenshot",
      "Look at the focused monitor. Returns the image, and the size that "
      "click coordinates refer to. Only useful when you can see images.",
      object({}), Risk::Safe, "computer", nullptr, nullptr,
      [this](const QJsonObject &, Tool::Done done) {
        if (!m_settings->flag("llm.vision")) {
          done(fail("Vision is switched off, so a screenshot would not help."));
          return;
        }
        // Not saved anywhere, but still not taken of something private.
        const WindowInfo window = desktop::activeWindow();
        const PrivacyCheck check = privacyGate(
            window, m_settings->list("privacy.excludedApps"),
            m_settings->list("privacy.excludedTitles"),
            m_settings->flag("privacy.blockSensitive"), false);
        if (!check.allowed) {
          done(fail("The focused window is private (" + check.reason +
                    "), so I won't look at it."));
          return;
        }
        const desktop::Monitor monitor = desktop::focusedMonitor();
        desktop::capture({}, monitor.name,
                         [this, monitor, done](QImage image, QString error) {
                           if (image.isNull()) {
                             done(fail(error));
                             return;
                           }
                           const QImage small =
                               image.width() > 1600
                                   ? image.scaledToWidth(1600,
                                                         Qt::SmoothTransformation)
                                   : image;
                           m_shotArea = monitor.geometry;
                           m_shotSize = small.size();
                           QByteArray jpeg;
                           QBuffer buffer(&jpeg);
                           buffer.open(QIODevice::WriteOnly);
                           small.save(&buffer, "JPEG", 80);
                           done(ok({{"width", small.width()},
                                    {"height", small.height()},
                                    {"image", QString::fromLatin1(jpeg.toBase64())}}));
                         },
                         this);
      }});

  // Screenshot pixels to desktop coordinates, or straight through if no
  // screenshot has been taken.
  const auto toDesktop = [this](int x, int y) {
    if (m_shotSize.isEmpty() || m_shotArea.isEmpty())
      return QPoint(x, y);
    return QPoint(m_shotArea.x() + x * m_shotArea.width() / m_shotSize.width(),
                  m_shotArea.y() + y * m_shotArea.height() / m_shotSize.height());
  };
  const QJsonObject point =
      object({{"x", integer("Horizontal position in screenshot pixels", 0, 20000)},
              {"y", integer("Vertical position in screenshot pixels", 0, 20000)},
              {"button", oneOf("Mouse button", {"left", "right", "middle"})},
              {"count", integer("1 for a click, 2 for a double click", 1, 3)}},
             {"x", "y"});
  m_tools.add(Tool{
      "computer.click", "Move the pointer to a point and click it.", point,
      Risk::Medium, "computer",
      [](const QJsonObject &a) {
        return QStringLiteral("Click at %1, %2").arg(a.value("x").toInt()).arg(
            a.value("y").toInt());
      },
      nullptr,
      [toDesktop](const QJsonObject &a, Tool::Done done) {
        const QPoint at = toDesktop(a.value("x").toInt(), a.value("y").toInt());
        if (!desktop::moveCursor(at.x(), at.y())) {
          done(fail("Could not move the pointer (Hyprland is needed)."));
          return;
        }
        const QString button = a.value("button").toString("left");
        QString error;
        if (!desktop::click(button == "right" ? 2 : button == "middle" ? 3 : 1,
                            a.value("count").toInt(1), &error))
          done(fail(error));
        else
          done(ok({{"at", QJsonArray{at.x(), at.y()}}}));
      }});
  m_tools.add(Tool{
      "computer.move_mouse", "Move the pointer without clicking.",
      object({{"x", integer("Horizontal position in screenshot pixels", 0, 20000)},
              {"y", integer("Vertical position in screenshot pixels", 0, 20000)}},
             {"x", "y"}),
      Risk::Low, "computer", nullptr, nullptr,
      [toDesktop](const QJsonObject &a, Tool::Done done) {
        const QPoint at = toDesktop(a.value("x").toInt(), a.value("y").toInt());
        done(desktop::moveCursor(at.x(), at.y())
                 ? ok()
                 : fail("Could not move the pointer (Hyprland is needed)."));
      }});
  m_tools.add(Tool{
      "computer.scroll", "Scroll whatever is under the pointer.",
      object({{"direction", oneOf("Which way", {"up", "down", "left", "right"})},
              {"amount", integer("How far, in notches", 1, 30)}},
             {"direction"}),
      Risk::Low, "computer", nullptr, nullptr,
      [](const QJsonObject &a, Tool::Done done) {
        const QString d = a.value("direction").toString();
        const int n = a.value("amount").toInt(5);
        QString error;
        const bool done_ = desktop::scroll(d == "left" ? -n : d == "right" ? n : 0,
                                           d == "up" ? n : d == "down" ? -n : 0,
                                           &error);
        done(done_ ? ok() : fail(error));
      }});
  m_tools.add(Tool{
      "computer.type", "Type text into the focused window.",
      object({{"text", string("What to type", 4000)}}, {"text"}), Risk::Medium,
      "computer",
      [](const QJsonObject &a) {
        const QString text = a.value("text").toString();
        return QStringLiteral("Type \"%1\"")
            .arg(text.size() > 60 ? text.left(57) + "…" : text);
      },
      nullptr,
      [](const QJsonObject &a, Tool::Done done) {
        QString error;
        done(desktop::typeText(a.value("text").toString(), &error) ? ok()
                                                                   : fail(error));
      }});
  m_tools.add(Tool{
      "computer.keypress",
      "Press a key or shortcut in the focused window, e.g. \"ctrl+t\", "
      "\"alt+Left\", \"Return\".",
      object({{"keys", string("The key combination", 60)}}, {"keys"}),
      Risk::Medium, "computer",
      [](const QJsonObject &a) {
        return QStringLiteral("Press %1").arg(a.value("keys").toString());
      },
      nullptr,
      [](const QJsonObject &a, Tool::Done done) {
        QString error;
        done(desktop::pressKeys(a.value("keys").toString(), &error)
                 ? ok()
                 : fail(error));
      }});

  // --- windows ---
  m_tools.add(Tool{
      "window.list", "List open windows with their address, app and title.",
      object({}), Risk::Safe, "window", nullptr, nullptr,
      [this](const QJsonObject &, Tool::Done done) {
        if (!desktop::hyprlandAvailable()) {
          done(fail("Window control needs Hyprland."));
          return;
        }
        QJsonArray list;
        for (const WindowInfo &w : desktop::windows()) {
          // Private windows are listed by app only.
          const bool open =
              privacyGate(w, m_settings->list("privacy.excludedApps"),
                          m_settings->list("privacy.excludedTitles"),
                          m_settings->flag("privacy.blockSensitive"), false)
                  .allowed;
          list.append(QJsonObject{{"address", w.address},
                                  {"app", w.appClass},
                                  {"title", open ? w.title : "(private)"}});
        }
        m_turnTainted = true;
        done(ok({{"windows", list}}));
      }});
  m_tools.add(Tool{
      "window.focus", "Bring a window to the front, by address.",
      object({{"address", string("Window address from window_list", 20)}},
             {"address"}),
      Risk::Low, "window", nullptr, nullptr,
      [](const QJsonObject &a, Tool::Done done) {
        done(desktop::focusWindow(a.value("address").toString())
                 ? ok()
                 : fail("No such window."));
      }});
  m_tools.add(Tool{
      "window.close", "Close a window, by address. Unsaved work may be lost.",
      object({{"address", string("Window address from window_list", 20)}},
             {"address"}),
      Risk::Medium, "window",
      [](const QJsonObject &a) {
        for (const WindowInfo &w : desktop::windows())
          if (w.address == a.value("address").toString())
            return QStringLiteral("Close %1").arg(w.appClass);
        return QStringLiteral("Close that window");
      },
      nullptr,
      [](const QJsonObject &a, Tool::Done done) {
        done(desktop::closeWindow(a.value("address").toString())
                 ? ok()
                 : fail("No such window."));
      }});
  m_tools.add(Tool{
      "window.close_all", "Close every open window. Unsaved work may be lost.",
      object({}), Risk::Medium, "window",
      [](const QJsonObject &) {
        const int n = int(desktop::windows().size());
        return QStringLiteral("Close all %1 window%2").arg(n).arg(n == 1 ? "" : "s");
      },
      nullptr,
      [](const QJsonObject &, Tool::Done done) {
        if (!desktop::hyprlandAvailable()) {
          done(fail("Window control needs Hyprland."));
          return;
        }
        int closed = 0;
        for (const WindowInfo &w : desktop::windows())
          if (desktop::closeWindow(w.address))
            ++closed;
        done(ok({{"closed", closed}}));
      }});

  // --- applications ---
  m_tools.add(Tool{
      "apps.launch",
      "Start an installed application by name, e.g. \"discord\", "
      "\"terminal\", \"browser\".",
      object({{"name", string("The application", 80)}}, {"name"}), Risk::Low,
      "apps",
      [this](const QJsonObject &a) {
        const desktop::App *app = m_apps.find(a.value("name").toString());
        return QStringLiteral("Open %1")
            .arg(app ? app->name : a.value("name").toString());
      },
      nullptr,
      [this](const QJsonObject &a, Tool::Done done) {
        const desktop::App *app = m_apps.find(a.value("name").toString());
        if (!app) {
          done(fail("No installed application matches that name."));
          return;
        }
        QString error;
        if (desktop::launch(*app, &error))
          done(ok({{"app", app->name}}));
        else
          done(fail(error));
      }});
  m_tools.add(Tool{
      "apps.list", "List installed applications.", object({}), Risk::Safe,
      "apps", nullptr, nullptr,
      [this](const QJsonObject &, Tool::Done done) {
        done(ok({{"apps", QJsonArray::fromStringList(m_apps.names())}}));
      }});
  const auto matching = [](const QString &name) {
    QVector<WindowInfo> found;
    const QString want = name.toLower().simplified();
    for (const WindowInfo &w : desktop::windows())
      if (w.appClass.toLower().contains(want) ||
          w.initialClass.toLower().contains(want) ||
          w.title.toLower().contains(want))
        found << w;
    return found;
  };
  m_tools.add(Tool{
      "apps.focus", "Switch to an open application by name.",
      object({{"name", string("The application", 80)}}, {"name"}), Risk::Low,
      "apps", nullptr, nullptr,
      [matching](const QJsonObject &a, Tool::Done done) {
        const QVector<WindowInfo> found = matching(a.value("name").toString());
        if (found.isEmpty()) {
          done(fail("no window matches that name"));
          return;
        }
        done(desktop::focusWindow(found.first().address) ? ok()
                                                         : fail("could not focus"));
      }});
  m_tools.add(Tool{
      "apps.close", "Close every window of an application, by name.",
      object({{"name", string("The application", 80)}}, {"name"}),
      Risk::Medium, "apps",
      [matching](const QJsonObject &a) {
        const int n = int(matching(a.value("name").toString()).size());
        return QStringLiteral("Close %1 (%2 window%3)")
            .arg(a.value("name").toString())
            .arg(n)
            .arg(n == 1 ? "" : "s");
      },
      nullptr,
      [matching](const QJsonObject &a, Tool::Done done) {
        const QVector<WindowInfo> found = matching(a.value("name").toString());
        if (found.isEmpty()) {
          done(fail("no window matches that name"));
          return;
        }
        int closed = 0;
        for (const WindowInfo &w : found)
          if (desktop::closeWindow(w.address))
            ++closed;
        done(ok({{"closed", closed}}));
      }});

  // --- files ---
  const auto policy = [this, home] {
    return PathPolicy(m_settings->list("agent.fileRoots"), home);
  };
  m_tools.add(Tool{
      "files.read", "Read a text file (up to 64 KB).",
      object({{"path", string("Absolute path, or relative to home", 1000)}},
             {"path"}),
      Risk::Low, "files", nullptr, nullptr,
      [this, policy](const QJsonObject &a, Tool::Done done) {
        const PathCheck check = policy().check(a.value("path").toString(), false);
        if (!check.allowed) {
          done(fail("Not allowed: " + check.reason));
          return;
        }
        QFile file(check.path);
        if (!QFileInfo(check.path).isFile() || !file.open(QIODevice::ReadOnly)) {
          done(fail("Not a readable file."));
          return;
        }
        const QByteArray bytes = file.read(kMaxFileRead);
        if (bytes.contains('\0')) {
          done(fail("That is a binary file."));
          return;
        }
        m_turnTainted = true;
        done(ok({{"path", check.path},
                 {"content", QString::fromUtf8(bytes)},
                 {"truncated", file.size() > kMaxFileRead}}));
      }});
  m_tools.add(Tool{
      "files.list", "List a folder.",
      object({{"path", string("Absolute path, or relative to home", 1000)}},
             {"path"}),
      Risk::Safe, "files", nullptr, nullptr,
      [policy](const QJsonObject &a, Tool::Done done) {
        const PathCheck check = policy().check(a.value("path").toString(), false);
        if (!check.allowed || !QFileInfo(check.path).isDir()) {
          done(fail(check.allowed ? "Not a folder." : "Not allowed: " + check.reason));
          return;
        }
        QJsonArray entries;
        const QFileInfoList list =
            QDir(check.path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                           QDir::DirsFirst | QDir::Name);
        for (const QFileInfo &info : list.mid(0, 300))
          entries.append(QJsonObject{{"name", info.fileName()},
                                     {"dir", info.isDir()},
                                     {"bytes", info.isDir() ? 0 : info.size()},
                                     {"modified", info.lastModified().toString(Qt::ISODate)}});
        done(ok({{"path", check.path}, {"entries", entries},
                 {"truncated", list.size() > 300}}));
      }});
  m_tools.add(Tool{
      "files.search", "Find files whose names contain some text.",
      object({{"query", string("Part of the file name", 200)},
              {"path", string("Folder to search in; home by default", 1000)}},
             {"query"}),
      Risk::Safe, "files", nullptr, nullptr,
      [policy, home](const QJsonObject &a, Tool::Done done) {
        const PathPolicy rules = policy();
        const PathCheck root = rules.check(a.value("path").toString(home), false);
        if (!root.allowed || !QFileInfo(root.path).isDir()) {
          done(fail(root.allowed ? "Not a folder." : "Not allowed: " + root.reason));
          return;
        }
        const QString want = a.value("query").toString().toLower();
        QJsonArray found;
        int visited = 0;
        // Bounded, so a search of a huge home directory cannot hang her.
        QDirIterator it(root.path, QDir::AllEntries | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext() && visited < 50000 && found.size() < 50) {
          const QString path = it.next();
          ++visited;
          if (path.contains("/.") || path.contains("/node_modules/"))
            continue; // hidden trees and dependency dumps
          if (it.fileName().toLower().contains(want) &&
              !PathPolicy::sensitive(path, home))
            found.append(path);
        }
        done(ok({{"matches", found}, {"complete", !it.hasNext()}}));
      }});
  m_tools.add(Tool{
      "files.write",
      "Create or overwrite a text file. Always asks the user first.",
      object({{"path", string("Absolute path, or relative to home", 1000)},
              {"content", string("The full new contents", 200000)}},
             {"path", "content"}),
      Risk::High, "files",
      [policy](const QJsonObject &a) {
        const PathCheck check = policy().check(a.value("path").toString(), true);
        const QString path = check.allowed ? check.path : a.value("path").toString();
        return QFileInfo::exists(path)
                   ? QStringLiteral("Overwrite %1").arg(path)
                   : QStringLiteral("Create %1").arg(path);
      },
      nullptr,
      [this, policy](const QJsonObject &a, Tool::Done done) {
        const PathCheck check = policy().check(a.value("path").toString(), true);
        if (!check.allowed) {
          done(fail("Not allowed: " + check.reason));
          return;
        }
        QSaveFile file(check.path);
        if (!file.open(QIODevice::WriteOnly)) {
          done(fail("Could not open it for writing."));
          return;
        }
        file.write(a.value("content").toString().toUtf8());
        if (!file.commit()) {
          done(fail("Could not save it."));
          return;
        }
        // Something she made is worth remembering where it is.
        Artifact artifact;
        artifact.kind = "file";
        artifact.value = check.path;
        artifact.title = QFileInfo(check.path).fileName();
        artifact.lastSeen = QDateTime::currentDateTime();
        m_store->addArtifact(artifact);
        done(ok({{"path", check.path}}));
      }});

  // --- browser ---
  m_tools.add(Tool{
      "browser.open", "Open a web page in the default browser.",
      object({{"url", string("An http or https URL", 2000)}}, {"url"}),
      Risk::Low, "browser",
      [](const QJsonObject &a) {
        return QStringLiteral("Open %1").arg(a.value("url").toString().left(200));
      },
      // A URL can carry data out. Opening a plain address is harmless, but
      // one with a query or a long path -- or any URL after this turn has
      // read files or command output, which could be instructions planted
      // for the model -- needs a yes.
      [this](const QJsonObject &a) {
        const QUrl url(a.value("url").toString());
        const bool carries = url.hasQuery() || url.hasFragment() ||
                             url.path().size() > 60;
        return carries || m_turnTainted ? Risk::Medium : Risk::Low;
      },
      [this](const QJsonObject &a, Tool::Done done) {
        const QUrl url(a.value("url").toString(), QUrl::StrictMode);
        if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") ||
            url.host().isEmpty()) {
          done(fail("Only http and https addresses can be opened."));
          return;
        }
        if (!QProcess::startDetached("xdg-open", {url.toString(QUrl::FullyEncoded)})) {
          done(fail("xdg-open is not available."));
          return;
        }
        Artifact artifact;
        artifact.kind = url.host() == "github.com" ? "repo" : "url";
        artifact.value = url.toString();
        artifact.lastSeen = QDateTime::currentDateTime();
        m_store->addArtifact(artifact);
        done(ok());
      }});
  for (const auto &[name, keys, what] :
       std::initializer_list<std::tuple<const char *, const char *, const char *>>{
           {"browser.back", "alt+Left", "Go back a page in the focused browser."},
           {"browser.forward", "alt+Right", "Go forward a page in the focused browser."},
           {"browser.refresh", "F5", "Reload the page in the focused browser."}}) {
    const QString combo = QString::fromLatin1(keys);
    m_tools.add(Tool{QString::fromLatin1(name), QString::fromLatin1(what),
                     object({}), Risk::Low, "browser", nullptr, nullptr,
                     [combo](const QJsonObject &, Tool::Done done) {
                       QString error;
                       done(desktop::pressKeys(combo, &error) ? ok() : fail(error));
                     }});
  }

  // --- shell: off unless switched on, and always asked about ---
  m_tools.add(Tool{
      "shell.run",
      "Run a shell command in the user's home folder and return its output. "
      "The user is always asked first. Times out after 60 seconds.",
      object({{"command", string("A bash command", 4000)}}, {"command"}),
      Risk::High, "shell",
      [](const QJsonObject &a) {
        return QStringLiteral("Run \"%1\"").arg(a.value("command").toString());
      },
      nullptr,
      [this, home](const QJsonObject &a, Tool::Done done) {
        auto *process = new QProcess(this);
        process->setWorkingDirectory(home);
        process->setProcessChannelMode(QProcess::MergedChannels);
        auto *timer = new QTimer(process);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, process, [process] { process->kill(); });
        connect(process, &QProcess::finished, this,
                [this, process, done](int code, QProcess::ExitStatus status) {
                  process->deleteLater();
                  m_turnTainted = true;
                  const QByteArray out = process->readAll();
                  QJsonObject result{
                      {"exit", code},
                      {"killed", status != QProcess::NormalExit},
                      {"output", EventLog::redact(
                                     QString::fromUtf8(out.right(kMaxShellOutput)))},
                      {"truncated", out.size() > kMaxShellOutput}};
                  result.insert("ok", status == QProcess::NormalExit && code == 0);
                  done(result);
                });
        connect(process, &QProcess::errorOccurred, this,
                [process, done](QProcess::ProcessError error) {
                  if (error != QProcess::FailedToStart)
                    return;
                  process->deleteLater();
                  done(fail("bash could not be started."));
                });
        timer->start(60000);
        // This is the one tool whose purpose is to run what it is given; the
        // confirmation that precedes it is the safeguard.
        process->start("bash", {"-c", a.value("command").toString()});
      }});

  // --- memory ---
  m_tools.add(Tool{
      "memory.search",
      "Search what the user did before: windows, pages, files and "
      "repositories, by words and time (\"yesterday\", \"last week\").",
      object({{"query", string("What to look for, including any time", 300)}},
             {"query"}),
      Risk::Safe, "memory", nullptr, nullptr,
      [this](const QJsonObject &a, Tool::Done done) {
        const QString query = a.value("query").toString();
        const QDateTime now = QDateTime::currentDateTime();
        QJsonArray memories;
        for (const MemoryRecord &r : m_store->search(query, now, 12)) {
          QJsonObject item{{"id", r.id},
                           {"when", when(r.started)},
                           {"app", r.app},
                           {"title", r.title},
                           {"pinned", r.pinned}};
          if (!r.summary.isEmpty())
            item.insert("summary", r.summary);
          if (!r.urls.isEmpty())
            item.insert("urls", r.urls);
          QJsonArray refs;
          for (const Artifact &artifact : m_store->artifactsFor(r.id))
            refs.append(artifact.value);
          if (!refs.isEmpty())
            item.insert("references", refs);
          memories.append(item);
        }
        const TimeRange range = parseTimeRange(query, now);
        QJsonArray artifacts;
        for (const Artifact &artifact :
             m_store->artifacts(range.rest, range.valid ? range.from : QDateTime(),
                                range.valid ? range.to : QDateTime(), 12))
          artifacts.append(QJsonObject{{"kind", artifact.kind},
                                       {"value", artifact.value},
                                       {"title", artifact.title},
                                       {"lastSeen", when(artifact.lastSeen)}});
        // Titles and summaries are text other people wrote.
        m_turnTainted = true;
        done(ok({{"memories", memories},
                 {"references", artifacts},
                 {"screenMemory", m_memory->status()}}));
      }});
  m_tools.add(Tool{
      "memory.note",
      "Remember a fact or a reference for later, e.g. where a file was saved.",
      object({{"text", string("What to remember", 2000)}}, {"text"}), Risk::Low,
      "memory", nullptr, nullptr,
      [this](const QJsonObject &a, Tool::Done done) {
        MemoryRecord record;
        record.started = record.lastSeen = QDateTime::currentDateTime();
        record.source = "note";
        record.app = "nala";
        record.title = a.value("text").toString().left(200);
        record.summary = a.value("text").toString();
        const qint64 id = m_store->insert(record);
        for (Artifact artifact : findArtifacts(record.summary)) {
          artifact.memoryId = id;
          artifact.lastSeen = record.started;
          m_store->addArtifact(artifact);
        }
        done(id ? ok({{"id", id}}) : fail("Memory is unavailable."));
      }});
  m_tools.add(Tool{
      "memory.pin", "Keep a memory permanently, by id.",
      object({{"id", integer("Memory id from memory_search", 1, INT_MAX)}}, {"id"}),
      Risk::Low, "memory", nullptr, nullptr,
      [this](const QJsonObject &a, Tool::Done done) {
        done(pinMemory(a.value("id").toInteger(), true) ? ok()
                                                         : fail("No such memory."));
      }});
  m_tools.add(Tool{
      "memory.delete", "Forget a memory, by id.",
      object({{"id", integer("Memory id from memory_search", 1, INT_MAX)}}, {"id"}),
      Risk::Medium, "memory",
      [](const QJsonObject &) { return QStringLiteral("Forget that memory"); },
      nullptr,
      [this](const QJsonObject &a, Tool::Done done) {
        done(forgetMemory(a.value("id").toInteger()) ? ok()
                                                      : fail("No such memory."));
      }});

  // --- herself ---
  m_tools.add(Tool{
      "nala.settings_open", "Open Nala's settings window.", object({}),
      Risk::Safe, "nala", nullptr, nullptr,
      [this](const QJsonObject &, Tool::Done done) {
        emit settingsRequested();
        done(ok());
      }});
  m_tools.add(Tool{
      "nala.settings_close", "Close Nala's settings window.", object({}),
      Risk::Safe, "nala", nullptr, nullptr,
      [this](const QJsonObject &, Tool::Done done) {
        emit settingsCloseRequested();
        done(ok());
      }});
  // Pausing is offered to the model; resuming deliberately is not. Only the
  // user turns her eyes back on.
  m_tools.add(Tool{
      "nala.pause_screen_memory", "Stop recording screen memory.",
      object({{"minutes", integer("For how long; 0 until resumed", 0, 10080)}}),
      Risk::Safe, "nala", nullptr, nullptr,
      [this](const QJsonObject &a, Tool::Done done) {
        m_memory->pause(a.value("minutes").toInt(0));
        done(ok({{"status", m_memory->status()}}));
      }});
}
