// SPDX-License-Identifier: Apache-2.0
#include <Pace/Platform/DaemonControls.hpp>

namespace Pace
{

DaemonControls& DaemonControls::Instance() noexcept
{
    static DaemonControls instance;
    return instance;
}

} // namespace Pace
