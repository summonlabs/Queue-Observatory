#include "qobs/core/StrongId.hpp"

#include <limits>

#include "qobs/core/Text.hpp"

namespace qobs {
namespace {

bool is_control(char c) noexcept {
  const auto value = static_cast<unsigned char>(c);
  return value < 0x20u || value == 0x7Fu;
}

}  // namespace

template <class Tag, class Value>
std::string StrongId<Tag, Value>::to_string() const {
  return std::to_string(static_cast<unsigned long long>(value_));
}

template <class Tag, class Value>
std::optional<StrongId<Tag, Value>> StrongId<Tag, Value>::parse(std::string_view text) noexcept {
  std::uint64_t raw = 0;
  const bool ok = text::starts_with(text, "0x") || text::starts_with(text, "0X")
                      ? text::parse_hex_u64(text, raw)
                      : text::parse_u64(text, raw);
  if (!ok) {
    return std::nullopt;
  }
  if (raw > static_cast<std::uint64_t>(std::numeric_limits<Value>::max())) {
    return std::nullopt;
  }
  return StrongId(static_cast<Value>(raw));
}

template <class Tag>
Result<StrongNameId<Tag>> StrongNameId<Tag>::create(std::string_view text) {
  if (text.empty()) {
    return Result<StrongNameId<Tag>>::failure(ErrorCode::InvalidArgument,
                                              "identity must not be empty");
  }
  if (text.size() > kMaxIdentityLength) {
    return Result<StrongNameId<Tag>>::failure(
        ErrorCode::LimitExceeded,
        "identity exceeds the maximum length of " + std::to_string(kMaxIdentityLength));
  }
  for (const char c : text) {
    if (is_control(c)) {
      return Result<StrongNameId<Tag>>::failure(ErrorCode::InvalidArgument,
                                                "identity must not contain control characters");
    }
    // Device and port names appear inside the canonical queue path text form
    // ("device|port|queue"), so they must not contain the path separator.
    if constexpr (std::is_same_v<Tag, DeviceTag> || std::is_same_v<Tag, PortTag>) {
      if (c == '|') {
        return Result<StrongNameId<Tag>>::failure(
            ErrorCode::InvalidArgument,
            "device and port names must not contain the queue path separator '|'");
      }
    }
  }
  return StrongNameId<Tag>(std::string(text));
}

template class StrongId<QueueTag, std::uint32_t>;
template class StrongId<TrafficClassTag, std::uint16_t>;
template class StrongId<GenerationTag, std::uint64_t>;
template class StrongId<SequenceTag, std::uint64_t>;
template class StrongId<RevisionTag, std::uint64_t>;
template class StrongId<BatchTag, std::uint64_t>;
template class StrongId<RecordTag, std::uint64_t>;

template class StrongNameId<DeviceTag>;
template class StrongNameId<PortTag>;
template class StrongNameId<SchedulingClassTag>;
template class StrongNameId<SourceTag>;
template class StrongNameId<IncarnationTag>;
template class StrongNameId<ClockDomainTag>;
template class StrongNameId<QueueNameTag>;

}  // namespace qobs
