#include "qobs/policy/Explanation.hpp"

#include "qobs/core/Text.hpp"

namespace qobs {

std::vector<ExplanationLine> explanation_lines(const ClassificationResult& result) {
  std::vector<ExplanationLine> lines;
  lines.reserve(kRuleCount + 8u);

  const auto add = [&lines](std::string key, std::string value) {
    ExplanationLine line;
    line.key = std::move(key);
    line.value = std::move(value);
    lines.push_back(std::move(line));
  };

  add("policy", result.policy_name + "@" + std::to_string(result.policy_version));
  add("queue", result.queue_text.empty() ? std::string("<unset>") : result.queue_text);
  add("state", std::string(to_string(result.state)));
  add("severity_rank", std::to_string(pressure_severity_rank(result.state)));
  add("deciding_rule", std::string(rule_name(result.deciding_rule)) + "#" +
                           std::to_string(static_cast<std::uint32_t>(result.deciding_rule)));
  add("basis", result.basis.label + " (" + std::string(to_string(result.basis.kind)) + ")");
  add("basis_value", result.basis.value.has_value() ? std::to_string(*result.basis.value)
                                                    : std::string("not_reported"));
  add("basis_limit", result.basis.limit.has_value() ? std::to_string(*result.basis.limit)
                                                    : std::string("not_reported"));
  add("assessment", describe(result.assessment));

  for (std::size_t index = 0; index < result.trace.size(); ++index) {
    const TraceEntry& entry = result.trace[index];
    std::string value;
    value.append("rule=");
    value.append(rule_name(entry.rule));
    value.append(" matched=");
    value.append(entry.matched ? "true" : "false");
    value.append(" operand=");
    value.append(std::to_string(entry.operand));
    value.append(" threshold=");
    value.append(std::to_string(entry.threshold));
    value.append(" detail=");
    value.append(entry.detail);
    add("trace." + std::to_string(index), std::move(value));
  }
  return lines;
}

std::string render_explanation(const ClassificationResult& result) {
  std::string out;
  out.reserve(512u);
  for (const ExplanationLine& line : explanation_lines(result)) {
    out.append(line.key);
    out.append(": ");
    out.append(line.value);
    out.push_back('\n');
  }
  return out;
}

std::string render_explanation_summary(const ClassificationResult& result) {
  std::string out;
  out.reserve(192u);
  out.append("state=");
  out.append(to_string(result.state));
  out.append(" rule=");
  out.append(rule_name(result.deciding_rule));
  out.append(" basis=");
  out.append(result.basis.label);
  out.append(" value=");
  out.append(result.basis.value.has_value() ? std::to_string(*result.basis.value)
                                            : std::string("not_reported"));
  out.append(" limit=");
  out.append(result.basis.limit.has_value() ? std::to_string(*result.basis.limit)
                                            : std::string("not_reported"));
  out.append(" freshness=");
  out.append(to_string(result.assessment.freshness));
  out.append(" quality=");
  out.append(to_string(result.assessment.quality));
  out.append(" policy=");
  out.append(result.policy_name);
  out.push_back('@');
  out.append(std::to_string(result.policy_version));
  return out;
}

std::uint64_t explanation_digest(const ClassificationResult& result) {
  // The digest is taken over the canonical text form, so any change in the
  // decision path -- not just the final state -- changes the digest.
  std::string canonical = render_explanation(result);
  canonical.append(render_explanation_summary(result));
  return text::fnv1a64(canonical);
}

}  // namespace qobs
