#include "vektor/rollout.hpp"

#include "vektor/deployment.hpp"

#include <grpcpp/grpcpp.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <future>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace vektor {
namespace {
struct StoredRollout {
  std::string deployment_id;
  std::string artifact;
  std::string workload_fingerprint;
  std::size_t next_wave{0};
  std::vector<std::string> applied_robots;
  bool complete{false};
  bool recovery_required{false};
};

[[noreturn]] void invalid(const std::string &field,
                          const std::string &message) {
  throw std::runtime_error("invalid rollout config at '" + field +
                           "': " + message);
}

std::string require_string(const YAML::Node &node, const std::string &field) {
  if (!node || !node.IsScalar())
    invalid(field, "expected a non-empty string");
  const auto value = node.as<std::string>();
  if (value.empty())
    invalid(field, "must not be empty");
  return value;
}

void reject_unknown(const YAML::Node &node,
                    const std::set<std::string> &allowed,
                    const std::string &field) {
  if (!node || !node.IsMap())
    invalid(field, "expected a mapping");
  for (const auto &entry : node) {
    const auto key = entry.first.as<std::string>();
    if (!allowed.contains(key))
      invalid(field + "." + key, "unknown field");
  }
}

WorkloadSpec parse_workload(const YAML::Node &node) {
  WorkloadSpec spec;
  if (!node)
    return spec;
  reject_unknown(node,
                 {"network", "restart_policy", "environment", "mounts",
                  "devices", "command"},
                 "workload");
  if (node["network"])
    spec.network =
        parse_network_mode(require_string(node["network"], "workload.network"));
  if (node["restart_policy"])
    spec.restart_policy =
        require_string(node["restart_policy"], "workload.restart_policy");
  if (const auto environment = node["environment"]; environment) {
    if (!environment.IsMap())
      invalid("workload.environment", "expected a mapping");
    for (const auto &entry : environment) {
      if (!entry.first.IsScalar() || !entry.second.IsScalar())
        invalid("workload.environment", "expected scalar names and values");
      const auto name = entry.first.as<std::string>();
      if (!spec.environment.emplace(name, entry.second.as<std::string>())
               .second)
        invalid("workload.environment." + name, "duplicate variable");
    }
  }
  if (const auto mounts = node["mounts"]; mounts) {
    if (!mounts.IsSequence())
      invalid("workload.mounts", "expected a sequence");
    for (std::size_t index = 0; index < mounts.size(); ++index) {
      const auto field = "workload.mounts[" + std::to_string(index) + "]";
      reject_unknown(mounts[index], {"source", "target", "read_only"}, field);
      spec.mounts.push_back(
          {require_string(mounts[index]["source"], field + ".source"),
           require_string(mounts[index]["target"], field + ".target"),
           mounts[index]["read_only"] &&
               mounts[index]["read_only"].as<bool>()});
    }
  }
  if (const auto devices = node["devices"]; devices) {
    if (!devices.IsSequence())
      invalid("workload.devices", "expected a sequence");
    for (std::size_t index = 0; index < devices.size(); ++index) {
      const auto field = "workload.devices[" + std::to_string(index) + "]";
      reject_unknown(devices[index], {"host_path", "container_path"}, field);
      spec.devices.push_back(
          {require_string(devices[index]["host_path"], field + ".host_path"),
           require_string(devices[index]["container_path"],
                          field + ".container_path")});
    }
  }
  if (const auto command = node["command"]; command) {
    if (!command.IsSequence())
      invalid("workload.command", "expected a sequence");
    for (std::size_t index = 0; index < command.size(); ++index)
      spec.command.push_back(require_string(
          command[index], "workload.command[" + std::to_string(index) + "]"));
  }
  try {
    validate_workload_spec(spec);
  } catch (const std::invalid_argument &error) {
    invalid("workload", error.what());
  }
  return spec;
}

std::filesystem::path resolve_path(const std::filesystem::path &base,
                                   const std::string &value) {
  auto path = std::filesystem::path(value);
  if (path.is_relative())
    path = base / path;
  return path.lexically_normal();
}

std::string json_string(std::string_view value) {
  std::ostringstream out;
  out << '"';
  for (const char character : value) {
    switch (character) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      out << character;
    }
  }
  out << '"';
  return out.str();
}

