// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace Pace::Node
{

/// What compiles running on this machine hold of its budget right now.
struct HeldCharges
{
    std::uint32_t slots {}; ///< Compile slots held.
    std::uint64_t bytes {}; ///< Memory those compiles are charged, in bytes.

    /// @return True when every field is equal.
    [[nodiscard]] bool operator==(HeldCharges const&) const = default;
};

/// Somewhere compiles hold part of this machine's budget, read once per report.
///
/// An interface because three different places hold charges -- a dispatched compile the worker is
/// running (`CompileCapacity`), a local lease this node granted on loopback and one it forwarded for a
/// launcher -- and the report is the SUM of them, which no one of them can state alone. Implementations
/// are thread-safe: the report is read on the presence loop's thread while compiles come and go.
class IHeldCharges
{
  public:
    IHeldCharges() = default;
    IHeldCharges(IHeldCharges const&) = delete;
    IHeldCharges& operator=(IHeldCharges const&) = delete;
    IHeldCharges(IHeldCharges&&) = delete;
    IHeldCharges& operator=(IHeldCharges&&) = delete;
    virtual ~IHeldCharges() = default;

    /// @return What is held now.
    [[nodiscard]] virtual HeldCharges Held() const = 0;
};

/// The sum of several holders, so the report states one figure.
class CombinedHeldCharges final: public IHeldCharges
{
  public:
    /// @param sources The holders to add up; each must outlive this.
    explicit CombinedHeldCharges(std::vector<IHeldCharges const*> sources):
        _sources { std::move(sources) }
    {
    }

    /// @return The slots and bytes every source holds, added.
    [[nodiscard]] HeldCharges Held() const override
    {
        auto total = HeldCharges {};
        for (auto const* const source: _sources)
        {
            auto const held = source->Held();
            total.slots += held.slots;
            total.bytes += held.bytes;
        }
        return total;
    }

  private:
    std::vector<IHeldCharges const*> _sources;
};

/// Charges held for compiles that no `CompileCapacity` counts: a local lease the node granted, or one
/// it forwarded for a launcher. One slot each, at the bytes the leader charged.
///
/// A small injectable class rather than state inside the reporter, so whoever grants a lease holds a
/// RAII `Handle` for as long as the lease lives and the charge goes when the handle does -- on every
/// path out, a closed connection included.
class HeldChargeRegistry final: public IHeldCharges
{
  public:
    /// One slot and its bytes, released when destroyed.
    class Handle
    {
      public:
        Handle() = default;
        Handle(Handle const&) = delete;
        Handle& operator=(Handle const&) = delete;
        Handle(Handle&& other) noexcept:
            _owner { std::exchange(other._owner, nullptr) },
            _bytes { other._bytes }
        {
        }
        Handle& operator=(Handle&& other) noexcept
        {
            if (this != &other)
            {
                Release();
                _owner = std::exchange(other._owner, nullptr);
                _bytes = other._bytes;
            }
            return *this;
        }
        ~Handle()
        {
            Release();
        }

      private:
        friend class HeldChargeRegistry;
        Handle(HeldChargeRegistry* owner, std::uint64_t bytes) noexcept:
            _owner { owner },
            _bytes { bytes }
        {
        }

        void Release() noexcept
        {
            if (_owner == nullptr)
                return;
            auto const guard = std::scoped_lock { _owner->_mutex };
            --_owner->_slots;
            _owner->_bytes -= _bytes;
            _owner = nullptr;
        }

        HeldChargeRegistry* _owner {};
        std::uint64_t _bytes {};
    };

    /// Record one compile holding @p bytes.
    /// @param bytes What the leader charged it.
    /// @return The charge; it is released when the handle is destroyed. The registry must outlive it.
    [[nodiscard]] Handle Hold(std::uint64_t bytes)
    {
        auto const guard = std::scoped_lock { _mutex };
        ++_slots;
        _bytes += bytes;
        return Handle { this, bytes };
    }

    /// @return Every charge held through a live handle.
    [[nodiscard]] HeldCharges Held() const override
    {
        auto const guard = std::scoped_lock { _mutex };
        return HeldCharges { .slots = _slots, .bytes = _bytes };
    }

  private:
    mutable std::mutex _mutex;
    std::uint32_t _slots {};
    std::uint64_t _bytes {};
};

} // namespace Pace::Node
