#pragma once
#include "commandrouter.h"
#include "desktop.h"
#include "identity.h"
#include "llm.h"
#include "wakeword.h"
#include "memory.h"
#include "tools.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QQueue>
#include <QTimer>
#include <QVariantList>
#include <functional>
#include <memory>

class AssistantSettings;
class EventLog;
class FishSpeech;
class Microphone;
class ScreenMemory;
class Speaker;
class SpeechToText;
class WhisperCli;
class WhisperServer;

// Nala's assistant: ears, voice, judgement and hands, and the state that ties
// them to how she looks.
//
//   microphone -> VAD -> whisper.cpp -> router --fast--> action
//                                          \--else--> model <-> tools
//                                                        |
//                                          speech bubble + Fish Speech
//
// The companion does not depend on any of it. Every backend is optional and
// reports itself missing rather than failing; she carries on being a desktop
// pet whatever is or is not installed.
class Assistant : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString state READ state NOTIFY stateChanged)
  Q_PROPERTY(QString bubble READ bubble NOTIFY bubbleChanged)
  Q_PROPERTY(QString heard READ heard NOTIFY bubbleChanged)
  Q_PROPERTY(QString question READ question NOTIFY questionChanged)
  Q_PROPERTY(bool listening READ listening NOTIFY stateChanged)
  Q_PROPERTY(bool micOpen READ micOpen NOTIFY stateChanged)
  Q_PROPERTY(double micLevel READ micLevel NOTIFY levelsChanged)
  Q_PROPERTY(double voiceLevel READ voiceLevel NOTIFY levelsChanged)
  Q_PROPERTY(QVariantList setup READ setup NOTIFY setupChanged)
  Q_PROPERTY(QString model READ model NOTIFY setupChanged)
  Q_PROPERTY(QStringList microphones READ microphones NOTIFY setupChanged)
  Q_PROPERTY(QStringList speakers READ speakers NOTIFY setupChanged)
  Q_PROPERTY(QStringList capabilities READ capabilities NOTIFY setupChanged)
  Q_PROPERTY(bool vision READ visionEnabled NOTIFY setupChanged)

  // Who she is.
  Q_PROPERTY(QString assistantName READ assistantName NOTIFY identityChanged)
  Q_PROPERTY(bool firstRun READ firstRun NOTIFY identityChanged)
  // What the microphone is doing, for the indicator on her: "off", "wake"
  // (only the wake-word detector hears it; nothing is transcribed), "command"
  // (a request is being taken down), "open" (everything is transcribed), or
  // "recording" (a training sample).
  Q_PROPERTY(QString micState READ micState NOTIFY stateChanged)

  // The wake word.
  Q_PROPERTY(bool wakeReady READ wakeReady NOTIFY wakeChanged)
  Q_PROPERTY(QString wakeStatus READ wakeStatus NOTIFY wakeChanged)
  Q_PROPERTY(bool wakeModelsPresent READ wakeModelsPresent NOTIFY wakeChanged)
  Q_PROPERTY(QVariantList wakePhrases READ wakePhraseList NOTIFY wakeChanged)
  Q_PROPERTY(double wakeScore READ wakeScore NOTIFY wakeScored)
  Q_PROPERTY(double wakeThreshold READ wakeThreshold NOTIFY wakeScored)
  Q_PROPERTY(bool wakeTesting READ wakeTesting NOTIFY wakeChanged)

  // Teaching her a phrase.
  Q_PROPERTY(QString trainingPhrase READ trainingPhrase NOTIFY trainingChanged)
  Q_PROPERTY(int trainingSamples READ trainingSamples NOTIFY trainingChanged)
  Q_PROPERTY(bool trainingSpeech READ trainingSpeech NOTIFY trainingChanged)
  Q_PROPERTY(bool trainingBusy READ trainingBusy NOTIFY trainingChanged)
  Q_PROPERTY(QString recordingKind READ recordingKind NOTIFY trainingChanged)

