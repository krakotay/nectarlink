// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QSize>
#include <QtCore/QRectF>
#include <memory>
#include <cstdint>

class QSGNode;
class QQuickWindow;

struct VideoYuvFrame
{
    QSize size;
    QSize pixels;
    int stride = 0;
    int planeRows = 0;
    bool bt601 = false;
    bool fullRange = false;
    QByteArray bytes;
    std::shared_ptr<void> native;
};

std::shared_ptr<VideoYuvFrame> sharedVideoYuvFrame(uintptr_t frame, int width, int height);

QSGNode *updateVideoYuvNode(QSGNode *old, QQuickWindow *window,
                          const std::shared_ptr<VideoYuvFrame> &frame, const QRectF &rect,
                          quint64 serial);
