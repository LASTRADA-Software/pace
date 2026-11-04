// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

namespace Pace::Node
{

/// @file InputsCapture.hpp
/// The one forced walk per process that captures what this node's fingerprints were computed from, run on a thread
/// of its own and handed back to the heartbeat thread.
///
/// **Why a thread of its own.** A warm start takes every fingerprint from the cache and registers at once; the inputs
/// `toolchain_diff` explains a fingerprint with exist only where a walk digested them, and a walk of a toolchain tree
/// takes seconds on Linux and has been measured past five minutes on a Windows CI runner (#1157). Run on the
/// heartbeat thread it would hold every heartbeat for that long, and a registration the scheduler stops hearing from
/// expires. So the walk runs HERE, and the heartbeat thread only asks, once per round, whether it has finished --
/// a question that never waits.
///
/// **Nothing a peer sends starts one.** The walk is started exactly once, by the heartbeat thread after its first
/// survey; a request reads only what has been captured.
///
/// **Stoppable.** The thread's stop token is the walk's; destroying this requests the stop and joins, and the walk
/// observes the token between files, so a node told to stop does not wait out a tree.
template <typename Result>
class InputsCapture
{
  public:
    /// What runs the walk; called once, on the capture thread, with that thread's stop token.
    using Walk = std::function<Result(std::stop_token const&)>;

    InputsCapture() = default;
    InputsCapture(InputsCapture const&) = delete;
    InputsCapture(InputsCapture&&) = delete;
    InputsCapture& operator=(InputsCapture const&) = delete;
    InputsCapture& operator=(InputsCapture&&) = delete;

    /// Stops the walk if it is still running, and joins it.
    ~InputsCapture()
    {
        // Explicitly, and before the members it writes go: a jthread member would be destroyed LAST only if it were
        // declared last, and an ordering a reader has to notice is one a later edit breaks.
        if (_thread.joinable())
        {
            _thread.request_stop();
            _thread.join();
        }
    }

    /// Start the walk on its own thread; returns at once, without waiting for any of it.
    ///
    /// A second call is ignored: there is one capture per process.
    /// @param walk What to run.
    void Start(Walk walk)
    {
        if (_thread.joinable())
            return;
        _thread = std::jthread { [this, walk = std::move(walk)](std::stop_token const& stop) {
            auto result = walk(stop);
            std::scoped_lock const guard { _mutex };
            _result = std::move(result);
        } };
    }

    /// Hand back the walk's result once it has finished, exactly once; never waits.
    /// @return The result, or nothing while the walk runs, before it started, and after it was taken.
    [[nodiscard]] std::optional<Result> Take()
    {
        std::scoped_lock const guard { _mutex };
        return std::exchange(_result, std::nullopt);
    }

  private:
    std::mutex _mutex;
    std::optional<Result> _result;
    std::jthread _thread;
};

} // namespace Pace::Node