StoredRollout load_state(const RolloutConfig &config, bool required) {
  if (!std::filesystem::exists(config.state_path)) {
    if (required)
      throw std::runtime_error(
          "rollout has not been started; run deploy first");
    return {config.deployment_id, config.artifact,
            workload_fingerprint(config.workload)};
  }
  try {
    const auto root = YAML::LoadFile(config.state_path.string());
    const auto schema_version = root["schema_version"].as<unsigned int>();
    if (schema_version != 1 && schema_version != 2)
      throw std::runtime_error("unsupported rollout state schema");
    StoredRollout state;
    state.deployment_id = root["deployment_id"].as<std::string>();
    state.artifact = root["artifact"].as<std::string>();
    state.workload_fingerprint =
        schema_version >= 2 ? root["workload_fingerprint"].as<std::string>()
                            : workload_fingerprint(WorkloadSpec{});
    state.next_wave = root["next_wave"].as<std::size_t>();
    state.complete = root["complete"].as<bool>();
    state.recovery_required = root["recovery_required"]
                                  ? root["recovery_required"].as<bool>()
                                  : false;
    for (const auto &robot : root["applied_robots"])
      state.applied_robots.push_back(robot.as<std::string>());
    if (state.deployment_id != config.deployment_id ||
        state.artifact != config.artifact ||
        state.workload_fingerprint != workload_fingerprint(config.workload))
      throw std::runtime_error("rollout state does not match this config");
    return state;
  } catch (const std::exception &error) {
    throw std::runtime_error("failed to load rollout state '" +
                             config.state_path.string() + "': " + error.what());
  }
}

void save_state(const RolloutConfig &config, const StoredRollout &state) {
  const auto parent = config.state_path.parent_path();
  if (!parent.empty())
    std::filesystem::create_directories(parent);
  YAML::Emitter output;
  output << YAML::BeginMap << YAML::Key << "schema_version" << YAML::Value << 2
         << YAML::Key << "deployment_id" << YAML::Value << state.deployment_id
         << YAML::Key << "artifact" << YAML::Value << state.artifact
         << YAML::Key << "workload_fingerprint" << YAML::Value
         << state.workload_fingerprint << YAML::Key << "next_wave"
         << YAML::Value << state.next_wave << YAML::Key << "complete"
         << YAML::Value << state.complete << YAML::Key << "recovery_required"
         << YAML::Value << state.recovery_required << YAML::Key
         << "applied_robots" << YAML::Value << YAML::BeginSeq;
  for (const auto &robot : state.applied_robots)
    output << robot;
  output << YAML::EndSeq << YAML::EndMap;
  const auto temporary = config.state_path.string() + ".tmp";
  {
    std::ofstream file(temporary, std::ios::trunc);
    if (!file || !(file << output.c_str() << '\n'))
      throw std::runtime_error("failed to write rollout state");
  }
  std::error_code error;
  std::filesystem::rename(temporary, config.state_path, error);
  if (error) {
    std::filesystem::remove(config.state_path, error);
    error.clear();
    std::filesystem::rename(temporary, config.state_path, error);
  }
  if (error)
    throw std::runtime_error("failed to commit rollout state: " +
                             error.message());
}

bool health_allowed(HealthState state, bool allow_degraded) {
  return state == HealthState::Healthy ||
         (allow_degraded && state == HealthState::Degraded);
}

