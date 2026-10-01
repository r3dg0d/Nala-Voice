// Unit tests for the multi-model layer: routing, fallback, sentence streaming,
// latency, context budgeting and the model/GPU catalogue. No server, no GPU.
//   QT_QPA_PLATFORM=offscreen build/nala-model-tests
#include "commandrouter.h"
#include "contextbudget.h"
#include "latency.h"
#include "llm.h"
#include "modelcatalog.h"
#include "memory.h"
#include "modelrouter.h"
#include "screenmemory.h"
#include "settings.h"
#include "assistant.h"
#include "audioutil.h"
#include "sentencestream.h"
#include "systemtools.h"
#include "tts.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

using namespace modelrouter;

namespace {

// A minimal HTTP server: records each request body and answers per path.
class FakeServer : public QObject {
public:
  struct Reply { int status = 200; QByteArray body; QStringList events; bool ndjson = false; };
  QTcpServer server;
  QMap<QString, Reply> replies;          // by path
  QList<QJsonObject> chatBodies;         // parsed /chat/completions requests
  QStringList paths;                     // every request path, in order
  QMap<QString, QByteArray> lastBody;    // by path
  // When set, decides the reply for /chat/completions from the request body.
  std::function<Reply(const QJsonObject &)> chat;
  // The same for Ollama's native /api/chat (answers as NDJSON lines).
  std::function<Reply(const QJsonObject &)> nativeChat;
  FakeServer() {
    server.listen(QHostAddress::LocalHost);
    connect(&server, &QTcpServer::newConnection, this, [this] {
      while (QTcpSocket *sock = server.nextPendingConnection()) {
        connect(sock, &QTcpSocket::readyRead, this, [this, sock] { onData(sock); });
      }
    });
  }
  QUrl url(const QString &path = "/v1") const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path));
  }

private:
  void onData(QTcpSocket *sock) {
    QByteArray buf = sock->property("buf").toByteArray() + sock->readAll();
    const int end = buf.indexOf("\r\n\r\n");
    if (end < 0) { sock->setProperty("buf", buf); return; }
    const QByteArray head = buf.left(end);
    int length = 0;
    for (const QByteArray &line : head.split('\n'))
      if (line.toLower().startsWith("content-length:"))
        length = line.mid(15).trimmed().toInt();
    if (buf.size() < end + 4 + length) { sock->setProperty("buf", buf); return; }
    const QByteArray body = buf.mid(end + 4, length);
    const QString path = QString::fromLatin1(head.split(' ').value(1));
    paths << path;
    lastBody[path] = body;
    if (path.endsWith("/chat/completions") || path == "/api/chat")
      chatBodies << QJsonDocument::fromJson(body).object();
    Reply r = replies.value(path, replies.value(QUrl(path).path(), Reply{404, "{}", {}}));
    if (path.endsWith("/chat/completions") && chat)
      r = chat(chatBodies.last());
    if (path == "/api/chat" && nativeChat) {
      r = nativeChat(chatBodies.last());
      r.ndjson = true;
    }
    QByteArray out = "HTTP/1.1 " + QByteArray::number(r.status) + " X\r\n"
                     "Connection: close\r\n";
    out += r.events.isEmpty() ? "Content-Type: application/json\r\n\r\n" + r.body
                              : QByteArray("Content-Type: text/event-stream\r\n\r\n");
    sock->write(out);
    sock->flush();
    // Events go out one at a time so the client really sees them arrive.
    auto events = QSharedPointer<QStringList>::create(r.events);
    const bool ndjson = r.ndjson;
    auto *timer = new QTimer(sock);
    connect(timer, &QTimer::timeout, sock, [sock, events, timer, ndjson] {
      if (events->isEmpty()) { sock->disconnectFromHost(); timer->stop(); return; }
      sock->write(ndjson ? events->takeFirst().toUtf8() + "\n"
                         : "data: " + events->takeFirst().toUtf8() + "\n\n");
      sock->flush();
    });
    if (r.events.isEmpty()) sock->disconnectFromHost(); else timer->start(5);
  }
};

QString chunk(const QString &delta) {
  return QStringLiteral("{\"choices\":[{\"delta\":%1,\"finish_reason\":null}]}").arg(delta);
}
QString contentChunk(const QString &text) {
  return chunk(QStringLiteral("{\"content\":\"%1\"}").arg(text));
}

// The events of a streamed plain-text answer, in small pieces.
QStringList streamOf(const QString &text, int piece = 6) {
  QStringList events;
  for (int i = 0; i < text.size(); i += piece)
    events << contentChunk(QString(text.mid(i, piece)).replace("\"", "\\\""));
  events << "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}" << "[DONE]";
  return events;
}

