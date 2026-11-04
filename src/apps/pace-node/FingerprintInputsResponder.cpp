// SPDX-License-Identifier: Apache-2.0
#include "FingerprintInputsResponder.hpp"

#include <Pace/Protocol/FingerprintInputsWire.hpp>
#include <Pace/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <utility>

#include <core/Ranges.hpp>

namespace Pace::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// What a node running no worker answers: it serves no toolchain, so it holds no inputs.
    constexpr Cc::UncountedRefusal NoWorkerHere {
        .code = Wire::NoCompileWorker::Code,
        .rationale = "a healthy answer from a node that runs no compile worker: it serves no toolchain to explain, "
                     "which is configuration rather than an event"
    };

    /// What a node whose survey has not concluded answers.
    constexpr Cc::UncountedRefusal SurveyInFlight {
        .code = Wire::ErrorCode::WorkerToolchainSurveyInFlight,
        .rationale = "what every node answers in the minutes after it starts, until its survey concludes; transient "
                     "and expected, so a counter would read a restart as an event"
    };

    /// What a fingerprint this node does not serve is answered with.
    constexpr Cc::UncountedRefusal NotServedHere {
        .code = Wire::ErrorCode::NoWorker,
        .rationale = "a diagnostic question about a fingerprint this node does not serve, answered with which; asking "
                     "about another node's toolchain is the ordinary way to learn that"
    };

    /// Why a served fingerprint has no inputs, and how that is answered: one row per absence.
    struct AbsenceRow
    {
        Wire::FingerprintInputsState state; ///< The absence.
        Cc::UncountedRefusal refusal;       ///< Its code -- whose retry column says whether asking again helps.
        std::string (*words)(ServedToolchainInputs const& toolchain); ///< What the asker is told.
    };

    /// The absences. A pin, and a capture that did not reproduce, do not change on an immediate retry, so they answer a
    /// code whose retry says so; a capture still running clears by itself, and answers one that says that.
    constexpr std::array AbsenceRows {
        AbsenceRow {
            .state = Wire::FingerprintInputsState::Pinned,
            .refusal = { .code = Wire::ErrorCode::FingerprintInputsUnrecorded,
                         .rationale = "an operator's pin is a decision, answered with what it means; not an event" },
            .words =
                [](ServedToolchainInputs const& toolchain) {
                    return std::format("this node serves {} with {} under an operator's --toolchain=<fingerprint>="
                                       "<compiler> pin, which is never probed, so it holds no inputs for it and will "
                                       "not while the pin stands",
                                       toolchain.fingerprint,
                                       toolchain.compiler);
                } },
        AbsenceRow { .state = Wire::FingerprintInputsState::NotYetCaptured,
                     .refusal = { .code = Wire::ErrorCode::WorkerToolchainSurveyInFlight,
                                  .rationale = "what every warm-started node answers until its one capture walk finishes; "
                                               "transient and expected, so a counter would read a restart as an event" },
                     .words =
                         [](ServedToolchainInputs const& toolchain) {
                             return std::format(
                                 "this node took {} from its fingerprint cache and has not yet finished the one "
                                 "walk per start that captures what it was computed from; ask again shortly",
                                 toolchain.fingerprint);
                         } },
        AbsenceRow {
            .state = Wire::FingerprintInputsState::NotCaptured,
            .refusal = { .code = Wire::ErrorCode::FingerprintInputsUnrecorded,
                         .rationale = "a capture that concluded without this fingerprint stays so until a re-survey "
                                      "walks it or the node restarts; answered with why, and the node's log "
                                      "carries the event" },
            .words =
                [](ServedToolchainInputs const& toolchain) {
                    return std::format("this node took {} from its fingerprint cache and its capture walk did not "
                                       "reproduce it -- or found it after the capture ran -- so it holds no inputs for "
                                       "it until a re-survey walks this toolchain again or the node restarts",
                                       toolchain.fingerprint);
                } },
    };

    /// What a record no reply can carry is answered with.
    constexpr Cc::UncountedRefusal UnpageableRecord {
        .code = Wire::ErrorCode::PayloadTooLarge,
        .rationale = "this node's own record holds a banner or a path longer than one reply carries; no peer can "
                     "provoke it, and the words name which"
    };
} // namespace

FingerprintInputsResponder::FingerprintInputsResponder(IServedInputsSource const* source,
                                                       IMetricsSink& metrics,
                                                       std::string version,
                                                       std::size_t pageEntries,
                                                       std::size_t replyCeiling) noexcept:
    _source { source },
    _metrics { metrics },
    _version { std::move(version) },
    _pageEntries { std::max<std::size_t>(pageEntries, 1) },
    _replyCeiling { std::min(replyCeiling, Wire::FingerprintInputsReplyCeiling) }
{
}

