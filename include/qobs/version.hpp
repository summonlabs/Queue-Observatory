#pragma once

// Queue Observatory -- vendor-neutral observation and historical
// interpretation of network queue state.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string_view>

#define QOBS_VERSION_MAJOR 1
#define QOBS_VERSION_MINOR 0
#define QOBS_VERSION_PATCH 0
#define QOBS_VERSION_STRING "1.0.0"

// The on-disk and on-wire formats carry their own independent versions. These
// constants are the ones this build reads and writes.
#define QOBS_PERSISTENCE_FORMAT_VERSION 1u
#define QOBS_WIRE_FORMAT_VERSION 1u
#define QOBS_POLICY_VERSION 1u
#define QOBS_METADATA_SCHEMA_VERSION 1u

namespace qobs {

/// Semantic version of the runtime, as integers and as text.
struct Version {
  std::uint32_t major{0};
  std::uint32_t minor{0};
  std::uint32_t patch{0};

  friend constexpr bool operator==(const Version&, const Version&) = default;
  friend constexpr auto operator<=>(const Version&, const Version&) = default;
};

[[nodiscard]] constexpr Version version() noexcept {
  return Version{QOBS_VERSION_MAJOR, QOBS_VERSION_MINOR, QOBS_VERSION_PATCH};
}

[[nodiscard]] std::string_view version_string() noexcept;
[[nodiscard]] std::string_view build_description() noexcept;
[[nodiscard]] std::string_view build_compiler() noexcept;
[[nodiscard]] std::string_view build_configuration() noexcept;
[[nodiscard]] bool build_has_address_sanitizer() noexcept;
[[nodiscard]] bool build_has_lock_audit() noexcept;

}  // namespace qobs
