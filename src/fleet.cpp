#include "vektor/fleet.hpp"

#include <grpcpp/security/credentials.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <future>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace vektor {
namespace {
[[noreturn]] void invalid(const std::string &field,
                          const std::string &message) {
  throw std::runtime_error("invalid fleet config at '" + field +
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
  if (!node.IsMap())
    invalid(field, "expected a mapping");
  for (const auto &entry : node) {
    const auto key = entry.first.as<std::string>();
    if (!allowed.contains(key))
      invalid(field + "." + key, "unknown field");
  }
}

std::filesystem::path resolve_path(const std::filesystem::path &base,
                                   const YAML::Node &node,
                                   const std::string &field) {
  auto path = std::filesystem::path(require_string(node, field));
  if (path.is_relative())
    path = base / path;
  return path.lexically_normal();
}

bool is_loopback_address(const std::string &address) {
  return address.starts_with("127.0.0.1:") ||
         address.starts_with("localhost:") || address.starts_with("[::1]:") ||
         address.starts_with("unix:");
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot read fleet TLS file '" + path.string() +
                             "'");
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::shared_ptr<grpc::ChannelCredentials>
channel_credentials(const FleetTransportConfig &transport) {
  if (transport.insecure)
    return grpc::InsecureChannelCredentials();
  grpc::SslCredentialsOptions ssl;
  ssl.pem_root_certs = read_file(*transport.ca_certificate);
  ssl.pem_cert_chain = read_file(*transport.client_certificate);
  ssl.pem_private_key = read_file(*transport.client_private_key);
  return grpc::SslCredentials(ssl);
}

HealthState from_proto(vektor::agent::v1::HealthState state) {
  switch (state) {
  case vektor::agent::v1::HEALTH_STATE_HEALTHY:
    return HealthState::Healthy;
  case vektor::agent::v1::HEALTH_STATE_DEGRADED:
    return HealthState::Degraded;
  case vektor::agent::v1::HEALTH_STATE_UNHEALTHY:
    return HealthState::Unhealthy;
  case vektor::agent::v1::HEALTH_STATE_UNREACHABLE:
  case vektor::agent::v1::HEALTH_STATE_UNSPECIFIED:
    return HealthState::Unreachable;
  }
  return HealthState::Unreachable;
}

std::string utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::optional<std::chrono::system_clock::time_point>
parse_utc_timestamp(const std::string &value) {
  std::tm utc{};
  std::istringstream input(value);
  input >> std::get_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  if (input.fail() || input.peek() != std::char_traits<char>::eof())
    return std::nullopt;
  const auto time = timegm(&utc);
  if (time == static_cast<std::time_t>(-1))
    return std::nullopt;
  return std::chrono::system_clock::from_time_t(time);
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

FleetRobotStatus
poll_robot(const FleetRobotConfig &robot, std::chrono::milliseconds timeout,
           std::chrono::milliseconds max_snapshot_age,
           const std::shared_ptr<grpc::ChannelCredentials> &credentials) {
  grpc::ChannelArguments arguments;
  if (!robot.tls_server_name.empty())
    arguments.SetSslTargetNameOverride(robot.tls_server_name);
  auto channel =
      grpc::CreateCustomChannel(robot.endpoint, credentials, arguments);
  auto stub = vektor::agent::v1::Agent::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + timeout);
  vektor::agent::v1::GetStatusRequest request;
  vektor::agent::v1::StatusSnapshot response;
  const auto rpc = stub->GetStatus(&context, request, &response);

  FleetRobotStatus result;
  result.robot = robot;
  if (!rpc.ok()) {
    result.error = rpc.error_message().empty() ? "agent request failed"
                                               : rpc.error_message();
    return result;
  }
  result.snapshot = std::move(response);
  result.state = from_proto(result.snapshot->state());
  if (result.snapshot->schema_version() != 1) {
    result.state = HealthState::Unhealthy;
    result.error = "unsupported snapshot schema version " +
                   std::to_string(result.snapshot->schema_version());
    return result;
  }
  if (result.snapshot->robot_id() != robot.id) {
    result.state = HealthState::Unhealthy;
    result.error = "agent identity mismatch: expected '" + robot.id +
                   "', received '" + result.snapshot->robot_id() + "'";
    return result;
  }
  const auto snapshot_time = parse_utc_timestamp(result.snapshot->timestamp());
  if (!snapshot_time) {
    result.state = HealthState::Unreachable;
    result.error = "agent returned an invalid snapshot timestamp";
    return result;
  }
  const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now() - *snapshot_time);
  if (age > max_snapshot_age) {
    result.state = HealthState::Unreachable;
    result.error = "agent snapshot is stale";
  } else if (age < -max_snapshot_age) {
    result.state = HealthState::Unhealthy;
    result.error = "agent snapshot timestamp is too far in the future";
  }
  return result;
}
} // namespace