std::vector<std::byte> FingerprintInputsResponder::Answer(std::optional<std::span<std::byte const>> payload) const
{
    auto const request = payload.has_value() ? Wire::DecodeFingerprintInputsRequest(*payload) : std::nullopt;
    if (!request.has_value())
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCode::MalformedFrame,
                            .counter = IMetricsSink::Counter::NodeFingerprintInputsRefusedMalformed },
                          "fingerprint-inputs takes exactly two fields: a fingerprint of at most 64 bytes, or nothing "
                          "to ask for the index, and an eight-byte page cursor");

    if (_source == nullptr)
        return Cc::RefuseWithoutCounter(
            NoWorkerHere,
            std::format("{}, so it serves no toolchain and holds no fingerprint inputs", Wire::NoCompileWorker::Stem));

    auto const served = _source->ServedInputsNow();
    if (served == nullptr)
        return Cc::RefuseWithoutCounter(SurveyInFlight,
                                        "this node has not finished identifying its toolchains; ask again shortly");

    if (request->fingerprint.empty())
        return Index(*served);

    auto const* const toolchain = core::findOrNull(*served, request->fingerprint, &ServedToolchainInputs::fingerprint);
    if (toolchain == nullptr)
        return Cc::RefuseWithoutCounter(NotServedHere,
                                        std::format("this node serves no toolchain with fingerprint {} (it serves {}); "
                                                    "ask with no fingerprint for the list",
                                                    request->fingerprint,
                                                    served->size()));
    if (toolchain->inputs == nullptr)
    {
        // A null record with no reason is a publication defect, answered as a capture that did not run rather than
        // as a pin -- the permanent answer that does not invent a cause.
        auto const* const absence = core::findOrNull(AbsenceRows, toolchain->state, &AbsenceRow::state);
        auto const& row = absence != nullptr ? *absence : AbsenceRows.back();
        return Cc::RefuseWithoutCounter(row.refusal, row.words(*toolchain));
    }
    return Page(toolchain->fingerprint, *toolchain->inputs, request->cursor);
}

std::vector<std::byte> FingerprintInputsResponder::Index(ServedInputs const& served) const
{
    auto index = Wire::FingerprintIndexFields { .version = _version,
                                                .total = static_cast<std::uint32_t>(served.size()),
                                                .entries = {} };
    // Cut to the ceiling rather than refused: `total` beside a shorter list says it was cut, and an asker that
    // needed an entry past the cut says so by name rather than resolving a name against half a list.
    auto used = Wire::FingerprintIndexOverhead(_version);
    for (auto const& toolchain: served)
    {
        auto entry = Wire::FingerprintIndexEntry { .fingerprint = toolchain.fingerprint,
                                                   .label = toolchain.label,
                                                   .compiler = toolchain.compiler,
                                                   .state = toolchain.state };
        auto const cost = Wire::EncodedIndexEntryBytes(entry);
        if (used + cost > _replyCeiling)
            break;
        used += cost;
        index.entries.push_back(std::move(entry));
    }
    return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeFingerprintIndex(index));
}

std::vector<std::byte> FingerprintInputsResponder::Page(std::string const& fingerprint,
                                                        Cc::FingerprintInputs const& inputs,
                                                        std::uint64_t cursor) const
{
    auto const total = inputs.files.size();
    auto const overhead = Wire::FingerprintInputsPageOverhead(fingerprint, _version, inputs);
    if (overhead > _replyCeiling)
        return Cc::RefuseWithoutCounter(UnpageableRecord,
                                        std::format("the inputs of {} hold a banner of {} bytes, more than one reply of "
                                                    "{} bytes carries",
                                                    fingerprint,
                                                    inputs.banner.size(),
                                                    _replyCeiling));

    // A cursor past the end is an empty LAST page, never an error and never an index past the vector: the asker
    // learns there is nothing further, which is the truth.
    auto const start = static_cast<std::size_t>(std::min<std::uint64_t>(cursor, total));
    auto budget = _replyCeiling - overhead;
    auto end = start;
    while (end < total && end - start < _pageEntries)
    {
        auto const cost = Wire::EncodedFileBytes(inputs.files[end]);
        if (cost > budget)
            break;
        budget -= cost;
        ++end;
    }
    // An entry that cannot fit a page on its own would stall every reader at the same cursor: refused by name.
    if (end == start && start < total)
        return Cc::RefuseWithoutCounter(UnpageableRecord,
                                        std::format("file {} of the inputs of {} has a path of {} bytes, more than one "
                                                    "reply of {} bytes carries",
                                                    start,
                                                    fingerprint,
                                                    inputs.files[start].relativePath.size(),
                                                    _replyCeiling));

    auto page = Wire::FingerprintInputsPage {
        .fingerprint = fingerprint,
        .version = _version,
        .cursor = start,
        .totalFiles = total,
        .nextCursor = end < total ? std::optional<std::uint64_t> { end } : std::nullopt,
        .inputs =
            Cc::FingerprintInputs {
                .schema = inputs.schema, .banner = inputs.banner, .driverGrammar = inputs.driverGrammar, .files = {} },
    };
    page.inputs.files.assign(inputs.files.begin() + static_cast<std::ptrdiff_t>(start),
                             inputs.files.begin() + static_cast<std::ptrdiff_t>(end));
    return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeFingerprintInputsPage(page));
}

} // namespace Pace::Node
