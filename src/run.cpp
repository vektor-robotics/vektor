#include "vektor/run.hpp"

#include "vektor/deployment.hpp"
#include "vektor/run_events.hpp"

#include <openssl/evp.h>
#include <yaml-cpp/yaml.h>

#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace vektor {
namespace {
std::string utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

std::string required_scalar(const YAML::Node &node, const char *field) {
  if (!node[field] || !node[field].IsScalar())
    throw std::invalid_argument(std::string("run definition requires '") +
                                field + "'");
  const auto value = node[field].as<std::string>();
  if (value.empty())
    throw std::invalid_argument(std::string("run definition field '") + field +
                                "' cannot be empty");
  return value;
}

std::map<std::string, std::string> string_map(const YAML::Node &node,
                                              const char *field) {
  std::map<std::string, std::string> result;
  if (!node[field])
    return result;
  if (!node[field].IsMap())
    throw std::invalid_argument(std::string("run definition field '") + field +
                                "' must be a map");
  for (const auto &entry : node[field]) {
    if (!entry.first.IsScalar() || !entry.second.IsScalar())
      throw std::invalid_argument(std::string("run definition field '") +
                                  field + "' must contain scalar values");
    result.emplace(entry.first.as<std::string>(),
                   entry.second.as<std::string>());
  }
  return result;
}

std::map<std::string, double> numeric_map(const YAML::Node &node,
                                          const char *field) {
  std::map<std::string, double> result;
  if (!node[field])
    return result;
  if (!node[field].IsMap())
    throw std::runtime_error(std::string("run manifest field '") + field +
                             "' must be a map");
  for (const auto &entry : node[field]) {
    if (!entry.first.IsScalar() || !entry.second.IsScalar())
      throw std::runtime_error(std::string("run manifest field '") + field +
                               "' must contain numeric scalar values");
    result.emplace(entry.first.as<std::string>(), entry.second.as<double>());
  }
  return result;
}

std::vector<std::string> string_sequence(const YAML::Node &node,
                                         const char *field) {
  if (!node[field] || !node[field].IsSequence())
    throw std::invalid_argument(std::string("run definition field '") + field +
                                "' must be a sequence");
  std::vector<std::string> result;
  for (const auto &entry : node[field]) {
    if (!entry.IsScalar())
      throw std::invalid_argument(std::string("run definition field '") +
                                  field + "' must contain scalar values");
    result.push_back(entry.as<std::string>());
  }
  return result;
}

void validate_run_id(const std::string &run_id) {
  if (!is_valid_deployment_id(run_id))
    throw std::invalid_argument(
        "run ID must start with an alphanumeric character and contain only "
        "alphanumeric characters, '.', '_', or '-'");
}

void validate_metrics(const std::map<std::string, double> &metrics) {
  if (metrics.size() > 256)
    throw std::invalid_argument("a run may contain at most 256 metrics");
  static const std::regex key_pattern("^[A-Za-z][A-Za-z0-9_.-]{0,127}$");
  for (const auto &[key, value] : metrics) {
    if (!std::regex_match(key, key_pattern))
      throw std::invalid_argument("invalid run metric name '" + key + "'");
    if (!std::isfinite(value))
      throw std::invalid_argument("run metric '" + key + "' must be finite");
  }
}

void validate_definition(const RunManifest &manifest) {
  validate_run_id(manifest.run_id);
  if (manifest.name.empty() || manifest.policy.empty() ||
      manifest.robot_id.empty() || manifest.operator_id.empty())
    throw std::invalid_argument(
        "run definition name, policy, robot_id, and operator are required");
  if (!is_pinned_oci_artifact(manifest.artifact))
    throw std::invalid_argument(
        "run definition artifact must be digest-pinned");
  if (!is_valid_workload_id(manifest.workload_id))
    throw std::invalid_argument("run definition workload_id is invalid");
  static const std::regex digest_pattern("^sha256:[0-9a-f]{64}$");
  if (!std::regex_match(manifest.policy_sha256, digest_pattern))
    throw std::invalid_argument(
        "run definition policy fingerprint must be a SHA-256 digest");
  if (manifest.topics.empty() || manifest.topics.size() > 64)
    throw std::invalid_argument("run definition requires 1 to 64 ROS topics");
  static const std::regex topic_pattern("^/[A-Za-z0-9_~/]+$");
  for (const auto &topic : manifest.topics) {
    if (topic.size() > 256 || !std::regex_match(topic, topic_pattern))
      throw std::invalid_argument("invalid ROS topic '" + topic + "'");
  }
  static const std::regex storage_pattern("^[A-Za-z0-9_-]{1,32}$");
  if (!std::regex_match(manifest.storage_id, storage_pattern))
    throw std::invalid_argument("invalid rosbag2 storage ID");
  if (manifest.health_history_path.empty() ||
      manifest.deployment_audit_path.empty())
    throw std::invalid_argument(
        "run definition requires health and deployment event sources");
  if (manifest.max_imported_events == 0 || manifest.max_imported_events > 1024)
    throw std::invalid_argument(
        "run definition max_imported_events must be between 1 and 1024");
}

std::string sha256_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::invalid_argument("failed to open run policy '" + path.string() +
                                "'");
  using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("failed to initialize policy fingerprint");
  std::array<char, 4096> buffer{};
  while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
    if (EVP_DigestUpdate(context.get(), buffer.data(),
                         static_cast<std::size_t>(input.gcount())) != 1)
      throw std::runtime_error("failed to fingerprint run policy");
  }
  if (!input.eof())
    throw std::runtime_error("failed to read run policy '" + path.string() +
                             "'");
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1)
    throw std::runtime_error("failed to finalize policy fingerprint");
  std::ostringstream output;
  output << "sha256:" << std::hex << std::setfill('0');
  for (unsigned int index = 0; index < size; ++index)
    output << std::setw(2) << static_cast<unsigned int>(digest[index]);
  return output.str();
}

