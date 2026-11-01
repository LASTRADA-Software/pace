// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file ToolchainFingerprint.hpp
/// The toolchain fingerprint lives in `<Pace/CompileCache/ToolchainFingerprint.hpp>`, header-only, so a node can
/// recompute another node's fingerprint from the inputs record it sent (`toolchain_diff`). This header keeps the
/// launcher's own spelling of the include.

#include <Pace/CompileCache/ToolchainFingerprint.hpp>
