#include "sidecar_client/SidecarProtocol.h"

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "sidecar protocol test failed: " << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  using namespace CorridorKey::Sidecar;

  bool ok = true;

  ok &= expect(isSafeRequestId("req-health"), "safe request ids should be accepted");
  ok &= expect(!isSafeRequestId("SecretShow"), "project-like ids should be rejected");
  ok &= expect(!isSafeRequestId("req-\nleak"), "control characters should be rejected");

  const SidecarRequest request{"req-health", "health", {}};
  const std::string encoded = encodeRequest(request);
  ok &= expect(encoded.find("\"request_id\":\"req-health\"") != std::string::npos,
               "request_id should be serialized");
  ok &= expect(encoded.find("\"command\":\"health\"") != std::string::npos,
               "command should be serialized");
  ok &= expect(encoded.find("\"payload\":{}") != std::string::npos,
               "empty payload should be serialized as an object");
  ok &= expect(encoded.find('\n') == std::string::npos,
               "encoded request should not include the transport newline");

  const SidecarResponse health = decodeResponse(
      "{\"error\":null,\"ok\":true,\"payload\":{\"backend\":\"stub\","
      "\"cache\":\"disabled\",\"gpu\":\"unknown\",\"model\":\"not_loaded\","
      "\"version\":\"0.1.0\",\"vram\":\"unknown\",\"warmup\":\"cold\"},"
      "\"request_id\":\"req-health\"}");
  ok &= expect(health.requestId == "req-health", "response request_id should parse");
  ok &= expect(health.ok, "ok response should parse");
  ok &= expect(!health.error.has_value(), "ok response should not have an error");
  ok &= expect(health.payloadValue("backend") == "stub", "payload backend should parse");
  ok &= expect(health.payloadValue("model") == "not_loaded", "payload model should parse");
  ok &= expect(health.payloadValue("warmup") == "cold", "payload warmup should parse");

  const SidecarResponse backend = decodeResponse(
      "{\"error\":null,\"ok\":true,\"payload\":{\"backend\":\"torch_cpu\","
      "\"warnings\":[\"fixture-adapter\"]},\"request_id\":\"req-backend\"}");
  ok &= expect(backend.payloadValue("warnings") == "fixture-adapter",
               "scalar payload arrays should parse for backend diagnostics");

  const SidecarResponse error = decodeResponse(
      "{\"error\":{\"code\":\"unknown_command\",\"message\":\"Unknown command\"},"
      "\"ok\":false,\"payload\":{},\"request_id\":\"req-unknown\"}");
  ok &= expect(error.requestId == "req-unknown", "error response request_id should parse");
  ok &= expect(!error.ok, "error response should parse ok=false");
  ok &= expect(error.error.has_value(), "error response should include error details");
  ok &= expect(error.error->code == "unknown_command", "error code should parse");
  ok &= expect(error.error->message == "Unknown command", "error message should parse");

  bool rejected = false;
  try {
    (void)decodeResponse("{\"request_id\":{\"project\":\"SecretShow\"},\"ok\":true,"
                         "\"payload\":{},\"error\":null}");
  } catch (const ProtocolError&) {
    rejected = true;
  }
  ok &= expect(rejected, "structured response request ids should be rejected");

  return ok ? 0 : 1;
}
