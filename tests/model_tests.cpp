// Unit tests for the multi-model layer: routing, fallback, sentence streaming,
// latency, context budgeting and the model/GPU catalogue. No server, no GPU.
//   QT_QPA_PLATFORM=offscreen build/nala-model-tests
#include "contextbudget.h"
#include "latency.h"
#include "modelcatalog.h"
#include "modelrouter.h"
#include "sentencestream.h"

#include <QJsonDocument>
#include <QTest>

using namespace modelrouter;

class ModelTests : public QObject {
  Q_OBJECT

private slots:
  // --- routing ---------------------------------------------------------------
  void routesByRequest_data() {
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("role");
    QTest::newRow("joke") << "tell me a joke" << "fast";
    QTest::newRow("small talk") << "how are you today" << "fast";
    QTest::newRow("explain") << "explain how nix flakes work" << "main";
    QTest::newRow("debug") << "help me debug this rust compiler error" << "main";
    QTest::newRow("analyse") << "analyze this source tree and explain why it crashes" << "main";
    QTest::newRow("command") << "close discord" << "fast";
    QTest::newRow("long") << QString("please ").repeated(40) + "do it" << "main";
  }
  void routesByRequest() {
    QFETCH(QString, text);
    QFETCH(QString, role);
    Options o;
    QCOMPARE(roleName(choose(text, o).role), role);
  }

  void modeOverridesTheClassifier() {
    Options o;
    o.mode = "main";
    QCOMPARE(choose("tell me a joke", o).role, Role::Main);
    o.mode = "speed";
    QCOMPARE(choose("explain how nix flakes work", o).role, Role::Speed);
  }

  void routingCanBeSwitchedOff() {
    Options o;
    o.autoRouting = false;
    const Choice c = choose("tell me a joke", o);
    QCOMPARE(c.role, Role::Main);
    QVERIFY(c.reason.contains("off"));
  }

  void perTaskRolesAreHonoured() {
    Options o;
    o.conversation = "speed";
    QCOMPARE(choose("tell me a joke", o).role, Role::Speed);
    // ...but reasoning still goes to the main model.
    QCOMPARE(choose("explain why the sky is blue", o).role, Role::Main);
    o.coding = "fast";
    QCOMPARE(choose("why does this python function raise an exception", o).role,
             Role::Fast);
  }

  // --- fallback --------------------------------------------------------------
  void fallbackOrderDefaults() {
    QCOMPARE(fallbackOrder(Role::Main),
             (QList<Role>{Role::Main, Role::Fast, Role::Speed}));
    QCOMPARE(fallbackOrder(Role::Fast),
             (QList<Role>{Role::Fast, Role::Speed, Role::Main}));
  }

  void fallbackOrderIsConfigurable() {
    const QList<Role> order = fallbackOrder(Role::Main, {"speed", "fast"});
    QCOMPARE(order.first(), Role::Main);
    QCOMPARE(order.value(1), Role::Speed);
    QCOMPARE(order.size(), 3);
  }

  void findsModelsLoosely() {
    const QStringList have = {"gpt-oss:20b", "qwen3:30b-a3b", "qwen3:8b"};
    QCOMPARE(findModel("qwen3-30b-a3b", have), QString("qwen3:30b-a3b"));
    QCOMPARE(findModel("GPT-OSS-20B", have), QString("gpt-oss:20b"));
    QCOMPARE(findModel("qwen3.8-27b", have), QString());
    QCOMPARE(findModel("", have), QString());
  }

  void exactBeatsContains() {
    const QStringList have = {"qwen3:30b-a3b-instruct", "qwen3:30b-a3b"};
    QCOMPARE(findModel("qwen3:30b-a3b", have), QString("qwen3:30b-a3b"));
  }

