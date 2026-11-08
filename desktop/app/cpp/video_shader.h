// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QtCore/QString>
#include <rhi/qshader.h>

inline QString videoShaderError(const QShader& shader, QShader::Stage stage)
{
    if (!shader.isValid() || shader.stage() != stage)
        return QStringLiteral("Invalid video shader or shader stage");
    const QShaderKey standard(QShader::SpirvShader, QShaderVersion(100), QShader::StandardShader);
    if (shader.shader(standard).shader().isEmpty())
        return QStringLiteral("Video shader has no Vulkan SPIR-V 1.0 standard variant");
    const QShaderKey batchable(QShader::SpirvShader, QShaderVersion(100), QShader::BatchableVertexShader);
    if (stage == QShader::VertexStage && shader.shader(batchable).shader().isEmpty())
        return QStringLiteral("Video vertex shader has no Qt Quick batchable Vulkan variant; rebuild with qsb --batchable");
    return {};
}
