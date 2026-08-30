#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace vektor {

enum class RunStatus { Active, Completed };

struct RunEvent {
  std::string type;
  std::string timestamp;
  std::string message;
};

struct RunArtifact {
  std::string kind;
  std::string uri;
  std::string sha256;
  std::uintmax_t size_bytes{0};
};

struct RunManifest {
  unsigned int schema_version{1};
  std::string run_id;
  std::string name;
  RunStatus status{RunStatus::Active};
  std::string artifact;
  std::string workload_id;
  std::string policy;
  std::string policy_sha256;
  std::map<std::string, std::string> parameters;
  std::map<std::string, std::string> environment;
  std::vector<std::string> topics;
  std::string storage_id{"sqlite3"};
  std::string health_history_path;
  std::string deployment_audit_path;
  std::uintmax_t deployment_audit_offset{0};
  std::size_t max_imported_events{256};
  std::string robot_id;
  std::string operator_id;
  std::string started_at;
  std::string stopped_at;
  std::string outcome;
  std::map<std::string, double> metrics;
  std::vector<std::string> annotations;
  std::int64_t recorder_pid{0};
  std::string bag_path;
  std::vector<RunEvent> events;
  std::vector<RunArtifact> artifacts;
};

const char *run_status_name(RunStatus status);
RunManifest load_run_definition(const std::filesystem::path &path);
std::string run_manifest_to_json(const RunManifest &manifest);
void print_run_manifest(const RunManifest &manifest, std::ostream &output);

class RunStore {
public:
  explicit RunStore(std::filesystem::path directory);
  RunManifest start(RunManifest definition) const;
  RunManifest stop(const std::string &run_id, const std::string &outcome,
                   const std::vector<std::string> &annotations = {},
                   const std::map<std::string, double> &metrics = {}) const;
  RunManifest attach_recorder(const std::string &run_id, std::int64_t pid,
                              const std::filesystem::path &bag_path) const;
  RunManifest complete_capture(
      const std::string &run_id, const std::string &outcome,
      const std::vector<std::string> &annotations,
      const std::optional<RunArtifact> &artifact = std::nullopt,
      const std::map<std::string, double> &metrics = {}) const;
  void export_run(const std::string &run_id,
                  const std::filesystem::path &output_directory) const;
  RunManifest get(const std::string &run_id) const;
  std::filesystem::path path_for(const std::string &run_id) const;

private:
  void persist(const RunManifest &manifest, bool must_not_exist) const;
  std::filesystem::path directory_;
};

} // namespace vektor
