#pragma once

#include <QString>
#include <rhi/qshader.h>

namespace pci {

QShader loadShaderResource(const QString &resourcePath,
                           QString *errorMessage = nullptr);

} // namespace pci
