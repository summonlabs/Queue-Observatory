#include "qobs/core/Time.hpp"

#include <chrono>
#include <cstdio>
#include <string>

#include "qobs/core/Checked.hpp"
#include "qobs/core/Text.hpp"

namespace qobs {
namespace {

/// Days from 1970-01-01 for a proleptic Gregorian civil date.
/// Standard days-from-civil algorithm; no locale, no time zone database.
constexpr std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
  year -= month <= 2u ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const auto year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned day_of_year =
      (153u * (month + (month > 2u ? static_cast<unsigned>(-3) : 9u)) + 2u) / 5u + day - 1u;
  const unsigned day_of_era =
      year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

constexpr void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month,
                               unsigned& day) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto day_of_era = static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era =
      (day_of_era - day_of_era / 1460u + day_of_era / 36524u - day_of_era / 146096u) / 365u;
  year = static_cast<std::int64_t>(year_of_era) + era * 400;
  const unsigned day_of_year =
      day_of_era - (365u * year_of_era + year_of_era / 4u - year_of_era / 100u);
  const unsigned month_prime = (5u * day_of_year + 2u) / 153u;
  day = day_of_year - (153u * month_prime + 2u) / 5u + 1u;
  month = month_prime + (month_prime < 10u ? 3u : static_cast<unsigned>(-9));
  year += month <= 2u ? 1 : 0;
}

void append_two(std::string& out, unsigned value) {
  out.push_back(static_cast<char>('0' + ((value / 10u) % 10u)));
  out.push_back(static_cast<char>('0' + (value % 10u)));
}

constexpr Nanos kNanosPerSecond = 1000000000;

}  // namespace

std::string_view to_string(ClockComparability value) noexcept {
  switch (value) {
    case ClockComparability::Comparable:
      return "comparable";
    case ClockComparability::DifferentDomains:
      return "different_domains";
    case ClockComparability::UnknownDomain:
      return "unknown_domain";
  }
  return "unmapped";
}

TimeComparison compare_observation_times(const ObservationTime& lhs,
                                         const ObservationTime& rhs) noexcept {
  TimeComparison result;
  if (!lhs.domain.valid() || !rhs.domain.valid()) {
    result.comparability = ClockComparability::UnknownDomain;
    return result;
  }
  if (lhs.domain != rhs.domain) {
    result.comparability = ClockComparability::DifferentDomains;
    return result;
  }
  result.comparability = ClockComparability::Comparable;
  result.ordering = lhs.ns < rhs.ns ? -1 : (lhs.ns > rhs.ns ? 1 : 0);
  return result;
}

ReceiveTime SystemClock::now() const { return receive_now(); }

ManualClock::ManualClock(SteadyTime start) : steady_(start), wall_(WallTime{0}) {}

ReceiveTime ManualClock::now() const { return ReceiveTime{steady_, wall_}; }

void ManualClock::advance(Nanos delta) noexcept {
  steady_.ns += delta;
  wall_.ns += delta;
}

void ManualClock::set_steady(SteadyTime value) noexcept { steady_ = value; }

void ManualClock::set_wall(WallTime value) noexcept { wall_ = value; }

SteadyTime steady_now() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return SteadyTime{
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()};
}

WallTime wall_now() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return WallTime{std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()};
}

ReceiveTime receive_now() noexcept { return ReceiveTime{steady_now(), wall_now()}; }

std::string format_wall_utc(WallTime value) {
  Nanos total = value.ns;
  Nanos seconds = total / kNanosPerSecond;
  Nanos fraction = total % kNanosPerSecond;
  if (fraction < 0) {
    fraction += kNanosPerSecond;
    seconds -= 1;
  }
  const std::int64_t days = seconds / 86400;
  Nanos second_of_day = seconds % 86400;
  if (second_of_day < 0) {
    second_of_day += 86400;
  }
  std::int64_t year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civil_from_days(days, year, month, day);
  const auto hour = static_cast<unsigned>(second_of_day / 3600);
  const auto minute = static_cast<unsigned>((second_of_day % 3600) / 60);
  const auto second = static_cast<unsigned>(second_of_day % 60);

  std::string out;
  out.reserve(40u);
  if (year < 0) {
    out.push_back('-');
    year = -year;
  }
  const auto year_value = static_cast<unsigned long long>(year);
  std::string year_text = std::to_string(year_value);
  while (year_text.size() < 4u) {
    year_text.insert(year_text.begin(), '0');
  }
  out.append(year_text);
  out.push_back('-');
  append_two(out, month);
  out.push_back('-');
  append_two(out, day);
  out.push_back('T');
  append_two(out, hour);
  out.push_back(':');
  append_two(out, minute);
  out.push_back(':');
  append_two(out, second);
  out.push_back('.');
  const auto nanos_value = static_cast<unsigned long long>(fraction);
  std::string fraction_text = std::to_string(nanos_value);
  while (fraction_text.size() < 9u) {
    fraction_text.insert(fraction_text.begin(), '0');
  }
  out.append(fraction_text);
  out.push_back('Z');
  return out;
}

