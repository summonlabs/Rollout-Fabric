// Rollout Fabric - version and ABI identification.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace rollout_fabric {

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;
inline constexpr std::string_view kVersionString = "1.0.0";

inline constexpr std::string_view kProductName = "Rollout Fabric";
inline constexpr std::string_view kProductVendor = "Summon Software Labs";
inline constexpr std::string_view kProductSlug = "rollout_fabric";

// ABI tag of the control-plane and persistence contract. A controller
// incarnation refuses to adopt a journal written under a different ABI tag,
// and a worker refuses a controller that announces a different one.
inline constexpr std::uint32_t kRuntimeAbiVersion = 1;

// Human readable one-line identification of the runtime image.
[[nodiscard]] std::string version_banner();

}  // namespace rollout_fabric