LlmClient::Config configFor(const FakeServer &s, const QString &model) {
  LlmClient::Config c;
  c.endpoint = s.url();
  c.model = model;
  c.timeoutSec = 10;
  return c;
}

} // namespace

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

  void latencyKeysSurviveTheLogRedactor() {
    LatencyTrace t;
    for (int s = 0; s < LatencyTrace::StageCount; ++s)
      t.set(LatencyTrace::Stage(s), 10);
    for (const QString &key : t.toJson().keys()) {
      QVERIFY2(!key.contains("token", Qt::CaseInsensitive), qPrintable(key));
      QVERIFY2(!key.contains("key", Qt::CaseInsensitive), qPrintable(key));
      QVERIFY2(!key.contains("secret", Qt::CaseInsensitive), qPrintable(key));
    }
    QCOMPARE(t.toJson().value("llm_first_ms").toInt(), 10);
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

  // --- the client, against a fake server ---------------------------------------
  void streamsTextWithoutReasoning() {
    FakeServer srv;
    srv.replies["/v1/chat/completions"].events = {
        chunk("{\"reasoning\":\"hmm\"}"),
        contentChunk("<think>plan"), contentChunk("</think>Sure. "),
        contentChunk("Opening Fire"), contentChunk("fox now."),
        "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}", "[DONE]"};
    QNetworkAccessManager net;
    LlmClient llm(&net);
    llm.configure(configFor(srv, "m"));
    QStringList deltas;
    connect(&llm, &LlmClient::delta, this, [&](const QString &t) { deltas << t; });
    QSignalSpy first(&llm, &LlmClient::firstToken);
    QSignalSpy prefill(&llm, &LlmClient::prefillDone);
    QSignalSpy done(&llm, &LlmClient::replied);
    llm.chatStream(QJsonArray{QJsonObject{{"role", "user"}, {"content", "hi"}}});
    QVERIFY(done.wait(5000));
    const auto reply = done.first().first().value<LlmReply>();
    QCOMPARE(reply.content, QString("Sure. Opening Firefox now."));
    QCOMPARE(deltas.join(""), QString("Sure. Opening Firefox now."));
    QVERIFY(!deltas.join("").contains("plan"));
    QVERIFY(deltas.size() >= 2);            // it arrived in pieces
    QCOMPARE(first.size(), 1);
    QCOMPARE(prefill.size(), 1);            // the reasoning token counted as prefill
    QVERIFY(srv.chatBodies.first().value("stream").toBool());
  }

  void rejectsTruncatedStreams_data() {
    QTest::addColumn<bool>("native");
    QTest::addColumn<bool>("toolCall");
    QTest::newRow("openai-sse-text") << false << false;
    QTest::newRow("ollama-ndjson-text") << true << false;
    QTest::newRow("openai-sse-tool") << false << true;
    QTest::newRow("ollama-ndjson-tool") << true << true;
  }

  void rejectsTruncatedStreams() {
    QFETCH(bool, native);
    QFETCH(bool, toolCall);
    FakeServer srv;
    srv.replies["/api/version"].body = "{\"version\":\"test\"}";
    srv.replies["/api/show"].body = "{\"capabilities\":[\"tools\"]}";
    const QString path = native ? "/api/chat" : "/v1/chat/completions";
    srv.replies[path].ndjson = native;
    srv.replies[path].events =
        native
            ? QStringList{QStringLiteral("{\"message\":{\"content\":\"Partial "
                                         "answer\"},\"done\":false}")}
            : QStringList{contentChunk("Partial answer")};
    if (toolCall) {
      srv.replies[path].events =
          native ? QStringList{QStringLiteral(
                       "{\"message\":{\"tool_calls\":[{\"function\":{\"name\":"
                       "\"get_time\",\"arguments\":{}}}]},\"done\":false}")}
                 : QStringList{chunk("{\"tool_calls\":[{\"index\":0,\"id\":"
                                     "\"call\",\"function\":{\"name\":\"get_"
                                     "time\",\"arguments\":\"{}\"}}]}")};
    }
    QNetworkAccessManager net;
    LlmClient llm(&net);
    auto config = configFor(srv, "m");
    config.provider = native ? "ollama" : "llamacpp";
    llm.configure(config);
    QSignalSpy done(&llm, &LlmClient::replied);
    QSignalSpy failed(&llm, &LlmClient::failed);
    llm.chatStream(
        QJsonArray{QJsonObject{{"role", "user"}, {"content", "hi"}}});
    QVERIFY(failed.wait(5000));
    QCOMPARE(done.size(), 0);
    QCOMPARE(failed.size(), 1);
    QVERIFY(failed.first().first().toString().contains("before completion"));

    // A complete next turn must recover without retaining the partial answer.
    srv.replies[path].events =
        native
            ? QStringList{QStringLiteral("{\"message\":{\"content\":\"Complete "
                                         "answer\"},\"done\":true}")}
            : streamOf("Complete answer");
    llm.chatStream(
        QJsonArray{QJsonObject{{"role", "user"}, {"content", "retry"}}});
    QVERIFY(done.wait(5000));
    QCOMPARE(done.first().first().value<LlmReply>().content,
             QString("Complete answer"));
    QCOMPARE(failed.size(), 1);
  }

  void acceptsEitherSseCompletionMarker_data() {
    QTest::addColumn<QString>("marker");
    QTest::newRow("done-sentinel") << QString("[DONE]");
    QTest::newRow("finish-reason")
        << QString("{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}");
  }

  void acceptsEitherSseCompletionMarker() {
    QFETCH(QString, marker);
    FakeServer srv;
    srv.replies["/v1/chat/completions"].events = {contentChunk("Complete"),
                                                  marker};
    QNetworkAccessManager net;
    LlmClient llm(&net);
    auto config = configFor(srv, "m");
    config.provider = "llamacpp";
    llm.configure(config);
    QSignalSpy done(&llm, &LlmClient::replied);
    QSignalSpy failed(&llm, &LlmClient::failed);
    llm.chatStream(
        QJsonArray{QJsonObject{{"role", "user"}, {"content", "hi"}}});
    QVERIFY(done.wait(5000));
    QCOMPARE(done.first().first().value<LlmReply>().content,
             QString("Complete"));
    QCOMPARE(failed.size(), 0);
  }

  void assemblesStreamedToolCalls() {
    FakeServer srv;
    srv.replies["/v1/chat/completions"].events = {
        chunk("{\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"function\":{\"name\":\"set_volume\",\"arguments\":\"{\\\"le\"}}]}"),
        chunk("{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"vel\\\": 40}\"}}]}"),
        "{\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}", "[DONE]"};
    QNetworkAccessManager net;
    LlmClient llm(&net);
    llm.configure(configFor(srv, "m"));
    QSignalSpy done(&llm, &LlmClient::replied);
    llm.chatStream(QJsonArray{QJsonObject{{"role", "user"}, {"content", "quieter"}}});
    QVERIFY(done.wait(5000));
    const auto reply = done.first().first().value<LlmReply>();
    QCOMPARE(reply.toolCalls.size(), 1);
    QCOMPARE(reply.toolCalls.first().name, QString("set_volume"));
    QCOMPARE(reply.toolCalls.first().arguments.value("level").toInt(), 40);
    QVERIFY(reply.toolCalls.first().argumentsValid);
  }

  void nonStreamingStillWorks() {
    FakeServer srv;
    srv.replies["/v1/chat/completions"].body =
        "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"<think>x</think>Hello.\"},\"finish_reason\":\"stop\"}]}";
    QNetworkAccessManager net;
    LlmClient llm(&net);
    llm.configure(configFor(srv, "m"));
    QSignalSpy done(&llm, &LlmClient::replied);
    llm.chat(QJsonArray{QJsonObject{{"role", "user"}, {"content", "hi"}}});
    QVERIFY(done.wait(5000));
    QCOMPARE(done.first().first().value<LlmReply>().content, QString("Hello."));
    QVERIFY(!srv.chatBodies.first().value("stream").toBool());
  }

  void reportsServerErrorsWhileStreaming() {
    FakeServer srv;
    srv.replies["/v1/chat/completions"] = {500, "{\"error\":{\"message\":\"CUDA error: out of memory\"}}", {}};
    QNetworkAccessManager net;
    LlmClient llm(&net);
    llm.configure(configFor(srv, "m"));
    QSignalSpy failed(&llm, &LlmClient::failed);
    llm.chatStream(QJsonArray{QJsonObject{{"role", "user"}, {"content", "hi"}}});
    QVERIFY(failed.wait(5000));
    QVERIFY(failed.first().first().toString().contains("GPU memory"));
  }

  void thinkingSwitchesFollowTheServer() {
    // Not Ollama: Qwen's template switch.
    {
      FakeServer srv;
      srv.replies["/v1/chat/completions"].body =
          "{\"choices\":[{\"message\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}";
      QNetworkAccessManager net;
      LlmClient llm(&net);
      llm.configure(configFor(srv, "qwen3:30b-a3b"));
      QSignalSpy done(&llm, &LlmClient::replied);
      llm.chat(QJsonArray{QJsonObject{{"role", "user"}, {"content", "hi"}}});
      QVERIFY(done.wait(5000));
      QVERIFY(!srv.chatBodies.first().value("chat_template_kwargs").toObject()
                   .value("enable_thinking").toBool(true));
    }
    // Ollama, gpt-oss: reasoning cannot be turned off; "low" is the floor.
    {
      FakeServer srv;
      srv.replies["/api/version"].body = "{\"version\":\"0.34.3\"}";
      srv.replies["/v1/chat/completions"].body =
          "{\"choices\":[{\"message\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}";
      QNetworkAccessManager net;
      LlmClient llm(&net);
      auto c = configFor(srv, "gpt-oss:20b");
      c.endpoint = srv.url("/v1");
      c.native = false; // this one is about the OpenAI-compatible endpoint
      llm.configure(c);
      QSignalSpy done(&llm, &LlmClient::replied);
      llm.chat(QJsonArray{QJsonObject{{"role", "user"}, {"content", "hi"}}});
      QVERIFY(done.wait(5000));
      QCOMPARE(srv.chatBodies.first().value("reasoning_effort").toString(), QString("low"));
      // ...and thinking "on" asks for more.
      llm.setThinkingOverride("on");
      QSignalSpy again(&llm, &LlmClient::replied);
      llm.chat(QJsonArray{QJsonObject{{"role", "user"}, {"content", "why"}}});
      QVERIFY(again.wait(5000));
      QCOMPARE(srv.chatBodies.last().value("reasoning_effort").toString(), QString("medium"));
    }
  }

  void switchesModelPerRequest() {
    FakeServer srv;
    srv.replies["/v1/chat/completions"].body =
        "{\"choices\":[{\"message\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}";
    QNetworkAccessManager net;
    LlmClient llm(&net);
    llm.configure(configFor(srv, "big"));
    for (const QString &m : {QString("big"), QString("small")}) {
      llm.useModel(m);
      QSignalSpy done(&llm, &LlmClient::replied);
      llm.chat(QJsonArray{QJsonObject{{"role", "user"}, {"content", "hi"}}});
      QVERIFY(done.wait(5000));
    }
    QCOMPARE(srv.chatBodies.size(), 2);
    QCOMPARE(srv.chatBodies.at(0).value("model").toString(), QString("big"));
    QCOMPARE(srv.chatBodies.at(1).value("model").toString(), QString("small"));
  }

  // --- Ollama's native API ---------------------------------------------------------
  static QString nd(const QString &content, const QString &thinking = QString()) {
    QString msg = "{\"role\":\"assistant\",\"content\":\"" + content + "\"";
    if (!thinking.isEmpty())
      msg += ",\"thinking\":\"" + thinking + "\"";
    return "{\"model\":\"m\",\"message\":" + msg + "},\"done\":false}";
  }
  static QString ndDone() {
    return "{\"model\":\"m\",\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
           "\"done\":true,\"done_reason\":\"stop\",\"prompt_eval_count\":120,\"eval_count\":40,"
           "\"load_duration\":2000000000,\"prompt_eval_duration\":300000000,"
           "\"eval_duration\":800000000}";
  }
  static LlmClient::Config nativeConfig(const FakeServer &s, const QString &model) {
    LlmClient::Config c = configFor(s, model);
    c.numCtx = 16384;
    c.keepAlive = "30m";
    return c;
  }

  void nativeAskedForTheContextWeWant() {
    FakeServer srv;
    srv.replies["/api/version"].body = "{\"version\":\"0.34.3\"}";
    srv.replies["/api/chat"].body = "{\"message\":{\"role\":\"assistant\",\"content\":\"ok\"},\"done\":true}";
    QNetworkAccessManager net;
    LlmClient llm(&net);
    llm.configure(nativeConfig(srv, "qwen3.8:27b"));
    llm.setThinkingOverride("off");
    QSignalSpy done(&llm, &LlmClient::replied);
    llm.chat(QJsonArray{QJsonObject{{"role", "user"}, {"content", "hi"}}});
    QVERIFY(done.wait(5000));
    const QJsonObject body = QJsonDocument::fromJson(srv.lastBody["/api/chat"]).object();
    QCOMPARE(body.value("options").toObject().value("num_ctx").toInt(), 16384);
    QCOMPARE(body.value("keep_alive").toString(), QString("30m"));
    QCOMPARE(body.value("think").toBool(true), false);
    QVERIFY(!srv.paths.contains("/v1/chat/completions"));
    QCOMPARE(done.first().first().value<LlmReply>().content, QString("ok"));
  }

  void nativeStreamsAndReportsTimings() {
    FakeServer srv;
    srv.replies["/api/version"].body = "{\"version\":\"0.34.3\"}";
    srv.replies["/api/chat"].events = {nd("", "let me think"), nd("Sure. "), nd("Done."), ndDone()};
    srv.replies["/api/chat"].ndjson = true;
    QNetworkAccessManager net;
    LlmClient llm(&net);
    llm.configure(nativeConfig(srv, "m"));
    QStringList deltas;
    connect(&llm, &LlmClient::delta, this, [&](const QString &t) { deltas << t; });
    QSignalSpy first(&llm, &LlmClient::firstToken);
    QSignalSpy prefill(&llm, &LlmClient::prefillDone);
    QSignalSpy stats(&llm, &LlmClient::stats);
    QSignalSpy usage(&llm, &LlmClient::usage);
    QSignalSpy done(&llm, &LlmClient::replied);
    llm.chatStream(QJsonArray{QJsonObject{{"role", "user"}, {"content", "hi"}}});
    QVERIFY(done.wait(5000));
    QCOMPARE(done.first().first().value<LlmReply>().content, QString("Sure. Done."));
    QCOMPARE(deltas.join(""), QString("Sure. Done."));
    QCOMPARE(prefill.size(), 1);   // the thinking token counted as the first token of any kind
    QCOMPARE(first.size(), 1);     // ...and the first visible one came later
    QCOMPARE(stats.size(), 1);
    QCOMPARE(stats.first().at(0).toInt(), 120);
    QCOMPARE(stats.first().at(1).toInt(), 40);
    QCOMPARE(stats.first().at(2).toLongLong(), qint64(2000));
    QCOMPARE(stats.first().at(4).toLongLong(), qint64(800));
    QCOMPARE(usage.first().at(0).toInt(), 40); // the server's count, not one per chunk
    QCOMPARE(usage.first().at(1).toInt(), 1);
  }

  void nativeToolCallsAndTheirResults() {
    FakeServer srv;
    srv.replies["/api/version"].body = "{\"version\":\"0.34.3\"}";
    srv.replies["/api/chat"].events = {
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"function\":"
        "{\"name\":\"volume_set\",\"arguments\":{\"level\":25}}}]},\"done\":false}",
        ndDone()};
    srv.replies["/api/chat"].ndjson = true;
    QNetworkAccessManager net;
    LlmClient llm(&net);
    llm.configure(nativeConfig(srv, "m"));
    QSignalSpy done(&llm, &LlmClient::replied);
    llm.chatStream(QJsonArray{QJsonObject{{"role", "user"}, {"content", "quieter"}}});
    QVERIFY(done.wait(5000));
    const auto reply = done.first().first().value<LlmReply>();
    QCOMPARE(reply.toolCalls.size(), 1);
    QCOMPARE(reply.toolCalls.first().name, QString("volume_set"));
    QCOMPARE(reply.toolCalls.first().arguments.value("level").toInt(), 25);
    QCOMPARE(reply.finishReason, QString("tool_calls"));
  }

  void nativeRetriesWithoutThinkWhenRefused() {
    FakeServer srv;
    srv.replies["/api/version"].body = "{\"version\":\"0.34.3\"}";
    int calls = 0;
    srv.chat = nullptr;
    srv.replies["/api/chat"] = {400, "{\"error\":\"\\\"m\\\" does not support thinking\"}", {}};
    QNetworkAccessManager net;
    LlmClient llm(&net);
    llm.configure(nativeConfig(srv, "m"));
    llm.setThinkingOverride("on");
    QSignalSpy failed(&llm, &LlmClient::failed);
    QSignalSpy done(&llm, &LlmClient::replied);
    llm.chat(QJsonArray{QJsonObject{{"role", "user"}, {"content", "hi"}}});
    // The server keeps refusing here, so the second (think-less) try fails too;
    // what matters is that it was asked again without "think".
    QVERIFY(failed.wait(5000));
    Q_UNUSED(calls);
    int chats = srv.paths.count("/api/chat");
    QCOMPARE(chats, 2);
    QVERIFY(failed.first().first().toString().contains("does not support thinking"));
  }

  void nativeGptOssTakesALevel() {
    FakeServer srv;
    srv.replies["/api/version"].body = "{\"version\":\"0.34.3\"}";
    srv.replies["/api/chat"].body = "{\"message\":{\"role\":\"assistant\",\"content\":\"ok\"},\"done\":true}";
    QNetworkAccessManager net;
    LlmClient llm(&net);
    llm.configure(nativeConfig(srv, "gpt-oss:20b"));
    llm.setThinkingOverride("off");
    QSignalSpy done(&llm, &LlmClient::replied);
    llm.chat(QJsonArray{QJsonObject{{"role", "user"}, {"content", "hi"}}});
    QVERIFY(done.wait(5000));
    QCOMPARE(QJsonDocument::fromJson(srv.lastBody["/api/chat"]).object().value("think").toString(),
             QString("low"));
  }

  void nativeAlwaysSendsThinkOffButNotOnToAModelThatCannot() {
    FakeServer srv;
    srv.replies["/api/version"].body = "{\"version\":\"0.34.3\"}";
    srv.replies["/api/show"].body = "{\"capabilities\":[\"completion\",\"tools\"]}"; // no "thinking"
    srv.replies["/api/chat"].body = "{\"message\":{\"role\":\"assistant\",\"content\":\"ok\"},\"done\":true}";
    QNetworkAccessManager net;
    LlmClient llm(&net);
    llm.configure(nativeConfig(srv, "gemma-ish"));
    QSignalSpy done(&llm, &LlmClient::replied);
    llm.setThinkingOverride("off");
    llm.chat(QJsonArray{QJsonObject{{"role", "user"}, {"content", "hi"}}});
    QVERIFY(done.wait(5000));
    // Off is sent: such a model may still reason silently if it is left out.
    QVERIFY(QJsonDocument::fromJson(srv.lastBody["/api/chat"]).object().contains("think"));
    QCOMPARE(QJsonDocument::fromJson(srv.lastBody["/api/chat"]).object().value("think").toBool(true), false);
    // On is withheld: the server said it cannot.
    llm.setThinkingOverride("on");
    QSignalSpy again(&llm, &LlmClient::replied);
    llm.chat(QJsonArray{QJsonObject{{"role", "user"}, {"content", "why"}}});
    QVERIFY(again.wait(5000));
    QVERIFY(!QJsonDocument::fromJson(srv.lastBody["/api/chat"]).object().contains("think"));
  }

  void nativeIsSkippedWhenSwitchedOff() {
    FakeServer srv;
    srv.replies["/api/version"].body = "{\"version\":\"0.34.3\"}";
    srv.replies["/v1/chat/completions"].body =
        "{\"choices\":[{\"message\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}";
    QNetworkAccessManager net;
    LlmClient llm(&net);
    auto c = nativeConfig(srv, "m");
    c.native = false;
    llm.configure(c);
    QSignalSpy done(&llm, &LlmClient::replied);
    llm.chat(QJsonArray{QJsonObject{{"role", "user"}, {"content", "hi"}}});
    QVERIFY(done.wait(5000));
    QVERIFY(!srv.paths.contains("/api/chat"));
  }

  void convertsMessagesToTheNativeShape() {
    const QJsonArray in{
        QJsonObject{{"role", "system"}, {"content", "be brief"}},
        QJsonObject{{"role", "user"}, {"content", QJsonArray{
            QJsonObject{{"type", "text"}, {"text", "what is this"}},
            QJsonObject{{"type", "image_url"},
                        {"image_url", QJsonObject{{"url", "data:image/jpeg;base64,QUJD"}}}}}}},
        QJsonObject{{"role", "assistant"}, {"content", ""}, {"tool_calls", QJsonArray{
            QJsonObject{{"id", "call_9"}, {"type", "function"},
                        {"function", QJsonObject{{"name", "volume_get"}, {"arguments", "{\"a\":1}"}}}}}}},
        QJsonObject{{"role", "tool"}, {"tool_call_id", "call_9"}, {"content", "{\"ok\":true}"}}};
    const QJsonArray out = LlmClient::ollamaMessages(in);
    QCOMPARE(out.size(), 4);
    QCOMPARE(out.at(1).toObject().value("content").toString(), QString("what is this"));
    QCOMPARE(out.at(1).toObject().value("images").toArray().first().toString(), QString("QUJD"));
    const QJsonObject call = out.at(2).toObject().value("tool_calls").toArray().first().toObject()
                                 .value("function").toObject();
    QCOMPARE(call.value("name").toString(), QString("volume_get"));
    QCOMPARE(call.value("arguments").toObject().value("a").toInt(), 1); // a string became an object
    QCOMPARE(out.at(3).toObject().value("tool_name").toString(), QString("volume_get"));
  }

  void catalogueQueriesTheServer() {
    FakeServer srv;
    srv.replies["/api/version"].body = "{\"version\":\"0.34.3\"}";
    srv.replies["/api/tags"].body = "{\"models\":[{\"name\":\"gpt-oss:20b\",\"size\":13800000000}]}";
    srv.replies["/api/ps"].body = "{\"models\":[{\"name\":\"gpt-oss:20b\",\"size\":14000000000,\"size_vram\":14000000000}]}";
    QNetworkAccessManager net;
    LlmClient llm(&net);
    llm.configure(configFor(srv, "gpt-oss:20b"));
    QVector<catalog::Installed> installed;
    QVector<catalog::Loaded> loaded;
    QString version;
    bool a = false, b = false, c = false;
    llm.installedModels([&](QVector<catalog::Installed> i, QString) { installed = i; a = true; });
    QTRY_VERIFY(a);
    llm.loadedModels([&](QVector<catalog::Loaded> l, QString) { loaded = l; b = true; });
    QTRY_VERIFY(b);
    llm.health([&](QString v, QString) { version = v; c = true; });
    QTRY_VERIFY(c);
    QCOMPARE(installed.value(0).name, QString("gpt-oss:20b"));
    QCOMPARE(loaded.value(0).gpuPercent(), 100);
    QCOMPARE(version, QString("0.34.3"));
  }

  void offlineServerFailsCleanly() {
    LlmClient::Config c;
    c.endpoint = QUrl("http://127.0.0.1:1/v1");
    c.model = "m";
    c.timeoutSec = 5;
    QNetworkAccessManager net;
    LlmClient llm(&net);
    llm.configure(c);
    QSignalSpy failed(&llm, &LlmClient::failed);
    llm.chatStream(QJsonArray{QJsonObject{{"role", "user"}, {"content", "hi"}}});
    QVERIFY(failed.wait(8000));
    QVERIFY(failed.first().first().toString().contains("model server"));
    QVector<catalog::Installed> installed{{"x"}};
    QString err;
    bool done = false;
    llm.installedModels([&](QVector<catalog::Installed> i, QString e) { installed = i; err = e; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QVERIFY(installed.isEmpty());
    QVERIFY(!err.isEmpty());
  }

  // --- deterministic commands --------------------------------------------------
  void routerHandlesVolumeAndMediaWithoutAModel_data() {
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("action");
    QTest::addColumn<int>("number"); // level or delta; -999 when none
    QTest::newRow("set") << "Turn the volume down to 40%." << "volume.set" << 40;
    QTest::newRow("set words") << "set the volume to forty five percent" << "volume.set" << 45;
    QTest::newRow("bare") << "volume 30" << "volume.set" << 30;
    QTest::newRow("down") << "turn my volume down" << "volume.step" << -10;
    QTest::newRow("up a bit") << "turn the volume up a bit" << "volume.step" << 5;
    QTest::newRow("louder") << "louder" << "volume.step" << 10;
    QTest::newRow("quieter") << "make it quieter" << "volume.step" << -10;
    QTest::newRow("mute") << "mute" << "volume.mute" << -999;
    QTest::newRow("unmute") << "unmute the sound" << "volume.unmute" << -999;
    QTest::newRow("pause") << "pause" << "media.pause" << -999;
    QTest::newRow("pause music") << "pause the music" << "media.pause" << -999;
    QTest::newRow("resume") << "resume" << "media.play" << -999;
    QTest::newRow("next") << "next song" << "media.next" << -999;
    QTest::newRow("skip") << "skip" << "media.next" << -999;
    QTest::newRow("previous") << "previous track" << "media.previous" << -999;
    QTest::newRow("time") << "what time is it" << "time.now" << -999;
    QTest::newRow("time2") << "what's the time" << "time.now" << -999;
    QTest::newRow("date") << "what's the date" << "date.today" << -999;
    QTest::newRow("lock") << "lock the screen" << "session.lock" << -999;
    QTest::newRow("shot") << "take a screenshot" << "screenshot.take" << -999;
    QTest::newRow("video") << "start a video recording" << "record.start" << -999;
    QTest::newRow("stop video") << "stop the video recording" << "record.stop" << -999;
  }
  void routerHandlesVolumeAndMediaWithoutAModel() {
    QFETCH(QString, text);
    QFETCH(QString, action);
    QFETCH(int, number);
    CommandRouter router("nala");
    const Route r = router.route("Nala, " + text, {"nala"});
    QVERIFY2(r.matched, qPrintable(text));
    QCOMPARE(r.action, action);
    if (number != -999) {
      const int got = r.args.contains("level") ? r.args.value("level").toInt()
                                               : r.args.value("delta").toInt();
      QCOMPARE(got, number);
    }
  }

  void routerLeavesOpenEndedRequestsToTheModel_data() {
    QTest::addColumn<QString>("text");
    QTest::newRow("play a genre") << "play some jazz";
    QTest::newRow("explain volume") << "explain how the volume knob works";
    QTest::newRow("skip ahead") << "skip to the part about nix";
    QTest::newRow("time zones") << "what time is it in tokyo when it is noon in paris";
    QTest::newRow("record a song") << "record a song about cats";
  }
  void routerLeavesOpenEndedRequestsToTheModel() {
    QFETCH(QString, text);
    CommandRouter router("nala");
    QVERIFY2(!router.route(text, {"nala"}).matched, qPrintable(text));
  }

  void privacyCommandsStillWin() {
    // "Recording" and "pause" are also media words; the privacy switch is first.
    CommandRouter router("nala");
    QCOMPARE(router.route("pause screen memory").action, QString("memory.pause"));
    QCOMPARE(router.route("stop screen recording").action, QString("memory.pause"));
    QCOMPARE(router.route("start screen recording").action, QString("memory.resume"));
    QCOMPARE(router.route("stop looking at my screen").action, QString("memory.pause"));
  }

  void buildsVolumeCommandsWithoutAShell() {
    QString err;
    auto c = systemtools::setVolume(40, &err);
    QVERIFY(c);
    QCOMPARE(c->program, QString("wpctl"));
    QCOMPARE(c->args, (QStringList{"set-volume", "@DEFAULT_AUDIO_SINK@", "0.40"}));
    QVERIFY(!systemtools::setVolume(101, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(!systemtools::setVolume(-1, &err));
    auto step = systemtools::stepVolume(-10, &err);
    QVERIFY(step);
    QCOMPARE(step->args.last(), QString("10%-"));
    QVERIFY(step->args.contains("-l")); // never past 100 %
    QCOMPARE(systemtools::stepVolume(5)->args.last(), QString("5%+"));
    QVERIFY(!systemtools::stepVolume(0));
    QVERIFY(!systemtools::stepVolume(80));
    QVERIFY(!systemtools::muteVolume("explode"));
    QCOMPARE(systemtools::muteVolume("mute")->args.last(), QString("1"));
  }

  void parsesVolumeOutput() {
    auto v = systemtools::parseVolume("Volume: 0.40\n");
    QVERIFY(v.ok);
    QCOMPARE(v.percent, 40);
    QVERIFY(!v.muted);
    v = systemtools::parseVolume("Volume: 0.55 [MUTED]");
    QVERIFY(v.muted);
    QCOMPARE(v.percent, 55);
    QVERIFY(!systemtools::parseVolume("garbage").ok);
  }

  void mediaOnlyAllowsKnownActions() {
    QVERIFY(systemtools::media("play-pause"));
    QVERIFY(!systemtools::media("open --url http://evil"));
    QVERIFY(!systemtools::media(""));
  }

  void safeCommandsAreAllowListed_data() {
    QTest::addColumn<QString>("line");
    QTest::addColumn<bool>("allowed");
    QTest::newRow("uname") << "uname -a" << true;
    QTest::newRow("df") << "df -h" << true;
    QTest::newRow("free") << "free --human" << true;
    QTest::newRow("nvidia") << "nvidia-smi" << true;
    QTest::newRow("rm") << "rm -rf" << false;
    QTest::newRow("pipe") << "uname | sh" << false;
    QTest::newRow("chain") << "uname; reboot" << false;
    QTest::newRow("subst") << "echo $(id)" << false;
    QTest::newRow("path") << "df /etc/shadow" << false;
    QTest::newRow("redirect") << "date > /etc/passwd" << false;
    QTest::newRow("backtick") << "date `reboot`" << false;
    QTest::newRow("unknown") << "cat /etc/shadow" << false;
    QTest::newRow("bad flag") << "df -h --output=source,target,size" << false;
    QTest::newRow("newline") << "uname\nreboot" << false;
    QTest::newRow("empty") << "   " << false;
  }
  void safeCommandsAreAllowListed() {
    QFETCH(QString, line);
    QFETCH(bool, allowed);
    QString err;
    const auto c = systemtools::safeCommand(line, &err);
    QCOMPARE(bool(c), allowed);
    if (!allowed)
      QVERIFY2(!err.isEmpty(), qPrintable(line));
    else
      QVERIFY(!c->program.contains('/'));
  }

  void notificationTitlesCannotBecomeOptions() {
    const auto c = systemtools::notify("--icon=/etc/passwd", "body");
    QVERIFY(c);
    const int dashdash = c->args.indexOf("--");
    QVERIFY(dashdash >= 0);
    QVERIFY(c->args.indexOf("--icon=/etc/passwd") > dashdash);
    QVERIFY(!systemtools::notify("   ", "x"));
  }

  void clipboardTextGoesOnStdinNotTheCommandLine() {
    const auto c = systemtools::clipboardWrite("secret; rm -rf ~");
    QVERIFY(c);
    QVERIFY(c->args.isEmpty());
    QCOMPARE(c->input, QByteArray("secret; rm -rf ~"));
  }

  void namesFilesByTimeWithoutOverwriting() {
    const QDateTime a(QDate(2026, 9, 29), QTime(10, 30, 5));
    const QDateTime b = a.addSecs(1);
    QVERIFY(systemtools::screenshotPath("/x", a) != systemtools::screenshotPath("/x", b));
    QCOMPARE(systemtools::screenshotPath("/x", a), QString("/x/nala-20260929-103005.png"));
  }

  void speaksTimeAndDate() {
    const QDateTime t(QDate(2026, 9, 29), QTime(15, 7, 0));
    QCOMPARE(systemtools::spokenTime(t), QString("3:07 PM"));
    QCOMPARE(systemtools::spokenDate(t), QString("Tuesday, September 29, 2026"));
  }

  void parsesSpokenNumbers() {
    QCOMPARE(systemtools::parseSpokenNumber("forty five"), 45);
    QCOMPARE(systemtools::parseSpokenNumber("forty-five"), 45);
    QCOMPARE(systemtools::parseSpokenNumber("a hundred"), 100);
    QCOMPARE(systemtools::parseSpokenNumber("seventy"), 70);
    QCOMPARE(systemtools::parseSpokenNumber("40"), 40);
    QCOMPARE(systemtools::parseSpokenNumber("banana"), -1);
    QCOMPARE(systemtools::parseSpokenNumber(""), -1);
  }

  // --- voice ---------------------------------------------------------------------
  void qwenStreamsRawPcm() {
    FakeServer srv;
    srv.replies["/v1/audio/speech"].body = QByteArray(4801, '\x10'); // an odd byte on the end
    QNetworkAccessManager net;
    QwenTts tts(&net);
    QwenTts::Config c;
    c.endpoint = srv.url("");
    c.sampleRate = 24000;
    tts.configure(c);
    QSignalSpy format(&tts, &TextToSpeech::format);
    QSignalSpy done(&tts, &TextToSpeech::done);
    QByteArray got;
    connect(&tts, &TextToSpeech::audio, this, [&](const QByteArray &b) { got += b; });
    tts.synthesize("Hello there.");
    QVERIFY(done.wait(5000));
    QCOMPARE(format.size(), 1);
    QCOMPARE(format.first().at(0).toInt(), 24000);
    QCOMPARE(format.first().at(1).toInt(), 1);
    QCOMPARE(format.first().at(2).toInt(), 16);
    QCOMPARE(got.size(), 4800); // whole samples only: the stray byte is not played
    const QJsonObject body = QJsonDocument::fromJson(srv.lastBody["/v1/audio/speech"]).object();
    QCOMPARE(body.value("input").toString(), QString("Hello there."));
    QCOMPARE(body.value("response_format").toString(), QString("pcm"));
    QVERIFY(body.value("stream").toBool());
    QVERIFY(!body.contains("instruct"));
  }

  void qwenAcceptsWavToo() {
    FakeServer srv;
    srv.replies["/v1/audio/speech"].body = audio::wav(QByteArray(2000, '\x01'), 22050);
    QNetworkAccessManager net;
    QwenTts tts(&net);
    QwenTts::Config c;
    c.endpoint = srv.url("");
    c.streaming = false;
    c.instruct = "warm and calm";
    tts.configure(c);
    QSignalSpy format(&tts, &TextToSpeech::format);
    QSignalSpy done(&tts, &TextToSpeech::done);
    QByteArray got;
    connect(&tts, &TextToSpeech::audio, this, [&](const QByteArray &b) { got += b; });
    tts.synthesize("Hi.");
    QVERIFY(done.wait(5000));
    QCOMPARE(format.first().at(0).toInt(), 22050); // the header wins over the setting
    QCOMPARE(got.size(), 2000);
    const QJsonObject body = QJsonDocument::fromJson(srv.lastBody["/v1/audio/speech"]).object();
    QCOMPARE(body.value("response_format").toString(), QString("wav"));
    QCOMPARE(body.value("instruct").toString(), QString("warm and calm"));
  }

  void qwenReportsAnUnreachableServer() {
    QNetworkAccessManager net;
    QwenTts tts(&net);
    QwenTts::Config c;
    c.endpoint = QUrl("http://127.0.0.1:1");
    tts.configure(c);
    QSignalSpy failed(&tts, &TextToSpeech::failed);
    tts.synthesize("Hi.");
    QVERIFY(failed.wait(5000));
    QVERIFY(failed.first().first().toString().contains("Qwen3-TTS"));
  }

  void fishTakesOverWhenQwenIsDown() {
    FakeServer qwenSrv, fishSrv;
    qwenSrv.replies["/v1/audio/speech"] = {500, "{}", {}};
    fishSrv.replies["/v1/tts"].body = audio::wav(QByteArray(1000, '\x02'), 44100);
    QNetworkAccessManager net;
    QwenTts qwen(&net);
    QwenTts::Config qc;
    qc.endpoint = qwenSrv.url("");
    qwen.configure(qc);
    FishSpeech fish(&net);
    fish.configure(fishSrv.url(""), {}, false, {});
    TtsChain chain;
    chain.setEngines({&qwen, &fish});

    for (int sentence = 0; sentence < 2; ++sentence) {
      QSignalSpy done(&chain, &TextToSpeech::done);
      QSignalSpy failed(&chain, &TextToSpeech::failed);
      QByteArray got;
      QMetaObject::Connection c = connect(&chain, &TextToSpeech::audio, this,
                                          [&](const QByteArray &b) { got += b; });
      chain.synthesize("One sentence.");
      QVERIFY(done.wait(5000));
      disconnect(c);
      QCOMPARE(failed.size(), 0);
      QCOMPARE(got.size(), 1000);
      QCOMPARE(chain.lastEngine(), QString("fish-speech"));
    }
    // The second sentence did not wait for Qwen to fail again.
    QCOMPARE(qwenSrv.paths.count("/v1/audio/speech"), 1);
    QCOMPARE(fishSrv.paths.count("/v1/tts"), 2);
    QCOMPARE(chain.downEngines(), QStringList{"qwen3-tts"});
  }

  void chainFailsCleanlyWhenNoVoiceWorks() {
    FakeServer a, b;
    a.replies["/v1/audio/speech"] = {500, "{}", {}};
    b.replies["/v1/tts"] = {500, "{}", {}};
    QNetworkAccessManager net;
    QwenTts qwen(&net);
    QwenTts::Config qc;
    qc.endpoint = a.url("");
    qwen.configure(qc);
    FishSpeech fish(&net);
    fish.configure(b.url(""), {}, false, {});
    TtsChain chain;
    chain.setEngines({&qwen, &fish});
    QSignalSpy failed(&chain, &TextToSpeech::failed);
    QSignalSpy done(&chain, &TextToSpeech::done);
    chain.synthesize("Hi.");
    QVERIFY(failed.wait(5000));
    QCOMPARE(done.size(), 0);
    QVERIFY(failed.first().first().toString().contains("Qwen3-TTS"));
    QVERIFY(failed.first().first().toString().contains("Fish Speech"));
  }

  void chainStopSilencesEverything() {
    FakeServer srv;
    srv.replies["/v1/audio/speech"].body = QByteArray(2000, '\x05');
    QNetworkAccessManager net;
    QwenTts qwen(&net);
    QwenTts::Config qc;
    qc.endpoint = srv.url("");
    qwen.configure(qc);
    TtsChain chain;
    chain.setEngines({&qwen});
    QSignalSpy done(&chain, &TextToSpeech::done);
    QSignalSpy audioSpy(&chain, &TextToSpeech::audio);
    chain.synthesize("Hi.");
    chain.stop();
    QTest::qWait(300);
    QCOMPARE(done.size(), 0);
    QCOMPARE(audioSpy.size(), 0);
  }

  void withApiPathStripsTrailingSlash() {
    QCOMPARE(withApiPath(QUrl("http://127.0.0.1:8880"), "/v1/models").toString(),
             QString("http://127.0.0.1:8880/v1/models"));
    QCOMPARE(withApiPath(QUrl("http://127.0.0.1:8880/"), "/v1/models").toString(),
             QString("http://127.0.0.1:8880/v1/models"));
    QCOMPARE(withApiPath(QUrl("http://127.0.0.1:8080/"), "v1/tts").toString(),
             QString("http://127.0.0.1:8080/v1/tts"));
    QCOMPARE(withApiPath(QUrl("http://127.0.0.1:8080/fish/"), "/v1/health").toString(),
             QString("http://127.0.0.1:8080/fish/v1/health"));
  }

  void qwenAndFishReachServerWhenEndpointHasTrailingSlash() {
    FakeServer qwenSrv, fishSrv;
    qwenSrv.replies["/v1/audio/speech"].body = QByteArray(800, '\x01');
    fishSrv.replies["/v1/tts"].body = audio::wav(QByteArray(600, '\x02'), 44100);
    QNetworkAccessManager net;
    QwenTts qwen(&net);
    QwenTts::Config qc;
    qc.endpoint = QUrl(qwenSrv.url("").toString() + "/"); // trailing slash
    qc.streaming = true;
    qwen.configure(qc);
    FishSpeech fish(&net);
    fish.configure(QUrl(fishSrv.url("").toString() + "/"), {}, false, {});

    QSignalSpy qDone(&qwen, &TextToSpeech::done);
    QByteArray qGot;
    connect(&qwen, &TextToSpeech::audio, this, [&](const QByteArray &b) { qGot += b; });
    qwen.synthesize("Hi.");
    QVERIFY(qDone.wait(5000));
    QCOMPARE(qwenSrv.paths.count("/v1/audio/speech"), 1);
    QVERIFY(qGot.size() >= 800);

    QSignalSpy fDone(&fish, &TextToSpeech::done);
    QByteArray fGot;
    connect(&fish, &TextToSpeech::audio, this, [&](const QByteArray &b) { fGot += b; });
    fish.synthesize("Hi.");
    QVERIFY(fDone.wait(5000));
    QCOMPARE(fishSrv.paths.count("/v1/tts"), 1);
    QCOMPARE(fGot.size(), 600);
  }

  void chainRetriesPrimaryAfterCooldown() {
    FakeServer qwenSrv, fishSrv;
    qwenSrv.replies["/v1/audio/speech"] = {500, "{}", {}};
    fishSrv.replies["/v1/tts"].body = audio::wav(QByteArray(400, '\x03'), 44100);
    QNetworkAccessManager net;
    QwenTts qwen(&net);
    QwenTts::Config qc;
    qc.endpoint = qwenSrv.url("");
    qwen.configure(qc);
    FishSpeech fish(&net);
    fish.configure(fishSrv.url(""), {}, false, {});
    TtsChain chain;
    chain.setCooldownMs(40);
    chain.setEngines({&qwen, &fish});

    QSignalSpy done1(&chain, &TextToSpeech::done);
    chain.synthesize("One.");
    QVERIFY(done1.wait(5000));
    QCOMPARE(chain.lastEngine(), QString("fish-speech"));
    QCOMPARE(chain.downEngines(), QStringList{"qwen3-tts"});
    QCOMPARE(qwenSrv.paths.count("/v1/audio/speech"), 1);

    // Primary comes back; after the cooldown the chain should try it again.
    qwenSrv.replies["/v1/audio/speech"] = {200, QByteArray(500, '\x04'), {}};
    QTest::qWait(60);
    QCOMPARE(chain.downEngines(), QStringList{});
    QSignalSpy done2(&chain, &TextToSpeech::done);
    chain.synthesize("Two.");
    QVERIFY(done2.wait(5000));
    QCOMPARE(chain.lastEngine(), QString("qwen3-tts"));
    QCOMPARE(qwenSrv.paths.count("/v1/audio/speech"), 2);
    QCOMPARE(fishSrv.paths.count("/v1/tts"), 1); // second sentence did not need fish
  }

  void voiceBrokenRecoversAfterTtsCooldown() {
    Rig rig;
    rig.a->setTtsCooldownMsForTest(40);
    rig.a->markVoiceBrokenForTest();
    QVERIFY(rig.a->voiceBrokenForTest());
    QVERIFY(rig.a->voiceBrokenNowForTest());
    // Still inside the cooldown: recovery must not clear it yet.
    rig.a->recoverVoiceForTest();
    QVERIFY(rig.a->voiceBrokenForTest());
    QTest::qWait(60);
    QVERIFY(!rig.a->voiceBrokenNowForTest());
    rig.a->recoverVoiceForTest();
    QVERIFY(!rig.a->voiceBrokenForTest());
  }

  // --- the assistant, end to end against a fake model server -------------------------
private:
  struct Rig {
    QTemporaryDir dir;
    FakeServer srv;
    std::unique_ptr<Assistant> a;
    explicit Rig(const QStringList &installed = {"big-27b:q4", "gpt-oss:20b", "qwen3:30b-a3b"}) {
      Assistant::Paths paths{dir.filePath("assistant.json"), dir.filePath("mem"), QString()};
      a = std::make_unique<Assistant>(paths, true);
      auto *s = a->settings();
      s->set("llm.endpoint", srv.url("/v1").toString());
      s->set("llm.mainModel", "big-27b");
      s->set("llm.fastModel", "gpt-oss:20b");
      s->set("llm.speedModel", "qwen3-30b-a3b");
      s->set("llm.thinking", "off");
      s->set("memory.enabled", false);
      a->setInstalledModels(installed);
      srv.chat = [](const QJsonObject &body) {
        FakeServer::Reply r;
        r.events = streamOf("Answer from " + body.value("model").toString() + ".");
        return r;
      };
      srv.nativeChat = [](const QJsonObject &body) {
        FakeServer::Reply r;
        r.events = {ModelTests::nd("Answer from " + body.value("model").toString() + "."),
                    ModelTests::ndDone()};
        return r;
      };
    }
    QString modelOf(int i) const { return srv.chatBodies.value(i).value("model").toString(); }
    bool waitForReplies(int n) {
      QSignalSpy said(a.get(), &Assistant::said);
      for (int i = 0; i < 100 && said.size() < n; ++i)
        QTest::qWait(50);
      return said.size() >= n;
    }
  };

private slots:
  void currentQuestionUsesWebEvidenceOnce() {
    Rig rig;FakeServer search;
    search.replies["/search"].body = R"({"results":[{"title":"Release notes","url":"https://nixos.org/","content":"Current release evidence"}]})";
    rig.a->settings()->set("agent.web", true);
    rig.a->settings()->set("web.provider", "searxng");
    rig.a->settings()->set("web.searxng.endpoint", search.url("/search").toString());
    rig.srv.nativeChat = [](const QJsonObject &) {
      FakeServer::Reply r;r.body = R"({"message":{"role":"assistant","content":"Current release [web:1]"},"done":true})";return r;
    };
    rig.srv.chat = [](const QJsonObject &) {
      FakeServer::Reply r;r.body = R"({"choices":[{"message":{"role":"assistant","content":"Current release [web:1]"},"finish_reason":"stop"}]})";return r;
    };
    rig.a->ask("What is the latest NixOS release?");
    QTRY_VERIFY_WITH_TIMEOUT(!rig.srv.chatBodies.isEmpty(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(rig.a->webSources().size(), 1, 3000);
    const auto body = rig.srv.chatBodies.last();
    bool sourceTool = false;
    for (auto message : body.value("messages").toArray())
      if (message.toObject().value("role") == "tool" && message.toObject().value("content").toString().contains("web:1")) sourceTool = true;
    QVERIFY(sourceTool);
    for (auto tool : body.value("tools").toArray())
      QVERIFY(tool.toObject().value("function").toObject().value("name").toString() != "web_search");
    QCOMPARE(search.paths.size(), 1);
    QJsonObject repeated;
    rig.a->callTool("web.search", {{"query", "NixOS release again"}}, [&](auto r) { repeated = r; });
    QVERIFY(!repeated["ok"].toBool());QCOMPARE(search.paths.size(), 1);QVERIFY(rig.a->question().isEmpty());
  }
  void routesEachRequestToTheRightModel() {
    Rig rig;
    QSignalSpy said(rig.a.get(), &Assistant::said);
    rig.a->ask("tell me a joke");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 1, 5000);
    QCOMPARE(rig.modelOf(0), QString("gpt-oss:20b"));
    QCOMPARE(rig.a->lastPick().role, modelrouter::Role::Fast);
    rig.a->ask("explain how nix flakes work in detail");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 2, 5000);
    QCOMPARE(rig.modelOf(1), QString("big-27b:q4"));
    QVERIFY(said.last().first().toString().contains("Answer from big-27b:q4"));
  }

  void fallsBackWhenTheMainModelIsMissing() {
    Rig rig({"gpt-oss:20b", "qwen3:30b-a3b"});
    QSignalSpy said(rig.a.get(), &Assistant::said);
    rig.a->ask("explain how nix flakes work");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 1, 5000);
    QCOMPARE(rig.modelOf(0), QString("gpt-oss:20b"));
    QVERIFY(rig.a->lastPick().fellBack);
    QVERIFY(rig.a->lastPick().reason.contains("not installed"));
  }

  void usesAnyLocalModelAsALastResortButNeverInventsOne() {
    Rig rig({"llama3:8b"});
    QSignalSpy said(rig.a.get(), &Assistant::said);
    rig.a->ask("tell me a joke");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 1, 5000);
    QCOMPARE(rig.modelOf(0), QString("llama3:8b"));
    // ...unless the user does not want that.
    Rig strict({"llama3:8b"});
    strict.a->settings()->set("llm.useAnyLocalModel", false);
    QVERIFY(strict.a->pickModelFor("tell me a joke").model.isEmpty() ||
            strict.a->pickModelFor("tell me a joke").model == "gpt-oss:20b");
  }

  void retriesOnAnotherLocalModelWhenOneFails() {
    Rig rig;
    rig.srv.chat = [](const QJsonObject &body) {
      FakeServer::Reply r;
      if (body.value("model").toString() == "big-27b:q4") {
        r.status = 500;
        r.body = "{\"error\":{\"message\":\"CUDA error: out of memory\"}}";
      } else {
        r.events = streamOf("Handled by " + body.value("model").toString() + ".");
      }
      return r;
    };
    QSignalSpy said(rig.a.get(), &Assistant::said);
    rig.a->ask("explain how nix flakes work in detail");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 1, 8000);
    QCOMPARE(rig.srv.chatBodies.size(), 2);
    QCOMPARE(rig.modelOf(0), QString("big-27b:q4"));
    QCOMPARE(rig.modelOf(1), QString("gpt-oss:20b"));
    QVERIFY(said.first().first().toString().contains("Handled by gpt-oss:20b"));
  }

  void neverFallsBackToAnythingNonLocal() {
    Rig rig;
    rig.srv.chat = [](const QJsonObject &) {
      FakeServer::Reply r;
      r.status = 500;
      r.body = "{\"error\":{\"message\":\"broken\"}}";
      return r;
    };
    QSignalSpy said(rig.a.get(), &Assistant::said);
    rig.a->ask("tell me a joke");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 1, 10000);
    // Every local model was tried once, then a plain apology -- no fourth try.
    QVERIFY(rig.srv.chatBodies.size() <= 3);
    QVERIFY(said.first().first().toString().contains("can't reach my brain"));
  }

  void modelSwitchingByVoice() {
    Rig rig;
    QSignalSpy said(rig.a.get(), &Assistant::said);
    // "For this": the next request only, defaults untouched.
    rig.a->ask("Nala, use the main model for this");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 1, 3000);
    QCOMPARE(rig.a->settings()->string("llm.mode"), QString("auto"));
    rig.a->ask("tell me a joke");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 2, 5000);
    QCOMPARE(rig.modelOf(0), QString("big-27b:q4"));
    rig.a->ask("tell me another joke");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 3, 5000);
    QCOMPARE(rig.modelOf(1), QString("gpt-oss:20b")); // back to automatic

    // "From now on": a standing preference.
    rig.a->ask("from now on use the fast model");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 4, 3000);
    QCOMPARE(rig.a->settings()->string("llm.mode"), QString("fast"));
    rig.a->ask("explain how nix flakes work in detail");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 5, 5000);
    QCOMPARE(rig.modelOf(2), QString("gpt-oss:20b"));
    rig.a->ask("switch back to automatic model selection");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 6, 3000);
    QCOMPARE(rig.a->settings()->string("llm.mode"), QString("auto"));
    QCOMPARE(rig.srv.chatBodies.size(), 3); // phrases never reached a model
  }

  void routingCanBeTurnedOff() {
    Rig rig;
    rig.a->settings()->set("llm.autoRouting", false);
    QSignalSpy said(rig.a.get(), &Assistant::said);
    rig.a->ask("tell me a joke");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 1, 5000);
    QCOMPARE(rig.modelOf(0), QString("big-27b:q4"));
  }

  void thinkingFollowsTheRequestInAutoMode() {
    Rig rig;
    rig.a->settings()->set("llm.thinking", "auto");
    QSignalSpy said(rig.a.get(), &Assistant::said);
    rig.srv.replies["/api/version"].body = "{\"version\":\"0.34.3\"}";
    rig.a->ask("tell me a joke");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 1, 5000);
    rig.a->ask("explain how nix flakes work in detail");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 2, 5000);
    // Ollama, native API: gpt-oss (the fast model) takes a level and cannot be
    // switched off, so it gets its floor; the main model on a hard question
    // thinks, and nothing about the reasoning reaches the answer.
    QCOMPARE(rig.srv.chatBodies.at(0).value("think").toString(), QString("low"));
    QVERIFY(rig.srv.chatBodies.at(1).value("think").toBool(false));
    QCOMPARE(rig.srv.chatBodies.at(1).value("options").toObject().value("num_ctx").toInt(), 16384);
  }

  void speaksTheFirstSentenceWithoutWaitingForTheRest() {
    Rig rig;
    rig.a->setVoiceForTest(true);
    rig.srv.chat = [](const QJsonObject &) {
      FakeServer::Reply r;
      r.events = streamOf("<think>plan the reply</think>Sure. I can open Firefox for you. "
                          "Would you also like me to restore your previous tabs?", 5);
      return r;
    };
    QSignalSpy said(rig.a.get(), &Assistant::said);
    rig.a->ask("tell me a joke");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 1, 8000);
    QTRY_COMPARE_WITH_TIMEOUT(rig.a->spokenForTest().size(), 2, 3000);
    QCOMPARE(rig.a->spokenForTest().at(0), QString("Sure. I can open Firefox for you."));
    QCOMPARE(rig.a->spokenForTest().at(1),
             QString("Would you also like me to restore your previous tabs?"));
    for (const QString &s : rig.a->spokenForTest())
      QVERIFY(!s.contains("plan the reply")); // reasoning never reaches the voice
    QVERIFY(!said.first().first().toString().contains("think"));
    QCOMPARE(rig.a->history().last().toObject().value("content").toString(),
             QString("Sure. I can open Firefox for you. Would you also like me to "
                     "restore your previous tabs?"));
  }

  void keepsTheHistoryShort() {
    Rig rig;
    rig.a->settings()->set("llm.contextTurns", 2);
    QSignalSpy said(rig.a.get(), &Assistant::said);
    for (int i = 0; i < 8; ++i) {
      rig.a->ask(QString("tell me joke number %1").arg(i));
      QTRY_COMPARE_WITH_TIMEOUT(said.size(), i + 1, 5000);
    }
    QVERIFY2(rig.a->history().size() <= 2 * 2 + 4, "history was not compacted");
    // The prompt of the last request carried no more than the recent turns.
    const QJsonArray sent = rig.srv.chatBodies.last().value("messages").toArray();
    QVERIFY(sent.size() <= 1 + 2 * 2 + 1 + 1);
  }

  void timesEachStage() {
    Rig rig;
    QSignalSpy said(rig.a.get(), &Assistant::said);
    rig.a->ask("tell me a joke");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 1, 5000);
    const QString report = rig.a->latencyReport();
    QVERIFY2(report.contains("LLM first token"), qPrintable(report));
    QVERIFY(report.contains("Total perceived latency"));
  }

  void aCommandNeverTouchesTheModel() {
    Rig rig;
    QSignalSpy said(rig.a.get(), &Assistant::said);
    rig.a->ask("what time is it");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 1, 3000);
    QVERIFY(said.first().first().toString().startsWith("It's "));
    QCOMPARE(rig.srv.chatBodies.size(), 0);
    QVERIFY(rig.a->latencyReport().contains("Command execution"));
  }

  void statusShowsWhatIsAvailable() {
    Rig rig({"gpt-oss:20b", "qwen3:30b-a3b"});
    rig.srv.replies["/api/version"].body = "{\"version\":\"0.34.3\"}";
    rig.srv.replies["/api/tags"].body =
        "{\"models\":[{\"name\":\"gpt-oss:20b\",\"size\":13800000000},"
        "{\"name\":\"qwen3:30b-a3b\",\"size\":18600000000}]}";
    rig.srv.replies["/api/ps"].body =
        "{\"models\":[{\"name\":\"gpt-oss:20b\",\"size\":14000000000,\"size_vram\":14000000000}]}";
    QString text;
    bool done = false;
    rig.a->modelCommand("status", [&](QString t) { text = t; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QVERIFY2(text.contains("Ollama 0.34.3"), qPrintable(text));
    QVERIFY(text.contains("Main model:"));
    QVERIFY2(text.contains("available: NO"), qPrintable(text)); // the 27B is not installed
    QVERIFY(text.contains("Fast model:"));
    QVERIFY(text.contains("loaded: yes"));
    QVERIFY(text.contains("Routing: automatic"));
    QVERIFY(text.contains("Speech recognition (STT)"));
    QVERIFY(text.contains("Voice (TTS)"));
  }

  void modelCommandsChangeSettings() {
    Rig rig;
    QString out;
    bool done = false;
    const auto run = [&](const QString &cmd) {
      done = false;
      rig.a->modelCommand(cmd, [&](QString t) { out = t; done = true; });
      QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    };
    run("mode fast");
    QCOMPARE(rig.a->settings()->string("llm.mode"), QString("fast"));
    run("mode nonsense");
    QVERIFY(out.contains("usage"));
    QCOMPARE(rig.a->settings()->string("llm.mode"), QString("fast"));
    run("speed qwen3:30b-a3b");
    QCOMPARE(rig.a->settings()->string("llm.speedModel"), QString("qwen3:30b-a3b"));
    run("thinking on");
    QCOMPARE(rig.a->settings()->string("llm.thinking"), QString("on"));
    run("thinking sometimes");
    QVERIFY(out.contains("usage"));
    run("routing off");
    QVERIFY(!rig.a->settings()->flag("llm.autoRouting"));
    run("bogus");
    QVERIFY(out.contains("usage"));
  }

  void startsWithNoServerAtAll() {
    QTemporaryDir dir;
    Assistant::Paths paths{dir.filePath("assistant.json"), dir.filePath("mem"), QString()};
    Assistant a(paths, true);
    a.settings()->set("llm.endpoint", "http://127.0.0.1:1/v1");
    QSignalSpy said(&a, &Assistant::said);
    a.ask("tell me a joke");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 1, 10000);
    QVERIFY(said.first().first().toString().contains("can't reach my brain"));
    QString out;
    bool done = false;
    a.modelCommand("status", [&](QString t) { out = t; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QVERIFY(out.contains("not reachable"));
    QVERIFY(out.contains("available: unknown"));
  }

  void startsWithAServerThatHasNoModels() {
    FakeServer srv;
    srv.replies["/v1/models"].body = "{\"data\":[]}";
    QTemporaryDir dir;
    Assistant::Paths paths{dir.filePath("assistant.json"), dir.filePath("mem"), QString()};
    Assistant a(paths, true);
    a.settings()->set("llm.endpoint", srv.url("/v1").toString());
    QSignalSpy said(&a, &Assistant::said);
    a.ask("tell me a joke");
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 1, 10000);
    QVERIFY2(said.first().first().toString().contains("no models"),
             qPrintable(said.first().first().toString())); // says why, does not crash
    QString out;
    bool done = false;
    a.modelCommand("status", [&](QString t) { out = t; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QVERIFY(out.contains("Main model:"));
  }

  // Oneshot mode is what `nala doctor` uses when she is not already running:
  // real settings/wake, no microphone, diagnose only when asked.
  void oneshotDiagnoseReportsVersionWithoutOpeningMic() {
    FakeServer llm;
    llm.replies["/v1/models"].body =
        R"({"data":[{"id":"big-27b:q4"},{"id":"gpt-oss:20b"},{"id":"qwen3:30b-a3b"}]})";
    FakeServer stt;
    stt.replies["/"].status = 200;
    stt.replies["/"].body = "ok";
    QTemporaryDir dir;
    Assistant::Paths paths{dir.filePath("assistant.json"), dir.filePath("mem"), QString()};
    Assistant a(paths, Assistant::Mode::Oneshot);
    QVERIFY(!a.micOpen());
    a.settings()->set("llm.endpoint", llm.url("/v1").toString());
    a.settings()->set("llm.mainModel", "big-27b");
    a.settings()->set("llm.fastModel", "gpt-oss:20b");
    a.settings()->set("llm.speedModel", "qwen3:30b-a3b");
    a.settings()->set("stt.serverUrl", stt.url("/").toString());
    a.settings()->set("stt.activation", "push"); // wake optional
    a.settings()->set("tts.engine", "none");
    a.settings()->set("memory.enabled", false);
    QString report;
    bool done = false;
    a.diagnose([&](QString t) { report = t; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QVERIFY2(report.startsWith(QStringLiteral("nala ") + QStringLiteral(NALA_VERSION)),
             qPrintable(report.left(80)));
    QVERIFY(report.contains("language model server"));
    QVERIFY(report.contains("main model"));
    QVERIFY(report.contains("whisper-server"));
    QVERIFY(!a.micOpen()); // oneshot must never open the mic
    // Do not assert !diagnoseHasFail here: CI sandboxes lack mic/speakers/
    // Hyprland, which are required lines and correctly print FAIL.
  }

  // Scripting contract: required FAIL lines make diagnoseHasFail true;
  // optional "--" lines (voices, missing roles) do not.
  void diagnoseHasFailDetectsRequiredFailuresOnly() {
    QVERIFY(!Assistant::diagnoseHasFail(QStringLiteral("nala 1.3.10\nok   mic: yes\n")));
    QVERIFY(Assistant::diagnoseHasFail(
        QStringLiteral("nala 1.3.10\nFAIL language model server: unreachable\n")));
    QVERIFY(Assistant::diagnoseHasFail(
        QStringLiteral("ok   speakers: yes\nFAIL Hyprland: not running\n")));
    // Optional / soft misses print as "--", not FAIL.
    QVERIFY(!Assistant::diagnoseHasFail(
        QStringLiteral("nala 1.3.10\n--   Qwen3-TTS: Connection refused\n"
                       "--   fast model: not installed\n")));
    // A word "FAIL" in detail must not trip the check (prefix only).
    QVERIFY(!Assistant::diagnoseHasFail(
        QStringLiteral("ok   note: said FAIL in detail\n")));
  }

  // Unreachable LLM → required FAIL on language model server (oneshot).
  void oneshotDiagnoseFailsWhenLanguageServerDown() {
    FakeServer stt;
    stt.replies["/"].status = 200;
    stt.replies["/"].body = "ok";
    QTemporaryDir dir;
    Assistant::Paths paths{dir.filePath("assistant.json"), dir.filePath("mem"), QString()};
    Assistant a(paths, Assistant::Mode::Oneshot);
    a.settings()->set("llm.endpoint", QStringLiteral("http://127.0.0.1:1/v1"));
    a.settings()->set("stt.serverUrl", stt.url("/").toString());
    a.settings()->set("stt.activation", "push");
    a.settings()->set("tts.engine", "none");
    a.settings()->set("memory.enabled", false);
    QString report;
    bool done = false;
    a.diagnose([&](QString t) { report = t; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QVERIFY2(report.contains(QStringLiteral("FAIL")), qPrintable(report.left(400)));
    QVERIFY(report.contains("language model server"));
    QVERIFY(Assistant::diagnoseHasFail(report));
    QVERIFY(!a.micOpen());
  }

  // Oneshot must never arm screen capture, even when memory.enabled is on in
  // the settings file (doctor / memory CLI without a window).
  void oneshotNeverArmsScreenCaptureWhenMemoryEnabled() {
    QTemporaryDir dir;
    {
      AssistantSettings seed(dir.filePath("assistant.json"), true);
      QVERIFY(seed.set("memory.enabled", true));
      QVERIFY(seed.set("memory.paused", false));
      seed.flush();
    }
    Assistant::Paths paths{dir.filePath("assistant.json"), dir.filePath("mem"), QString()};
    Assistant a(paths, Assistant::Mode::Oneshot);
    QCOMPARE(a.settings()->flag("memory.enabled"), true);
    QVERIFY(a.memory()->offlineForTest());
    QVERIFY(!a.memory()->captureActiveForTest());
    QVERIFY(!a.micOpen());
    // Status still reflects the configured intent (recording when she is live).
    QCOMPARE(a.memory()->status(), QString("recording"));
  }

  // `nala memory …` oneshot: pause/resume/status without a companion, and the
  // pause survives a fresh oneshot load (settings flush).
  void oneshotMemoryCommandPauseStatusResume() {
    QTemporaryDir dir;
    Assistant::Paths paths{dir.filePath("assistant.json"), dir.filePath("mem"), QString()};
    {
      Assistant a(paths, Assistant::Mode::Oneshot);
      QVERIFY(a.memory()->offlineForTest());
      QVERIFY(!a.memory()->captureActiveForTest());
      const QString status = a.memoryCommand("status");
      QVERIFY2(status.startsWith("screen memory off"), qPrintable(status));
      // Pause while disabled: status must still say paused (killswitch wins).
      const QString pausedOff = a.memoryCommand("pause 60");
      QVERIFY2(pausedOff.contains("paused"), qPrintable(pausedOff));
      QVERIFY(!pausedOff.contains("off"));
      QVERIFY(a.memory()->paused());
      QVERIFY(!a.memory()->enabled());
      QVERIFY(!a.memory()->captureActiveForTest()); // oneshot stays offline
      QCOMPARE(a.memoryCommand("bogus"),
               QString("usage: nala memory pause [minutes] | resume | status | clear screen [all]"));
      QCOMPARE(a.memoryCommand("clear junk"),
               QString("usage: nala memory clear screen [all]   (screen history only; "
                       "notes stay, and pinned memories stay unless you say \"all\")"));
      const QString cleared = a.memoryCommand("clear screen");
      QVERIFY2(cleared.startsWith("Forgot "), qPrintable(cleared));
    }
    // Fresh oneshot must still see the pause written to disk (even while off).
    {
      Assistant b(paths, Assistant::Mode::Oneshot);
      QVERIFY(b.memory()->paused());
      QVERIFY(!b.memory()->enabled());
      QVERIFY(b.memoryCommand(QString()).contains("paused"));
      QVERIFY(!b.memoryCommand("status").contains("off"));
      const QString resumed = b.memoryCommand("resume");
      QVERIFY2(resumed.contains("off") || resumed.contains("recording"),
               qPrintable(resumed));
      QVERIFY(!b.memory()->paused());
    }
  }

  void badToolArgumentsNeverRunAnything() {
    Rig rig;
    const auto call = [&](const QString &tool, const QJsonObject &args) {
      QJsonObject result;
      bool done = false;
      rig.a->callTool(tool, args, [&](QJsonObject r) { result = r; done = true; });
      for (int i = 0; i < 60 && !done; ++i)
        QTest::qWait(50);
      if (!done)
        result = QJsonObject{{"error", "never answered"}};
      return result;
    };
    // Out of range, wrong type, missing, unknown value: all refused by the schema.
    QVERIFY(call("volume.set", {{"level", 150}}).value("error").toString().contains("Invalid"));
    QVERIFY(call("volume.set", {{"level", "loud"}}).value("error").toString().contains("Invalid"));
    QVERIFY(call("volume.set", {}).value("error").toString().contains("Invalid"));
    QVERIFY(call("media.control", {{"action", "eject; reboot"}}).value("error").toString().contains("Invalid"));
    QVERIFY(call("volume.mute", {{"mode", "explode"}}).value("error").toString().contains("Invalid"));
    // A command outside the allow-list is refused with the reason, and not run.
    const QJsonObject refused = call("system.run_safe", {{"command", "rm -rf"}});
    QVERIFY(!refused.value("ok").toBool());
    QVERIFY(refused.value("error").toString().contains("not on the list"));
    const QJsonObject withPath = call("system.run_safe", {{"command", "rm -rf /"}});
    QVERIFY(!withPath.value("ok").toBool());
    QVERIFY(withPath.value("error").toString().contains("no paths"));
    QVERIFY(!call("system.run_safe", {{"command", "uname | sh"}}).value("ok").toBool());
    // Unknown tool.
    QVERIFY(call("volume.explode", {}).value("error").toString().contains("no tool"));
  }

  void recordingAsksBeforeItStarts() {
    Rig rig;
    QJsonObject result;
    bool done = false;
    QSignalSpy asked(rig.a.get(), &Assistant::questionChanged);
    rig.a->callTool("record.start", {}, [&](QJsonObject r) { result = r; done = true; });
    QVERIFY(!rig.a->question().isEmpty());          // it asked, and nothing ran yet
    QVERIFY(rig.a->question().contains("recording"));
    QVERIFY(!done);
    rig.a->answer(false);
    QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
    QVERIFY(!result.value("ok").toBool());
  }

  void followsOllamaHostFromTheEnvironment() {
    QCOMPARE(catalog::endpointFromOllamaHost("127.0.0.1:11435"), QString("http://127.0.0.1:11435/v1"));
    QCOMPARE(catalog::endpointFromOllamaHost(":11435"), QString("http://127.0.0.1:11435/v1"));
    QCOMPARE(catalog::endpointFromOllamaHost("0.0.0.0"), QString("http://127.0.0.1:11434/v1"));
    QCOMPARE(catalog::endpointFromOllamaHost("http://box:11434/"), QString("http://box:11434/v1"));
    QCOMPARE(catalog::endpointFromOllamaHost("myhost"), QString("http://myhost:11434/v1"));
    QVERIFY(catalog::endpointFromOllamaHost("").isEmpty());
    QVERIFY(catalog::endpointFromOllamaHost("host:notaport").isEmpty());
    QVERIFY(catalog::endpointFromOllamaHost("bad host;rm").isEmpty());
  }

  void clearsScreenHistoryOnly() {
    Rig rig;
    const QString text = rig.a->clearScreenMemory(false);
    QVERIFY(text.contains("Forgot 0"));
  }

private:
public slots:
  void unusedToKeepMocHappy() {}

private slots:
  // --- screen history retention -------------------------------------------------
  void retentionNamesMapToMinutes() {
    QCOMPARE(ScreenMemory::retentionMinutes("1h"), 60);
    QCOMPARE(ScreenMemory::retentionMinutes("1d"), 1440);
    QCOMPARE(ScreenMemory::retentionMinutes("7d"), 7 * 1440);
    QCOMPARE(ScreenMemory::retentionMinutes("30d"), 30 * 1440);
    QCOMPARE(ScreenMemory::retentionMinutes("off"), 1);
    QCOMPARE(ScreenMemory::retentionMinutes("manual"), 0);
    QCOMPARE(ScreenMemory::retentionMinutes("custom"), 0);
  }

  void retentionCanBeFinerThanADay() {
    QTemporaryDir dir;
    MemoryStore store;
    QVERIFY(store.open(dir.path()));
    const QDateTime now = QDateTime::currentDateTime();
    const auto shot = [&](int minutesAgo) {
      const QString path = store.shotsDir() + QStringLiteral("/%1.jpg").arg(minutesAgo);
      QFile f(path);
      if (!f.open(QIODevice::WriteOnly))
        return qint64(0);
      f.write(QByteArray(1024, 'j'));
      f.close();
      MemoryRecord r;
      r.started = r.lastSeen = now.addSecs(-60 * minutesAgo);
      r.app = "a";
      r.shotPath = path;
      r.shotBytes = 1024;
      return store.insert(r);
    };
    const qint64 old = shot(90), fresh = shot(10);
    const auto sweep = store.enforce(7, 0, 0, now, 60); // "1h" beats the 7 days
    QCOMPARE(sweep.screenshots, 1);
    QVERIFY(store.get(old).shotPath.isEmpty());
    QVERIFY(!store.get(fresh).shotPath.isEmpty());
  }

  void clearingScreenMemoryLeavesNotesAndPins() {
    QTemporaryDir dir;
    MemoryStore store;
    QVERIFY(store.open(dir.path()));
    const QDateTime now = QDateTime::currentDateTime();
    const auto add = [&](const QString &source, bool pinned) {
      MemoryRecord r;
      r.started = r.lastSeen = now;
      r.app = "a";
      r.source = source;
      r.pinned = pinned;
      return store.insert(r);
    };
    add("screen", false);
    add("screen", false);
    const qint64 pinned = add("screen", true);
    const qint64 note = add("note", false);
    QCOMPARE(store.countSource("screen"), 3);
    QCOMPARE(store.forgetSource("screen"), 2);
    QVERIFY(store.get(pinned).id != 0);   // kept, and the caller can say so
    QVERIFY(store.get(note).id != 0);     // notes are not screen history
    QCOMPARE(store.countSource("screen", true), 1);
    QCOMPARE(store.forgetSource("screen", true), 1);
    QCOMPARE(store.countSource("screen"), 0);
    QCOMPARE(store.count(), 1);
  }

  // --- catalogue and GPU -----------------------------------------------------
  void parsesOllamaTagsAndPs() {
    const QJsonObject tags = QJsonDocument::fromJson(
        "{\"models\":[{\"name\":\"gpt-oss:20b\",\"size\":13800000000,\n"
"            \"details\":{\"parameter_size\":\"20.9B\",\"quantization_level\":\"MXFP4\"}},\n"
"            {\"name\":\"qwen3:30b-a3b\",\"size\":18600000000,\"details\":{}}]}").object();
    const auto installed = catalog::parseTags(tags);
    QCOMPARE(installed.size(), 2);
    QCOMPARE(installed.first().name, QString("gpt-oss:20b"));
    QCOMPARE(installed.first().quantization, QString("MXFP4"));

    const QJsonObject ps = QJsonDocument::fromJson(
        "{\"models\":[{\"name\":\"qwen3:30b-a3b\",\"size\":20000000000,\n"
"            \"size_vram\":15000000000,\"expires_at\":\"2026-09-29T00:00:00Z\"}]}").object();
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