std::string format_steady(SteadyTime value) {
  return "steady+" + std::to_string(static_cast<long long>(value.ns)) + "ns";
}

Result<WallTime> parse_wall_utc(std::string_view text) {
  // Accepted form: YYYY-MM-DDTHH:MM:SS[.fraction]Z, fraction of 1..9 digits.
  if (text.size() < 20u || text.back() != 'Z') {
    return Result<WallTime>::failure(ErrorCode::ParseError,
                                     "timestamp must end with Z (UTC designator)");
  }
  const std::string_view body = text.substr(0, text.size() - 1u);
  if (body.size() < 19u) {
    return Result<WallTime>::failure(ErrorCode::ParseError, "timestamp is too short");
  }
  if (body[4] != '-' || body[7] != '-' || body[10] != 'T' || body[13] != ':' || body[16] != ':') {
    return Result<WallTime>::failure(ErrorCode::ParseError,
                                     "timestamp separators are not in the expected positions");
  }
  const auto digits_at = [&body](std::size_t offset, std::size_t count,
                                 unsigned& out) -> bool {
    unsigned accumulator = 0;
    for (std::size_t index = 0; index < count; ++index) {
      const char c = body[offset + index];
      if (c < '0' || c > '9') {
        return false;
      }
      accumulator = accumulator * 10u + static_cast<unsigned>(c - '0');
    }
    out = accumulator;
    return true;
  };

  unsigned year = 0;
  unsigned month = 0;
  unsigned day = 0;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  if (!digits_at(0, 4, year) || !digits_at(5, 2, month) || !digits_at(8, 2, day) ||
      !digits_at(11, 2, hour) || !digits_at(14, 2, minute) || !digits_at(17, 2, second)) {
    return Result<WallTime>::failure(ErrorCode::ParseError,
                                     "timestamp contains a non-digit in a numeric field");
  }
  if (month < 1u || month > 12u || day < 1u || day > 31u || hour > 23u || minute > 59u ||
      second > 60u) {
    return Result<WallTime>::failure(ErrorCode::OutOfRange,
                                     "timestamp field is outside its valid range");
  }

  std::uint64_t fraction_nanos = 0;
  if (body.size() > 19u) {
    if (body[19] != '.') {
      return Result<WallTime>::failure(ErrorCode::ParseError,
                                       "expected a fractional-second separator");
    }
    const std::string_view fraction = body.substr(20);
    if (fraction.empty() || fraction.size() > 9u) {
      return Result<WallTime>::failure(ErrorCode::ParseError,
                                       "fractional seconds must have 1 to 9 digits");
    }
    unsigned value = 0;
    for (const char c : fraction) {
      if (c < '0' || c > '9') {
        return Result<WallTime>::failure(ErrorCode::ParseError,
                                         "fractional seconds must be digits");
      }
      value = value * 10u + static_cast<unsigned>(c - '0');
    }
    for (std::size_t index = fraction.size(); index < 9u; ++index) {
      value *= 10u;
    }
    fraction_nanos = value;
  }

  const std::int64_t days =
      days_from_civil(static_cast<std::int64_t>(year), month, day);
  const auto seconds_of_day =
      static_cast<std::int64_t>(hour) * 3600 + static_cast<std::int64_t>(minute) * 60 +
      static_cast<std::int64_t>(second);
  const auto total_seconds = checked::mul_i64(days, 86400);
  if (!total_seconds.has_value()) {
    return Result<WallTime>::failure(ErrorCode::Overflow, "timestamp is outside the supported range");
  }
  const auto with_day = checked::add_i64(*total_seconds, seconds_of_day);
  if (!with_day.has_value()) {
    return Result<WallTime>::failure(ErrorCode::Overflow, "timestamp is outside the supported range");
  }
  const auto scaled = checked::mul_i64(*with_day, kNanosPerSecond);
  if (!scaled.has_value()) {
    return Result<WallTime>::failure(ErrorCode::Overflow, "timestamp is outside the supported range");
  }
  const auto with_fraction =
      checked::add_i64(*scaled, static_cast<std::int64_t>(fraction_nanos));
  if (!with_fraction.has_value()) {
    return Result<WallTime>::failure(ErrorCode::Overflow, "timestamp is outside the supported range");
  }
  return WallTime{*with_fraction};
}

Result<SteadyTime> parse_steady(std::string_view text) {
  constexpr std::string_view kPrefix = "steady+";
  constexpr std::string_view kSuffix = "ns";
  if (!text::starts_with(text, kPrefix) || !text::ends_with(text, kSuffix)) {
    return Result<SteadyTime>::failure(ErrorCode::ParseError,
                                       "steady timestamp must use the steady+<nanos>ns form");
  }
  const std::string_view digits =
      text.substr(kPrefix.size(), text.size() - kPrefix.size() - kSuffix.size());
  std::int64_t value = 0;
  if (!text::parse_i64(digits, value)) {
    return Result<SteadyTime>::failure(ErrorCode::ParseError,
                                       "steady timestamp payload is not an integer");
  }
  return SteadyTime{value};
}

}  // namespace qobs