std::vector<FleetRobotConfig>
robots_by_id(const FleetConfig &fleet, const std::vector<std::string> &ids) {
  std::vector<FleetRobotConfig> robots;
  for (const auto &id : ids) {
    const auto found =
        std::find_if(fleet.robots.begin(), fleet.robots.end(),
                     [&](const auto &robot) { return robot.id == id; });
    if (found == fleet.robots.end())
      throw std::runtime_error("rollout robot '" + id +
                               "' is missing from fleet inventory");
    robots.push_back(*found);
  }
  return robots;
}

enum class RpcAction { Prepare, Activate, Observe, Rollback };

RolloutRobotResult call_agent(const FleetConfig &fleet,
                              const FleetRobotConfig &robot,
                              const RolloutConfig &rollout, RpcAction action) {
  auto stub = vektor::agent::v1::Agent::NewStub(
      make_fleet_channel(robot, fleet.transport));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       rollout.operation_timeout);
  vektor::agent::v1::DeploymentRecord response;
  grpc::Status status;
  if (action == RpcAction::Prepare) {
    vektor::agent::v1::PrepareDeploymentRequest request;
    request.set_deployment_id(rollout.deployment_id);
    request.set_artifact(rollout.artifact);
    *request.mutable_workload() = to_proto(rollout.workload);
    status = stub->PrepareDeployment(&context, request, &response);
  } else if (action == RpcAction::Activate) {
    vektor::agent::v1::ActivateDeploymentRequest request;
    request.set_deployment_id(rollout.deployment_id);
    status = stub->ActivateDeployment(&context, request, &response);
  } else if (action == RpcAction::Rollback) {
    vektor::agent::v1::RollbackDeploymentRequest request;
    request.set_deployment_id(rollout.deployment_id);
    status = stub->RollbackDeployment(&context, request, &response);
  } else {
    vektor::agent::v1::GetDeploymentRequest request;
    status = stub->GetDeployment(&context, request, &response);
  }
  if (!status.ok())
    return {robot.id, false,
            status.error_message().empty() ? "deployment RPC failed"
                                           : status.error_message()};
  if (response.schema_version() != 3 ||
      response.deployment_id() != rollout.deployment_id)
    return {robot.id, false, "invalid deployment response from agent"};
  if (action == RpcAction::Prepare &&
      response.phase() != vektor::agent::v1::DEPLOYMENT_PHASE_STAGED)
    return {robot.id, false, "agent did not stage the deployment"};
  if ((action == RpcAction::Activate || action == RpcAction::Observe) &&
      (response.phase() != vektor::agent::v1::DEPLOYMENT_PHASE_ACTIVE ||
       response.artifact() != rollout.artifact ||
       response.observed_artifact() != rollout.artifact ||
       response.workload_fingerprint() !=
           workload_fingerprint(rollout.workload) ||
       response.observed_workload_fingerprint() !=
           workload_fingerprint(rollout.workload) ||
       !response.runtime_running() || !response.runtime_managed() ||
       response.drift_detected()))
    return {robot.id, false, "desired and observed runtime state do not match"};
  if (action == RpcAction::Rollback &&
      response.phase() != vektor::agent::v1::DEPLOYMENT_PHASE_ROLLED_BACK)
    return {robot.id, false, "agent did not complete rollback"};
  auto message = response.message();
  if (action == RpcAction::Activate || action == RpcAction::Observe)
    message += "; observed " + response.observed_artifact() + " as " +
               response.runtime_id();
  return {robot.id, true, std::move(message)};
}

std::vector<RolloutRobotResult>
call_agents(const FleetConfig &fleet,
            const std::vector<FleetRobotConfig> &robots,
            const RolloutConfig &rollout, RpcAction action) {
  std::vector<std::future<RolloutRobotResult>> pending;
  pending.reserve(robots.size());
  for (const auto &robot : robots)
    pending.push_back(std::async(std::launch::async, [&, robot] {
      return call_agent(fleet, robot, rollout, action);
    }));
  std::vector<RolloutRobotResult> results;
  results.reserve(pending.size());
  for (auto &request : pending)
    results.push_back(request.get());
  return results;
}