  void resolvesWithFallback() {
    const QString names[3] = {"qwen3.8-27b", "gpt-oss:20b", "qwen3:30b-a3b"};
    // Main missing: fast answers.
    Resolved r = resolve(Role::Main, names, {"gpt-oss:20b", "qwen3:30b-a3b"});
    QCOMPARE(r.model, QString("gpt-oss:20b"));
    QVERIFY(r.fellBack);
    QCOMPARE(r.role, Role::Fast);
    // Only the speed model exists.
    r = resolve(Role::Main, names, {"qwen3:30b-a3b"});
    QCOMPARE(r.role, Role::Speed);
    // Everything present: no fallback.
    r = resolve(Role::Main, names, {"qwen3.8-27b", "gpt-oss:20b", "qwen3:30b-a3b"});
    QVERIFY(!r.fellBack);
    QCOMPARE(r.model, QString("qwen3.8-27b"));
    // Nothing configured is installed: no silent guess.
    r = resolve(Role::Fast, names, {"llama3:8b"});
    QVERIFY(r.model.isEmpty());
  }

  void parsesModelPhrases_data() {
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("matches");
    QTest::addColumn<QString>("mode");
    QTest::addColumn<bool>("persistent");
    QTest::newRow("fast") << "use the fast model" << true << "fast" << false;
    QTest::newRow("smart") << "use the smart model for this" << true << "main" << false;
    QTest::newRow("auto") << "switch back to automatic model selection" << true << "auto" << true;
    QTest::newRow("always") << "from now on use the main model" << true << "main" << true;
    QTest::newRow("speed") << "use the speed model" << true << "speed" << false;
    QTest::newRow("unrelated") << "what is the fastest model" << false << "" << false;
    QTest::newRow("chat") << "open firefox" << false << "" << false;
  }
  void parsesModelPhrases() {
    QFETCH(QString, text);
    QFETCH(bool, matches);
    QFETCH(QString, mode);
    QFETCH(bool, persistent);
    ModelPhrase p;
    QCOMPARE(parseModelPhrase(text, &p), matches);
    if (matches) {
      QCOMPARE(p.mode, mode);
      QCOMPARE(p.persistent, persistent);
    }
  }

  // --- sentence streaming ----------------------------------------------------
  void speaksTheFirstSentenceNotTheFirstToken() {
    SentenceStream s;
    QStringList spoken;
    const QString text = "Sure. I can open Firefox for you. Would you also like me to "
                         "restore your previous tabs?";
    // Three characters at a time, like a model.
    for (int i = 0; i < text.size(); i += 3)
      spoken << s.feed(text.mid(i, 3));
    // "Sure." alone is a clipped syllable: it is joined with what follows.
    QVERIFY(!spoken.isEmpty());
    QCOMPARE(spoken.first(), QString("Sure. I can open Firefox for you."));
    spoken << s.flush();
    QCOMPARE(spoken.join(' '),
             QString("Sure. I can open Firefox for you. Would you also like me "
                     "to restore your previous tabs?"));
  }

  void doesNotSplitDecimalsOrAbbreviations() {
    SentenceStream s;
    QStringList out = s.feed("Dr. Smith measured 3.5 metres, e.g. about eleven feet. Then he left. ");
    out << s.flush();
    // The first sentence is intact: "Dr.", "3.5" and "e.g." are not boundaries.
    QCOMPARE(out.value(0), QString("Dr. Smith measured 3.5 metres, e.g. about eleven feet."));
    // The short closing sentence waits for the end of the reply.
    QCOMPARE(out.value(1), QString("Then he left."));
    QCOMPARE(out.size(), 2);
  }

  void neverEmitsTinyFragments() {
    SentenceStream s;
    QStringList out;
    for (const QChar c : QString("Ok. Yes. Fine. Right. "))
      out << s.feed(QString(c));
    out << s.flush();
    // "Ok." alone would be a clipped syllable; only the closing piece of a
    // reply may be short, because nothing follows it to join with.
    QVERIFY(out.size() >= 2);
    QVERIFY2(out.first().size() >= 8, qPrintable(out.first()));
    for (int i = 0; i + 1 < out.size(); ++i)
      QVERIFY2(out.at(i).size() >= 8, qPrintable(out.at(i)));
  }

  void breaksRunOns() {
    SentenceStream::Options o;
    o.maxChars = 60;
    SentenceStream s(o);
    QStringList out = s.feed(QString("word, ").repeated(30));
    out << s.flush();
    QVERIFY(out.size() > 1);
    for (const QString &piece : out)
      QVERIFY(piece.size() <= 70);
  }

