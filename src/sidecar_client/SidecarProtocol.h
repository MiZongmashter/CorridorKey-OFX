#pragma once

#include <cmath>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CorridorKey::Sidecar {

class ProtocolError : public std::runtime_error {
 public:
  explicit ProtocolError(const std::string& message) : std::runtime_error(message) {}
};

struct SidecarRequest {
  std::string requestId;
  std::string command;
  std::map<std::string, std::string> payload;
};

struct InferRequestContract {
  std::string jobId;
  std::string frameId;
  int renderWindowX1 = 0;
  int renderWindowY1 = 0;
  int renderWindowX2 = 0;
  int renderWindowY2 = 0;
  std::string sourceFrameBlobPath;
  std::string alphaHintFrameBlobPath;
  std::string alphaHintSource;
  std::string screenColor;
  std::string quality;
  std::string inputColorSpace;
  double despillStrength = 5.0;
  std::string backend;
  std::string outputMode;
};

struct WarmupRequestContract {
  std::string jobId;
  std::string backend;
  std::string quality;
};

struct SidecarError {
  std::string code;
  std::string message;
};

struct SidecarResponse {
  std::string requestId;
  bool requestIdWasNull = false;
  bool ok = false;
  std::map<std::string, std::string> payload;
  std::optional<SidecarError> error;

  std::string payloadValue(const std::string& key) const {
    const auto found = payload.find(key);
    return found == payload.end() ? std::string{} : found->second;
  }
};

inline bool isSafeRequestId(std::string_view requestId) {
  if (requestId.size() < 5 || requestId.size() > 128) {
    return false;
  }
  if (requestId.substr(0, 4) != "req-") {
    return false;
  }

  const auto isLowerAlnum = [](unsigned char value) {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
  };
  if (!isLowerAlnum(static_cast<unsigned char>(requestId[4]))) {
    return false;
  }

  for (std::size_t index = 5; index < requestId.size(); ++index) {
    const unsigned char value = static_cast<unsigned char>(requestId[index]);
    if (!isLowerAlnum(value) && value != '_' && value != '.' && value != ':' && value != '-') {
      return false;
    }
  }
  return true;
}

inline bool isSafeJobId(std::string_view jobId) {
  if (jobId.size() < 5 || jobId.size() > 128) {
    return false;
  }
  if (jobId.substr(0, 4) != "job-") {
    return false;
  }

  const auto isLowerAlnum = [](unsigned char value) {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
  };
  if (!isLowerAlnum(static_cast<unsigned char>(jobId[4]))) {
    return false;
  }

  for (std::size_t index = 5; index < jobId.size(); ++index) {
    const unsigned char value = static_cast<unsigned char>(jobId[index]);
    if (!isLowerAlnum(value) && value != '_' && value != '.' && value != ':' && value != '-') {
      return false;
    }
  }
  return true;
}

inline const char* outputModeToken(int choice) {
  switch (choice) {
    case 0:
      return "processed_rgba";
    case 1:
      return "matte";
    case 2:
      return "straight_fg";
    case 3:
      return "alpha_hint_view";
    case 4:
      return "checker_comp";
    case 5:
      return "status";
    default:
      throw ProtocolError("invalid output mode choice");
  }
}

inline const char* screenColorToken(int choice) {
  switch (choice) {
    case 0:
      return "auto";
    case 1:
      return "green";
    case 2:
      return "blue";
    default:
      throw ProtocolError("invalid screen color choice");
  }
}

inline const char* qualityToken(int choice) {
  switch (choice) {
    case 0:
      return "draft_512";
    case 1:
      return "high_1024";
    case 2:
      return "full_2048";
    default:
      throw ProtocolError("invalid quality choice");
  }
}

inline const char* inputColorSpaceToken(int choice) {
  switch (choice) {
    case 0:
      return "host_managed";
    case 1:
      return "srgb_rec709";
    case 2:
      return "linear";
    default:
      throw ProtocolError("invalid input color space choice");
  }
}

inline const char* alphaHintSourceToken(int choice) {
  switch (choice) {
    case 0:
      return "external";
    case 1:
      return "source_alpha";
    case 2:
      return "red_channel";
    case 3:
      return "rough_fallback";
    default:
      throw ProtocolError("invalid alpha hint source choice");
  }
}

