// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QtCore/QString>
#include <QtCore/QtLogging>

// Qt reports pipeline construction failure as a warning. It cannot render the
// frame afterward, so continuing would hide the failure behind a black view.
inline bool isQtGraphicsFailure(QtMsgType type, const QString& message)
{
    if (type != QtWarningMsg && type != QtCriticalMsg && type != QtFatalMsg) return false;
    return message.startsWith(QStringLiteral("No SPIR-V "))
        || message == QStringLiteral("Failed to build graphics pipeline state");
}
