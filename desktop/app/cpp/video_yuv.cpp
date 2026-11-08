// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_yuv.h"
#include "native_video_config.h"
#if NECTARLINK_NATIVE_VIDEO
#include "linux_video_codec.h"
#endif

#include <QtGui/QMatrix4x4>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGGeometryNode>
#include <QtQuick/QSGMaterial>
#include <QtQuick/QSGMaterialShader>
#include <QtQuick/QSGRendererInterface>
#include <QtQuick/QSGSimpleTextureNode>
#include <QtQuick/QSGTexture>
#include <rhi/qrhi.h>
#if NECTARLINK_NATIVE_VIDEO
#include <rhi/qrhi_platform.h>
#endif
#include <algorithm>
#include <cstring>
#include <vector>

namespace {
#if NECTARLINK_NATIVE_VIDEO
class VideoSubmitWaits final : public QObject {
public:
    std::vector<VkSemaphore> waits;
    explicit VideoSubmitWaits(QQuickWindow* window) : QObject(window) {
        setObjectName(QStringLiteral("nectarlink-video-waits"));
        connect(window, &QQuickWindow::afterFrameEnd, this, [this] { waits.clear(); }, Qt::DirectConnection);
    }
    void append(QRhi* rhi, VkSemaphore semaphore) {
        waits.push_back(semaphore);
        QRhiVulkanQueueSubmitParams params{};
        params.waitSemaphoreCount = uint32_t(waits.size()); params.waitSemaphores = waits.data();
        rhi->setQueueSubmitParams(&params);
    }
};
#endif

class PlaneTexture final : public QSGTexture
{
public:
    explicit PlaneTexture(bool chroma) : m_chroma(chroma) { setFiltering(Linear); }
    qint64 comparisonKey() const override { return qint64(quintptr(this)); }
    QRhiTexture *rhiTexture() const override { return m_texture.get(); }
    QSize textureSize() const override { return m_size; }
    bool hasAlphaChannel() const override { return false; }
    bool hasMipmaps() const override { return false; }

    void setPixels(const QSize &size, const QByteArray &data, int stride)
    {
        if (size != m_size) m_texture.reset();
        m_size = size;
        m_data = data;
        m_stride = stride;
        m_dirty = !data.isEmpty();
    }

