// Queue Observatory transport client.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A real client process: it connects over TCP, performs the handshake, sends
// canonical observation documents and prints exactly what the server reported.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "qobs/core/Text.hpp"
#include "qobs/ingest/Ingest.hpp"
#include "qobs/transport/Server.hpp"

namespace {

struct Arguments {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  std::string document_path{};
  std::string document_text{};
  std::size_t repeat{1};
  bool request_status{false};
  bool expect_accept{false};
  std::size_t expect_accepted{0};
};

bool parse(int argc, char** argv, Arguments& arguments) {
  for (int index = 1; index < argc; ++index) {
    const std::string token = argv[index];
    const auto next = [&](std::string& out) -> bool {
      if (index + 1 >= argc) {
        return false;
      }
      out = argv[++index];
      return true;
    };
    std::string text;
    if (token == "--host") {
      if (!next(arguments.host)) {
        return false;
      }
    } else if (token == "--port") {
      if (!next(text)) {
        return false;
      }
      std::uint64_t value = 0;
      if (!qobs::text::parse_u64(text, value) || value > 65535u) {
        return false;
      }
      arguments.port = static_cast<std::uint16_t>(value);
    } else if (token == "--document") {
      if (!next(arguments.document_path)) {
        return false;
      }
    } else if (token == "--document-text") {
      if (!next(arguments.document_text)) {
        return false;
      }
    } else if (token == "--repeat") {
      if (!next(text)) {
        return false;
      }
      std::uint64_t value = 0;
      if (!qobs::text::parse_u64(text, value) || value == 0u) {
        return false;
      }
      arguments.repeat = static_cast<std::size_t>(value);
    } else if (token == "--status") {
      arguments.request_status = true;
    } else if (token == "--expect-accept") {
      arguments.expect_accept = true;
    } else if (token == "--expect-accepted") {
      if (!next(text)) {
        return false;
      }
      std::uint64_t value = 0;
      if (!qobs::text::parse_u64(text, value)) {
        return false;
      }
      arguments.expect_accepted = static_cast<std::size_t>(value);
    } else {
      std::fprintf(stderr, "qobs-transport-client: unknown option '%s'\n", token.c_str());
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Arguments arguments;
  if (!parse(argc, argv, arguments)) {
    std::fputs(
        "usage: qobs-transport-client --port N [--host H] [--document P | --document-text T]\n"
        "                             [--repeat N] [--status] [--expect-accept]\n"
        "                             [--expect-accepted N]\n",
        stderr);
    return 2;
  }
  if (arguments.port == 0 && arguments.document_path.empty()) {
    std::fputs("qobs-transport-client: --port is required\n", stderr);
    return 2;
  }

  std::string document = arguments.document_text;
  if (!arguments.document_path.empty()) {
    const qobs::Status read = qobs::read_document_file(arguments.document_path, 1u << 20, document);
    if (!read.ok()) {
      std::fprintf(stderr, "qobs-transport-client: %s\n", read.to_string().c_str());
      return 1;
    }
  }

  qobs::TransportClient client;
  const qobs::Status connected =
      client.connect_to(arguments.host, arguments.port, "qobs-transport-client");
  if (!connected.ok()) {
    std::fprintf(stderr, "qobs-transport-client: %s\n", connected.to_string().c_str());
    return 1;
  }
  std::printf("welcome=%s\n", client.welcome().c_str());

  std::size_t accepted_total = 0;
  if (!document.empty()) {
    for (std::size_t iteration = 0; iteration < arguments.repeat; ++iteration) {
      qobs::BatchReply reply;
      const qobs::Status sent = client.send_batch(document, reply);
      if (!sent.ok()) {
        std::fprintf(stderr, "qobs-transport-client: %s\n", sent.to_string().c_str());
        return 1;
      }
      std::printf(
          "ticket=%llu lines=%zu samples=%zu accepted=%zu rejected=%zu fenced=%zu "
          "duplicates=%zu applied=%s\n",
          static_cast<unsigned long long>(reply.ticket), reply.report.decode.lines,
          reply.report.decode.sample_count, reply.report.admission.accepted,
          reply.report.admission.rejected, reply.report.admission.fenced,
          reply.report.admission.duplicates, reply.report.accepted ? "true" : "false");
      accepted_total += reply.report.admission.accepted;
    }
  }

  if (arguments.request_status) {
    std::string payload;
    const qobs::Status status = client.request_status(payload);
    if (!status.ok()) {
      std::fprintf(stderr, "qobs-transport-client: %s\n", status.to_string().c_str());
      return 1;
    }
    std::printf("status=%s\n", payload.c_str());
  }

  const qobs::Status closed = client.close();
  if (!closed.ok()) {
    std::fprintf(stderr, "qobs-transport-client: %s\n", closed.to_string().c_str());
    return 1;
  }

  if (arguments.expect_accept && accepted_total == 0u) {
    std::fputs("qobs-transport-client: expected at least one accepted sample\n", stderr);
    return 6;
  }
  if (arguments.expect_accepted != 0u && accepted_total != arguments.expect_accepted) {
    std::fprintf(stderr, "qobs-transport-client: expected %zu accepted samples, saw %zu\n",
                 arguments.expect_accepted, accepted_total);
    return 7;
  }
  return 0;
}
