#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace vektor {

enum class NetworkMode { Host, Bridge, None };

struct BindMount {
  std::string source;
  std::string target;
  bool read_only{false};

  bool operator==(const BindMount &) const = default;
};

struct DeviceMapping {
  std::string host_path;
  std::string container_path;

  bool operator==(const DeviceMapping &) const = default;
};

struct WorkloadSpec {
  NetworkMode network{NetworkMode::Host};
  std::string restart_policy{"unless-stopped"};
  std::string cpu_limit;
  std::string memory_limit;
  std::map<std::string, std::string> environment;
  std::vector<BindMount> mounts;
  std::vector<DeviceMapping> devices;
  std::vector<std::string> command;

  bool operator==(const WorkloadSpec &) const = default;
};

const char *network_mode_name(NetworkMode mode);
NetworkMode parse_network_mode(const std::string &value);
void validate_workload_spec(const WorkloadSpec &spec);
bool is_default_workload_spec(const WorkloadSpec &spec);
std::string workload_fingerprint(const WorkloadSpec &spec);

struct RuntimeObservation {
  bool running{false};
  bool ready{false};
  std::string artifact;
  std::string runtime_id;
  bool managed{false};
  std::string workload_fingerprint;
  std::string readiness_status;
};

// Version 1 runtime contract. Implementations must make activate and stop
// idempotent and report the artifact reference used to create the workload.
// The bounded overloads preserve compatibility with existing drivers while
// allowing runtimes to enforce operation and readiness deadlines.
class RuntimeDriver {
public:
  virtual ~RuntimeDriver() = default;
  virtual unsigned int interface_version() const noexcept { return 1; }
  virtual void prepare(const std::string &artifact) = 0;
  virtual RuntimeObservation activate(const std::string &artifact,
                                      const WorkloadSpec &spec) = 0;
  virtual RuntimeObservation stop() = 0;
  virtual RuntimeObservation inspect() = 0;

  virtual void prepare(const std::string &artifact,
                       std::chrono::milliseconds operation_timeout);
  virtual RuntimeObservation
  activate(const std::string &artifact, const WorkloadSpec &spec,
           std::chrono::milliseconds operation_timeout,
           std::chrono::milliseconds readiness_timeout);
  virtual RuntimeObservation stop(std::chrono::milliseconds operation_timeout);
  virtual RuntimeObservation
  inspect(std::chrono::milliseconds operation_timeout);
};

class OciRuntimeDriver final : public RuntimeDriver {
public:
  OciRuntimeDriver(std::string executable, std::string container_name);

  void prepare(const std::string &artifact) override;
  RuntimeObservation activate(const std::string &artifact,
                              const WorkloadSpec &spec) override;
  RuntimeObservation stop() override;
  RuntimeObservation inspect() override;
  void prepare(const std::string &artifact,
               std::chrono::milliseconds operation_timeout) override;
  RuntimeObservation
  activate(const std::string &artifact, const WorkloadSpec &spec,
           std::chrono::milliseconds operation_timeout,
           std::chrono::milliseconds readiness_timeout) override;
  RuntimeObservation stop(std::chrono::milliseconds operation_timeout) override;
  RuntimeObservation
  inspect(std::chrono::milliseconds operation_timeout) override;

private:
  std::string executable_;
  std::string container_name_;
};

bool is_valid_runtime_container_name(const std::string &value);
std::string workload_runtime_container_name(const std::string &base_name,
                                            const std::string &workload_id);

} // namespace vektor