RunStatus parse_status(const std::string &value) {
  if (value == "active")
    return RunStatus::Active;
  if (value == "completed")
    return RunStatus::Completed;
  throw std::runtime_error("unknown persisted run status '" + value + "'");
}

std::string json_escape(std::string_view value) {
  std::ostringstream output;
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\b':
      output << "\\b";
      break;
    case '\f':
      output << "\\f";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (character < 0x20)
        output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
               << static_cast<unsigned int>(character) << std::dec;
      else
        output << static_cast<char>(character);
    }
  }
  return output.str();
}

void append_json_map(std::ostringstream &output,
                     const std::map<std::string, std::string> &values) {
  output << '{';
  bool first = true;
  for (const auto &[key, value] : values) {
    if (!first)
      output << ',';
    output << '"' << json_escape(key) << "\":\"" << json_escape(value) << '"';
    first = false;
  }
  output << '}';
}

void append_json_metrics(std::ostringstream &output,
                         const std::map<std::string, double> &values) {
  output << '{';
  bool first = true;
  for (const auto &[key, value] : values) {
    if (!first)
      output << ',';
    output << '"' << json_escape(key) << "\":" << std::setprecision(17)
           << value;
    first = false;
  }
  output << '}';
}

YAML::Node encode(const RunManifest &manifest) {
  YAML::Node node;
  node["schema_version"] = manifest.schema_version;
  node["run_id"] = manifest.run_id;
  node["name"] = manifest.name;
  node["status"] = run_status_name(manifest.status);
  node["artifact"] = manifest.artifact;
  node["workload_id"] = manifest.workload_id;
  node["policy"] = manifest.policy;
  node["policy_sha256"] = manifest.policy_sha256;
  node["parameters"] = manifest.parameters;
  node["environment"] = manifest.environment;
  node["topics"] = manifest.topics;
  node["storage_id"] = manifest.storage_id;
  node["health_history_path"] = manifest.health_history_path;
  node["deployment_audit_path"] = manifest.deployment_audit_path;
  node["deployment_audit_offset"] = manifest.deployment_audit_offset;
  node["max_imported_events"] = manifest.max_imported_events;
  node["robot_id"] = manifest.robot_id;
  node["operator"] = manifest.operator_id;
  node["started_at"] = manifest.started_at;
  node["stopped_at"] = manifest.stopped_at;
  node["outcome"] = manifest.outcome;
  node["metrics"] = manifest.metrics;
  node["annotations"] = manifest.annotations;
  node["recorder_pid"] = manifest.recorder_pid;
  node["bag_path"] = manifest.bag_path;
  for (const auto &event : manifest.events) {
    YAML::Node encoded;
    encoded["type"] = event.type;
    encoded["timestamp"] = event.timestamp;
    encoded["message"] = event.message;
    node["events"].push_back(encoded);
  }
  for (const auto &artifact : manifest.artifacts) {
    YAML::Node encoded;
    encoded["kind"] = artifact.kind;
    encoded["uri"] = artifact.uri;
    encoded["sha256"] = artifact.sha256;
    encoded["size_bytes"] = artifact.size_bytes;
    node["artifacts"].push_back(encoded);
  }
  return node;
}

