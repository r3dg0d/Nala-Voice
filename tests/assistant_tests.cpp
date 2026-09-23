// Unit tests for the assistant: everything that can be checked without a
// microphone, a compositor, or a model server. Run with ctest, or directly:
//   QT_QPA_PLATFORM=offscreen build/nala-assistant-tests
#include "assistant.h"
#include "audioutil.h"
#include "commandrouter.h"
#include "desktop.h"
#include "eventlog.h"
#include "identity.h"
#include "llm.h"
#include "memory.h"
#include "policy.h"
#include "screenmemory.h"
#include "settings.h"
#include "speech.h"
#include "tools.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPainter>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>
#include <memory>
#include <vector>

namespace {

QVector<int16_t> tone(int ms, double amplitude, double hz = 220.0) {
  QVector<int16_t> out(audio::kSttRate * ms / 1000);
  for (int i = 0; i < out.size(); ++i)
    out[i] = int16_t(amplitude * 32767.0 *
                     std::sin(2.0 * M_PI * hz * i / audio::kSttRate));
  return out;
}

QVector<int16_t> noise(int ms, double amplitude, quint32 seed = 1) {
  QVector<int16_t> out(audio::kSttRate * ms / 1000);
  QRandomGenerator random(seed);
  for (int16_t &s : out)
    s = int16_t((random.generateDouble() * 2.0 - 1.0) * amplitude * 32767.0);
  return out;
}

// A picture with some structure, so the hash has something to describe.
QImage scene(int variant, QSize size = {640, 400}) {
  QImage image(size, QImage::Format_RGB32);
  image.fill(Qt::white);
  QPainter p(&image);
  p.setPen(Qt::NoPen);
  if (variant == 0) {
    p.setBrush(Qt::black);
    p.drawRect(0, 0, size.width() / 3, size.height());
    p.setBrush(Qt::blue);
    p.drawEllipse(QPoint(size.width() * 2 / 3, size.height() / 2), 80, 80);
  } else {
    p.setBrush(Qt::darkGreen);
    p.drawRect(size.width() / 2, 0, size.width() / 2, size.height() / 2);
    p.setBrush(Qt::red);
    p.drawRect(0, size.height() / 2, size.width() / 3, size.height() / 2);
  }
  return image;
}

bool touch(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly);
}

WindowInfo window(const QString &klass, const QString &title,
                  const QString &address = "0x1") {
  WindowInfo w;
  w.valid = true;
  w.address = address;
  w.appClass = klass;
  w.initialClass = klass;
  w.title = title;
  return w;
}

// A wake-word detector that detects when told to, and remembers whether it
// was asked to stop listening.
class MockWake : public wake::WakeWordBackend {
public:
  using WakeWordBackend::WakeWordBackend;
  QString name() const override { return "mock"; }
  bool initialize(QString *) override { return true; }
  bool ready() const override { return true; }
  void start() override { running = true; }
  void stop() override { running = false; }
  void pause() override { paused = true; }
  void resume(int graceMs) override {
    paused = false;
    lastGrace = graceMs;
  }
  void processAudio(const int16_t *, int count) override { heard += count; }
  bool registerWakeword(const wake::Model &) override { return true; }
  void removeWakeword(const QString &) override {}
  QStringList wakewords() const override { return {"hey nala"}; }
  void setSensitivity(double) override {}
  void setConfirmation(int, int) override {}
  void fire(const QString &phrase = "hey nala") {
    wake::Detection d;
    d.phrase = phrase;
    d.score = 0.9f;
    emit detected(d);
  }
  bool running = false, paused = false;
  int lastGrace = -1;
  qint64 heard = 0;
};

// A recogniser that "hears" whatever it is told to.
class ScriptedStt : public SpeechToText {
public:
  using SpeechToText::SpeechToText;
  QString name() const override { return "scripted"; }
  void transcribe(const QByteArray &, const QString &) override {
    ++calls;
    emit transcribed(next, 1);
  }
  void cancel() override {}
  QString next;
  int calls = 0;
};

QByteArray speech(int ms) {
  const QVector<int16_t> t = tone(ms, 0.3);
  return QByteArray(reinterpret_cast<const char *>(t.constData()), t.size() * 2);
}

} // namespace

