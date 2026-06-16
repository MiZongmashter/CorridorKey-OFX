#pragma once

#include "sidecar_client/SidecarProcess.h"
#include "sidecar_client/SidecarProtocol.h"

#include <functional>
#include <memory>
#include <string>

namespace CorridorKey::Sidecar {

using CancelCallback = std::function<bool()>;

class SidecarClient {
 public:
  static SidecarClient launch(const SidecarLaunchOptions& options);

  explicit SidecarClient(std::unique_ptr<SidecarProcess> process,
                         std::chrono::milliseconds timeout);
  ~SidecarClient();

  SidecarClient(const SidecarClient&) = delete;
  SidecarClient& operator=(const SidecarClient&) = delete;
  SidecarClient(SidecarClient&&) noexcept = default;
  SidecarClient& operator=(SidecarClient&&) noexcept = default;

  SidecarResponse health();
  SidecarResponse status();
  SidecarResponse infer(const InferRequestContract& contract);
  SidecarResponse infer(const InferRequestContract& contract,
                        const CancelCallback& shouldCancel);
  SidecarResponse warmup(const WarmupRequestContract& contract,
                         const CancelCallback& shouldCancel = {});
  void shutdown();
  std::string readStderrAvailable();

 private:
  SidecarResponse request(const std::string& command);
  SidecarResponse request(const SidecarRequest& request);
  SidecarResponse requestCancellable(const SidecarRequest& request,
                                     const std::string& jobId,
                                     const CancelCallback& shouldCancel);
  std::string nextRequestId();

  std::unique_ptr<SidecarProcess> process_;
  std::chrono::milliseconds timeout_;
  unsigned long requestCounter_ = 0;
  bool shutdown_ = false;
};

}  // namespace CorridorKey::Sidecar