RunManifest decode(const YAML::Node &node) {
  RunManifest manifest;
  if (!node["schema_version"] || node["schema_version"].as<unsigned int>() != 1)
    throw std::runtime_error("unsupported run manifest schema version");
  manifest.run_id = required_scalar(node, "run_id");
  validate_run_id(manifest.run_id);
  manifest.name = required_scalar(node, "name");
  manifest.status = parse_status(required_scalar(node, "status"));
  manifest.artifact = required_scalar(node, "artifact");
  manifest.workload_id = required_scalar(node, "workload_id");
  manifest.policy = required_scalar(node, "policy");
  manifest.policy_sha256 = required_scalar(node, "policy_sha256");
  manifest.parameters = string_map(node, "parameters");
  manifest.environment = string_map(node, "environment");
  manifest.topics = string_sequence(node, "topics");
  manifest.storage_id = required_scalar(node, "storage_id");
  manifest.health_history_path = required_scalar(node, "health_history_path");
  manifest.deployment_audit_path =
      required_scalar(node, "deployment_audit_path");
  if (node["deployment_audit_offset"])
    manifest.deployment_audit_offset =
        node["deployment_audit_offset"].as<std::uintmax_t>();
  if (node["max_imported_events"])
    manifest.max_imported_events =
        node["max_imported_events"].as<std::size_t>();
  manifest.robot_id = required_scalar(node, "robot_id");
  manifest.operator_id = required_scalar(node, "operator");
  validate_definition(manifest);
  manifest.started_at = required_scalar(node, "started_at");
  if (node["stopped_at"])
    manifest.stopped_at = node["stopped_at"].as<std::string>();
  if (node["outcome"])
    manifest.outcome = node["outcome"].as<std::string>();
  manifest.metrics = numeric_map(node, "metrics");
  validate_metrics(manifest.metrics);
  if (node["annotations"])
    manifest.annotations = node["annotations"].as<std::vector<std::string>>();
  if (node["recorder_pid"])
    manifest.recorder_pid = node["recorder_pid"].as<std::int64_t>();
  if (node["bag_path"])
    manifest.bag_path = node["bag_path"].as<std::string>();
  if (node["events"]) {
    for (const auto &event : node["events"])
      manifest.events.push_back(
          {required_scalar(event, "type"), required_scalar(event, "timestamp"),
           event["message"] ? event["message"].as<std::string>()
                            : std::string{}});
  }
  if (node["artifacts"]) {
    for (const auto &artifact : node["artifacts"])
      manifest.artifacts.push_back(
          {required_scalar(artifact, "kind"), required_scalar(artifact, "uri"),
           required_scalar(artifact, "sha256"),
           artifact["size_bytes"].as<std::uintmax_t>()});
  }
  return manifest;
}
} // namespace

const char *run_status_name(RunStatus status) {
  return status == RunStatus::Active ? "active" : "completed";
}