public:
  struct Paths {
    QString settings;  // assistant.json
    QString memoryDir; // memory.db and shots/
    QString log;       // assistant.log
  };

  Assistant(const Paths &paths, bool testing, QObject *parent = nullptr);
  ~Assistant() override;

  AssistantSettings *settings() const { return m_settings; }
  EventLog *log() const { return m_log; }
  ScreenMemory *memory() const { return m_memory; }
  MemoryStore *store() const { return m_store.get(); }
  const ToolRegistry &tools() const { return m_tools; }
  desktop::AppIndex &apps() { return m_apps; }

  QString state() const { return m_state; }
  QString bubble() const { return m_bubble; }
  QString heard() const { return m_heard; }
  QString question() const { return m_question; }
  bool listening() const { return m_state == QLatin1String("listening"); }
  bool micOpen() const;
  double micLevel() const { return m_micLevel; }
  double voiceLevel() const { return m_voiceLevel; }
  QVariantList setup() const { return m_setup; }
  QString model() const;
  QStringList microphones() const;
  QStringList speakers() const;
  QStringList capabilities() const { return m_llm->capabilities(); }
  bool visionEnabled() const;

  const Identity &identity() const { return m_identity; }
  QString assistantName() const { return m_identity.name; }
  bool firstRun() const;
  QString micState() const;

  bool wakeReady() const { return m_wakeReady; }
  QString wakeStatus() const { return m_wakeStatus; }
  bool wakeModelsPresent() const;
  QVariantList wakePhraseList() const;
  double wakeScore() const { return m_wakeScore; }
  double wakeThreshold() const { return m_wakeThreshold; }
  bool wakeTesting() const { return m_wakeTest.isActive(); }

  QString trainingPhrase() const { return m_training.phrase; }
  int trainingSamples() const { return int(m_training.positives.size()); }
  bool trainingSpeech() const { return !m_training.speech.isEmpty(); }
  bool trainingBusy() const { return m_training.busy; }
  QString recordingKind() const { return m_training.recording; }

  // Wake-word setup, for Preferences and the first-run wizard.
  Q_INVOKABLE void installWakeModels();
  Q_INVOKABLE void startTraining(const QString &phrase);
  // "phrase": one utterance of it; "speech": ordinary talk, until
  // stopRecording(); "noise": a few seconds of the room.
  Q_INVOKABLE void recordSample(const QString &kind);
  Q_INVOKABLE void stopRecording();
  Q_INVOKABLE void finishTraining();
  Q_INVOKABLE void cancelTraining();
  // Train again from the recordings kept for `phrase`.
  Q_INVOKABLE void retrain(const QString &phrase);
  Q_INVOKABLE bool deleteWakeData(const QString &phrase);
  Q_INVOKABLE int deleteAllWakeData();
  Q_INVOKABLE void setPhraseEnabled(const QString &phrase, bool enabled);
  // For half a minute detections only light up the meter; nothing happens.
  Q_INVOKABLE void testWake();
  Q_INVOKABLE void startMicTest();
  Q_INVOKABLE void stopMicTest();
  Q_INVOKABLE void testVoice();
  Q_INVOKABLE void finishSetup();

  // Profiles: identity, wake phrases, voice and interface; never secrets,
  // memories or recordings. Return an error, or empty on success.
  Q_INVOKABLE QString exportProfile(const QString &path) const;
  Q_INVOKABLE QString importProfile(const QString &path);

  // Push to talk: open the microphone for one utterance, or close it if it is
  // already open. Bound to a key through `nala listen`.
  Q_INVOKABLE void toggleListening();
  Q_INVOKABLE void startListening();
  Q_INVOKABLE void stopListening();
  // As if it had been heard. `nala ask …`, the tests, and the chat field.
  Q_INVOKABLE void ask(const QString &text);
  // Stop talking, stop thinking, stop acting, and say no to any open question.
  Q_INVOKABLE void stop();
  // Answer the open question.
  Q_INVOKABLE void answer(bool yes);
  Q_INVOKABLE void dismissBubble();

  // Health of every backend, refreshed in the background. `done` gets a
  // human-readable report.
  Q_INVOKABLE void diagnose();
  void diagnose(std::function<void(QString)> done);

  // Memory timeline and its controls, for the QML pages.
  Q_INVOKABLE QVariantList timeline(const QString &text, const QString &app,
                                    int days) const;
  Q_INVOKABLE QVariantMap memoryDetail(qint64 id) const;
  Q_INVOKABLE QStringList memoryApps() const;
  Q_INVOKABLE bool pinMemory(qint64 id, bool pinned);
  Q_INVOKABLE bool forgetMemory(qint64 id);
  Q_INVOKABLE int forgetMinutes(int minutes);
  Q_INVOKABLE QVariantList searchMemory(const QString &query) const;
  Q_INVOKABLE QString formatBytes(qint64 bytes) const;

  // The companion's side of things, set by main().
  void setCompanion(std::function<void(const QString &)> command) {
    m_companion = std::move(command);
  }

  // Test seams.
  void setSpeechBackend(SpeechToText *stt);
  // Use `backend` for wake words instead of the neural one (tests).
  void setWakeBackend(wake::WakeWordBackend *backend, bool ready = true);
  // Audio as if from the microphone: frames for the detector, utterances as
  // the voice-activity detector would cut them.
  void hearAudio(const QVector<int16_t> &samples16k);
  void hearUtterance(const QByteArray &pcm16k) { onUtterance(pcm16k); }
  // Developer mode: play a recording into the microphone path in real time,
  // as if it had been said aloud. Returns an error, or empty.
  QString hearFile(const QString &path);
  bool armed() const { return m_armed; }
  bool followingUp() const { return m_followUp.isActive(); }
  bool speakingForTest() const { return m_speaking; }
  void setSpeakingForTest(bool speaking);
  void hearForTest(const QString &transcript) { onTranscript(transcript, 0); }
  QJsonArray history() const { return m_history; }
  // Run a tool exactly as the model would, permission checks included.
  void callTool(const QString &name, const QJsonObject &args,
                std::function<void(QJsonObject)> done);
  // The reply the model produced, fed back in, for driving the loop in tests.
  void injectModelReply(const LlmReply &reply);