inline const char* backendToken(int choice) {
  switch (choice) {
    case 0:
      return "auto";
    case 1:
      return "torch_cuda";
    case 2:
      return "torch_cpu";
    case 3:
      return "torch_mps";
    case 4:
      return "mlx";
    default:
      throw ProtocolError("invalid backend choice");
  }
}

inline bool isKnownOutputMode(std::string_view value) {
  return value == "processed_rgba" || value == "matte" || value == "straight_fg" ||
         value == "alpha_hint_view" || value == "checker_comp" || value == "status";
}

inline bool isKnownScreenColor(std::string_view value) {
  return value == "auto" || value == "green" || value == "blue";
}

inline bool isKnownQuality(std::string_view value) {
  return value == "draft_512" || value == "high_1024" || value == "full_2048";
}

inline bool isKnownInputColorSpace(std::string_view value) {
  return value == "host_managed" || value == "srgb_rec709" || value == "linear";
}

inline bool isKnownAlphaHintSource(std::string_view value) {
  return value == "external" || value == "source_alpha" || value == "red_channel" ||
         value == "rough_fallback";
}

inline bool isKnownBackend(std::string_view value) {
  return value == "auto" || value == "torch_cuda" || value == "torch_cpu" ||
         value == "torch_mps" || value == "mlx" || value == "stub";
}

inline bool isValidDespillStrength(double value) {
  return std::isfinite(value) && value >= 0.0 && value <= 10.0;
}

inline std::string formatContractDouble(double value) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::fixed << std::setprecision(3) << value;
  std::string text = output.str();
  while (text.size() > 1 && text.back() == '0') {
    text.pop_back();
  }
  if (!text.empty() && text.back() == '.') {
    text.pop_back();
  }
  return text;
}

inline std::string unsignedHex64(std::uint64_t value) {
  std::ostringstream output;
  output << std::hex << std::nouppercase << std::setfill('0') << std::setw(16)
         << value;
  return output.str();
}

inline std::string requestHashForPayload(
    const std::map<std::string, std::string>& payload) {
  std::uint64_t hash = 14695981039346656037ULL;
  const auto update = [&hash](std::string_view value) {
    for (const unsigned char c : value) {
      hash ^= static_cast<std::uint64_t>(c);
      hash *= 1099511628211ULL;
    }
  };

  for (const auto& [key, value] : payload) {
    if (key == "request_hash") {
      continue;
    }
    update(key);
    update(std::string_view{"\0", 1});
    update(value);
    update(std::string_view{"\0", 1});
  }
  return unsignedHex64(hash);
}

inline std::optional<std::filesystem::path> tempFrameBlobJobPath(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }

  std::error_code ec;
  const std::filesystem::path rawTempRoot = std::filesystem::temp_directory_path(ec);
  if (ec || rawTempRoot.empty()) {
    return std::nullopt;
  }
  std::filesystem::path tempRoot = std::filesystem::weakly_canonical(rawTempRoot, ec);
  if (ec || tempRoot.empty()) {
    return std::nullopt;
  }
  tempRoot = tempRoot.lexically_normal();
  while (!tempRoot.has_filename()) {
    const std::filesystem::path parent = tempRoot.parent_path();
    if (parent.empty() || parent == tempRoot) {
      break;
    }
    tempRoot = parent;
  }

  const std::filesystem::path rawPath{std::string{value}};
  if (!rawPath.is_absolute()) {
    return std::nullopt;
  }
  std::filesystem::path path = std::filesystem::weakly_canonical(rawPath, ec);
  if (ec || path.empty()) {
    return std::nullopt;
  }
  path = path.lexically_normal();
  if (path.extension() != ".ckfb") {
    return std::nullopt;
  }

  auto tempIt = tempRoot.begin();
  auto pathIt = path.begin();
  for (; tempIt != tempRoot.end(); ++tempIt, ++pathIt) {
    if (pathIt == path.end() || *pathIt != *tempIt) {
      return std::nullopt;
    }
  }
  if (pathIt == path.end()) {
    return std::nullopt;
  }
  const std::string jobName = pathIt->string();
  if (jobName.rfind("corridorkey-render-", 0) != 0) {
    return std::nullopt;
  }
  const std::filesystem::path jobPath = tempRoot / *pathIt;
  ++pathIt;
  if (pathIt == path.end()) {
    return std::nullopt;
  }
  ++pathIt;
  if (pathIt != path.end()) {
    return std::nullopt;
  }
  return jobPath.lexically_normal();
}