bool all_succeeded(const std::vector<RolloutRobotResult> &results) {
  return std::all_of(results.begin(), results.end(),
                     [](const auto &result) { return result.success; });
}

RolloutReport execute_wave(const RolloutConfig &config, StoredRollout &state,
                           const FleetConfig &fleet) {
  if (state.next_wave >= config.waves.size())
    throw std::runtime_error("all rollout waves are already complete");
  const auto &wave = config.waves[state.next_wave];
  const auto robots = select_fleet_robots(fleet, wave.selectors, wave.limit);
  RolloutReport report{config.deployment_id, "deploy", wave.name};
  report.next_wave = state.next_wave;
  report.complete = state.complete;
  const auto remember_incomplete_rollback = [&] {
    for (const auto &robot : robots) {
      if (std::find(state.applied_robots.begin(), state.applied_robots.end(),
                    robot.id) == state.applied_robots.end())
        state.applied_robots.push_back(robot.id);
    }
    state.recovery_required = true;
    save_state(config, state);
  };

  FleetConfig target_fleet = fleet;
  target_fleet.robots = robots;
  const auto preflight = poll_fleet(target_fleet);
  if (!health_allowed(preflight.state, config.allow_degraded)) {
    report.message = "preflight health gate failed";
    for (const auto &robot : preflight.robots)
      report.robots.push_back(
          {robot.robot.id, health_allowed(robot.state, config.allow_degraded),
           health_state_name(robot.state)});
    return report;
  }

  report.robots = call_agents(fleet, robots, config, RpcAction::Prepare);
  if (!all_succeeded(report.robots)) {
    const auto rollback =
        call_agents(fleet, robots, config, RpcAction::Rollback);
    const bool rolled_back = all_succeeded(rollback);
    if (!rolled_back)
      remember_incomplete_rollback();
    report.message = rolled_back
                         ? "prepare failed; wave rolled back"
                         : "prepare failed; automatic rollback is incomplete";
    return report;
  }
  report.robots = call_agents(fleet, robots, config, RpcAction::Activate);
  if (!all_succeeded(report.robots)) {
    const auto rollback =
        call_agents(fleet, robots, config, RpcAction::Rollback);
    const bool rolled_back = all_succeeded(rollback);
    if (!rolled_back)
      remember_incomplete_rollback();
    report.message =
        rolled_back ? "activation failed; wave rolled back"
                    : "activation failed; automatic rollback is incomplete";
    return report;
  }
  if (config.settle_time.count() > 0)
    std::this_thread::sleep_for(config.settle_time);
  const auto verification = poll_fleet(target_fleet);
  if (!health_allowed(verification.state, config.allow_degraded)) {
    const auto rollback =
        call_agents(fleet, robots, config, RpcAction::Rollback);
    const bool rolled_back = all_succeeded(rollback);
    if (!rolled_back)
      remember_incomplete_rollback();
    report.robots.clear();
    for (const auto &robot : verification.robots)
      report.robots.push_back(
          {robot.robot.id, health_allowed(robot.state, config.allow_degraded),
           health_state_name(robot.state)});
    report.message = rolled_back
                         ? "post-deploy health gate failed; wave rolled back"
                         : "post-deploy health gate failed; automatic rollback "
                           "is incomplete";
    return report;
  }

  for (const auto &robot : robots)
    state.applied_robots.push_back(robot.id);
  ++state.next_wave;
  state.complete = state.next_wave == config.waves.size();
  save_state(config, state);
  report.success = true;
  report.complete = state.complete;
  report.next_wave = state.next_wave;
  report.message = state.complete
                       ? "rollout complete"
                       : "wave healthy; rollout paused for promotion";
  return report;
}
} // namespace

