// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Pace/CompileCache/ToolchainFingerprint.hpp>
#include <Pace/Core/WireFields.hpp>
#include <Pace/Metrics/IMetricsSink.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Pace::Node
{

/// @file FingerprintInputsResponder.hpp
/// `FINGERPRINT-INPUTS`: what one of this node's toolchain fingerprints was computed from, one page at a time.
///
/// **The record is the survey's, never re-gathered.** The heartbeat thread publishes what its survey digested
/// (`ServedToolchain::inputs`) and this reads only what was published, on a reactor thread, without touching the
/// served map -- whose single writer is load-bearing (`NodeRuntimeState`). Asking never walks a tree: a request a
/// peer can send must not buy a multi-second walk, and a walk would describe the tree as it is NOW rather than what
/// the fingerprint was computed from.
///
/// **Gated where `NODE-STATUS` is**, by `NodeStatusResponder`, which routes the verb here once the caller is a member
/// a route identifies: a banner and an include tree describe the host.

/// One served toolchain, as `FINGERPRINT-INPUTS` reads it.
struct ServedToolchainInputs
{
    std::string fingerprint; ///< What it is filed under.
    std::string label;       ///< What a person calls it; empty when nothing said.
    std::string compiler;    ///< The compiler path it is served with.

    /// What the fingerprint was computed from; null for an operator's pin or a toolchain whose inputs nothing
    /// captured. Shared and immutable, so publishing it copies no file list.
    std::shared_ptr<Cc::FingerprintInputs const> inputs;
};

/// Every served toolchain, in fingerprint order.
using ServedInputs = std::vector<ServedToolchainInputs>;

/// Where `FINGERPRINT-INPUTS` reads the served toolchains from.
///
/// A seam, because the answer is published by the heartbeat thread and read on a reactor thread, and a test has to
/// be able to state one without running a survey.
class IServedInputsSource
{
  public:
    IServedInputsSource() = default;
    IServedInputsSource(IServedInputsSource const&) = delete;
    IServedInputsSource(IServedInputsSource&&) = delete;
    IServedInputsSource& operator=(IServedInputsSource const&) = delete;
    IServedInputsSource& operator=(IServedInputsSource&&) = delete;
    virtual ~IServedInputsSource() = default;

    /// @return What the last concluded survey serves, or null before any survey has concluded -- which is not a
    ///         survey that serves nothing (an empty list).
    [[nodiscard]] virtual std::shared_ptr<ServedInputs const> ServedInputsNow() const = 0;
};

/// How many files one page may list at most, before the byte ceiling is consulted.
///
/// **Derived from the ceiling and the smallest entry**: an entry is at least its two field prefixes, so no page of
/// `FingerprintInputsReplyCeiling` could hold more than this many -- which makes it a bound the BYTES always reach
/// first on a real tree, and a number a test can lower (to 3) to exercise every page boundary on a small record.
inline constexpr std::size_t DefaultFingerprintPageEntries =
    CompileCacheWire::FingerprintInputsReplyCeiling / (2 * WireFields::FieldPrefixSize);

/// Answers `FINGERPRINT-INPUTS` for a caller its gate already admitted.
class FingerprintInputsResponder
{
  public:
    /// @param source What the survey published; null on a node that runs no worker, which serves no toolchain.
    ///        Must outlive this.
    /// @param metrics Where a malformed request is counted; must outlive this.
    /// @param version This build's `pace-node` version, named in every reply so the asker can name both sides'.
    /// @param pageEntries The most files one page lists; `DefaultFingerprintPageEntries` in production.
    FingerprintInputsResponder(IServedInputsSource const* source,
                               IMetricsSink& metrics,
                               std::string version,
                               std::size_t pageEntries = DefaultFingerprintPageEntries) noexcept;

    /// Answer one request.
    /// @param payload The request payload, or nothing when the frame did not carry all of it.
    /// @return The reply frame: a page, the index, or a refusal saying why there is none.
    [[nodiscard]] std::vector<std::byte> Answer(std::optional<std::span<std::byte const>> payload) const;

  private:
    /// @param served What the survey published.
    /// @return The index, as many entries as the ceiling holds.
    [[nodiscard]] std::vector<std::byte> Index(ServedInputs const& served) const;

    /// @param fingerprint The fingerprint asked about.
    /// @param inputs Its record.
    /// @param cursor Where the page starts; past the end is an empty last page.
    /// @return The page, or the refusal for an entry no page can hold.
    [[nodiscard]] std::vector<std::byte> Page(std::string const& fingerprint,
                                              Cc::FingerprintInputs const& inputs,
                                              std::uint64_t cursor) const;

    IServedInputsSource const* _source;
    IMetricsSink& _metrics;
    std::string _version;
    std::size_t _pageEntries;
};

} // namespace Pace::Node