  void speakableStripsMarkdown() {
    QCOMPARE(SentenceStream::speakable("**Bold** and `code` and [a link](https://x.io)."),
             QString("Bold and code and a link."));
    QVERIFY(SentenceStream::speakable("```\nrm -rf /\n```").contains("code omitted"));
    QCOMPARE(SentenceStream::speakable("- one\n- two"), QString("one two"));
    QCOMPARE(SentenceStream::speakable("see https://example.com/x now"),
             QString("see a link now"));
  }

  void flushReturnsTheTail() {
    SentenceStream s;
    QVERIFY(s.feed("Almost done").isEmpty());
    QCOMPARE(s.flush(), QString("Almost done"));
  }

  // --- reasoning never reaches the voice -------------------------------------
  void thinkFilterHidesReasoning() {
    ThinkFilter f;
    QString out;
    for (const QString &chunk : {"Hello <thi", "nk>secret plan</th", "ink> world"})
      out += f.feed(chunk);
    out += f.flush();
    QCOMPARE(out, QString("Hello  world"));
    QVERIFY(!out.contains("secret"));
  }

  void thinkFilterPassesPlainTextAndLoneAngle() {
    ThinkFilter f;
    QString out = f.feed("a < b and c > d");
    out += f.flush();
    QCOMPARE(out, QString("a < b and c > d"));
  }

  void thinkFilterHandlesAnUnclosedThought() {
    ThinkFilter f;
    QString out = f.feed("<think>still thinking");
    out += f.flush();
    QVERIFY(out.isEmpty());
    QVERIFY(f.inThought());
  }

  // --- latency ---------------------------------------------------------------
  void latencyReports() {
    LatencyTrace t;
    t.set(LatencyTrace::Wake, 18);
    t.set(LatencyTrace::Stt, 372);
    t.set(LatencyTrace::Route, 12);
    t.set(LatencyTrace::LlmFirstToken, 241);
    t.set(LatencyTrace::TtsFirstAudio, 180);
    QCOMPARE(t.perceived(), qint64(372 + 12 + 241 + 180));
    const QString r = t.report();
    QVERIFY(r.contains("Wake word: 18 ms"));
    QVERIFY(r.contains("STT: 372 ms"));
    QVERIFY(r.contains("Total perceived latency: 805 ms"));
    t.reset();
    QVERIFY(!t.has(LatencyTrace::Stt));
    QCOMPARE(t.perceived(), qint64(0));
  }

  void commandsSkipTheModelAndVoice() {
    LatencyTrace t;
    t.set(LatencyTrace::Stt, 300);
    t.set(LatencyTrace::Route, 1);
    t.set(LatencyTrace::Action, 40);
    QCOMPARE(t.perceived(), qint64(341));
  }

  // --- context budget --------------------------------------------------------
  void keepsRecentTurnsOnly() {
    QJsonArray history;
    for (int i = 0; i < 10; ++i) {
      history.append(QJsonObject{{"role", "user"}, {"content", QString("q%1").arg(i)}});
      history.append(QJsonObject{{"role", "assistant"}, {"content", QString("a%1").arg(i)}});
    }
    const QJsonObject sys{{"role", "system"}, {"content", "be brief"}};
    const ctx::Fit f = ctx::fit(sys, {}, history, 3, 16384, 800);
    QCOMPARE(f.history.size(), 6);
    QCOMPARE(f.history.first().toObject().value("content").toString(), QString("q7"));
    QCOMPARE(f.dropped.size(), 14);
  }

  void shedsOldestPairsToFitTheBudget() {
    QJsonArray history;
    const QString big = QString("x").repeated(4000); // ~1000 tokens
    for (int i = 0; i < 6; ++i) {
      history.append(QJsonObject{{"role", "user"}, {"content", big}});
      history.append(QJsonObject{{"role", "assistant"}, {"content", big}});
    }
    const QJsonObject sys{{"role", "system"}, {"content", "s"}};
    const ctx::Fit f = ctx::fit(sys, {}, history, 20, 3000, 500);
    QVERIFY(f.tokens <= 2500 + 1000); // newest exchange is always kept
    QVERIFY(f.history.size() >= 2);
    QVERIFY(f.history.size() < history.size());
    // History never starts with an answer to a question it no longer has.
    QCOMPARE(f.history.first().toObject().value("role").toString(), QString("user"));
    QVERIFY(!f.dropped.isEmpty());
  }