inline bool isTempFrameBlobPath(std::string_view value) {
  return tempFrameBlobJobPath(value).has_value();
}

inline void validateInferRequestContract(const InferRequestContract& request) {
  if (!isSafeJobId(request.jobId)) {
    throw ProtocolError("infer request job_id is invalid");
  }
  if (request.frameId.empty()) {
    throw ProtocolError("infer request frame_id must not be empty");
  }
  if (request.renderWindowX2 <= request.renderWindowX1 ||
      request.renderWindowY2 <= request.renderWindowY1) {
    throw ProtocolError("infer request render window must be non-empty");
  }
  const std::optional<std::filesystem::path> sourceJob =
      tempFrameBlobJobPath(request.sourceFrameBlobPath);
  const std::optional<std::filesystem::path> alphaHintJob =
      tempFrameBlobJobPath(request.alphaHintFrameBlobPath);
  if (!sourceJob || !alphaHintJob || *sourceJob != *alphaHintJob) {
    throw ProtocolError("infer request frame paths must be temp frame blob paths");
  }
  if (!isKnownAlphaHintSource(request.alphaHintSource)) {
    throw ProtocolError("infer request alpha hint source is invalid");
  }
  if (!isKnownScreenColor(request.screenColor)) {
    throw ProtocolError("infer request screen color is invalid");
  }
  if (!isKnownQuality(request.quality)) {
    throw ProtocolError("infer request quality is invalid");
  }
  if (!isKnownInputColorSpace(request.inputColorSpace)) {
    throw ProtocolError("infer request input color space is invalid");
  }
  if (!isValidDespillStrength(request.despillStrength)) {
    throw ProtocolError("infer request despill strength is out of range");
  }
  if (!isKnownBackend(request.backend)) {
    throw ProtocolError("infer request backend is invalid");
  }
  if (!isKnownOutputMode(request.outputMode)) {
    throw ProtocolError("infer request output mode is invalid");
  }
}

inline std::map<std::string, std::string> inferRequestPayload(
    const InferRequestContract& request) {
  validateInferRequestContract(request);

  std::map<std::string, std::string> payload = {
      {"alpha_hint_frame_blob_path", request.alphaHintFrameBlobPath},
      {"alpha_hint_source", request.alphaHintSource},
      {"backend", request.backend},
      {"despill_strength", formatContractDouble(request.despillStrength)},
      {"frame_id", request.frameId},
      {"input_color_space", request.inputColorSpace},
      {"job_id", request.jobId},
      {"output_mode", request.outputMode},
      {"quality", request.quality},
      {"render_window_x1", std::to_string(request.renderWindowX1)},
      {"render_window_x2", std::to_string(request.renderWindowX2)},
      {"render_window_y1", std::to_string(request.renderWindowY1)},
      {"render_window_y2", std::to_string(request.renderWindowY2)},
      {"screen_color", request.screenColor},
      {"source_frame_blob_path", request.sourceFrameBlobPath},
  };
  payload.emplace("request_hash", requestHashForPayload(payload));
  return payload;
}

inline SidecarRequest makeInferRequest(std::string requestId,
                                       const InferRequestContract& request) {
  return SidecarRequest{std::move(requestId), "infer", inferRequestPayload(request)};
}

inline std::map<std::string, std::string> warmupRequestPayload(
    const WarmupRequestContract& request) {
  if (!isSafeJobId(request.jobId)) {
    throw ProtocolError("warmup request job_id is invalid");
  }
  if (!isKnownBackend(request.backend)) {
    throw ProtocolError("warmup request backend is invalid");
  }
  if (!isKnownQuality(request.quality)) {
    throw ProtocolError("warmup request quality is invalid");
  }
  return {
      {"backend", request.backend},
      {"job_id", request.jobId},
      {"quality", request.quality},
  };
}

inline SidecarRequest makeWarmupRequest(std::string requestId,
                                        const WarmupRequestContract& request) {
  return SidecarRequest{std::move(requestId), "warmup", warmupRequestPayload(request)};
}

inline SidecarRequest makeCancelRequest(std::string requestId, std::string jobId) {
  if (!isSafeJobId(jobId)) {
    throw ProtocolError("cancel request job_id is invalid");
  }
  return SidecarRequest{std::move(requestId), "cancel", {{"job_id", std::move(jobId)}}};
}

