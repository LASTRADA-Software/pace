// SPDX-License-Identifier: Apache-2.0
#include <Pace/Protocol/KeyspaceNotifier.hpp>
#include <Pace/Server/DaemonStartupGate.hpp>

#include <format>

namespace Pace
{

std::optional<std::string> ServingRulesRejection(CliOutcome outcome, EffectiveConfig const& assembled)
{
    if (!JudgedByServingRules(outcome))
        return std::nullopt;

    // Asked of the assembly, which knows what was named in EITHER source.
    if (auto const shape = ValidateBindFlagShape(assembled); !shape.has_value())
        return shape.error().context;

    auto const& effective = assembled.Configuration();
    if (auto const eventsMask = ParseKeyspaceEvents(effective.notifyKeyspaceEvents); !eventsMask.has_value())
        return std::format(
            "invalid --notify-keyspace-events '{}': {}", effective.notifyKeyspaceEvents, eventsMask.error().context);

    return DaemonStartupRejection(effective);
}

} // namespace Pace
