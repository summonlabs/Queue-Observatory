#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "qobs/policy/Classification.hpp"

namespace qobs {

/// Deterministic renderer for classification results.
///
/// The rendering is a stable, line-oriented text form with a fixed field order.
/// Two runs over the same evidence and policy produce byte-identical output, on
/// any platform and in any locale.
[[nodiscard]] std::string render_explanation(const ClassificationResult& result);

/// Compact single-line form used by tables and logs.
[[nodiscard]] std::string render_explanation_summary(const ClassificationResult& result);

/// Digest over the canonical explanation. Used to detect that a decision has
/// changed even when the resulting state has not.
[[nodiscard]] std::uint64_t explanation_digest(const ClassificationResult& result);

/// A structured view of the explanation, useful for tests and for the JSON
/// exporter, without re-parsing the text form.
struct ExplanationLine {
  std::string key{};
  std::string value{};
};

[[nodiscard]] std::vector<ExplanationLine> explanation_lines(const ClassificationResult& result);

}  // namespace qobs
