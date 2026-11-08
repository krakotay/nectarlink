// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_video_config.h"
#if defined(__linux__)
#if !NECTARLINK_NATIVE_VIDEO
#error Linux desktop requires FFmpeg development headers and Vulkan Video support
#endif
#include "linux_video_codec.h"
#include <QtGui/QVulkanInstance>
#include <QtQuick/QQuickGraphicsDevice>
#include <QtQuick/QQuickWindow>
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QSet>

// The basic render loop serializes all Qt windows on the GUI thread. FFmpeg
// uses queue 0; Qt uses queue 1, so neither submits to the other's queue.
class VideoVulkanWindows final : public QObject {
    QVulkanInstance instance;
    NectarlinkVulkanDevice device{};
    QSet<QQuickWindow*> configured;
public:
    explicit VideoVulkanWindows(QCoreApplication* app) : QObject(app) {
        instance.setApiVersion(QVersionNumber(1, 3, 0));
        if (!instance.create()) qFatal("Create Vulkan 1.3 instance failed: VkResult=%d", int(instance.errorCode()));
        char error[1024]{};
        if (!nectarlink_video_vulkan_init(uintptr_t(instance.vkInstance()), &device, error, sizeof(error)))
            qFatal("Initialize Vulkan video: %s", error);
        app->installEventFilter(this);
    }
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::PlatformSurface || event->type() == QEvent::Show) {
            if (auto* window = qobject_cast<QQuickWindow*>(object); window && !configured.contains(window)) {
                window->setVulkanInstance(&instance);
                window->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceObjects(
                    reinterpret_cast<VkPhysicalDevice>(device.physical), reinterpret_cast<VkDevice>(device.device),
                    int(device.family), int(device.queue)));
                configured.insert(window);
                connect(window, &QObject::destroyed, this, [this, window] { configured.remove(window); });
                connect(window, &QQuickWindow::sceneGraphError, this, [](QQuickWindow::SceneGraphError, const QString& message) {
                    qFatal("Vulkan scene graph error: %s", qPrintable(message));
                });
            }
        }
        return false;
    }
};
#endif