signals:
  void stateChanged();
  void bubbleChanged();
  void questionChanged();
  void levelsChanged();
  void setupChanged();
  void settingsRequested();
  void settingsCloseRequested();
  void timelineRequested();
  // Something she said, for the log window and tests.
  void said(const QString &text);
  void identityChanged();
  // Heard her wake phrase: react now, before anything else happens.
  void wakeDetected(const QString &phrase, double score);
  void wakeChanged();
  void wakeScored();
  void trainingChanged();
  void trainingSample(const QString &kind, int index, bool ok,
                      const QString &message);
  void trainingFinished(bool ok, const QString &message);
  void wakeModelsInstalled(bool ok, const QString &message);
  void setupRequested();

private:
  void setState(const QString &state);
  void settle();
  void applySettings(const QString &key = {});
  void registerTools();

  // Hearing.
  void onUtterance(const QByteArray &pcm16k);
  void onFrames(const QVector<int16_t> &samples);
  void onWake(const wake::Detection &detection);
  bool wakeMode() const; // the neural detector decides who is addressed
  void reloadWake();
  void arm(int ms);
  void disarm();
  void chime();
  void trainOn(const QString &phrase, const QVector<wake::Clip> &positives,
               const QVector<wake::Clip> &negatives);
  void onTranscript(const QString &text, qint64 ms);
  bool continuous() const;
  void openMicrophone();

  // Deciding.
  void handle(const QString &text, bool spoken);
  void runFast(const Route &route);
  void think(const QString &text);
  void onModelReply(const LlmReply &reply);
  void runToolCalls();
  QJsonObject systemMessage() const;
  QSet<QString> categories() const;

  // Asking first.
  void confirm(const QString &question, std::function<void(bool)> then);

  // Speaking.
  void say(const QString &text, bool speak = true);
  void speakNext();
  void finishSpeaking();

  AssistantSettings *m_settings = nullptr;
  EventLog *m_log = nullptr;
  std::unique_ptr<MemoryStore> m_store;
  ScreenMemory *m_memory = nullptr;
  QNetworkAccessManager m_network;
  LlmClient *m_llm = nullptr;
  LlmClient *m_memoryLlm = nullptr;
  Microphone *m_mic = nullptr;
  Speaker *m_speaker = nullptr;
  WhisperServer *m_whisperServer = nullptr;
  WhisperCli *m_whisperCli = nullptr;
  SpeechToText *m_sttOverride = nullptr;
  bool m_triedServer = false;
  FishSpeech *m_fish = nullptr;
  CommandRouter m_router;
  Identity m_identity;
  std::unique_ptr<wake::WakeWordBackend> m_ownedWake;
  wake::WakeWordBackend *m_wake = nullptr;
  bool m_wakeReady = false;
  QString m_wakeStatus;
  double m_wakeScore = 0.0, m_wakeThreshold = 0.0;
  bool m_armed = false;     // woken: the next utterance is for her
  bool m_fromWake = false;  // the utterance in hand followed a wake
  QTimer m_armTimer;
  QTimer m_wakeTest;
  QTimer m_unloadTimer;
  bool m_micTest = false;
  struct Training {
    QString phrase;
    QVector<wake::Clip> positives;
    wake::Clip speech, noise;
    QString recording; // what is being recorded now, if anything
    wake::Clip buffer;
    bool busy = false;
    bool micWasOpen = false;
  } m_training;
  QTimer m_recordTimer;
  ToolRegistry m_tools;
  desktop::AppIndex m_apps;
  std::function<void(const QString &)> m_companion;
  bool m_testing = false;

  QString m_state = QStringLiteral("idle");
  QString m_bubble;
  QString m_heard;
  QString m_question;
  double m_micLevel = 0.0;
  double m_voiceLevel = 0.0;
  QVariantList m_setup;
  QTimer m_bubbleTimer;
  QTimer m_errorTimer;
  QTimer m_listenTimeout;
  QTimer m_followUp;     // after waking or answering: no wake phrase needed
  QTimer m_questionTimer;
  bool m_pushToTalk = false; // the microphone is open for one utterance
  bool m_transcribing = false;
  bool m_thinking = false;
  bool m_asleep = false;
  bool m_serverDead = false;  // whisper-server refused; use the cli for now
  QByteArray m_lastPcm;       // kept only until it has been transcribed
  bool m_deaf = false;       // "stop listening" in continuous mode

  // The conversation the model sees, trimmed to the last few turns.
  QJsonArray m_history;
  // The turn in progress.
  int m_turn = 0;
  int m_steps = 0;
  QJsonArray m_turnMessages;
  QString m_turnText;
  // Set once this turn has read file contents, command output, window titles
  // or memories: text someone else may have written, which could carry
  // instructions. After that, nothing may leave the machine
  // without asking.
  bool m_turnTainted = false;
  QQueue<ToolCall> m_pendingCalls;
  bool m_acting = false;

  std::function<void(bool)> m_onAnswer;

  QStringList m_speech; // sentences waiting to be synthesised
  bool m_synthesizing = false;
  bool m_speaking = false;
  bool m_voiceBroken = false;
  bool m_followAfter = false; // what is being said is an answer
  bool m_chiming = false;     // the speaker is playing the wake chime

  // Where the last screenshot handed to the model came from, so its pixel
  // coordinates can be turned back into the desktop's.
  QRect m_shotArea;
  QSize m_shotSize;
};