FleetConfig load_fleet_config(const std::string &path) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path);
  } catch (const YAML::Exception &error) {
    throw std::runtime_error("failed to load fleet config '" + path +
                             "': " + error.what());
  }
  reject_unknown(root,
                 {"fleet_id", "request_timeout_ms", "max_snapshot_age_ms",
                  "transport", "robots"},
                 "root");

  FleetConfig config;
  config.fleet_id = require_string(root["fleet_id"], "fleet_id");
  const auto timeout = root["request_timeout_ms"]
                           ? root["request_timeout_ms"].as<long long>()
                           : 2000;
  if (timeout <= 0)
    invalid("request_timeout_ms", "must be greater than zero");
  config.request_timeout = std::chrono::milliseconds(timeout);
  const auto max_age = root["max_snapshot_age_ms"]
                           ? root["max_snapshot_age_ms"].as<long long>()
                           : 15000;
  if (max_age <= 0)
    invalid("max_snapshot_age_ms", "must be greater than zero");
  config.max_snapshot_age = std::chrono::milliseconds(max_age);

  const auto transport = root["transport"];
  reject_unknown(
      transport,
      {"insecure", "ca_certificate", "client_certificate", "client_key"},
      "transport");
  config.transport.insecure =
      transport["insecure"] && transport["insecure"].as<bool>();
  const auto base = std::filesystem::absolute(path).parent_path();
  if (config.transport.insecure) {
    if (transport["ca_certificate"] || transport["client_certificate"] ||
        transport["client_key"])
      invalid("transport", "TLS fields cannot be combined with insecure mode");
  } else {
    config.transport.ca_certificate = resolve_path(
        base, transport["ca_certificate"], "transport.ca_certificate");
    config.transport.client_certificate = resolve_path(
        base, transport["client_certificate"], "transport.client_certificate");
    config.transport.client_private_key =
        resolve_path(base, transport["client_key"], "transport.client_key");
  }

  const auto robots = root["robots"];
  if (!robots || !robots.IsSequence() || robots.size() == 0)
    invalid("robots", "expected a non-empty sequence");
  std::set<std::string> ids;
  for (std::size_t index = 0; index < robots.size(); ++index) {
    const auto item = robots[index];
    const auto field = "robots[" + std::to_string(index) + "]";
    reject_unknown(item, {"id", "endpoint", "labels", "tls_server_name"},
                   field);
    FleetRobotConfig robot;
    robot.id = require_string(item["id"], field + ".id");
    robot.endpoint = require_string(item["endpoint"], field + ".endpoint");
    if (!ids.insert(robot.id).second)
      invalid(field + ".id", "duplicate robot ID");
    if (config.transport.insecure && !is_loopback_address(robot.endpoint))
      invalid(field + ".endpoint",
              "insecure transport is limited to loopback or Unix sockets");
    if (item["tls_server_name"])
      robot.tls_server_name =
          require_string(item["tls_server_name"], field + ".tls_server_name");
    if (const auto labels = item["labels"]; labels) {
      if (!labels.IsMap())
        invalid(field + ".labels", "expected a mapping");
      for (const auto &label : labels) {
        const auto key = require_string(label.first, field + ".labels key");
        robot.labels.emplace(
            key, require_string(label.second, field + ".labels." + key));
      }
    }
    config.robots.push_back(std::move(robot));
  }
  return config;
}

std::shared_ptr<grpc::Channel>
make_fleet_channel(const FleetRobotConfig &robot,
                   const FleetTransportConfig &transport) {
  grpc::ChannelArguments arguments;
  if (!robot.tls_server_name.empty())
    arguments.SetSslTargetNameOverride(robot.tls_server_name);
  return grpc::CreateCustomChannel(robot.endpoint,
                                   channel_credentials(transport), arguments);
}

LabelSelector parse_label_selector(const std::string &value) {
  const auto separator = value.find('=');
  if (separator == std::string::npos || separator == 0 ||
      separator + 1 == value.size())
    throw std::invalid_argument("selector must use key=value syntax");
  return {value.substr(0, separator), value.substr(separator + 1)};
}

