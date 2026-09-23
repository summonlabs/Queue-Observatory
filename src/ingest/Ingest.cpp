#include "qobs/ingest/Ingest.hpp"

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace qobs {

Status ingest_document(QueueStore& store, std::string_view payload, const IngestLimits& limits,
                       IngestReport& report) {
  report = IngestReport{};
  QOBS_TRY(decode_document(payload, limits, report.decode));

  for (ClassMetadataTable& table : report.decode.metadata) {
    MetadataAdmission admission;
    const Status status = store.apply_metadata(std::move(table), admission);
    if (!status.ok()) {
      return status;
    }
  }

  // A default-constructed token cannot be cancelled, which is correct here:
  // this synchronous path runs to completion or fails.
  const StopToken token;
  QOBS_TRY(store.ingest_batch(report.decode.samples, token, report.admission));
  report.accepted = true;
  return Status::success();
}

Status read_document_file(const std::string& path, std::size_t max_bytes, std::string& out) {
  out.clear();
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return Status::failure(ErrorCode::IoError, "the document size could not be read: " + path);
  }
  if (size > max_bytes) {
    return Status::failure(ErrorCode::LimitExceeded,
                           "the document is larger than the configured read limit: " + path);
  }
  std::FILE* stream = std::fopen(path.c_str(), "rb");
  if (stream == nullptr) {
    return Status::failure(ErrorCode::IoError, "the document could not be opened: " + path);
  }
  std::string buffer(static_cast<std::size_t>(size), '\0');
  const std::size_t read =
      buffer.empty() ? 0u : std::fread(buffer.data(), 1u, buffer.size(), stream);
  std::fclose(stream);
  if (read != buffer.size()) {
    return Status::failure(ErrorCode::IoError, "the document could not be read completely: " + path);
  }
  out = std::move(buffer);
  return Status::success();
}

}  // namespace qobs
