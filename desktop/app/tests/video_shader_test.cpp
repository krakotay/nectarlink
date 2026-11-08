// SPDX-License-Identifier: GPL-3.0-or-later
#include "../cpp/video_shader.h"
#include "../cpp/qt_graphics_error.h"
#include <QtCore/QFile>
#include <cstdio>
#include <cstdlib>

void require(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
QShader load(const char* path)
{
    QFile file(QString::fromLocal8Bit(path));
    require(file.open(QIODevice::ReadOnly), "open baked shader");
    return QShader::fromSerialized(file.readAll());
}
int main(int argc, char** argv)
{
    require(argc == 3 || argc == 4, "expected vertex and fragment packs, with optional previous vertex pack");
    auto vertex = load(argv[1]);
    require(videoShaderError(vertex,QShader::VertexStage).isEmpty(), "fixed vertex must support Qt Quick Vulkan batching");
    require(videoShaderError(load(argv[2]),QShader::FragmentStage).isEmpty(), "fragment must support Vulkan");
    if (argc == 4) require(!videoShaderError(load(argv[3]),QShader::VertexStage).isEmpty(), "reject the shipped pack that caused the black screen");
    vertex.removeShader({QShader::SpirvShader,QShaderVersion(100),QShader::BatchableVertexShader});
    require(videoShaderError(vertex,QShader::VertexStage).contains(QStringLiteral("batchable")), "diagnose missing batching variant");
    require(!videoShaderError(load(argv[2]),QShader::VertexStage).isEmpty(), "reject wrong shader stage");
    require(!videoShaderError({},QShader::VertexStage).isEmpty(), "reject invalid shader");
    require(isQtGraphicsFailure(QtWarningMsg,QStringLiteral("No SPIR-V 1.0 shader code found in baked shader X")), "shader warning is fatal rendering failure");
    require(isQtGraphicsFailure(QtWarningMsg,QStringLiteral("Failed to build graphics pipeline state")), "pipeline warning is fatal rendering failure");
    require(!isQtGraphicsFailure(QtWarningMsg,QStringLiteral("unrelated QML warning")), "unrelated warnings must not abort");
    require(!isQtGraphicsFailure(QtInfoMsg,QStringLiteral("Failed to build graphics pipeline state")), "informational text must not abort");
    std::puts("Shader variants and Qt graphics failure checks passed");
}