RunManifest load_run_definition(const std::filesystem::path &path) {
  YAML::Node node;
  try {
    node = YAML::LoadFile(path.string());
  } catch (const YAML::Exception &error) {
    throw std::invalid_argument("failed to load run definition '" +
                                path.string() + "': " + error.what());
  }
  if (!node.IsMap())
    throw std::invalid_argument("run definition must be a map");
  if (!node["schema_version"] || node["schema_version"].as<unsigned int>() != 1)
    throw std::invalid_argument("run definition requires schema_version: 1");
  RunManifest manifest;
  manifest.run_id = required_scalar(node, "run_id");
  validate_run_id(manifest.run_id);
  manifest.name = required_scalar(node, "name");
  manifest.artifact = required_scalar(node, "artifact");
  manifest.workload_id = required_scalar(node, "workload_id");
  manifest.policy = required_scalar(node, "policy");
  manifest.parameters = string_map(node, "parameters");
  manifest.environment = string_map(node, "environment");
  manifest.topics = string_sequence(node, "topics");
  if (node["storage_id"])
    manifest.storage_id = required_scalar(node, "storage_id");
  if (!node["event_sources"] || !node["event_sources"].IsMap())
    throw std::invalid_argument("run definition requires event_sources");
  const auto event_sources = node["event_sources"];
  const auto resolve_source = [&](const char *field) {
    const auto configured =
        std::filesystem::path(required_scalar(event_sources, field));
    return std::filesystem::absolute(configured.is_absolute()
                                         ? configured
                                         : path.parent_path() / configured)
        .lexically_normal()
        .string();
  };
  manifest.health_history_path = resolve_source("health_history");
  manifest.deployment_audit_path = resolve_source("deployment_audit");
  if (event_sources["max_events"])
    manifest.max_imported_events =
        event_sources["max_events"].as<std::size_t>();
  manifest.robot_id = required_scalar(node, "robot_id");
  manifest.operator_id = required_scalar(node, "operator");
  const auto policy_path = std::filesystem::path(manifest.policy).is_absolute()
                               ? std::filesystem::path(manifest.policy)
                               : path.parent_path() / manifest.policy;
  manifest.policy_sha256 = sha256_file(policy_path);
  validate_definition(manifest);
  return manifest;
}

RunStore::RunStore(std::filesystem::path directory)
    : directory_(std::move(directory)) {
  if (directory_.empty())
    throw std::invalid_argument("run state directory cannot be empty");
}

std::filesystem::path RunStore::path_for(const std::string &run_id) const {
  validate_run_id(run_id);
  return directory_ / (run_id + ".yaml");
}

void RunStore::persist(const RunManifest &manifest, bool must_not_exist) const {
  const auto path = path_for(manifest.run_id);
  std::filesystem::create_directories(directory_);
  if (must_not_exist && std::filesystem::exists(path))
    throw std::runtime_error("run '" + manifest.run_id + "' already exists");
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output)
      throw std::runtime_error("failed to write run manifest '" + temporary +
                               "'");
    output << encode(manifest);
    output.flush();
    if (!output)
      throw std::runtime_error("failed to write run manifest '" + temporary +
                               "'");
  }
  std::filesystem::rename(temporary, path);
}

RunManifest RunStore::start(RunManifest definition) const {
  validate_definition(definition);
  initialize_run_event_sources(definition);
  definition.schema_version = 1;
  definition.status = RunStatus::Active;
  definition.started_at = utc_timestamp();
  definition.stopped_at.clear();
  definition.outcome.clear();
  definition.metrics.clear();
  definition.recorder_pid = 0;
  definition.bag_path.clear();
  definition.events = {{"run_started", definition.started_at, {}}};
  definition.artifacts.clear();
  persist(definition, true);
  return definition;
}

