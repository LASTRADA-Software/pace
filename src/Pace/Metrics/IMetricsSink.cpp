// SPDX-License-Identifier: Apache-2.0
#include <Pace/Metrics/IMetricsSink.hpp>

namespace Pace::CounterSkew
{

// The one definition, for the count this file saw. A unit compiled against any other count names a symbol nothing
// defines, and the link fails there (#1361). Dependency-free, so pace-cc, which does not link Pace,
// compiles it in as well.
bool RequireExtent(Extent<CounterCount> /*extent*/) noexcept
{
    return true;
}

} // namespace Pace::CounterSkew
