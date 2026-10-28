// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

namespace Pace::Distributed
{

/// What a compile is estimated by: its exact cache key, then its shape across configurations.
struct EstimateKey
{
    std::string cacheKey;         ///< The object key; equal across worktrees.
    std::string sourcePath;       ///< Root-relative source path (`<SRCROOT>/...`), may be empty.
    std::string compilerIdentity; ///< The toolchain fingerprint.
};

/// Remembers the peak memory compiles were measured at and answers what the next one is
/// estimated to need.
///
/// Two bounded LRU maps back it: the latest measurement per exact cache key, and the maximum
/// per `(sourcePath, compilerIdentity)` for a key never seen before. A key that is neither
/// known exactly nor by shape is estimated at the default. Not thread-safe: the owner locks.
class PeakEstimates
{
  public:
    /// @param defaultBytes Estimate for a compile nothing is known about, and the floor a killed
    ///                     compile is raised to.
    /// @param capacity Entries each of the two maps keeps before evicting the least recently
    ///                 recorded. Zero remembers nothing.
    PeakEstimates(std::uint64_t defaultBytes, std::size_t capacity) noexcept;

    /// @param key The compile to estimate.
    /// @return The exact key's latest measurement, else the shape's maximum, else the default.
    [[nodiscard]] std::uint64_t Estimate(EstimateKey const& key) const;

    /// Records a measured peak: the exact key keeps the latest value, the shape keeps the maximum.
    /// @param key The compile that was measured.
    /// @param peakBytes Its measured peak.
    void Record(EstimateKey const& key, std::uint64_t peakBytes);

    /// Records that a compile was killed: its estimate is raised to the default unless already higher.
    /// @param key The compile that was killed.
    void RecordKilled(EstimateKey const& key);

    /// @return The estimate for a compile nothing is known about.
    [[nodiscard]] std::uint64_t DefaultBytes() const noexcept;

  private:
    /// A bounded map that evicts the least recently recorded entry.
    class BoundedLru
    {
      public:
        /// @param capacity Entries kept before evicting; zero keeps none.
        explicit BoundedLru(std::size_t capacity) noexcept;

        /// @param key The key to look up.
        /// @return The value for the key, or nullptr. Does not refresh recency.
        [[nodiscard]] std::uint64_t const* Find(std::string const& key) const;

        /// Stores a value, makes the entry most recent and evicts past capacity.
        /// @param key The key to store under.
        /// @param value The value to store.
        void Put(std::string const& key, std::uint64_t value);

      private:
        using Entries = std::list<std::pair<std::string, std::uint64_t>>;
        std::size_t _capacity;
        Entries _entries; ///< Most recent first.
        std::unordered_map<std::string, Entries::iterator> _index;
    };

    std::uint64_t _defaultBytes;
    BoundedLru _byKey;
    BoundedLru _byShape;
};

} // namespace Pace::Distributed
