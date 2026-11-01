// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/Cache/IStorage.hpp>

namespace Pace::Node
{

/// What a cache's storage says about each of its tiers, read per request.
///
/// A seam rather than the cache tier itself, so the responder does not take the cache's whole header and a
/// test can state a cache's tiers. `CacheTier` is production's; a header of its own so the cache tier
/// can implement it without taking the status responder's.
class ITierStatsSource
{
  public:
    ITierStatsSource() = default;
    ITierStatsSource(ITierStatsSource const&) = delete;
    ITierStatsSource(ITierStatsSource&&) = delete;
    ITierStatsSource& operator=(ITierStatsSource const&) = delete;
    ITierStatsSource& operator=(ITierStatsSource&&) = delete;
    virtual ~ITierStatsSource() = default;

    /// @return One entry per tier the cache has; an absent entry is a tier it lacks. Safe from any thread.
    [[nodiscard]] virtual TieredStorageStats SnapshotTiers() const noexcept = 0;
};

} // namespace Pace::Node