  void clipsHugeToolOutput() {
    const QString big = QString("line of output\n").repeated(5000);
    const QString c = ctx::clip(big, 12000);
    QVERIFY(c.size() <= 12100);
    QVERIFY(c.contains("characters omitted"));
    QVERIFY(c.startsWith("line of output"));
    QCOMPARE(ctx::clip("short", 12000), QString("short"));
  }

  void summaryRequestCarriesTheDroppedTurns() {
    QJsonArray dropped{QJsonObject{{"role", "user"}, {"content", "my cat is called Miso"}},
                       QJsonObject{{"role", "assistant"}, {"content", "Nice name."}}};
    const QJsonArray req = ctx::summaryRequest("They like cats.", dropped);
    QCOMPARE(req.size(), 2);
    const QString body = req.at(1).toObject().value("content").toString();
    QVERIFY(body.contains("They like cats."));
    QVERIFY(body.contains("Miso"));
    QVERIFY(ctx::capSummary(QString("z").repeated(5000), 100).size() <= 101);
  }

  // --- catalogue and GPU -----------------------------------------------------
  void parsesOllamaTagsAndPs() {
    const QJsonObject tags = QJsonDocument::fromJson(
        R"({"models":[{"name":"gpt-oss:20b","size":13800000000,
            "details":{"parameter_size":"20.9B","quantization_level":"MXFP4"}},
            {"name":"qwen3:30b-a3b","size":18600000000,"details":{}}]})").object();
    const auto installed = catalog::parseTags(tags);
    QCOMPARE(installed.size(), 2);
    QCOMPARE(installed.first().name, QString("gpt-oss:20b"));
    QCOMPARE(installed.first().quantization, QString("MXFP4"));

    const QJsonObject ps = QJsonDocument::fromJson(
        R"({"models":[{"name":"qwen3:30b-a3b","size":20000000000,
            "size_vram":15000000000,"expires_at":"2026-09-29T00:00:00Z"}]})").object();
    const auto loaded = catalog::parsePs(ps);
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.first().gpuPercent(), 75); // 25 % runs from system RAM
  }

  void parsesNvidiaSmi() {
    const auto g = catalog::parseNvidiaSmi("NVIDIA GeForce RTX 4090, 1651, 24564, 3\n");
    QVERIFY(g.present);
    QCOMPARE(g.name, QString("NVIDIA GeForce RTX 4090"));
    QCOMPARE(g.freeMiB(), qint64(24564 - 1651));
    QVERIFY(!catalog::parseNvidiaSmi("").present);
    QVERIFY(!catalog::parseNvidiaSmi("garbage").present);
    QVERIFY(!catalog::parseNvidiaSmi("No devices were found").present);
  }

  void plansEvictionUnderVramPressure() {
    catalog::Gpu gpu;
    gpu.present = true;
    gpu.totalMiB = 24564;
    gpu.usedMiB = 20000; // 4.5 GB free
    QVector<catalog::Loaded> loaded;
    loaded.append({"gpt-oss:20b", 14LL << 30, 14LL << 30, {}});
    loaded.append({"small", 2LL << 30, 2LL << 30, {}});
    const qint64 need = 17LL << 30; // the 27B model at Q4
    QVERIFY(!catalog::fits(gpu, need));
    const QStringList plan = catalog::evictionPlan(gpu, loaded, "qwen3.8-27b", need);
    QCOMPARE(plan.value(0), QString("gpt-oss:20b")); // largest first
    // Already loaded models are never evicted for themselves.
    QVERIFY(!catalog::evictionPlan(gpu, loaded, "gpt-oss:20b", need).contains("gpt-oss:20b"));
    // Plenty of room: nothing to do.
    gpu.usedMiB = 1000;
    QVERIFY(catalog::evictionPlan(gpu, loaded, "qwen3.8-27b", need).isEmpty());
    // No GPU information: assume it fits rather than evict blindly.
    QVERIFY(catalog::fits(catalog::Gpu{}, need));
  }
};

QTEST_MAIN(ModelTests)
#include "model_tests.moc"
