// SPDX-License-Identifier: Apache-2.0
#include <Pace/Distributed/PeakEstimates.hpp>

#include <algorithm>

namespace Pace::Distributed
{

namespace
{
/// The shape's map key. Length-prefixed, never joined, so no path/identity pair can collide with another.
[[nodiscard]] std::string ShapeKey(EstimateKey const& key)
{
    return std::to_string(key.sourcePath.size()) + ':' + key.sourcePath + key.compilerIdentity;
}
} // namespace

PeakEstimates::BoundedLru::BoundedLru(std::size_t capacity) noexcept: _capacity { capacity }
{
}

std::uint64_t const* PeakEstimates::BoundedLru::Find(std::string const& key) const
{
    auto const it = _index.find(key);
    return it == _index.end() ? nullptr : &it->second->second;
}

void PeakEstimates::BoundedLru::Put(std::string const& key, std::uint64_t value)
{
    if (_capacity == 0)
        return;
    if (auto const it = _index.find(key); it != _index.end())
    {
        it->second->second = value;
        _entries.splice(_entries.begin(), _entries, it->second);
        return;
    }
    _entries.emplace_front(key, value);
    _index.emplace(key, _entries.begin());
    if (_entries.size() > _capacity)
    {
        _index.erase(_entries.back().first);
        _entries.pop_back();
    }
}

PeakEstimates::PeakEstimates(std::uint64_t defaultBytes, std::size_t capacity) noexcept:
    _defaultBytes { defaultBytes }, _byKey { capacity }, _byShape { capacity }
{
}

std::uint64_t PeakEstimates::Estimate(EstimateKey const& key) const
{
    if (auto const* exact = _byKey.Find(key.cacheKey))
        return *exact;
    if (!key.sourcePath.empty())
        if (auto const* shape = _byShape.Find(ShapeKey(key)))
            return *shape;
    return _defaultBytes;
}

void PeakEstimates::Record(EstimateKey const& key, std::uint64_t peakBytes)
{
    _byKey.Put(key.cacheKey, peakBytes);
    if (key.sourcePath.empty())
        return;
    auto const shapeKey = ShapeKey(key);
    auto const* const known = _byShape.Find(shapeKey);
    _byShape.Put(shapeKey, known != nullptr ? std::max(*known, peakBytes) : peakBytes);
}

void PeakEstimates::RecordKilled(EstimateKey const& key)
{
    auto const* const known = _byKey.Find(key.cacheKey);
    Record(key, std::max(known != nullptr ? *known : std::uint64_t { 0 }, _defaultBytes));
}

std::uint64_t PeakEstimates::DefaultBytes() const noexcept
{
    return _defaultBytes;
}

} // namespace Pace::Distributed