inline std::string escapeJsonString(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  for (const unsigned char c : value) {
    switch (c) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (c < 0x20) {
          const char* digits = "0123456789abcdef";
          escaped += "\\u00";
          escaped.push_back(digits[(c >> 4) & 0x0f]);
          escaped.push_back(digits[c & 0x0f]);
        } else {
          escaped.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return escaped;
}

inline std::string encodeRequest(const SidecarRequest& request) {
  if (!isSafeRequestId(request.requestId)) {
    throw ProtocolError("unsafe sidecar request_id");
  }
  if (request.command.empty()) {
    throw ProtocolError("sidecar command must not be empty");
  }

  std::string encoded = "{\"command\":\"";
  encoded += escapeJsonString(request.command);
  encoded += "\",\"payload\":{";

  bool first = true;
  for (const auto& [key, value] : request.payload) {
    if (!first) {
      encoded += ',';
    }
    first = false;
    encoded += '"';
    encoded += escapeJsonString(key);
    encoded += "\":\"";
    encoded += escapeJsonString(value);
    encoded += '"';
  }

  encoded += "},\"request_id\":\"";
  encoded += escapeJsonString(request.requestId);
  encoded += "\"}";
  return encoded;
}

namespace Detail {

struct JsonValue {
  enum class Type {
    Null,
    Bool,
    String,
    Array,
    Object,
  };

  Type type = Type::Null;
  bool boolValue = false;
  std::string stringValue;
  std::vector<JsonValue> arrayValue;
  std::map<std::string, JsonValue> objectValue;
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view input) : input_(input) {}

  JsonValue parse() {
    skipWhitespace();
    JsonValue value = parseValue();
    skipWhitespace();
    if (position_ != input_.size()) {
      throw ProtocolError("unexpected trailing JSON content");
    }
    return value;
  }

 private:
  JsonValue parseValue() {
    skipWhitespace();
    if (position_ >= input_.size()) {
      throw ProtocolError("unexpected end of JSON");
    }
    const char c = input_[position_];
    if (c == '{') {
      return parseObject();
    }
    if (c == '[') {
      return parseArray();
    }
    if (c == '"') {
      JsonValue value;
      value.type = JsonValue::Type::String;
      value.stringValue = parseString();
      return value;
    }
    if (consumeLiteral("true")) {
      JsonValue value;
      value.type = JsonValue::Type::Bool;
      value.boolValue = true;
      return value;
    }
    if (consumeLiteral("false")) {
      JsonValue value;
      value.type = JsonValue::Type::Bool;
      value.boolValue = false;
      return value;
    }
    if (consumeLiteral("null")) {
      return {};
    }
    throw ProtocolError("unsupported JSON value in sidecar response");
  }

  JsonValue parseObject() {
    expect('{');
    JsonValue value;
    value.type = JsonValue::Type::Object;
    skipWhitespace();
    if (peek('}')) {
      ++position_;
      return value;
    }

    while (true) {
      skipWhitespace();
      if (!peek('"')) {
        throw ProtocolError("JSON object key must be a string");
      }
      std::string key = parseString();
      skipWhitespace();
      expect(':');
      value.objectValue.emplace(std::move(key), parseValue());
      skipWhitespace();
      if (peek('}')) {
        ++position_;
        break;
      }
      expect(',');
    }
    return value;
  }

  JsonValue parseArray() {
    expect('[');
    JsonValue value;
    value.type = JsonValue::Type::Array;
    skipWhitespace();
    if (peek(']')) {
      ++position_;
      return value;
    }

    while (true) {
      value.arrayValue.push_back(parseValue());
      skipWhitespace();
      if (peek(']')) {
        ++position_;
        break;
      }
      expect(',');
    }
    return value;
  }

  std::string parseString() {
    expect('"');
    std::string result;
    while (position_ < input_.size()) {
      const unsigned char c = static_cast<unsigned char>(input_[position_++]);
      if (c == '"') {
        return result;
      }
      if (c < 0x20) {
        throw ProtocolError("control character in JSON string");
      }
      if (c != '\\') {
        result.push_back(static_cast<char>(c));
        continue;
      }
      if (position_ >= input_.size()) {
        throw ProtocolError("unterminated JSON escape");
      }
      const char escaped = input_[position_++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          result.push_back(escaped);
          break;
        case 'b':
          result.push_back('\b');
          break;
        case 'f':
          result.push_back('\f');
          break;
        case 'n':
          result.push_back('\n');
          break;
        case 'r':
          result.push_back('\r');
          break;
        case 't':
          result.push_back('\t');
          break;
        case 'u':
          for (int index = 0; index < 4; ++index) {
            if (position_ >= input_.size() ||
                !std::isxdigit(static_cast<unsigned char>(input_[position_++]))) {
              throw ProtocolError("invalid JSON unicode escape");
            }
          }
          result.push_back('?');
          break;
        default:
          throw ProtocolError("invalid JSON escape");
      }
    }
    throw ProtocolError("unterminated JSON string");
  }

  bool consumeLiteral(std::string_view literal) {
    if (input_.substr(position_, literal.size()) != literal) {
      return false;
    }
    position_ += literal.size();
    return true;
  }

  bool peek(char expected) const {
    return position_ < input_.size() && input_[position_] == expected;
  }

  void expect(char expected) {
    if (!peek(expected)) {
      throw ProtocolError("unexpected JSON token");
    }
    ++position_;
  }

  void skipWhitespace() {
    while (position_ < input_.size() &&
           std::isspace(static_cast<unsigned char>(input_[position_]))) {
      ++position_;
    }
  }

  std::string_view input_;
  std::size_t position_ = 0;
};

inline const JsonValue& requireField(const JsonValue& object, const std::string& key) {
  if (object.type != JsonValue::Type::Object) {
    throw ProtocolError("sidecar response must be a JSON object");
  }
  const auto found = object.objectValue.find(key);
  if (found == object.objectValue.end()) {
    throw ProtocolError("sidecar response is missing field: " + key);
  }
  return found->second;
}

inline std::string scalarToString(const JsonValue& value) {
  if (value.type == JsonValue::Type::String) {
    return value.stringValue;
  }
  if (value.type == JsonValue::Type::Bool) {
    return value.boolValue ? "true" : "false";
  }
  if (value.type == JsonValue::Type::Null) {
    return {};
  }
  if (value.type == JsonValue::Type::Array) {
    std::string result;
    for (const JsonValue& item : value.arrayValue) {
      if (item.type == JsonValue::Type::Array || item.type == JsonValue::Type::Object) {
        throw ProtocolError("nested payload values are not supported");
      }
      if (!result.empty()) {
        result += ',';
      }
      result += scalarToString(item);
    }
    return result;
  }
  throw ProtocolError("nested payload values are not supported");
}

}  // namespace Detail

inline SidecarResponse decodeResponse(std::string_view line) {
  const Detail::JsonValue root = Detail::JsonParser(line).parse();
  if (root.type != Detail::JsonValue::Type::Object) {
    throw ProtocolError("sidecar response must be a JSON object");
  }

  SidecarResponse response;

  const Detail::JsonValue& requestId = Detail::requireField(root, "request_id");
  if (requestId.type == Detail::JsonValue::Type::Null) {
    response.requestIdWasNull = true;
  } else if (requestId.type == Detail::JsonValue::Type::String) {
    if (!isSafeRequestId(requestId.stringValue)) {
      throw ProtocolError("unsafe response request_id");
    }
    response.requestId = requestId.stringValue;
  } else {
    throw ProtocolError("sidecar response request_id must be a string or null");
  }

  const Detail::JsonValue& ok = Detail::requireField(root, "ok");
  if (ok.type != Detail::JsonValue::Type::Bool) {
    throw ProtocolError("sidecar response ok field must be a bool");
  }
  response.ok = ok.boolValue;

  const Detail::JsonValue& payload = Detail::requireField(root, "payload");
  if (payload.type != Detail::JsonValue::Type::Object) {
    throw ProtocolError("sidecar response payload must be an object");
  }
  for (const auto& [key, value] : payload.objectValue) {
    response.payload.emplace(key, Detail::scalarToString(value));
  }

  const Detail::JsonValue& error = Detail::requireField(root, "error");
  if (error.type == Detail::JsonValue::Type::Null) {
    return response;
  }
  if (error.type != Detail::JsonValue::Type::Object) {
    throw ProtocolError("sidecar response error must be an object or null");
  }
  const Detail::JsonValue& code = Detail::requireField(error, "code");
  const Detail::JsonValue& message = Detail::requireField(error, "message");
  if (code.type != Detail::JsonValue::Type::String ||
      message.type != Detail::JsonValue::Type::String) {
    throw ProtocolError("sidecar error code and message must be strings");
  }
  response.error = SidecarError{code.stringValue, message.stringValue};
  return response;
}

}  // namespace CorridorKey::Sidecar