RunManifest RunStore::get(const std::string &run_id) const {
  const auto path = path_for(run_id);
  if (!std::filesystem::exists(path))
    throw std::runtime_error("run '" + run_id + "' does not exist");
  try {
    return decode(YAML::LoadFile(path.string()));
  } catch (const YAML::Exception &error) {
    throw std::runtime_error("failed to load run manifest '" + path.string() +
                             "': " + error.what());
  }
}

RunManifest RunStore::stop(const std::string &run_id,
                           const std::string &outcome,
                           const std::vector<std::string> &annotations,
                           const std::map<std::string, double> &metrics) const {
  return complete_capture(run_id, outcome, annotations, std::nullopt, metrics);
}

RunManifest
RunStore::attach_recorder(const std::string &run_id, std::int64_t pid,
                          const std::filesystem::path &bag_path) const {
  auto manifest = get(run_id);
  if (manifest.status != RunStatus::Active)
    throw std::runtime_error("cannot attach a recorder to completed run '" +
                             run_id + "'");
  if (manifest.recorder_pid != 0)
    throw std::runtime_error("run '" + run_id + "' already has a recorder");
  if (pid <= 1 || bag_path.empty())
    throw std::invalid_argument("valid recorder PID and bag path are required");
  manifest.recorder_pid = pid;
  manifest.bag_path =
      std::filesystem::absolute(bag_path).lexically_normal().string();
  manifest.events.push_back(
      {"capture_started", utc_timestamp(), "rosbag2 recorder launched"});
  persist(manifest, false);
  return manifest;
}

RunManifest
RunStore::complete_capture(const std::string &run_id,
                           const std::string &outcome,
                           const std::vector<std::string> &annotations,
                           const std::optional<RunArtifact> &artifact,
                           const std::map<std::string, double> &metrics) const {
  auto manifest = get(run_id);
  if (manifest.status != RunStatus::Active)
    throw std::runtime_error("run '" + run_id + "' is already completed");
  manifest.status = RunStatus::Completed;
  manifest.stopped_at = utc_timestamp();
  manifest.outcome = outcome;
  validate_metrics(metrics);
  manifest.metrics = metrics;
  manifest.recorder_pid = 0;
  manifest.annotations.insert(manifest.annotations.end(), annotations.begin(),
                              annotations.end());
  auto imported_events =
      collect_run_source_events(manifest, manifest.stopped_at);
  manifest.events.insert(manifest.events.end(), imported_events.begin(),
                         imported_events.end());
  if (artifact)
    manifest.artifacts.push_back(*artifact);
  manifest.events.push_back(
      {"capture_stopped", manifest.stopped_at,
       artifact ? "rosbag2 artifact fingerprinted" : "capture completed"});
  persist(manifest, false);
  return manifest;
}

void RunStore::export_run(const std::string &run_id,
                          const std::filesystem::path &output_directory) const {
  if (output_directory.empty())
    throw std::invalid_argument("export directory cannot be empty");
  if (std::filesystem::exists(output_directory))
    throw std::runtime_error("export directory already exists: " +
                             output_directory.string());
  const auto manifest = get(run_id);
  if (manifest.status != RunStatus::Completed)
    throw std::runtime_error("cannot export active run '" + run_id + "'");
  std::filesystem::create_directories(output_directory);
  {
    std::ofstream yaml(output_directory / "manifest.yaml");
    if (!yaml)
      throw std::runtime_error("failed to write exported YAML manifest");
    yaml << encode(manifest);
  }
  {
    std::ofstream json(output_directory / "manifest.json");
    if (!json)
      throw std::runtime_error("failed to write exported JSON manifest");
    json << run_manifest_to_json(manifest) << '\n';
  }
}

