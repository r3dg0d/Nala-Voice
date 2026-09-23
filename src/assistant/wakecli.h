#pragma once
#include "wakeword.h"

#include <QStringList>
#include <functional>

// `nala wakeword …`: setting up, training and tuning wake phrases from the
// command line. Runs on its own, without the companion.
int runWakewordCli(const QStringList &args);

// Shared with the preferences' setup flow.
bool installFeatureModels(const QString &dir, QString *error,
                          std::function<void(QString)> progress = {});
bool readClip(const QString &path, wake::Clip *clip, QString *error);
