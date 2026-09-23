#include "qobs/transport/Server.hpp"

#include <vector>

#include "qobs/core/Json.hpp"
#include "qobs/version.hpp"

namespace qobs {

TransportClient::~TransportClient() {
  const Status ignored = close();
  (void)ignored;
}

Status TransportClient::connect_to(const std::string& host, std::uint16_t port,
                                   std::string client_name) {
  QOBS_TRY(validate_limits(limits_));
  auto connection = Socket::connect_to(host, port);
  if (!connection.has_value()) {
    return Status(connection.error());
  }
  connection_ = std::move(connection).value();
  reader_ = transport::FrameReader(limits_);

  JsonWriter writer;
  QOBS_TRY(writer.begin_object());
  QOBS_TRY(writer.member_string("client", std::move(client_name)));
  QOBS_TRY(writer.member_u64("wire_version", QOBS_WIRE_FORMAT_VERSION));
  QOBS_TRY(writer.member_string("client_version", QOBS_VERSION_STRING));
  QOBS_TRY(writer.end_object());
  QOBS_TRY(send_frame(transport::FrameType::Hello, writer.take()));

  Frame welcome;
  QOBS_TRY(receive_frame(welcome));
  if (welcome.type != transport::FrameType::Welcome) {
    return Status::failure(ErrorCode::Internal,
                           "the server did not answer the handshake with a welcome frame");
  }
  welcome_ = welcome.payload;
  return Status::success();
}

Status TransportClient::send_frame(transport::FrameType type, std::string payload) {
  transport::Frame frame;
  frame.type = type;
  frame.payload = std::move(payload);
  std::vector<std::byte> encoded;
  QOBS_TRY(transport::encode_frame(frame, limits_, encoded));
  return connection_.send_all(encoded);
}

Status TransportClient::receive_frame(transport::Frame& frame) {
  std::vector<std::byte> buffer(64u * 1024u);
  for (;;) {
    bool produced = false;
    QOBS_TRY(reader_.next(frame, produced));
    if (produced) {
      return Status::success();
    }
    std::size_t received = 0;
    QOBS_TRY(connection_.receive(buffer, received));
    if (received == 0) {
      return Status::failure(ErrorCode::IoError,
                             "the server closed the connection before sending a complete frame");
    }
    QOBS_TRY(reader_.append(std::span<const std::byte>(buffer.data(), received)));
  }
}

Status TransportClient::send_batch(std::string_view document, BatchReply& reply) {
  reply = BatchReply{};
  if (document.size() > limits_.max_frame_bytes) {
    return Status::failure(ErrorCode::LimitExceeded,
                           "the document exceeds the configured frame size");
  }
  QOBS_TRY(send_frame(transport::FrameType::Batch, std::string(document)));
  Frame response;
  QOBS_TRY(receive_frame(response));
  if (response.type == transport::FrameType::Error) {
    return Status::failure(ErrorCode::Internal,
                           "the server rejected the batch frame: " + response.payload);
  }
  if (response.type != transport::FrameType::BatchResult) {
    return Status::failure(ErrorCode::Internal,
                           "the server answered a batch frame with an unexpected frame type");
  }
  const auto document_value = parse_json(response.payload, JsonLimits{});
  if (!document_value.has_value()) {
    return Status::failure(ErrorCode::ParseError,
                           "the batch result frame carries an unreadable payload");
  }
  const JsonValue& object = document_value.value();
  const auto read_u64 = [&object](std::string_view key) -> std::uint64_t {
    const JsonValue* value = object.find(key);
    if (value == nullptr) {
      return 0;
    }
    if (value->is_uint()) {
      return value->as_uint();
    }
    if (value->is_int() && value->as_int() >= 0) {
      return static_cast<std::uint64_t>(value->as_int());
    }
    return 0;
  };
  reply.ticket = read_u64("ticket");
  reply.report.decode.lines = static_cast<std::size_t>(read_u64("lines"));
  reply.report.decode.sample_count = static_cast<std::size_t>(read_u64("samples_decoded"));
  reply.report.decode.metadata_records = static_cast<std::size_t>(read_u64("metadata_records"));
  reply.report.decode.malformed = static_cast<std::size_t>(read_u64("malformed"));
  reply.report.decode.unsupported = static_cast<std::size_t>(read_u64("unsupported"));
  reply.report.decode.version_mismatches =
      static_cast<std::size_t>(read_u64("version_mismatches"));
  reply.report.admission.presented = static_cast<std::size_t>(read_u64("presented"));
  reply.report.admission.accepted = static_cast<std::size_t>(read_u64("accepted"));
  reply.report.admission.rejected = static_cast<std::size_t>(read_u64("rejected"));
  reply.report.admission.fenced = static_cast<std::size_t>(read_u64("fenced"));
  reply.report.admission.duplicates = static_cast<std::size_t>(read_u64("duplicates"));
  reply.report.admission.queues_created = static_cast<std::size_t>(read_u64("queues_created"));
  if (const JsonValue* applied = object.find("applied"); applied != nullptr && applied->is_bool()) {
    reply.report.accepted = applied->as_bool();
  }
  if (const JsonValue* code = object.find("error_code");
      code != nullptr && code->is_string()) {
    reply.error_code = code->as_string();
  }
  if (const JsonValue* message = object.find("error_message");
      message != nullptr && message->is_string()) {
    reply.error_message = message->as_string();
  }
  return Status::success();
}

Status TransportClient::request_status(std::string& payload) {
  QOBS_TRY(send_frame(transport::FrameType::StatusRequest, std::string{}));
  Frame response;
  QOBS_TRY(receive_frame(response));
  if (response.type != transport::FrameType::StatusReply) {
    return Status::failure(ErrorCode::Internal,
                           "the server answered a status request with an unexpected frame type");
  }
  payload = std::move(response.payload);
  return Status::success();
}

Status TransportClient::close() {
  if (!connection_.valid()) {
    return Status::success();
  }
  const Status ignored = send_frame(transport::FrameType::Bye, std::string{});
  (void)ignored;
  connection_.close();
  return Status::success();
}

}  // namespace qobs