class AssistantTests : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() {
    // Never let a test reach the real compositor: a tool that got past its
    // confirmation would act on the desktop the tests are running on.
    qunsetenv("HYPRLAND_INSTANCE_SIGNATURE");
    QVERIFY(!desktop::hyprlandAvailable());
  }

  // --- router --------------------------------------------------------------

  void routerFastCommands_data() {
    QTest::addColumn<QString>("said");
    QTest::addColumn<QString>("action");
    const QList<QPair<QString, QString>> cases = {
        {"Hey Nala, open settings.", "settings.open"},
        {"Open Nala settings", "settings.open"},
        {"close your preferences", "settings.close"},
        {"Stop.", "assistant.stop"},
        {"Never mind", "assistant.stop"},
        {"Quiet!", "assistant.stop"},
        {"stop talking", "assistant.stop"},
        {"Stop listening.", "listen.stop"},
        {"Pause memory.", "memory.pause"},
        {"Nala, turn off screen recording.", "memory.pause"},
        {"Nala, pause screen memory", "memory.pause"},
        {"Nala, stop remembering my screen.", "memory.pause"},
        {"privacy mode", "memory.pause"},
        {"Resume memory", "memory.resume"},
        {"Nala, resume screen recording.", "memory.resume"},
        {"Turn screen recording back on", "memory.resume"},
        {"Nala, turn memory back on.", "memory.resume"},
        {"Mute yourself", "tts.mute"},
        {"Open terminal.", "apps.launch"},
        {"Open browser", "apps.launch"},
        {"Open Discord.", "apps.launch"},
        {"Close all windows.", "window.closeAll"},
        {"Go to sleep.", "nala.sleep"},
        {"Wake up!", "nala.wake"},
        {"Nala, forget the last five minutes.", "memory.forget"},
        {"Nala, forget the last hour.", "memory.forget"},
        {"Forget today", "memory.forget"},
        {"Delete screenshots older than seven days", "memory.deleteOlder"},
        {"Keep this memory.", "memory.pin"},
        {"Remember this permanently.", "memory.pin"},
        {"Never record this app.", "memory.excludeCurrent"},
        {"Nala, pause memory for one hour.", "memory.pause"},
        {"How much storage are your memories using?", "memory.status"},
        {"yes", "confirm.yes"},
        {"go ahead", "confirm.yes"},
        {"no thanks", "confirm.no"},
        {"show me your memories", "memory.timeline"},
    };
    for (const auto &c : cases)
      QTest::newRow(qPrintable(c.first)) << c.first << c.second;
  }
  void routerFastCommands() {
    QFETCH(QString, said);
    QFETCH(QString, action);
    CommandRouter router;
    const Route route = router.route(said, Identity{}.addressForms());
    QVERIFY2(route.matched, qPrintable("not matched: " + route.text));
    QCOMPARE(route.action, action);
  }

  void routerEscalatesComplexRequests_data() {
    QTest::addColumn<QString>("said");
    QTest::newRow("feed") << "Hey Nala, open my X feed and doomscroll.";
    QTest::newRow("past") << "What was that GitHub project I looked at yesterday?";
    QTest::newRow("crash") << "Why did Minecraft crash?";
    QTest::newRow("resume") << "Write me a resume based on my previous work experience.";
    QTest::newRow("command") << "Open the terminal command that fixed NVIDIA last week.";
  }
  void routerEscalatesComplexRequests() {
    QFETCH(QString, said);
    CommandRouter router;
    const Route route = router.route(said, {"hey nala", "nala"});
    QVERIFY2(!route.matched, qPrintable("wrongly matched " + route.action));
    QVERIFY(!route.text.isEmpty());
  }

  void routerArguments() {
    CommandRouter router;
    QCOMPARE(router.route("forget the last five minutes").args.value("minutes").toInt(), 5);
    QCOMPARE(router.route("forget the last hour").args.value("minutes").toInt(), 60);
    QCOMPARE(router.route("pause memory for one hour").args.value("minutes").toInt(), 60);
    QCOMPARE(router.route("pause screen memory for half an hour").args.value("minutes").toInt(), 30);
    QCOMPARE(router.route("pause memory").args.value("minutes").toInt(), 0);
    QCOMPARE(router.route("delete screenshots older than two weeks").args.value("days").toInt(), 14);
    QCOMPARE(router.route("open discord").args.value("name").toString(), QString("discord"));
    QCOMPARE(router.route("forget everything").args.value("range").toString(), QString("all"));
  }

  void routerWakePhrases() {
    CommandRouter router;
    const QStringList wakes = {"hey nala", "ok nala", "nala"};
    Route r = router.route("Hey Nala.", wakes);
    QVERIFY(r.addressed);
    QVERIFY(r.text.isEmpty());
    r = router.route("stop, Nala", wakes);
    QVERIFY(r.addressed);
    QCOMPARE(r.action, QString("assistant.stop"));
    r = router.route("open discord", wakes);
    QVERIFY(!r.addressed);
    // Misheard names still pause memory -- and only that.
    r = router.route("Hey Artler, pause screen memory.", wakes);
    QCOMPARE(r.action, QString("memory.pause"));
    r = router.route("hey arlo turn off screen recording for one hour", wakes);
    QCOMPARE(r.action, QString("memory.pause"));
    QCOMPARE(r.args.value("minutes").toInt(), 60);
    r = router.route("hey arlo resume screen memory", wakes);
    QVERIFY(!r.matched);
    r = router.route("hey arlo open discord", wakes);
    QVERIFY(!r.matched);
    // After a detected wake, a misheard wake phrase still leaves any command
    // routable -- but only then.
    QCOMPARE(router.routeAfterWake("Hit Nala resume screen recording.", wakes).action,
             QString("memory.resume"));
    QCOMPARE(router.routeAfterWake("Hey Nana open settings", wakes).action,
             QString("settings.open"));
    QVERIFY(!router.route("Hit Nala resume screen recording.", wakes).matched);
    QVERIFY(!router.routeAfterWake("what was that repo from yesterday", wakes).matched);
    QVERIFY(!router.routeAfterWake("Hit Nala, don't open Discord.", wakes).matched);
    // "Nalanda" is not her name.
    r = router.route("nalanda university", wakes);
    QVERIFY(!r.addressed);
  }

  void routerNormalises() {
    QCOMPARE(CommandRouter::normalise("  Could you please OPEN Discord, for me?! "),
             QString("open discord"));
    QCOMPARE(CommandRouter::normalise("[BLANK_AUDIO]"), QString());
    QCOMPARE(CommandRouter::normalise("Don’t record this app."),
             QString("don't record this app"));
    QCOMPARE(CommandRouter::normalise("open github.com"), QString("open github.com"));
  }

  // --- audio ---------------------------------------------------------------

  void vadFindsAnUtterance() {
    audio::VoiceActivity vad;
    QVector<int16_t> input = noise(600, 0.002);
    input += tone(900, 0.3);
    input += noise(1000, 0.002, 2);
    int started = 0, ended = 0;
    // Odd chunk sizes, as a sound card delivers them.
    for (int at = 0; at < input.size(); at += 437) {
      const int n = std::min<int>(437, int(input.size()) - at);
      const auto event = vad.feed(input.constData() + at, n);
      started += event == audio::VoiceActivity::Started;
      ended += event == audio::VoiceActivity::Ended;
    }
    QCOMPARE(started, 1);
    QCOMPARE(ended, 1);
    const QVector<int16_t> speech = vad.utterance();
    const int ms = int(speech.size() * 1000 / audio::kSttRate);
    // The speech, the pre-roll before it and the silence that ended it.
    QVERIFY2(ms > 900 && ms < 2100, qPrintable(QString::number(ms)));
  }

  void vadIgnoresSilenceAndClicks() {
    audio::VoiceActivity vad;
    QVector<int16_t> input = noise(2000, 0.002);
    input += tone(30, 0.5); // a click, one frame long
    input += noise(1500, 0.002, 3);
    int events = 0;
    events += vad.feed(input.constData(), int(input.size())) != audio::VoiceActivity::None;
    QCOMPARE(events, 0);
  }

  void vadCapsLongUtterances() {
    audio::VadConfig config;
    config.maxUtteranceMs = 2000;
    audio::VoiceActivity vad(config);
    QVector<int16_t> input = noise(300, 0.002);
    input += tone(5000, 0.3);
    bool ended = false;
    for (int at = 0; at < input.size() && !ended; at += 480)
      ended = vad.feed(input.constData() + at,
                       std::min<int>(480, int(input.size()) - at)) ==
              audio::VoiceActivity::Ended;
    QVERIFY(ended);
  }

  void wavRoundTrip() {
    const QByteArray pcm(3200, 'x');
    const QByteArray file = audio::wav(pcm, 22050, 2);
    const audio::WavInfo info = audio::parseWav(file);
    QVERIFY(info.ok);
    QCOMPARE(info.sampleRate, 22050);
    QCOMPARE(info.channels, 2);
    QCOMPARE(info.bitsPerSample, 16);
    QCOMPARE(file.mid(info.dataOffset), pcm);
  }

  void wavStreamingHeaders() {
    // A server that does not know the length yet writes 0xFFFFFFFF, and
    // some put a LIST chunk before the data.
    QByteArray file = audio::wav(QByteArray(), 44100);
    file.replace(4, 4, QByteArray("\xff\xff\xff\xff", 4));
    file.replace(40, 4, QByteArray("\xff\xff\xff\xff", 4));
    QVERIFY(audio::parseWav(file).ok);

    QByteArray listed = audio::wav(QByteArray(10, 'a'), 16000);
    QByteArray list("LIST", 4);
    list += QByteArray("\x04\x00\x00\x00", 4) + "INFO";
    listed.insert(36, list);
    const audio::WavInfo info = audio::parseWav(listed);
    QVERIFY(info.ok);
    QCOMPARE(listed.mid(info.dataOffset), QByteArray(10, 'a'));

    QVERIFY(!audio::parseWav("RIFF....WAVE").ok);
    QVERIFY(!audio::parseWav("not audio at all").ok);
  }

  void resamplesToMono16k() {
    QVector<int16_t> stereo(48000 * 2); // one second, 48 kHz stereo
    for (int i = 0; i < 48000; ++i) {
      stereo[2 * i] = 1000;
      stereo[2 * i + 1] = 3000;
    }
    const QVector<int16_t> mono = audio::toMono16k(stereo.constData(), 48000, 2, 48000);
    QVERIFY(std::abs(int(mono.size()) - 16000) <= 1);
    QCOMPARE(int(mono[100]), 2000);
  }

  void splitsSentences() {
    const QStringList parts = splitSentences(
        "Okay. I found three repositories from last week! The first one is "
        "Cardfinity, a Godot card game. Want me to open it?");
    QVERIFY(parts.size() >= 2);
    QVERIFY(parts.first().startsWith("Okay. I found"));
    QCOMPARE(parts.join(' ').simplified(),
             QString("Okay. I found three repositories from last week! The "
                     "first one is Cardfinity, a Godot card game. Want me to "
                     "open it?"));
    for (const QString &part : splitSentences(QString(900, 'a'), 220))
      QVERIFY(part.size() <= 221);
  }

  // --- settings --------------------------------------------------------------

  void settingsValidate() {
    QTemporaryDir dir;
    AssistantSettings settings(dir.filePath("a.json"), false);
    QCOMPARE(settings.flag("memory.enabled"), false); // opt-in
    QCOMPARE(settings.flag("agent.shell"), false);
    QVERIFY(!settings.set("no.such.key", true));
    QVERIFY(!settings.set("memory.enabled", "yes"));        // wrong type
    QVERIFY(!settings.set("memory.intervalSec", 1));        // below range
    QVERIFY(!settings.set("memory.intervalSec", 12.5));     // not whole
    QVERIFY(!settings.set("stt.activation", "sometimes"));  // not a choice
    QVERIFY(!settings.set("llm.endpoint", "file:///etc/passwd"));
    QVERIFY(!settings.set("llm.endpoint", "javascript:alert(1)"));
    QVERIFY(settings.set("llm.endpoint", "http://127.0.0.1:8081/v1/"));
    QCOMPARE(settings.string("llm.endpoint"), QString("http://127.0.0.1:8081/v1"));
    QVERIFY(settings.set("memory.intervalSec", 10));
    QVERIFY(settings.set("privacy.excludedApps",
                         QStringList{" keepassxc ", "", "keepassxc", "signal"}));
    QCOMPARE(settings.list("privacy.excludedApps"), (QStringList{"keepassxc", "signal"}));
  }

  void settingsPersistAndSurviveDamage() {
    QTemporaryDir dir;
    const QString path = dir.filePath("a.json");
    {
      AssistantSettings settings(path);
      QVERIFY(settings.set("memory.paused", true));
      QVERIFY(settings.set("llm.model", "qwen"));
      settings.flush();
    }
    QVERIFY((QFile::permissions(path) & (QFile::ReadGroup | QFile::ReadOther)) == 0);
    // Corrupt one value by hand: that value is lost, not the file.
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    json.insert("memory.intervalSec", "soon");
    json.insert("unknown.key", 1);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(json).toJson());
    file.close();

    AssistantSettings again(path);
    QCOMPARE(again.flag("memory.paused"), true);
    QCOMPARE(again.string("llm.model"), QString("qwen"));
    QCOMPARE(again.integer("memory.intervalSec"), 30);
  }

  // --- log redaction -----------------------------------------------------------

  void redactsSecrets() {
    // Assembled here so no credential-shaped string sits in the source for
    // secret scanners to trip over; these are not real tokens.
    const QString openai = QStringLiteral("sk") + "-abcdefghijklmnopqrstuvwx";
    const QString github = QStringLiteral("gh") + "p_0123456789abcdefghijABCDEFGHIJ";
    const QString text = EventLog::redact(
        "key " + openai + " and " + github +
        " with Authorization: Bearer abc.def.ghi123 and my password is hunter2");
    QVERIFY(!text.contains("sk-abcdef"));
    QVERIFY(!text.contains("ghp_0123"));
    QVERIFY(!text.contains("abc.def.ghi123"));
    QVERIFY(!text.contains("hunter2"));
    QVERIFY(text.contains("password is [redacted]"));
    QCOMPARE(EventLog::redact("open discord"), QString("open discord"));

    const QJsonObject json = EventLog::redact(QJsonObject{
        {"apiKey", "anything"},
        {"access_token", "x"},
        {"maxTokens", 800},
        {"args", QJsonObject{{"password", "p"}, {"path", "/home/a"}}}});
    QCOMPARE(json.value("apiKey").toString(), QString("[redacted]"));
    QCOMPARE(json.value("access_token").toString(), QString("[redacted]"));
    QCOMPARE(json.value("maxTokens").toInt(), 800);
    QCOMPARE(json.value("args").toObject().value("password").toString(), QString("[redacted]"));
    QCOMPARE(json.value("args").toObject().value("path").toString(), QString("/home/a"));
  }

  // --- model replies -------------------------------------------------------------

  void parsesToolCalls() {
    // Built rather than written as a raw string: moc cannot read raw string
    // literals, and silently skips the whole file if it meets one.
    const auto call = [](const QString &id, const QString &name,
                         const QJsonValue &arguments) {
      QJsonObject out{{"type", "function"},
                      {"function", QJsonObject{{"name", name},
                                               {"arguments", arguments}}}};
      if (!id.isEmpty())
        out.insert("id", id);
      return out;
    };
    const QJsonObject response{
        {"choices",
         QJsonArray{QJsonObject{
             {"finish_reason", "tool_calls"},
             {"message",
              QJsonObject{
                  {"role", "assistant"},
                  {"content", "<think>they want discord</think>"},
                  {"tool_calls",
                   QJsonArray{call("a", "apps_launch", "{\"name\": \"discord\"}"),
                              call({}, "window_list", QJsonObject{}),
                              call("c", "files_read", "{broken")}}}}}}}};
    QString error;
    const LlmReply reply = LlmClient::parse(response, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(reply.content, QString());
    QCOMPARE(reply.toolCalls.size(), 3);
    QCOMPARE(reply.toolCalls[0].arguments.value("name").toString(), QString("discord"));
    QVERIFY(reply.toolCalls[1].argumentsValid);
    QVERIFY(!reply.toolCalls[1].id.isEmpty()); // filled in
    QVERIFY(!reply.toolCalls[2].argumentsValid);
    QCOMPARE(reply.message.value("tool_calls").toArray().size(), 3);

    LlmClient::parse(QJsonObject{}, &error);
    QVERIFY(!error.isEmpty());
    QCOMPARE(LlmClient::stripReasoning("<think>\nhmm\n</think>\n\nHello!"), QString("Hello!"));
    QCOMPARE(LlmClient::stripReasoning("reasoning only</think>Hi"), QString("Hi"));
  }

  // --- tools and permissions -------------------------------------------------------

  void validatesArguments() {
    using namespace schema;
    const QJsonObject spec =
        object({{"path", string("p", 10)},
                {"count", integer("n", 1, 3)},
                {"mode", oneOf("m", {"a", "b"})},
                {"force", boolean("f")}},
               {"path"});
    QVERIFY(ToolRegistry::validate(spec, {{"path", "x"}}).isEmpty());
    QVERIFY(!ToolRegistry::validate(spec, {}).isEmpty());
    QVERIFY(!ToolRegistry::validate(spec, {{"path", 3}}).isEmpty());
    QVERIFY(!ToolRegistry::validate(spec, {{"path", "01234567890"}}).isEmpty());
    QVERIFY(!ToolRegistry::validate(spec, {{"path", "x"}, {"count", 4}}).isEmpty());
    QVERIFY(!ToolRegistry::validate(spec, {{"path", "x"}, {"count", 1.5}}).isEmpty());
    QVERIFY(!ToolRegistry::validate(spec, {{"path", "x"}, {"mode", "c"}}).isEmpty());
    QVERIFY(!ToolRegistry::validate(spec, {{"path", "x"}, {"force", "yes"}}).isEmpty());
    QVERIFY(!ToolRegistry::validate(spec, {{"path", "x"}, {"sudo", true}}).isEmpty());
  }

  void permissionMatrix() {
    for (const QString &mode : {"everything", "risky", "high"})
      QCOMPARE(decide(Risk::High, mode), Decision::Confirm); // never skippable
    QCOMPARE(decide(Risk::Safe, "everything"), Decision::Allow);
    QCOMPARE(decide(Risk::Low, "everything"), Decision::Confirm);
    QCOMPARE(decide(Risk::Low, "risky"), Decision::Allow);
    QCOMPARE(decide(Risk::Medium, "risky"), Decision::Confirm);
    QCOMPARE(decide(Risk::Medium, "high"), Decision::Allow);
  }

  void pathPolicy() {
    QTemporaryDir home, outside;
    QDir(home.path()).mkpath("work");
    QDir(home.path()).mkpath(".ssh");
    QVERIFY(touch(home.filePath("work/notes.txt")));
    QVERIFY(touch(home.filePath(".ssh/id_ed25519")));
    QVERIFY(touch(home.filePath("work/.env")));
    QVERIFY(touch(outside.filePath("secret.txt")));
    QFile::link(outside.path(), home.filePath("work/escape"));

    const PathPolicy policy({}, home.path());
    QVERIFY(policy.check("work/notes.txt", false).allowed);
    QVERIFY(policy.check("~/work/notes.txt", false).allowed);
    QVERIFY(policy.check(home.filePath("work/new.txt"), true).allowed);
    QVERIFY(!policy.check(home.filePath("work/new.txt"), false).allowed);
    QVERIFY(!policy.check(home.filePath("nowhere/new.txt"), true).allowed);
    QVERIFY(!policy.check(outside.filePath("secret.txt"), false).allowed);
    QVERIFY(!policy.check("work/../../" + QFileInfo(outside.path()).fileName() + "/secret.txt", false).allowed);
    QVERIFY(!policy.check("work/escape/secret.txt", false).allowed); // symlink out
    QVERIFY(!policy.check(".ssh/id_ed25519", false).allowed);
    QVERIFY(!policy.check("work/.env", false).allowed);
    QVERIFY(!policy.check("/etc/shadow", false).allowed);
    QVERIFY(!policy.check(QString("work/notes.txt") + QChar(0) + "x", false).allowed);

    // Narrowed to one folder.
    const PathPolicy narrow({home.filePath("work")}, home.path());
    QVERIFY(narrow.check("work/notes.txt", false).allowed);
    QVERIFY(!narrow.check(home.path(), false).allowed);
  }

  void privacyGateDropsSensitiveWindows() {
    const QStringList apps = AssistantSettings::defaults()
                                 .value("privacy.excludedApps")
                                 .toStringList();
    const auto allowed = [&](const WindowInfo &w, QStringList titles = {}) {
      return privacyGate(w, apps, titles, true, true).allowed;
    };
    QVERIFY(allowed(window("kitty", "nvim ~/Projects/nala/src/main.cpp")));
    QVERIFY(allowed(window("obsidian", "Ideas - My Vault - Obsidian")));
    QVERIFY(!allowed(window("org.keepassxc.KeePassXC", "Passwords.kdbx")));
    QVERIFY(!allowed(window("Bitwarden", "Bitwarden")));
    QVERIFY(!allowed(window("firefox", "Sign in – Google Accounts — Mozilla Firefox")));
    QVERIFY(!allowed(window("firefox", "Mozilla Firefox Private Browsing")));
    QVERIFY(!allowed(window("chromium", "New Tab - Chromium (Incognito)")));
    QVERIFY(!allowed(window("firefox", "Chase Bank - Accounts")));
    QVERIFY(!allowed(window("firefox", "Checkout - Store")));
    QVERIFY(!allowed(window("firefox", "Enter your verification code")));
    QVERIFY(!allowed(window("firefox", "something NSFW - Reddit")));
    QVERIFY(!allowed(window("discord", "#general"), {"#general"}));
    QVERIFY(!privacyGate(WindowInfo{}, apps, {}, true, true).allowed);
    QVERIFY(privacyGate(WindowInfo{}, apps, {}, true, false).allowed);
    // Categories are reported, never the title itself.
    const PrivacyCheck check = privacyGate(window("firefox", "Chase Bank"), apps, {}, true, true);
    QVERIFY(!check.reason.contains("Chase"));
  }

  void parsesTimePhrases() {
    const QDateTime now(QDate(2026, 9, 23), QTime(15, 0)); // a Wednesday
    TimeRange r = parseTimeRange("what github repo did I look at yesterday", now);
    QVERIFY(r.valid);
    QCOMPARE(r.from, QDateTime(QDate(2026, 9, 22), QTime(0, 0)));
    QCOMPARE(r.to, QDateTime(QDate(2026, 9, 23), QTime(0, 0)));
    QVERIFY(!r.rest.contains("yesterday"));
    QVERIFY(r.rest.contains("github repo"));

    r = parseTimeRange("last week", now);
    QCOMPARE(r.from.date(), QDate(2026, 9, 14));
    QCOMPARE(r.to.date(), QDate(2026, 9, 21));

    r = parseTimeRange("that website I had open monday", now);
    QCOMPARE(r.from.date(), QDate(2026, 9, 21));
    r = parseTimeRange("last wednesday", now);
    QCOMPARE(r.from.date(), QDate(2026, 9, 16));
    r = parseTimeRange("3 days ago", now);
    QCOMPARE(r.from.date(), QDate(2026, 9, 20));
    r = parseTimeRange("in the last 2 hours", now);
    QCOMPARE(r.from, now.addSecs(-7200));
    QCOMPARE(r.to, now);
    r = parseTimeRange("what command fixed nvidia", now);
    QVERIFY(!r.valid);
    QCOMPARE(r.rest, QString("what command fixed nvidia"));
  }

  // --- memory ---------------------------------------------------------------------

  void hashesTellFramesApart() {
    const QImage a = scene(0);
    QCOMPARE(hammingDistance(differenceHash(a), differenceHash(a)), 0);
    // Rescaled and recompressed: still the same moment.
    QImage b = a.scaled(1280, 800);
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    b.save(&buffer, "JPEG", 40);
    b.loadFromData(jpeg, "JPEG");
    QVERIFY(hammingDistance(differenceHash(a), differenceHash(b)) <= 4);
    // A different layout: a different moment.
    qInfo() << "rescaled" << hammingDistance(differenceHash(a), differenceHash(b))
            << "different" << hammingDistance(differenceHash(a), differenceHash(scene(1)));
    QVERIFY(hammingDistance(differenceHash(a), differenceHash(scene(1))) >
            AssistantSettings::defaults().value("memory.dedupeDistance").toInt());
  }

  void findsArtifacts() {
    const QVector<Artifact> found = findArtifacts(
        "GitHub - r3dg0d/Nala-Voice: a desktop companion — see "
        "https://example.com/docs?a=1, and ~/Documents/resume.pdf");
    QStringList values;
    for (const Artifact &a : found)
      values << a.kind + ":" + a.value;
    QVERIFY2(values.contains("repo:https://github.com/r3dg0d/Nala-Voice"),
             qPrintable(values.join(" | ")));
    QVERIFY(values.contains("url:https://example.com/docs?a=1"));
    QVERIFY(values.contains("file:~/Documents/resume.pdf"));
  }

  void storeSearchesAndForgets() {
    QTemporaryDir dir;
    MemoryStore store;
    QVERIFY(store.open(dir.path()));
    QVERIFY((QFile::permissions(dir.filePath("memory.db")) &
             (QFile::ReadGroup | QFile::ReadOther)) == 0);
    const QDateTime now = QDateTime::currentDateTime();

    MemoryRecord a;
    a.started = a.lastSeen = now.addDays(-1);
    a.app = "firefox";
    a.title = "r3dg0d/Nala-Voice: desktop companion - GitHub";
    const qint64 ida = store.insert(a);
    MemoryRecord b;
    b.started = b.lastSeen = now.addSecs(-120);
    b.app = "kitty";
    b.title = "sudo nixos-rebuild switch";
    b.summary = "Fixed the NVIDIA driver by pinning the kernel";
    const qint64 idb = store.insert(b);
    QVERIFY(ida && idb);

    QVector<MemoryRecord> hits = store.search("what github project did I look at yesterday", now);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().id, ida);
    hits = store.search("nvidia", now);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().id, idb);
    // Hostile search text is only ever text.
    store.search("\" OR 1=1 -- ) NEAR(", now);
    store.search("'; DROP TABLE memories; --", now);
    QCOMPARE(store.count(), 2);

    // "Forget the last five minutes" takes b and nothing else, pinned or not.
    store.setPinned(idb, true);
    QCOMPARE(store.forget(now.addSecs(-300), now.addSecs(60)), 1);
    QCOMPARE(store.count(), 1);
    QCOMPARE(store.get(idb).id, qint64(0));
  }

  void retentionSparesPinned() {
    QTemporaryDir dir;
    MemoryStore store;
    QVERIFY(store.open(dir.path()));
    const QDateTime now = QDateTime::currentDateTime();
    const auto shot = [&](int daysAgo, bool pinned) {
      const QString path = store.shotsDir() + QStringLiteral("/%1-%2.jpg")
                                                  .arg(daysAgo)
                                                  .arg(pinned);
      QFile file(path);
      if (!file.open(QIODevice::WriteOnly))
        return qint64(0);
      file.write(QByteArray(100 * 1024, 'j'));
      file.close();
      MemoryRecord r;
      r.started = r.lastSeen = now.addDays(-daysAgo);
      r.app = "app";
      r.title = QString::number(daysAgo);
      r.shotPath = path;
      r.shotBytes = 100 * 1024;
      r.pinned = pinned;
      return store.insert(r);
    };
    const qint64 old = shot(10, false);
    const qint64 oldPinned = shot(10, true);
    const qint64 recent1 = shot(2, false);
    const qint64 recent2 = shot(1, false);

    // Pictures older than a week go; the rows and the pinned one stay.
    MemoryStore::Sweep sweep = store.enforce(7, 0, 0, now);
    QCOMPARE(sweep.screenshots, 1);
    QVERIFY(store.get(old).shotPath.isEmpty());
    QVERIFY(store.get(old).id != 0);
    QVERIFY(!QFile::exists(store.shotsDir() + "/10-0.jpg"));
    QVERIFY(!store.get(oldPinned).shotPath.isEmpty());

    // Over a cap: the oldest unpinned picture goes first.
    const qint64 dbBytes = store.storageBytes() - 3 * 100 * 1024;
    sweep = store.enforce(0, 0, dbBytes + 2 * 100 * 1024 + 10, now);
    QCOMPARE(sweep.screenshots, 1);
    QVERIFY(store.get(recent1).shotPath.isEmpty());
    QVERIFY(!store.get(recent2).shotPath.isEmpty());
    QVERIFY(!store.get(oldPinned).shotPath.isEmpty());

    // Rows past their own retention go too, except pinned ones.
    sweep = store.enforce(0, 5, 0, now);
    QCOMPARE(sweep.rows, 1);
    QCOMPARE(store.get(old).id, qint64(0));
    QVERIFY(store.get(oldPinned).id != 0);
  }

  void storeNeverDeletesOutsideItsFolder() {
    QTemporaryDir dir, elsewhere;
    const QString victim = elsewhere.filePath("important.txt");
    QFile file(victim);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("keep me");
    file.close();
    MemoryStore store;
    QVERIFY(store.open(dir.path()));
    MemoryRecord r;
    r.started = r.lastSeen = QDateTime::currentDateTime().addDays(-30);
    r.shotPath = victim; // a row that has been tampered with
    r.shotBytes = 7;
    const qint64 id = store.insert(r);
    store.enforce(1, 0, 0, QDateTime::currentDateTime());
    store.forgetOne(id);
    QVERIFY(QFile::exists(victim));
  }

  // --- screen memory pipeline ---------------------------------------------------------

  void screenMemoryKillswitch() {
    QTemporaryDir dir;
    AssistantSettings settings(dir.filePath("assistant.json"));
    EventLog log({});
    MemoryStore store;
    QVERIFY(store.open(dir.filePath("memory")));
    QNetworkAccessManager network;
    LlmClient llm(&network);
    ScreenMemory memory(&settings, &store, &log, &llm);
    memory.setOffline(true);
    const WindowInfo editor = window("kitty", "nvim notes.md", "0xa");

    // Off by default: nothing is kept.
    memory.ingest(scene(0), editor);
    QTest::qWait(50);
    QCOMPARE(store.count(), 0);

    memory.setEnabled(true);
    memory.ingest(scene(0), editor);
    QTRY_COMPARE(store.count(), 1);

    // The same thing again folds into that memory rather than adding one.
    memory.ingest(scene(0), editor);
    QTRY_COMPARE(store.latest().frames, 2);
    QCOMPARE(store.count(), 1);

    // An excluded window is never kept.
    memory.ingest(scene(1), window("org.keepassxc.KeePassXC", "vault", "0xb"));
    QTest::qWait(100);
    QCOMPARE(store.count(), 1);

    // Paused: nothing is kept, and the pause is on disk at once.
    memory.pause();
    QVERIFY(memory.paused());
    QVERIFY(!memory.recording());
    {
      AssistantSettings fromDisk(dir.filePath("assistant.json"));
      QVERIFY(fromDisk.flag("memory.paused"));
    }
    memory.ingest(scene(1), window("firefox", "Docs", "0xc"));
    QTest::qWait(100);
    QCOMPARE(store.count(), 1);

    // A pause that lands while a frame is being processed still wins.
    memory.resume();
    memory.ingest(scene(1), window("firefox", "Docs", "0xc"));
    memory.pause();
    QTest::qWait(300);
    QCOMPARE(store.count(), 1);

    // A timed pause records when it ends.
    memory.resume();
    memory.pause(60);
    QVERIFY(memory.pausedUntil().isValid());
    QVERIFY(memory.pausedUntil() > QDateTime::currentDateTime().addSecs(3500));

    // Kept files are private.
    const QString shot = store.latest().shotPath;
    QVERIFY(QFile::exists(shot));
    QVERIFY((QFile::permissions(shot) & (QFile::ReadGroup | QFile::ReadOther)) == 0);
  }

  // --- applications ---------------------------------------------------------------------

  void parsesDesktopEntries() {
    bool ok = false;
    const desktop::App app = desktop::parseDesktopEntry(
        "[Desktop Entry]\nType=Application\nName=Discord\nName[de]=Diskord\n"
        "Exec=\"/opt/Discord/Discord\" --flag %U\nCategories=Network;Chat;\n"
        "[Desktop Action new]\nName=Other\nExec=rm -rf /\n",
        &ok);
    QVERIFY(ok);
    QCOMPARE(app.name, QString("Discord"));
    QCOMPARE(desktop::execArguments(app.exec),
             (QStringList{"/opt/Discord/Discord", "--flag"}));
    desktop::parseDesktopEntry("[Desktop Entry]\nType=Application\nName=X\n"
                               "Exec=x\nNoDisplay=true\n",
                               &ok);
    QVERIFY(!ok);
    QCOMPARE(desktop::execArguments("sh -c \"echo 100%%\" %f"),
             (QStringList{"sh", "-c", "echo 100%"}));
  }

  void findsApplications() {
    desktop::AppIndex index;
    const auto add = [&](const QString &id, const QString &name,
                         const QString &exec, QStringList categories) {
      desktop::App app;
      app.id = id;
      app.name = name;
      app.exec = exec;
      app.categories = categories;
      index.add(app);
    };
    add("discord.desktop", "Discord", "Discord", {"Network"});
    add("kitty.desktop", "kitty", "kitty", {"System", "TerminalEmulator"});
    add("org.mozilla.firefox.desktop", "Firefox", "firefox %u", {"Network", "WebBrowser"});
    add("nvidia-settings.desktop", "NVIDIA X Server Settings", "nvidia-settings", {"Settings"});
    QCOMPARE(index.find("discord")->name, QString("Discord"));
    QCOMPARE(index.find("the terminal")->name, QString("kitty"));
    QCOMPARE(index.find("browser")->name, QString("Firefox"));
    QCOMPARE(index.find("firefox")->name, QString("Firefox"));
    QVERIFY(index.find("photoshop") == nullptr);
    QVERIFY(desktop::validAddress("0x55d0c1a2b3c4"));
    QVERIFY(!desktop::validAddress("0x1;dispatch exec rm"));
    QVERIFY(!desktop::validAddress("address:0x1"));
  }

  // --- the assistant, end to end ------------------------------------------------------------

  void assistantKillswitchNeedsNoModel() {
    QTemporaryDir dir;
    Assistant assistant({dir.filePath("assistant.json"), dir.filePath("memory"), {}},
                        true);
    // Point the model somewhere nothing listens: the switch must not care.
    assistant.settings()->set("llm.endpoint", "http://127.0.0.1:9");
    assistant.memory()->setEnabled(true);
    QVERIFY(assistant.memory()->recording());

    assistant.hearForTest("Nala, turn off screen recording.");
    QVERIFY(assistant.memory()->paused());
    QVERIFY(!assistant.memory()->recording());
    // Everything else still works while it is paused.
    QSignalSpy said(&assistant, &Assistant::said);
    assistant.ask("mute yourself");
    QVERIFY(assistant.settings()->flag("tts.muted"));
    QVERIFY(!said.isEmpty());

    assistant.ask("turn screen recording back on");
    QVERIFY(!assistant.memory()->paused());
    assistant.ask("pause memory for one hour");
    QVERIFY(assistant.memory()->pausedUntil().isValid());
  }

  void assistantForgets() {
    QTemporaryDir dir;
    Assistant assistant({dir.filePath("a.json"), dir.filePath("memory"), {}}, true);
    MemoryRecord r;
    r.started = r.lastSeen = QDateTime::currentDateTime().addSecs(-60);
    r.title = "recent";
    assistant.store()->insert(r);
    r.started = r.lastSeen = QDateTime::currentDateTime().addDays(-2);
    r.title = "older";
    assistant.store()->insert(r);
    assistant.ask("forget the last five minutes");
    QCOMPARE(assistant.store()->count(), 1);
    QCOMPARE(assistant.store()->latest().title, QString("older"));

    // Forgetting everything asks first; "no" keeps it.
    assistant.ask("forget everything");
    QVERIFY(!assistant.question().isEmpty());
    assistant.ask("no");
    QVERIFY(assistant.question().isEmpty());
    QCOMPARE(assistant.store()->count(), 1);
    assistant.ask("forget everything");
    assistant.ask("yes");
    QCOMPARE(assistant.store()->count(), 0);
  }

  void assistantAsksBeforeRiskyTools() {
    QTemporaryDir dir;
    Assistant assistant({dir.filePath("a.json"), dir.filePath("memory"), {}}, true);
    QJsonObject result;
    bool finished = false;
    const auto done = [&](QJsonObject r) {
      result = r;
      finished = true;
    };

    // Writing a file always needs a yes, even with confirmation at its
    // loosest.
    assistant.settings()->set("agent.confirm", "high");
    const QString target = dir.filePath("hello.txt");
    assistant.callTool("files.write", {{"path", target}, {"content", "hi"}}, done);
    QVERIFY(!finished);
    QVERIFY(assistant.question().contains("hello.txt"));
    assistant.answer(false);
    QVERIFY(finished);
    QCOMPARE(result.value("ok").toBool(), false);
    QVERIFY(!QFile::exists(target));

    // "Stop" answers no as well.
    finished = false;
    assistant.settings()->set("agent.confirm", "risky");
    assistant.callTool("window.close_all", {}, done);
    QVERIFY(!assistant.question().isEmpty());
    assistant.stop();
    QVERIFY(finished);
    QVERIFY(assistant.question().isEmpty());

    // Switched-off categories refuse outright.
    finished = false;
    assistant.callTool("shell.run", {{"command", "true"}}, done);
    QVERIFY(finished);
    QVERIFY(result.value("error").toString().contains("switched off"));

    // Bad arguments never reach the tool.
    finished = false;
    assistant.callTool("files.read", {{"path", 42}}, done);
    QVERIFY(finished);
    QVERIFY(result.value("error").toString().startsWith("Invalid"));

    // Opening a URL that could carry data out asks first; a plain one does
    // not need to. (Declined here, so nothing is actually opened.)
    finished = false;
    assistant.callTool("browser.open",
                       {{"url", "https://example.com/?q=" + QString(80, 'x')}}, done);
    QVERIFY(!finished);
    QVERIFY(assistant.question().startsWith("Open https://example.com/"));
    assistant.answer(false);
    QVERIFY(finished);

    // A safe tool just runs.
    finished = false;
    assistant.callTool("memory.search", {{"query", "anything yesterday"}}, done);
    QVERIFY(finished);
    QVERIFY(result.value("ok").toBool());
  }

  void assistantFinalAnswerJoinsHistory() {
    QTemporaryDir dir;
    Assistant assistant({dir.filePath("a.json"), dir.filePath("memory"), {}}, true);
    QSignalSpy said(&assistant, &Assistant::said);
    LlmReply reply;
    reply.content = "You were working on Cardfinity.";
    reply.message = QJsonObject{{"role", "assistant"}, {"content", reply.content}};
    assistant.injectModelReply(reply);
    QCOMPARE(said.count(), 1);
    QCOMPARE(said.first().first().toString(), reply.content);
    QCOMPARE(assistant.history().size(), 2);
    QCOMPARE(assistant.state(), QString("idle"));
  }


  // --- identity -----------------------------------------------------------------

  void identityPropagates() {
    QTemporaryDir dir;
    AssistantSettings settings(dir.filePath("a.json"), false);
    QVERIFY(settings.set("identity.name", "Nova"));
    QVERIFY(settings.set("wake.phrases", QStringList{"Hey Nova", "computer"}));
    Identity id = Identity::from(settings);
    QCOMPARE(id.wakePhrases(), (QStringList{"hey nova", "computer"}));
    QVERIFY(id.addressForms().contains("nova"));
    QVERIFY(id.addressForms().contains("hey nova"));
    QVERIFY(!id.addressForms().contains("hey nala"));
    const QString prompt = id.systemPrompt(QDateTime::currentDateTime(), "off", false);
    QVERIFY(prompt.startsWith("You are Nova,"));
    QVERIFY(!prompt.contains("Nala"));
    QVERIFY(id.recognitionPrompt().startsWith("Hey Nova."));
    QVERIFY(id.recognitionPrompt().contains("Computer."));
    QVERIFY(id.recognitionPrompt().contains("Pause screen memory."));
    // A prompt of the user's own, with the name filled in.
    QVERIFY(id.systemPrompt(QDateTime::currentDateTime(), "off", false,
                            "You are {name}, a pirate.")
                .startsWith("You are Nova, a pirate."));
    settings.set("wake.acceptName", true);
    QVERIFY(Identity::from(settings).wakePhrases().contains("nova"));
    settings.set("identity.personality", "minimal");
    QVERIFY(Identity::from(settings)
                .systemPrompt(QDateTime::currentDateTime(), "off", false)
                .contains("terse"));
  }

  void routerKnowsARenamedAssistant() {
    CommandRouter router("Nova");
    Identity id;
    id.name = "Nova";
    id.phrases = {"hey nova"};
    QCOMPARE(router.route("Open Nova's settings.", id.addressForms()).action,
             QString("settings.open"));
    QCOMPARE(router.route("open nova settings", id.addressForms()).action,
             QString("settings.open"));
    const Route r = router.route("Hey Nova, turn off screen recording.", id.addressForms());
    QCOMPARE(r.action, QString("memory.pause"));
    QVERIFY(r.addressed);
    // Misheard names still reach the privacy switch.
    QCOMPARE(router.route("Hey Noah, pause screen memory", id.addressForms()).action,
             QString("memory.pause"));
  }

  void profilesCarryNoSecrets() {
    QTemporaryDir dir;
    AssistantSettings settings(dir.filePath("a.json"), false);
    settings.set("identity.name", "Luna");
    settings.set("wake.phrases", QStringList{"hey luna"});
    settings.set("llm.apiKey", "sk-secret-value-123456789");
    settings.set("llm.endpoint", "https://example.com/v1");
    const QJsonObject exported = profile::exportProfile(settings);
    const QByteArray text = QJsonDocument(exported).toJson();
    QVERIFY(!text.contains("sk-secret"));
    QVERIFY(!text.contains("example.com"));
    QVERIFY(!text.contains("memory."));
    QVERIFY(text.contains("Luna"));

    AssistantSettings fresh(dir.filePath("b.json"), false);
    QString error;
    const QStringList applied = profile::importProfile(fresh, exported, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(applied.contains("identity.name"));
    QCOMPARE(fresh.string("identity.name"), QString("Luna"));
    QCOMPARE(fresh.list("wake.phrases"), QStringList{"hey luna"});
    // Keys a profile must never set are ignored even if present.
    QJsonObject hostile = exported;
    QJsonObject values = hostile.value("settings").toObject();
    values.insert("agent.shell", true);
    values.insert("llm.endpoint", "https://evil.example/v1");
    hostile.insert("settings", values);
    profile::importProfile(fresh, hostile, &error);
    QCOMPARE(fresh.flag("agent.shell"), false);
    QVERIFY(!fresh.string("llm.endpoint").contains("evil"));
    QVERIFY(profile::importProfile(fresh, QJsonObject{{"x", 1}}, &error).isEmpty());
    QVERIFY(!error.isEmpty());
  }

  // --- the wake-word engine, without models ---------------------------------------

  void gateConfirmsAndCoolsDown() {
    wake::Gate gate(wake::Gate::Config{0.6f, 2, 1000});
    QVERIFY(!gate.update(0.9f, 0));   // one window is not enough
    QVERIFY(gate.update(0.9f, 80));   // two in a row is
    QVERIFY(!gate.update(0.9f, 160)); // cooling down
    QVERIFY(!gate.update(0.9f, 240));
    gate.update(0.1f, 1100);
    QVERIFY(!gate.update(0.9f, 1180));
    QVERIFY(gate.update(0.9f, 1260)); // cooled down
    // Below threshold never counts; a dip resets the run.
    gate.update(0.1f, 5000);
    QVERIFY(!gate.update(0.9f, 5080));
    QVERIFY(!gate.update(0.5f, 5160));
    QVERIFY(!gate.update(0.9f, 5240));
    // Suspended while she talks, and a grace period after.
    gate.suspend(9000);
    QVERIFY(!gate.update(1.0f, 9080));
    QVERIFY(!gate.update(1.0f, 9160));
    gate.resume(9200, 800);
    QVERIFY(!gate.update(1.0f, 9300));
    QVERIFY(!gate.update(1.0f, 9400));
    gate.update(1.0f, 10100);
    QVERIFY(gate.update(1.0f, 10180));
  }

  void logisticRegressionLearns() {
    QRandomGenerator random(3);
    QVector<QVector<float>> pos, neg;
    for (int i = 0; i < 200; ++i) {
      QVector<float> p(20), n(20);
      for (int d = 0; d < 20; ++d) {
        p[d] = float(random.generateDouble() + (d < 5 ? 1.5 : 0.0));
        n[d] = float(random.generateDouble());
      }
      pos << p;
      neg << n;
    }
    const wake::Fit fit = wake::fitLogistic(pos, neg, 30, 1e-3, 1);
    int right = 0;
    for (const auto &p : pos)
      right += wake::predict(fit, p.constData(), 20) > 0.5f;
    for (const auto &n : neg)
      right += wake::predict(fit, n.constData(), 20) < 0.5f;
    QVERIFY2(right > 380, qPrintable(QString::number(right)));
  }

  void wakeModelRoundTrips() {
    wake::Model m;
    m.phrase = "hey nala";
    m.windows = 2;
    m.mean = QVector<float>(2 * wake::kEmbedding, 0.0f);
    m.scale = QVector<float>(2 * wake::kEmbedding, 1.0f);
    m.weights = QVector<float>(2 * wake::kEmbedding, 0.01f);
    m.threshold = 0.7f;
    m.center = QVector<float>(wake::kEmbedding, 0.0f);
    QVector<float> t(3 * wake::kEmbedding, 0.0f);
    for (int f = 0; f < 3; ++f)
      t[f * wake::kEmbedding + f] = 1.0f;
    m.templates << t;
    m.matchThreshold = 0.8f;
    m.query = 6;
    const wake::Model back = wake::Model::fromJson(m.toJson());
    QVERIFY(back.valid());
    QVERIFY(back.hasTemplates());
    QCOMPARE(back.templates.first().size(), t.size());
    QCOMPARE(back.threshold, 0.7f);
    // Both tests have to pass: a strong classifier score is held back by a
    // weak template match, and the other way round.
    QVERIFY(back.combine(0.99f, 0.5f) < back.threshold);
    QVERIFY(back.combine(0.5f, 0.99f) < back.threshold);
    QVERIFY(back.combine(0.9f, 0.9f) >= back.threshold);
    // A query containing the template matches it closely; noise does not.
    QVector<float> query(6 * wake::kEmbedding, 0.0f);
    for (int f = 0; f < 3; ++f)
      query[(f + 2) * wake::kEmbedding + f] = 1.0f;
    query[0] = query[wake::kEmbedding + 50] = 1.0f;
    QVERIFY(back.match(query.constData(), 6) > 0.9f);
    QVector<float> other(6 * wake::kEmbedding, 0.0f);
    for (int f = 0; f < 6; ++f)
      other[f * wake::kEmbedding + 60 + f] = 1.0f;
    QVERIFY(back.match(other.constData(), 6) < 0.5f);
  }

  void augmentationKeepsLength() {
    const wake::Clip clip = tone(1000, 0.3);
    QCOMPARE(wake::augment::gain(clip, 6).size(), clip.size());
    QCOMPARE(wake::augment::reverb(clip, 0.5, 1).size(), clip.size());
    QCOMPARE(wake::augment::mix(clip, noise(300, 0.1), 10, 1).size(), clip.size());
    QVERIFY(std::abs(int(wake::augment::speed(clip, 1.1).size()) - int(clip.size() / 1.1)) <= 1);
    wake::Clip padded = noise(500, 0.0005);
    padded += clip;
    padded += noise(500, 0.0005, 2);
    const wake::Clip trimmed = wake::augment::trim(padded);
    QVERIFY(trimmed.size() < padded.size() - wake::kRate * 0.8);
  }

  // --- the wake word, in the assistant ---------------------------------------------

  void wakeWordGatesTheRecogniser() {
    QTemporaryDir dir;
    Assistant assistant({dir.filePath("a.json"), dir.filePath("memory"), {}}, true);
    assistant.settings()->set("llm.endpoint", "http://127.0.0.1:9");
    assistant.settings()->set("stt.activation", "wake");
    MockWake detector;
    ScriptedStt stt;
    assistant.setSpeechBackend(&stt);
    assistant.setWakeBackend(&detector, true);
    QVERIFY(detector.running);
    QCOMPARE(assistant.micState(), QString("off")); // tests never open the mic

    // Audio reaches the detector, and nobody addressed her: dropped, never
    // transcribed.
    assistant.hearAudio(tone(200, 0.1));
    QVERIFY(detector.heard > 0);
    assistant.hearUtterance(speech(800));
    QCOMPARE(stt.calls, 0);

    // Her name: she reacts at once and listens for the request.
    QSignalSpy woke(&assistant, &Assistant::wakeDetected);
    detector.fire();
    QCOMPARE(woke.count(), 1);
    QVERIFY(assistant.armed());
    QCOMPARE(assistant.state(), QString("listening"));

    // The request goes through the fast router: no model involved.
    QSignalSpy settingsOpened(&assistant, &Assistant::settingsRequested);
    stt.next = "Hey Nala, open Nala settings.";
    assistant.hearUtterance(speech(1200));
    QCOMPARE(stt.calls, 1);
    QCOMPARE(settingsOpened.count(), 1);
    QVERIFY(!assistant.armed());

    // The privacy switch after a wake.
    assistant.memory()->setEnabled(true);
    detector.fire();
    stt.next = "Hey Nala, pause screen recording.";
    assistant.hearUtterance(speech(1200));
    QVERIFY(assistant.memory()->paused());
    detector.fire();
    stt.next = "Hey Nala, resume screen recording.";
    assistant.hearUtterance(speech(1200));
    QVERIFY(!assistant.memory()->paused());
  }

  void sheDoesNotWakeHerself() {
    QTemporaryDir dir;
    Assistant assistant({dir.filePath("a.json"), dir.filePath("memory"), {}}, true);
    assistant.settings()->set("stt.activation", "wake");
    assistant.settings()->set("wake.postSpeechMs", 900);
    MockWake detector;
    ScriptedStt stt;
    assistant.setSpeechBackend(&stt);
    assistant.setWakeBackend(&detector, true);

    assistant.setSpeakingForTest(true);
    QVERIFY(detector.paused);                // not listening while she talks
    detector.fire();                         // and if something slipped through
    QVERIFY(!assistant.armed());             // it is ignored
    assistant.setSpeakingForTest(false);
    QVERIFY(!detector.paused);
    QCOMPARE(detector.lastGrace, 900);       // back after a grace period

    // Barge-in, when asked for: her name interrupts her.
    assistant.settings()->set("wake.bargeIn", true);
    assistant.setSpeakingForTest(true);
    QVERIFY(!detector.paused);
    detector.fire();
    QVERIFY(assistant.armed());
    QVERIFY(!assistant.speakingForTest());
  }

  void followUpsNeedNoWakePhrase() {
    QTemporaryDir dir;
    Assistant assistant({dir.filePath("a.json"), dir.filePath("memory"), {}}, true);
    assistant.settings()->set("stt.activation", "wake");
    assistant.settings()->set("wake.followUpSec", 10);
    MockWake detector;
    ScriptedStt stt;
    assistant.setSpeechBackend(&stt);
    assistant.setWakeBackend(&detector, true);

    // She answers; for a while the next thing said is for her.
    LlmReply reply;
    reply.content = "Your GPU is running the local model.";
    reply.message = QJsonObject{{"role", "assistant"}, {"content", reply.content}};
    assistant.injectModelReply(reply);
    QVERIFY(assistant.followingUp());
    stt.next = "open settings";
    QSignalSpy settingsOpened(&assistant, &Assistant::settingsRequested);
    assistant.hearUtterance(speech(900));
    QCOMPARE(stt.calls, 1);
    QCOMPARE(settingsOpened.count(), 1);

    // Off: back to needing her name.
    assistant.settings()->set("wake.followUpSec", 0);
    assistant.injectModelReply(reply);
    QVERIFY(!assistant.followingUp());
    assistant.hearUtterance(speech(900));
    QCOMPARE(stt.calls, 1);
  }

  void renamingRetiresTheOldName() {
    QTemporaryDir dir;
    Assistant assistant({dir.filePath("a.json"), dir.filePath("memory"), {}}, true);
    assistant.settings()->set("identity.name", "Nova");
    assistant.settings()->set("wake.phrases", QStringList{"hey nova"});
    assistant.settings()->set("stt.activation", "wake");
    QCOMPARE(assistant.assistantName(), QString("Nova"));
    // No trained detector: wake mode listens through the recogniser, and
    // only the enabled phrases count.
    QSignalSpy settingsOpened(&assistant, &Assistant::settingsRequested);
    assistant.hearForTest("Hey Nala, open settings.");
    QCOMPARE(settingsOpened.count(), 0);
    assistant.hearForTest("Hey Nova, open settings.");
    QCOMPARE(settingsOpened.count(), 1);
    // The model is told who she is.
    QSignalSpy said(&assistant, &Assistant::said);
    assistant.ask("hey nova how much storage are your memories using");
    QVERIFY(!said.isEmpty());
    // Pausing memory still works with the old name misheard or said.
    assistant.memory()->setEnabled(true);
    assistant.hearForTest("Hey Nala, turn off screen recording.");
    QVERIFY(assistant.memory()->paused());
  }

  // --- choosing a model ------------------------------------------------------------

  void picksTheRecommendedModel() {
    const QStringList preferred = AssistantSettings::defaults().value("llm.preferred").toStringList();
    QCOMPARE(preferred.first(), QString("qwen3.8-flash-next"));
    QCOMPARE(LlmClient::pickModel({"gemma4:latest", "hf.co/unsloth/Qwen3.8-Flash-Next-GGUF:UD-IQ1_S",
                                   "qwen3:8b"},
                                  preferred),
             QString("hf.co/unsloth/Qwen3.8-Flash-Next-GGUF:UD-IQ1_S"));
    QCOMPARE(LlmClient::pickModel({"gemma4-coder:latest", "qwen3.8-neo-coder:Q4_K_M"}, preferred),
             QString("qwen3.8-neo-coder:Q4_K_M"));
    QCOMPARE(LlmClient::pickModel({"llama3:8b"}, preferred), QString("llama3:8b"));
    QVERIFY(LlmClient::guessCapabilities("Qwen/Qwen3.8-Flash-Next").contains("vision"));
    QVERIFY(!LlmClient::guessCapabilities("qwen3.8-neo-coder").contains("vision"));
    QVERIFY(LlmClient::explain("CUDA error: out of memory").contains("GPU memory"));
  }

  // --- against real backends, when they are there ------------------------------
  //
  // Skipped unless pointed at something:
  //   NALA_TEST_WAV            a short recording of someone saying a command
  //   NALA_TEST_WHISPER_MODEL  a ggml model for whisper-cli
  //   NALA_TEST_WHISPER_SERVER a running whisper-server, e.g. http://127.0.0.1:8178
  //   NALA_TEST_LLM            an OpenAI-compatible endpoint, e.g. http://127.0.0.1:11434/v1
  //   NALA_TEST_FISH           a Fish Speech server, e.g. http://127.0.0.1:8080

  void liveWhisper() {
    const QString wavPath = qEnvironmentVariable("NALA_TEST_WAV");
    const QString model = qEnvironmentVariable("NALA_TEST_WHISPER_MODEL");
    const QString server = qEnvironmentVariable("NALA_TEST_WHISPER_SERVER");
    if (wavPath.isEmpty() || (model.isEmpty() && server.isEmpty()))
      QSKIP("set NALA_TEST_WAV and NALA_TEST_WHISPER_MODEL or _SERVER");
    QFile file(wavPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    const audio::WavInfo info = audio::parseWav(bytes);
    QVERIFY(info.ok);
    QCOMPARE(info.bitsPerSample, 16);
    const QByteArray raw = bytes.mid(info.dataOffset);
    const QVector<int16_t> mono = audio::toMono16k(
        reinterpret_cast<const int16_t *>(raw.constData()),
        int(raw.size() / 2 / info.channels), info.channels, info.sampleRate);
    const QByteArray pcm(reinterpret_cast<const char *>(mono.constData()),
                         mono.size() * 2);

    QNetworkAccessManager network;
    std::vector<std::unique_ptr<SpeechToText>> backends;
    if (!model.isEmpty()) {
      auto cli = std::make_unique<WhisperCli>();
      cli->configure(qEnvironmentVariable("NALA_TEST_WHISPER_CLI", "whisper-cli"), model);
      backends.push_back(std::move(cli));
    }
    if (!server.isEmpty()) {
      auto remote = std::make_unique<WhisperServer>(&network);
      remote->setUrl(QUrl(server));
      backends.push_back(std::move(remote));
    }
    for (auto &stt : backends) {
      QSignalSpy heard(stt.get(), &SpeechToText::transcribed);
      QSignalSpy failed(stt.get(), &SpeechToText::failed);
      stt->setPrompt(Identity{}.recognitionPrompt());
      stt->transcribe(pcm, "en");
      QTRY_VERIFY_WITH_TIMEOUT(!heard.isEmpty() || !failed.isEmpty(), 60000);
      QVERIFY2(failed.isEmpty(), qPrintable(failed.value(0).value(0).toString()));
      const QString text = heard.first().first().toString();
      qInfo() << stt->name() << "heard" << text << "in"
              << heard.first().at(1).toLongLong() << "ms";
      const Route route = CommandRouter().route(text, {"hey nala", "nala"});
      qInfo() << "routed to" << (route.matched ? route.action : "the model");
      QVERIFY(!text.isEmpty());
      // Whatever whisper made of her name, the privacy switch still works.
      if (qEnvironmentVariable("NALA_TEST_WAV_EXPECT") == "memory.pause")
        QCOMPARE(route.action, QString("memory.pause"));
    }
  }

  void liveModelCallsTools() {
    const QString endpoint = qEnvironmentVariable("NALA_TEST_LLM");
    if (endpoint.isEmpty())
      QSKIP("set NALA_TEST_LLM to an OpenAI-compatible endpoint");
    QTemporaryDir dir;
    Assistant assistant({dir.filePath("a.json"), dir.filePath("memory"), {}}, true);
    assistant.settings()->set("llm.endpoint", endpoint);
    assistant.settings()->set("llm.model", qEnvironmentVariable("NALA_TEST_LLM_MODEL"));
    assistant.settings()->set("llm.timeoutSec", 300);
    MemoryRecord r;
    r.started = r.lastSeen = QDateTime::currentDateTime().addDays(-1);
    r.app = "firefox";
    r.title = "r3dg0d/Nala-Voice: desktop companion - GitHub";
    const qint64 id = assistant.store()->insert(r);
    Artifact a;
    a.kind = "repo";
    a.value = "https://github.com/r3dg0d/Nala-Voice";
    a.memoryId = id;
    a.lastSeen = r.started;
    assistant.store()->addArtifact(a);

    QSignalSpy said(&assistant, &Assistant::said);
    assistant.ask("What was that GitHub project I looked at yesterday?");
    QTRY_VERIFY_WITH_TIMEOUT(!said.isEmpty(), 300000);
    const QString answer = said.last().first().toString();
    qInfo() << "model:" << assistant.model() << "answered:" << answer;
    // It has to have searched memory to know, and said what it found.
    QVERIFY2(answer.contains("Nala", Qt::CaseInsensitive), qPrintable(answer));
  }

  void liveFishSpeech() {
    const QString endpoint = qEnvironmentVariable("NALA_TEST_FISH");
    if (endpoint.isEmpty())
      QSKIP("set NALA_TEST_FISH to a Fish Speech server");
    QNetworkAccessManager network;
    FishSpeech fish(&network);
    fish.configure(QUrl(endpoint), {}, true, {});
    QSignalSpy format(&fish, &TextToSpeech::format);
    QSignalSpy chunks(&fish, &TextToSpeech::audio);
    QSignalSpy done(&fish, &TextToSpeech::done);
    QSignalSpy failed(&fish, &TextToSpeech::failed);
    fish.synthesize("Hello, I'm Nala.");
    QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() || !failed.isEmpty(), 120000);
    QVERIFY2(failed.isEmpty(), qPrintable(failed.value(0).value(0).toString()));
    QCOMPARE(format.count(), 1);
    QVERIFY(!chunks.isEmpty());
  }

  void toolSchemaIsWellFormed() {
    QTemporaryDir dir;
    Assistant assistant({dir.filePath("a.json"), dir.filePath("memory"), {}}, true);
    const QJsonArray all = assistant.tools().schema(
        {"computer", "window", "apps", "files", "browser", "shell", "memory", "nala",
         "system"});
    QCOMPARE(all.size(), assistant.tools().tools().size());
    QSet<QString> names;
    for (const QJsonValue &value : all) {
      const QJsonObject function = value.toObject().value("function").toObject();
      const QString name = function.value("name").toString();
      QVERIFY2(!name.contains('.'), qPrintable(name)); // wire names
      QVERIFY(!names.contains(name));
      names.insert(name);
      QCOMPARE(function.value("parameters").toObject().value("type").toString(),
               QString("object"));
      QVERIFY(!function.value("description").toString().isEmpty());
    }
    // The model is never handed a way to switch screen memory back on.
    QVERIFY(!names.contains("nala_resume_screen_memory"));
    for (const QString &name : names)
      QVERIFY(!name.contains("resume"));
  }
};

QTEST_MAIN(AssistantTests)
#include "assistant_tests.moc"
