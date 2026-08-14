#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace vektor {

enum class TrustMode { PublicKey, Keyless };

struct TrustPolicy {
  TrustMode mode{TrustMode::PublicKey};
  std::string cosign_executable{"cosign"};
  std::string key;
  std::string certificate_identity;
  std::string certificate_oidc_issuer;
  std::chrono::milliseconds timeout{30000};
};

struct ArtifactVerification {
  bool verified{false};
  std::string method;
  std::string signer;
  std::string issuer;
  std::string verified_at;
};

TrustPolicy load_trust_policy(const std::filesystem::path &path);

class ArtifactVerifier {
public:
  virtual ~ArtifactVerifier() = default;
  virtual unsigned int interface_version() const noexcept { return 1; }
  virtual ArtifactVerification
  verify(const std::string &artifact,
         std::chrono::milliseconds operation_timeout) = 0;
};

class CosignArtifactVerifier final : public ArtifactVerifier {
public:
  explicit CosignArtifactVerifier(TrustPolicy policy);

  ArtifactVerification
  verify(const std::string &artifact,
         std::chrono::milliseconds operation_timeout) override;

private:
  TrustPolicy policy_;
};

} // namespace vektor