RolloutConfig load_rollout_config(const std::string &path) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path);
  } catch (const YAML::Exception &error) {
    throw std::runtime_error("failed to load rollout config '" + path +
                             "': " + error.what());
  }
  reject_unknown(root,
                 {"schema_version", "deployment_id", "artifact", "fleet_config",
                  "state_file", "operation_timeout_ms", "settle_time_ms",
                  "allow_degraded", "workload", "waves"},
                 "root");
  if (!root["schema_version"] || root["schema_version"].as<unsigned int>() != 1)
    invalid("schema_version", "must be 1");
  RolloutConfig config;
  config.deployment_id = require_string(root["deployment_id"], "deployment_id");
  if (!is_valid_deployment_id(config.deployment_id))
    invalid("deployment_id", "use letters, numbers, '.', '_', or '-'");
  config.artifact = require_string(root["artifact"], "artifact");
  if (!is_pinned_oci_artifact(config.artifact))
    invalid("artifact", "must be pinned by a sha256 digest");
  config.workload = parse_workload(root["workload"]);
  const auto base = std::filesystem::absolute(path).parent_path();
  config.fleet_config_path =
      resolve_path(base, require_string(root["fleet_config"], "fleet_config"));
  config.state_path =
      root["state_file"]
          ? resolve_path(base, require_string(root["state_file"], "state_file"))
          : base / ".vektor" / (config.deployment_id + ".yaml");
  const auto operation_timeout =
      root["operation_timeout_ms"]
          ? root["operation_timeout_ms"].as<long long>()
          : 300000;
  if (operation_timeout <= 0)
    invalid("operation_timeout_ms", "must be greater than zero");
  config.operation_timeout = std::chrono::milliseconds(operation_timeout);
  const auto settle =
      root["settle_time_ms"] ? root["settle_time_ms"].as<long long>() : 5000;
  if (settle < 0)
    invalid("settle_time_ms", "must not be negative");
  config.settle_time = std::chrono::milliseconds(settle);
  config.allow_degraded =
      root["allow_degraded"] && root["allow_degraded"].as<bool>();
  const auto waves = root["waves"];
  if (!waves || !waves.IsSequence() || waves.size() == 0)
    invalid("waves", "expected a non-empty sequence");
  std::set<std::string> names;
  for (std::size_t index = 0; index < waves.size(); ++index) {
    const auto item = waves[index];
    const auto field = "waves[" + std::to_string(index) + "]";
    reject_unknown(item, {"name", "selectors", "limit"}, field);
    DeploymentWave wave;
    wave.name = require_string(item["name"], field + ".name");
    if (!names.insert(wave.name).second)
      invalid(field + ".name", "duplicate wave name");
    if (const auto selectors = item["selectors"]; selectors) {
      if (!selectors.IsSequence())
        invalid(field + ".selectors", "expected a sequence");
      for (const auto &selector : selectors)
        wave.selectors.push_back(
            parse_label_selector(selector.as<std::string>()));
    }
    if (item["limit"]) {
      const auto limit = item["limit"].as<long long>();
      if (limit <= 0)
        invalid(field + ".limit", "must be greater than zero");
      wave.limit = static_cast<std::size_t>(limit);
    }
    config.waves.push_back(std::move(wave));
  }

  const auto fleet = load_fleet_config(config.fleet_config_path.string());
  std::set<std::string> targeted;
  for (std::size_t index = 0; index < config.waves.size(); ++index) {
    for (const auto &robot : select_fleet_robots(
             fleet, config.waves[index].selectors, config.waves[index].limit)) {
      if (!targeted.insert(robot.id).second)
        invalid("waves[" + std::to_string(index) + "]",
                "robot '" + robot.id + "' appears in multiple waves");
    }
  }
  return config;
}

