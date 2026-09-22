// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/version.hpp"

#include <string>

namespace rollout_fabric {

std::string version_banner() {
  std::string banner;
  banner.reserve(96);
  banner.append(kProductName);
  banner.append(" ");
  banner.append(kVersionString);
  banner.append(" (");
  banner.append(kProductSlug);
  banner.append(", abi ");
  banner.append(std::to_string(kRuntimeAbiVersion));
  banner.append(", ");
  banner.append(kProductVendor);
  banner.append(")");
  return banner;
}

}  // namespace rollout_fabric
