#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "qobs/core/Limits.hpp"
#include "qobs/core/Result.hpp"
#include "qobs/ingest/Wire.hpp"
#include "qobs/store/Store.hpp"

namespace qobs {

struct IngestReport {
  DecodeOutcome decode{};
  BatchAdmission admission{};
  bool accepted{false};
};

/// Decode a document and hand every sample to the store. The decode outcome is
/// always reported, even when the document contained nothing usable.
[[nodiscard]] Status ingest_document(QueueStore& store, std::string_view payload,
                                     const IngestLimits& limits, IngestReport& report);

/// Read a whole file into memory with an explicit byte bound. The bound is a
/// parameter, never a property of the file.
[[nodiscard]] Status read_document_file(const std::string& path, std::size_t max_bytes,
                                        std::string& out);

}  // namespace qobs
