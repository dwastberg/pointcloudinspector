#pragma once

#include <pci/runtime/CompletionExecutor.h>

#include <memory>

class QObject;

namespace pci {

// The owner and its event dispatcher must remain alive until the executor is
// invalidated and every posting worker has joined.
[[nodiscard]] std::shared_ptr<CompletionExecutor>
makeQtCompletionExecutor(QObject *owner);

} // namespace pci