std::string run_manifest_to_json(const RunManifest &manifest) {
  std::ostringstream output;
  output << "{\"schema_version\":" << manifest.schema_version
         << ",\"run_id\":\"" << json_escape(manifest.run_id) << "\",\"name\":\""
         << json_escape(manifest.name) << "\",\"status\":\""
         << run_status_name(manifest.status) << "\",\"artifact\":\""
         << json_escape(manifest.artifact) << "\",\"workload_id\":\""
         << json_escape(manifest.workload_id) << "\",\"policy\":\""
         << json_escape(manifest.policy) << "\",\"policy_sha256\":\""
         << json_escape(manifest.policy_sha256) << "\",\"parameters\":";
  append_json_map(output, manifest.parameters);
  output << ",\"environment\":";
  append_json_map(output, manifest.environment);
  output << ",\"topics\":[";
  for (std::size_t index = 0; index < manifest.topics.size(); ++index) {
    if (index != 0)
      output << ',';
    output << '"' << json_escape(manifest.topics[index]) << '"';
  }
  output << "],\"storage_id\":\"" << json_escape(manifest.storage_id) << '"';
  output << ",\"event_sources\":{\"health_history\":\""
         << json_escape(manifest.health_history_path)
         << "\",\"deployment_audit\":\""
         << json_escape(manifest.deployment_audit_path)
         << "\",\"deployment_audit_offset\":"
         << manifest.deployment_audit_offset
         << ",\"max_events\":" << manifest.max_imported_events << '}';
  output << ",\"robot_id\":\"" << json_escape(manifest.robot_id)
         << "\",\"operator\":\"" << json_escape(manifest.operator_id)
         << "\",\"started_at\":\"" << json_escape(manifest.started_at)
         << "\",\"stopped_at\":\"" << json_escape(manifest.stopped_at)
         << "\",\"outcome\":\"" << json_escape(manifest.outcome)
         << "\",\"metrics\":";
  append_json_metrics(output, manifest.metrics);
  output << ",\"annotations\":[";
  for (std::size_t index = 0; index < manifest.annotations.size(); ++index) {
    if (index != 0)
      output << ',';
    output << '"' << json_escape(manifest.annotations[index]) << '"';
  }
  output << "],\"recorder_pid\":" << manifest.recorder_pid << ",\"bag_path\":\""
         << json_escape(manifest.bag_path) << "\",\"events\":[";
  for (std::size_t index = 0; index < manifest.events.size(); ++index) {
    if (index != 0)
      output << ',';
    const auto &event = manifest.events[index];
    output << "{\"type\":\"" << json_escape(event.type) << "\",\"timestamp\":\""
           << json_escape(event.timestamp) << "\",\"message\":\""
           << json_escape(event.message) << "\"}";
  }
  output << "],\"artifacts\":[";
  for (std::size_t index = 0; index < manifest.artifacts.size(); ++index) {
    if (index != 0)
      output << ',';
    const auto &artifact = manifest.artifacts[index];
    output << "{\"kind\":\"" << json_escape(artifact.kind) << "\",\"uri\":\""
           << json_escape(artifact.uri) << "\",\"sha256\":\""
           << json_escape(artifact.sha256)
           << "\",\"size_bytes\":" << artifact.size_bytes << '}';
  }
  output << "]}";
  return output.str();
}

void print_run_manifest(const RunManifest &manifest, std::ostream &output) {
  output << "VEKTOR RUN\n"
         << "id: " << manifest.run_id << '\n'
         << "name: " << manifest.name << '\n'
         << "status: " << run_status_name(manifest.status) << '\n'
         << "artifact: " << manifest.artifact << '\n'
         << "workload: " << manifest.workload_id << '\n'
         << "policy_sha256: " << manifest.policy_sha256 << '\n'
         << "topics: " << manifest.topics.size() << '\n'
         << "storage: " << manifest.storage_id << '\n'
         << "robot: " << manifest.robot_id << '\n'
         << "operator: " << manifest.operator_id << '\n'
         << "started_at: " << manifest.started_at << '\n';
  if (!manifest.stopped_at.empty())
    output << "stopped_at: " << manifest.stopped_at << '\n';
  if (!manifest.outcome.empty())
    output << "outcome: " << manifest.outcome << '\n';
  if (!manifest.metrics.empty())
    output << "metrics: " << manifest.metrics.size() << '\n';
  if (!manifest.artifacts.empty())
    output << "artifacts: " << manifest.artifacts.size() << '\n';
}

} // namespace vektor
