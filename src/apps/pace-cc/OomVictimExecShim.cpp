// SPDX-License-Identifier: Apache-2.0
//
// Test helper for OomVictimExec_test.cpp: the shim alone, with the production writer, standing in for `pace-node`
// -- whose `main` begins the same way -- so the property is read off real processes without the node's binary.

#include "OomVictimExec.hpp"

#include <cstddef>
#include <span>

int main(int argc, char** argv)
{
    Pace::Cc::ProcSelfOomScoreWriter writer;
    auto const failed = Pace::Cc::RunOomVictimExecIfInvoked(
        std::span<char const* const> { const_cast<char const* const*>(argv), static_cast<std::size_t>(argc) }, writer);
    // Reached only when the shim could not run its compiler, or when this was not invoked as the shim at all --
    // which a test spawning it through a runner never does.
    return failed.value_or(2);
}
