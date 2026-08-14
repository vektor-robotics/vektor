#pragma once

#include <string>

namespace vektor {

struct RuntimeObservation {
  bool running{false};
  std::string artifact;
  std::string runtime_id;
  bool managed{false};
};

// Version 1 runtime contract. Implementations must make activate and stop
// idempotent and report the artifact reference used to create the workload.
class RuntimeDriver {
public:
  virtual ~RuntimeDriver() = default;
  virtual unsigned int interface_version() const noexcept { return 1; }
  virtual void prepare(const std::string &artifact) = 0;
  virtual RuntimeObservation activate(const std::string &artifact) = 0;
  virtual RuntimeObservation stop() = 0;
  virtual RuntimeObservation inspect() = 0;
};

class OciRuntimeDriver final : public RuntimeDriver {
public:
  OciRuntimeDriver(std::string executable, std::string container_name);

  void prepare(const std::string &artifact) override;
  RuntimeObservation activate(const std::string &artifact) override;
  RuntimeObservation stop() override;
  RuntimeObservation inspect() override;

private:
  std::string executable_;
  std::string container_name_;
};

bool is_valid_runtime_container_name(const std::string &value);

} // namespace vektor