std::vector<FleetRobotConfig>
select_fleet_robots(const FleetConfig &config,
                    const std::vector<LabelSelector> &selectors,
                    std::optional<std::size_t> limit) {
  std::vector<FleetRobotConfig> selected;
  for (const auto &robot : config.robots) {
    const bool matches =
        std::all_of(selectors.begin(), selectors.end(), [&](const auto &item) {
          const auto label = robot.labels.find(item.key);
          return label != robot.labels.end() && label->second == item.value;
        });
    if (!matches)
      continue;
    selected.push_back(robot);
    if (limit && selected.size() == *limit)
      break;
  }
  if (selected.empty())
    throw std::invalid_argument("fleet selection matched no robots");
  return selected;
}

HealthState aggregate_fleet_state(const std::vector<FleetRobotStatus> &robots) {
  if (robots.empty())
    return HealthState::Unreachable;
  if (std::any_of(robots.begin(), robots.end(), [](const auto &robot) {
        return robot.state == HealthState::Unreachable;
      }))
    return HealthState::Unreachable;
  if (std::any_of(robots.begin(), robots.end(), [](const auto &robot) {
        return robot.state == HealthState::Unhealthy;
      }))
    return HealthState::Unhealthy;
  if (std::any_of(robots.begin(), robots.end(), [](const auto &robot) {
        return robot.state == HealthState::Degraded;
      }))
    return HealthState::Degraded;
  return HealthState::Healthy;
}

FleetReport poll_fleet(const FleetConfig &config,
                       const std::vector<LabelSelector> &selectors,
                       std::optional<std::size_t> limit) {
  const auto selected = select_fleet_robots(config, selectors, limit);
  const auto credentials = channel_credentials(config.transport);
  std::vector<std::future<FleetRobotStatus>> pending;
  pending.reserve(selected.size());
  for (const auto &robot : selected) {
    pending.push_back(std::async(std::launch::async, [&, robot] {
      return poll_robot(robot, config.request_timeout, config.max_snapshot_age,
                        credentials);
    }));
  }

  FleetReport report;
  report.timestamp = utc_timestamp();
  report.fleet_id = config.fleet_id;
  report.inventory_size = config.robots.size();
  report.robots.reserve(pending.size());
  for (auto &future : pending)
    report.robots.push_back(future.get());
  report.state = aggregate_fleet_state(report.robots);
  return report;
}

void print_fleet_report(const FleetReport &report, std::ostream &out) {
  out << "VEKTOR FLEET: " << health_state_name(report.state) << '\n'
      << "fleet: " << report.fleet_id << '\n'
      << "timestamp: " << report.timestamp << '\n'
      << "targets: " << report.robots.size() << '/' << report.inventory_size
      << "\n\n";
  for (const auto &robot : report.robots) {
    out << '[' << health_state_name(robot.state) << "] " << robot.robot.id
        << " @ " << robot.robot.endpoint;
    if (robot.snapshot)
      out << " sequence=" << robot.snapshot->sequence();
    if (!robot.error.empty())
      out << " - " << robot.error;
    out << '\n';
  }
}

std::string fleet_report_to_json(const FleetReport &report) {
  std::ostringstream out;
  out << "{\"schema_version\":1,\"timestamp\":" << json_string(report.timestamp)
      << ",\"fleet_id\":" << json_string(report.fleet_id)
      << ",\"state\":" << json_string(health_state_name(report.state))
      << ",\"inventory_size\":" << report.inventory_size
      << ",\"target_count\":" << report.robots.size() << ",\"robots\":[";
  for (std::size_t index = 0; index < report.robots.size(); ++index) {
    if (index > 0)
      out << ',';
    const auto &robot = report.robots[index];
    out << "{\"id\":" << json_string(robot.robot.id)
        << ",\"endpoint\":" << json_string(robot.robot.endpoint)
        << ",\"state\":" << json_string(health_state_name(robot.state))
        << ",\"labels\":{";
    std::size_t label_index = 0;
    for (const auto &[key, value] : robot.robot.labels) {
      if (label_index++ > 0)
        out << ',';
      out << json_string(key) << ':' << json_string(value);
    }
    out << '}';
    if (robot.snapshot) {
      out << ",\"robot_id\":" << json_string(robot.snapshot->robot_id())
          << ",\"snapshot_timestamp\":"
          << json_string(robot.snapshot->timestamp())
          << ",\"sequence\":" << robot.snapshot->sequence();
    }
    if (!robot.error.empty())
      out << ",\"error\":" << json_string(robot.error);
    out << '}';
  }
  out << "]}";
  return out.str();
}

} // namespace vektor