RolloutReport deploy_release(const RolloutConfig &config) {
  auto state = load_state(config, false);
  if (state.next_wave != 0 || !state.applied_robots.empty())
    throw std::runtime_error(
        "rollout already started; use promote or rollback");
  const auto fleet = load_fleet_config(config.fleet_config_path.string());
  return execute_wave(config, state, fleet);
}

RolloutReport promote_release(const RolloutConfig &config) {
  auto state = load_state(config, true);
  if (state.recovery_required)
    throw std::runtime_error(
        "the previous automatic rollback was incomplete; run rollback");
  if (state.complete)
    throw std::runtime_error("rollout is already complete");
  const auto fleet = load_fleet_config(config.fleet_config_path.string());
  if (!state.applied_robots.empty()) {
    FleetConfig active = fleet;
    active.robots = robots_by_id(fleet, state.applied_robots);
    const auto health = poll_fleet(active);
    if (!health_allowed(health.state, config.allow_degraded))
      return {config.deployment_id,
              "promote",
              {},
              false,
              false,
              state.next_wave,
              {},
              "active-wave health gate failed; promotion paused"};
    const auto observed =
        call_agents(fleet, active.robots, config, RpcAction::Observe);
    if (!all_succeeded(observed))
      return {config.deployment_id,
              "promote",
              {},
              false,
              false,
              state.next_wave,
              observed,
              "active-wave runtime drift detected; promotion paused"};
  }
  auto report = execute_wave(config, state, fleet);
  report.action = "promote";
  return report;
}

RolloutReport rollback_release(const RolloutConfig &config) {
  auto state = load_state(config, true);
  if (state.applied_robots.empty())
    throw std::runtime_error("rollout has no applied robots to roll back");
  const auto fleet = load_fleet_config(config.fleet_config_path.string());
  auto robots = robots_by_id(fleet, state.applied_robots);
  std::reverse(robots.begin(), robots.end());
  RolloutReport report{config.deployment_id, "rollback", "all"};
  for (const auto &robot : robots) {
    auto result = call_agents(fleet, {robot}, config, RpcAction::Rollback);
    report.robots.push_back(std::move(result.front()));
  }
  report.success = all_succeeded(report.robots);
  report.message = report.success ? "all applied robots rolled back"
                                  : "one or more robots failed to roll back";
  if (report.success) {
    state.applied_robots.clear();
    state.next_wave = 0;
    state.complete = false;
    state.recovery_required = false;
    save_state(config, state);
  }
  return report;
}

void print_rollout_report(const RolloutReport &report, std::ostream &out) {
  out << "VEKTOR " << report.action << ": "
      << (report.success ? "SUCCESS" : "FAILED") << '\n'
      << "deployment: " << report.deployment_id << '\n';
  if (!report.wave.empty())
    out << "wave: " << report.wave << '\n';
  out << "message: " << report.message << '\n';
  for (const auto &robot : report.robots)
    out << '[' << (robot.success ? "ok" : "failed") << "] " << robot.robot_id
        << " - " << robot.message << '\n';
}

std::string rollout_report_to_json(const RolloutReport &report) {
  std::ostringstream out;
  out << "{\"schema_version\":1,\"deployment_id\":"
      << json_string(report.deployment_id)
      << ",\"action\":" << json_string(report.action)
      << ",\"wave\":" << json_string(report.wave)
      << ",\"success\":" << (report.success ? "true" : "false")
      << ",\"complete\":" << (report.complete ? "true" : "false")
      << ",\"next_wave\":" << report.next_wave
      << ",\"message\":" << json_string(report.message) << ",\"robots\":[";
  for (std::size_t index = 0; index < report.robots.size(); ++index) {
    if (index > 0)
      out << ',';
    const auto &robot = report.robots[index];
    out << "{\"id\":" << json_string(robot.robot_id)
        << ",\"success\":" << (robot.success ? "true" : "false")
        << ",\"message\":" << json_string(robot.message) << '}';
  }
  out << "]}";
  return out.str();
}

} // namespace vektor
