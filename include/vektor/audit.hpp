#pragma once

#include <filesystem>
#include <mutex>
#include <string>

namespace vektor {

struct AuditEvent {
  std::string actor;
  std::string action;
  std::string outcome;
  std::string deployment_id;
  std::string artifact;
  std::string phase;
  std::string operation;
  std::string message;
};

class AuditSink {
public:
  virtual ~AuditSink() = default;
  virtual unsigned int interface_version() const noexcept { return 1; }
  virtual void append(const AuditEvent &event) = 0;
};

class JsonLinesAuditLog final : public AuditSink {
public:
  explicit JsonLinesAuditLog(std::filesystem::path path);
  void append(const AuditEvent &event) override;
  const std::filesystem::path &path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
  std::mutex mutex_;
};

} // namespace vektor
