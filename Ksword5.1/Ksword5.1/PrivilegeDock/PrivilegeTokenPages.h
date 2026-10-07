#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <QJsonObject>
#include <functional>

class QWidget;

namespace ks::privilege
{
    QWidget* createTokenComparePage(QWidget* parent);
    QWidget* createSessionsPage(QWidget* parent, std::function<void(DWORD, quint64)> openProcess = {});
    void focusSessionsAccount(QWidget* page, const QString& accountOrSid);
    QWidget* createIdentityLaunchPage(QWidget* parent);

    // Value-only and safe for worker threads. Stable entries have key/value/state; unreadable
    // fields are retained. metadata contains PID and the creation time obtained on the same handle.
    QJsonObject captureTokenSnapshot(DWORD pid);
}
