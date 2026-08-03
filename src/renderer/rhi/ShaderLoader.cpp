#include "renderer/rhi/ShaderLoader.h"

#include <QFile>

namespace pci {

QShader loadShaderResource(const QString &resourcePath, QString *errorMessage)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage =
                QStringLiteral("Could not open shader resource %1: %2")
                    .arg(resourcePath, file.errorString());
        }
        return {};
    }

    const QShader shader = QShader::fromSerialized(file.readAll());
    if (!shader.isValid() && errorMessage) {
        *errorMessage =
            QStringLiteral("Shader resource %1 is not a valid QShader package")
                .arg(resourcePath);
    }
    return shader;
}

} // namespace pci
