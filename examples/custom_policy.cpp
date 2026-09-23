// Example: define a policy, validate it, and show the decision it produces.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <string>

#include "qobs/policy/Classification.hpp"
#include "qobs/policy/Explanation.hpp"
#include "qobs/policy/Policy.hpp"

int main() {
  qobs::PressurePolicy policy;
  policy.name = "absolute-cells";
  policy.version = 7;
  policy.absolute.elevated_at = 200u;
  policy.absolute.pressured_at = 400u;
  policy.absolute.saturated_at = 800u;
  policy.relative.elevated_permille = 250;

  if (const qobs::Status status = qobs::validate_policy(policy); !status.ok()) {
    std::fprintf(stderr, "the policy is not coherent: %s\n", status.to_string().c_str());
    return 1;
  }

  qobs::ClassificationInput input;
  input.queue.device = qobs::DeviceId(std::string("leaf-01"));
  input.queue.port = qobs::PortId(std::string("ethernet1/1"));
  input.queue.queue = qobs::QueueId::from_raw(0);
  input.assessment.freshness = qobs::Freshness::Fresh;
  input.assessment.quality = qobs::EvidenceQuality::Complete;
  input.assessment.provenance = qobs::Provenance::Observed;
  input.assessment.authority = qobs::SourceAuthority::Primary;
  input.reported = qobs::field_bit(qobs::SampleField::OccupancyCells);
  input.occupancy_cells = 450u;
  input.drops.known = true;
  input.drops.delta = 0;

  const qobs::ClassificationResult result = qobs::classify(policy, input);
  std::fputs(qobs::render_explanation(result).c_str(), stdout);
  std::printf("digest=%llu\n",
              static_cast<unsigned long long>(qobs::explanation_digest(result)));
  return 0;
}