    void setNative(QRhi* rhi, QSize size, quint64 image)
    {
        m_texture.reset(rhi->newTexture(m_chroma ? QRhiTexture::RG8 : QRhiTexture::R8, size));
        if (!m_texture->createFrom({image, 5 /* VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL */}))
            qFatal("Import Vulkan NV12 plane into Qt failed");
        m_size = size; m_dirty = false;
    }
    void commitTextureOperations(QRhi *rhi, QRhiResourceUpdateBatch *updates) override
    {
        if (!m_texture) {
            m_texture.reset(rhi->newTexture(m_chroma ? QRhiTexture::RG8 : QRhiTexture::R8, m_size));
            if (!m_texture->create()) qFatal("Create video plane texture failed");
        }
        if (m_dirty) {
            QRhiTextureSubresourceUploadDescription data(m_data);
            data.setDataStride(quint32(m_stride));
            updates->uploadTexture(m_texture.get(), QRhiTextureUploadDescription({ {0, 0, data} }));
            m_dirty = false;
        }
    }
private:
    bool m_chroma;
    QSize m_size;
    QByteArray m_data;
    int m_stride = 0;
    bool m_dirty = false;
    std::unique_ptr<QRhiTexture> m_texture;
};

class YuvMaterial;
class YuvShader final : public QSGMaterialShader
{
public:
    YuvShader()
    {
        setShaderFileName(VertexStage, QStringLiteral(":/shaders/video-yuv.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/shaders/video-yuv.frag.qsb"));
    }
    bool updateUniformData(RenderState &state, QSGMaterial *next, QSGMaterial *) override;
    void updateSampledImage(RenderState &state, int binding, QSGTexture **texture,
                           QSGMaterial *next, QSGMaterial *) override;
};

class YuvMaterial final : public QSGMaterial
{
public:
    explicit YuvMaterial(QQuickWindow* target) : y(false), uv(true), window(target) { setFlag(Blending); }
    ~YuvMaterial() override { release(); }
    void release() {
#if NECTARLINK_NATIVE_VIDEO
        if (surface) {
            char error[512]{};
            if (!nectarlink_video_surface_release(surface, error, sizeof(error))) qFatal("Release Vulkan video surface: %s", error);
            surface = nullptr;
        }
#endif
    }
    QSGMaterialType *type() const override { static QSGMaterialType type; return &type; }
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override { return new YuvShader; }
    int compare(const QSGMaterial *other) const override
    {
        return this == other ? 0 : (quintptr(this) < quintptr(other) ? -1 : 1);
    }
    void setFrame(const std::shared_ptr<VideoYuvFrame> &frame)
    {
        release();
        m_frame = frame;
        prepared = false;
        if (frame->native) {
            y.setPixels(frame->pixels, {}, 0);
            uv.setPixels(QSize((frame->pixels.width() + 1) / 2, (frame->pixels.height() + 1) / 2), {}, 0);
            return;
        }
        setCpuPixels();
    }
    void setCpuPixels()
    {
        const auto &frame = m_frame;
        const int w = frame->pixels.width(), h = frame->pixels.height();
        const qsizetype uvStart = qsizetype(frame->stride) * frame->planeRows;
        y.setPixels(QSize(w, h), frame->bytes.left(frame->stride * h), frame->stride);
        uv.setPixels(QSize((w + 1) / 2, (h + 1) / 2),
                     frame->bytes.mid(uvStart, frame->stride * ((h + 1) / 2)), frame->stride);
    }
    void prepare(QSGMaterialShader::RenderState &state)
    {
        if (prepared) return;
#if NECTARLINK_NATIVE_VIDEO
        if (m_frame->native) {
            if (state.rhi()->backend() != QRhi::Vulkan) qFatal("Mirroring requires Qt Vulkan RHI");
            NectarlinkVulkanSurface native{};
            char error[1024]{};
            surface = nectarlink_video_frame_present(m_frame->native.get(), &native, error, sizeof(error));
            if (!surface) qFatal("Prepare Vulkan video frame: %s", error);
            if (QSize(native.width, native.height) != m_frame->pixels) qFatal("Vulkan video frame dimensions changed");
            y.setNative(state.rhi(), m_frame->pixels, native.images[0]);
            uv.setNative(state.rhi(), m_frame->pixels / 2, native.images[1]);
            auto* waits = static_cast<VideoSubmitWaits*>(window->findChild<QObject*>(QStringLiteral("nectarlink-video-waits"), Qt::FindDirectChildrenOnly));
            if (!waits) waits = new VideoSubmitWaits(window);
            waits->append(state.rhi(), VkSemaphore(native.ready));
        } else
#endif
        {
            y.commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
            uv.commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        }
        prepared = true;
    }
    PlaneTexture y, uv;
    std::shared_ptr<VideoYuvFrame> m_frame;
    bool prepared = false;
    QQuickWindow* window;
    void* surface = nullptr;
};

bool YuvShader::updateUniformData(RenderState &state, QSGMaterial *next, QSGMaterial *)
{
    auto *material = static_cast<YuvMaterial *>(next);
    material->prepare(state);
    QByteArray *uniforms = state.uniformData();
    Q_ASSERT(uniforms->size() >= 160);
    std::memcpy(uniforms->data(), state.combinedMatrix().constData(), 64);
    const float opacity = state.opacity();
    std::memcpy(uniforms->data() + 64, &opacity, 4);
    const auto &frame = *material->m_frame;
    const float offset[4] = {frame.fullRange ? 0.f : -16.f / 255.f, -128.f / 255.f, -128.f / 255.f, 0.f};
    std::memcpy(uniforms->data() + 80, offset, sizeof(offset));
    const float ys = frame.fullRange ? 1.f : 255.f / 219.f;
    const float cs = frame.fullRange ? 1.f : 255.f / 224.f;
    const float rv = (frame.bt601 ? 1.402f : 1.5748f) * cs;
    const float gu = (frame.bt601 ? -0.344136f : -0.187324f) * cs;
    const float gv = (frame.bt601 ? -0.714136f : -0.468124f) * cs;
    const float bu = (frame.bt601 ? 1.772f : 1.8556f) * cs;
    const float matrix[16] = {ys, ys, ys, 0, 0, gu, bu, 0, rv, gv, 0, 0, 0, 0, 0, 1};
    std::memcpy(uniforms->data() + 96, matrix, sizeof(matrix));
    return true;
}

void YuvShader::updateSampledImage(RenderState &state, int binding, QSGTexture **texture,
                                  QSGMaterial *next, QSGMaterial *)
{
    auto *material = static_cast<YuvMaterial *>(next);
    material->prepare(state);
    PlaneTexture *plane = binding == 1 ? &material->y : binding == 2 ? &material->uv : nullptr;
    if (!plane) return;
    plane->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
    *texture = plane;
}


class YuvNode final : public QSGGeometryNode
{
public:
    explicit YuvNode(QQuickWindow* window) : geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4), material(window)
    {
        geometry.setDrawingMode(QSGGeometry::DrawTriangleStrip);
        setGeometry(&geometry);
        setMaterial(&material);
    }
    QSGGeometry geometry;
    YuvMaterial material;
    quint64 serial = 0;
};
}

std::shared_ptr<VideoYuvFrame> sharedVideoYuvFrame(uintptr_t pointer, int width, int height)
{
#if NECTARLINK_NATIVE_VIDEO
    if (!pointer || QQuickWindow::graphicsApi() != QSGRendererInterface::Vulkan)
        qFatal("A Vulkan decoder frame and Vulkan Qt backend are required");
    void *raw = reinterpret_cast<void *>(pointer);
    NectarlinkVideoFrameInfo info{};
    if (!nectarlink_video_frame_info(raw, &info) || info.device != 3) qFatal("Expected a Vulkan NV12 decoder frame");
    auto frame = std::make_shared<VideoYuvFrame>();
    frame->size = QSize(width, height);
    frame->pixels = QSize(info.width, info.height);
    frame->bt601 = info.bt601;
    frame->fullRange = info.full_range;
    frame->native = std::shared_ptr<void>(nectarlink_video_frame_retain(raw), nectarlink_video_frame_destroy);
    return frame;
#else
    Q_UNUSED(pointer); Q_UNUSED(width); Q_UNUSED(height);
    return {};
#endif
}


QSGNode *updateVideoYuvNode(QSGNode *old, QQuickWindow *window,
                           const std::shared_ptr<VideoYuvFrame> &frame, const QRectF &rect,
                           quint64 serial)
{
    auto *node = dynamic_cast<YuvNode *>(old);
    if (!node) { delete old; node = new YuvNode(window); }
    if (node->serial != serial) {
        node->material.setFrame(frame);
        node->serial = serial;
        node->markDirty(QSGNode::DirtyMaterial);
    }
    QSGGeometry::updateTexturedRectGeometry(&node->geometry, rect, QRectF(0, 0, 1, 1));
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}
