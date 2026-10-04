#include <pci/desktop/dispatch/QtCompletionExecutor.h>

#include <QAbstractEventDispatcher>
#include <QMetaObject>
#include <QObject>
#include <QThread>

#include <memory>
#include <stdexcept>
#include <utility>

namespace pci {
namespace {

class QtCompletionExecutor final
    : public QObject
    , public CompletionExecutor {
    Q_OBJECT

public:
    [[nodiscard]] bool post(Completion completion) override
    {
        return queue_.post(std::move(completion), [this] {
            // Qt carries only a wake-up. Worker-created payloads cross the
            // explicitly synchronized queue, including with a prebuilt Qt.
            return QMetaObject::invokeMethod(
                this, "drain", Qt::QueuedConnection);
        });
    }

    void invalidate() noexcept override
    {
        queue_.invalidate();
    }

private slots:
    void drain()
    {
        queue_.drain();
    }

private:
    CompletionDispatchQueue queue_;
};

} // namespace

std::shared_ptr<CompletionExecutor> makeQtCompletionExecutor(QObject *owner)
{
    if (owner == nullptr) {
        throw std::invalid_argument("completion owner must not be null");
    }
    if (owner->thread() != QThread::currentThread()) {
        throw std::logic_error(
            "a completion executor must be created on its owner thread");
    }
    QObject *dispatcher = QAbstractEventDispatcher::instance(owner->thread());
    if (dispatcher == nullptr) {
        throw std::logic_error(
            "an async controller requires an event dispatcher");
    }
    return {new QtCompletionExecutor, [](QtCompletionExecutor *executor) {
                // The last posting worker may release this reference. Drop
                // payloads immediately and destroy the QObject on its owner.
                executor->invalidate();
                executor->deleteLater();
            }};
}

} // namespace pci

#include "QtCompletionExecutor.moc"
