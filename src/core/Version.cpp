#include "qobs/version.hpp"

namespace qobs {

std::string_view version_string() noexcept { return QOBS_VERSION_STRING; }

std::string_view build_description() noexcept {
  return "Queue Observatory " QOBS_VERSION_STRING
         " -- vendor-neutral network queue observation runtime";
}

std::string_view build_compiler() noexcept {
#if defined(_MSC_VER)
  return "msvc";
#elif defined(__clang__)
  return "clang";
#elif defined(__GNUC__)
  return "gcc";
#else
  return "unknown";
#endif
}

std::string_view build_configuration() noexcept {
#if defined(NDEBUG)
  return "release";
#else
  return "debug";
#endif
}

bool build_has_address_sanitizer() noexcept {
#if defined(__SANITIZE_ADDRESS__)
  return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
  return true;
#else
  return false;
#endif
#else
  return false;
#endif
}

bool build_has_lock_audit() noexcept {
#if defined(QOBS_ENABLE_LOCK_AUDIT) && QOBS_ENABLE_LOCK_AUDIT
  return true;
#else
  return false;
#endif
}

}  // namespace qobs
