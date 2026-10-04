#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace pci {

// Transfers owned work to a single owner thread. A successful post never runs
// the completion inline. The owner may invalidate the executor to reject new
// work and release accepted work that has not started.
class CompletionExecutor {
public:
    class Completion final {
    public:
        Completion() = default;

        template <typename Callback>
            requires(
                !std::is_same_v<std::remove_cvref_t<Callback>, Completion> &&
                std::is_invocable_v<std::decay_t<Callback> &>)
        Completion(Callback &&callback)
            : callback_(std::make_unique<Model<std::decay_t<Callback>>>(
                  std::forward<Callback>(callback)))
        {
        }

        Completion(Completion &&) noexcept = default;
        Completion &operator=(Completion &&) noexcept = default;
        Completion(const Completion &) = delete;
        Completion &operator=(const Completion &) = delete;

        explicit operator bool() const noexcept
        {
            return callback_ != nullptr;
        }

        void operator()()
        {
            callback_->invoke();
        }

    private:
        class Interface {
        public:
            virtual ~Interface() = default;
            virtual void invoke() = 0;
        };

        template <typename Callback> class Model final : public Interface {
        public:
            explicit Model(Callback callback)
                : callback_(std::move(callback))
            {
            }

            void invoke() override
            {
                std::invoke(callback_);
            }

        private:
            Callback callback_;
        };

        std::unique_ptr<Interface> callback_;
    };

    virtual ~CompletionExecutor() = default;

    [[nodiscard]] virtual bool post(Completion completion) = 0;
    virtual void invalidate() noexcept = 0;
};

// Thread-safe queue protocol shared by event-loop adapters and deterministic
// tests. post() serializes acceptance, invalidation, and wake scheduling so an
// accepted completion always has exactly one pending owner wake behind it.
// The wake request must only schedule drain(); it must never call it inline.
class CompletionDispatchQueue final {
public:
    using Completion = CompletionExecutor::Completion;
    using WakeRequest = std::function<bool()>;

    // Operation adapters with an owner-thread recovery pump retain accepted
    // work if only wake scheduling fails. Ordinary executors reject and
    // release.
    explicit CompletionDispatchQueue(bool retainOnWakeFailure = false)
        : retainOnWakeFailure_(retainOnWakeFailure)
    {
    }
    CompletionDispatchQueue(const CompletionDispatchQueue &) = delete;
    CompletionDispatchQueue &
    operator=(const CompletionDispatchQueue &) = delete;

    [[nodiscard]] bool post(Completion completion,
                            const WakeRequest &requestWake);

    // drain() and invalidate() belong to the same owner thread. Work posted
    // while a batch drains receives a later wake and cannot run reentrantly.
    void drain();
    void invalidate() noexcept;

private:
    [[nodiscard]] bool accepting() const noexcept;

    mutable std::mutex mutex_;
    std::deque<Completion> completions_;
    bool accepting_ = true;
    bool wakePending_ = false;
    bool retainOnWakeFailure_ = false;
};

} // namespace pci
