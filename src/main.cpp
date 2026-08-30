#include "vektor/agent.hpp"
#include "vektor/approval.hpp"
#include "vektor/authorization.hpp"
#include "vektor/capture.hpp"
#include "vektor/comparison.hpp"
#include "vektor/config.hpp"
#include "vektor/fleet.hpp"
#include "vektor/health_inspector.hpp"
#include "vektor/replay.hpp"
#include "vektor/reporter.hpp"
#include "vektor/rollout.hpp"
#include "vektor/run.hpp"
#include "vektor/status.hpp"
#include "vektor/support_bundle.hpp"
#include "vektor/trust.hpp"

#include <rclcpp/rclcpp.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
struct CliOptions {
  std::string command;
  std::string capture_action;
  std::string replay_action;
  std::string config_path;
  std::string format{"text"};
  std::string validation_type;
  std::string robot_id;
  std::string fleet_id;
  std::string workload_id;
  std::string wave;
  std::string approval_identity;
  std::string issued_at;
  std::string expires_at;
  bool watch{false};
  bool history{true};
  std::chrono::milliseconds interval{5000};
  std::optional<std::filesystem::path> history_path;
  std::string listen_address{"127.0.0.1:50051"};
  bool insecure{false};
  std::optional<std::filesystem::path> tls_certificate;
  std::optional<std::filesystem::path> tls_private_key;
  std::optional<std::filesystem::path> tls_client_ca;
  std::vector<std::string> selectors;
  std::optional<std::size_t> limit;
  std::filesystem::path deployment_state{".vektor/deployment.yaml"};
  std::filesystem::path audit_log{".vektor/audit.jsonl"};
  std::filesystem::path metrics{".vektor/metrics.prom"};
  std::filesystem::path output_directory;
  std::string oci_runtime{"docker"};
  std::string runtime_container{"vektor-workload"};
  std::optional<std::filesystem::path> trust_policy;
  std::optional<std::filesystem::path> authorization_policy;
  std::filesystem::path run_state_directory{".vektor/runs"};
  std::filesystem::path replay_directory{".vektor/replays"};
  std::string run_id;
  std::string baseline_run_id;
  std::string candidate_run_id;
  std::string outcome;
  std::vector<std::string> annotations;
  std::map<std::string, double> run_metrics;
};

[[noreturn]] void usage_error(const std::string &message = {}) {
  if (!message.empty())
    std::cerr << "vektor: " << message << "\n\n";
  std::cerr
      << "Usage:\n"
      << "  vektor check  --config <path> [--format text|json]\n"
      << "  vektor validate --type health|fleet|rollout|authorization|"
         "approval-policy|approvals|trust\n"
      << "                  --config <path> [--format text|json]\n"
      << "  vektor status --config <path> [--format text|json] "
         "[--robot-id <id>]\n"
      << "                [--watch] [--interval-ms <ms>] "
         "[--history <path>|--no-history]\n"
      << "  vektor agent  --config <path> [--robot-id <id>] "
         "[--interval-ms <ms>]\n"
      << "                [--listen <host:port>] "
         "[--history <path>|--no-history]\n"
      << "                (--tls-cert <path> --tls-key <path> "
         "--tls-ca <path>|--insecure)\n"
      << "                [--oci-runtime docker|podman] "
         "[--runtime-container <name>] "
         "[--deployment-state <path>] "
         "[--audit-log <path>] "
         "[--trust-policy <path>] "
         "[--authorization-policy <path>]\n"
      << "                [--fleet-id <id> --workload-id <id>]\n"
      << "  vektor fleet  --config <path> [--format text|json] "
         "[--selector key=value]...\n"
      << "                [--limit <count>] [--watch] "
         "[--interval-ms <ms>]\n"
      << "  vektor deploy|promote|rollback --config <rollout.yaml> "
         "[--format text|json]\n"
      << "  vektor capture start --config <run.yaml> "
         "[--state-dir <path>] [--format text|json]\n"
      << "  vektor capture stop --run-id <id> [--outcome <value>] "
         "[--annotation <text>]... [--metric <name=value>]...\n"
      << "                      [--state-dir <path>] "
         "[--format text|json]\n"
      << "  vektor capture show --run-id <id> [--state-dir <path>] "
         "[--format text|json]\n"
      << "  vektor capture export --run-id <id> --output <new-directory> "
         "[--state-dir <path>]\n"
      << "  vektor compare --baseline <run-id> --candidate <run-id> "
         "[--state-dir <path>] [--format text|json]\n"
      << "  vektor replay execute --config <replay.yaml> "
         "[--state-dir <path>] [--replay-dir <path>] "
         "[--format text|json]\n"
      << "  vektor approval-payload --config <rollout.yaml> "
         "--wave <name> --identity <id>\n"
      << "                --issued-at <UTC> --expires-at <UTC>\n"
      << "  vektor support-bundle --config <path> --output <new-directory>\n";
  throw std::invalid_argument("invalid command line");
}

std::string next_value(int &index, int argc, char **argv,
                       const std::string &option) {
  if (index + 1 >= argc)
    usage_error("missing value for " + option);
  return argv[++index];
}

CliOptions parse_cli(int argc, char **argv) {
  if (argc < 2)
    usage_error();
  CliOptions options;
  options.command = argv[1];
  if (options.command != "check" && options.command != "status" &&
      options.command != "agent" && options.command != "fleet" &&
      options.command != "validate" && options.command != "support-bundle" &&
      options.command != "deploy" && options.command != "promote" &&
      options.command != "rollback" && options.command != "approval-payload" &&
      options.command != "compare" && options.command != "replay")
    if (options.command != "capture")
      usage_error("unknown command '" + options.command + "'");

  int first_option = 2;
  if (options.command == "capture") {
    if (argc < 3)
      usage_error("capture requires start, stop, or show");
    options.capture_action = argv[2];
    if (options.capture_action != "start" && options.capture_action != "stop" &&
        options.capture_action != "show" && options.capture_action != "export")
      usage_error("capture requires start, stop, show, or export");
    first_option = 3;
  } else if (options.command == "replay") {
    if (argc < 3)
      usage_error("replay requires execute");
    options.replay_action = argv[2];
    if (options.replay_action != "execute")
      usage_error("replay requires execute");
    first_option = 3;
  }

  for (int index = first_option; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--config")
      options.config_path = next_value(index, argc, argv, argument);
    else if (argument == "--format")
      options.format = next_value(index, argc, argv, argument);
    else if (argument == "--type")
      options.validation_type = next_value(index, argc, argv, argument);
    else if (argument == "--robot-id")
      options.robot_id = next_value(index, argc, argv, argument);
    else if (argument == "--fleet-id")
      options.fleet_id = next_value(index, argc, argv, argument);
    else if (argument == "--workload-id")
      options.workload_id = next_value(index, argc, argv, argument);
    else if (argument == "--wave")
      options.wave = next_value(index, argc, argv, argument);
    else if (argument == "--identity")
      options.approval_identity = next_value(index, argc, argv, argument);
    else if (argument == "--issued-at")
      options.issued_at = next_value(index, argc, argv, argument);
    else if (argument == "--expires-at")
      options.expires_at = next_value(index, argc, argv, argument);
    else if (argument == "--watch")
      options.watch = true;
    else if (argument == "--interval-ms") {
      const auto value = next_value(index, argc, argv, argument);
      try {
        std::size_t consumed = 0;
        const auto parsed = std::stoll(value, &consumed);
        if (consumed != value.size())
          throw std::invalid_argument("trailing characters");
        options.interval = std::chrono::milliseconds(parsed);
      } catch (const std::exception &) {
        usage_error("invalid value for --interval-ms");
      }
      if (options.interval.count() <= 0)
        usage_error("--interval-ms must be greater than zero");
    } else if (argument == "--history")
      options.history_path = next_value(index, argc, argv, argument);
    else if (argument == "--no-history")
      options.history = false;
    else if (argument == "--listen")
      options.listen_address = next_value(index, argc, argv, argument);
    else if (argument == "--insecure")
      options.insecure = true;
    else if (argument == "--tls-cert")
      options.tls_certificate = next_value(index, argc, argv, argument);
    else if (argument == "--tls-key")
      options.tls_private_key = next_value(index, argc, argv, argument);
    else if (argument == "--tls-ca")
      options.tls_client_ca = next_value(index, argc, argv, argument);
    else if (argument == "--deployment-state")
      options.deployment_state = next_value(index, argc, argv, argument);
    else if (argument == "--audit-log")
      options.audit_log = next_value(index, argc, argv, argument);
    else if (argument == "--metrics")
      options.metrics = next_value(index, argc, argv, argument);
    else if (argument == "--output")
      options.output_directory = next_value(index, argc, argv, argument);
    else if (argument == "--oci-runtime")
      options.oci_runtime = next_value(index, argc, argv, argument);
    else if (argument == "--runtime-container")
      options.runtime_container = next_value(index, argc, argv, argument);
    else if (argument == "--trust-policy")
      options.trust_policy = next_value(index, argc, argv, argument);
    else if (argument == "--authorization-policy")
      options.authorization_policy = next_value(index, argc, argv, argument);
    else if (argument == "--state-dir")
      options.run_state_directory = next_value(index, argc, argv, argument);
    else if (argument == "--replay-dir")
      options.replay_directory = next_value(index, argc, argv, argument);
    else if (argument == "--run-id")
      options.run_id = next_value(index, argc, argv, argument);
    else if (argument == "--baseline")
      options.baseline_run_id = next_value(index, argc, argv, argument);
    else if (argument == "--candidate")
      options.candidate_run_id = next_value(index, argc, argv, argument);
    else if (argument == "--outcome")
      options.outcome = next_value(index, argc, argv, argument);
    else if (argument == "--annotation")
      options.annotations.push_back(next_value(index, argc, argv, argument));
    else if (argument == "--metric") {
      const auto value = next_value(index, argc, argv, argument);
      const auto separator = value.find('=');
      if (separator == std::string::npos || separator == 0 ||
          separator + 1 == value.size())
        usage_error("--metric must use name=value");
      const auto name = value.substr(0, separator);
      static const std::regex metric_name_pattern(
          "^[A-Za-z][A-Za-z0-9_.-]{0,127}$");
      if (!std::regex_match(name, metric_name_pattern))
        usage_error("invalid metric name '" + name + "'");
      double parsed = 0.0;
      try {
        std::size_t consumed = 0;
        parsed = std::stod(value.substr(separator + 1), &consumed);
        if (consumed != value.size() - separator - 1 || !std::isfinite(parsed))
          throw std::invalid_argument("invalid numeric metric");
      } catch (const std::exception &) {
        usage_error("invalid numeric value for metric '" + name + "'");
      }
      if (!options.run_metrics.emplace(name, parsed).second)
        usage_error("duplicate metric '" + name + "'");
    } else if (argument == "--selector")
      options.selectors.push_back(next_value(index, argc, argv, argument));
    else if (argument == "--limit") {
      const auto value = next_value(index, argc, argv, argument);
      try {
        if (value.empty() || value.front() == '-')
          throw std::invalid_argument("not an unsigned integer");
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed);
        if (consumed != value.size())
          throw std::invalid_argument("trailing characters");
        options.limit = static_cast<std::size_t>(parsed);
      } catch (const std::exception &) {
        usage_error("invalid value for --limit");
      }
      if (*options.limit == 0)
        usage_error("--limit must be greater than zero");
    } else
      usage_error("unknown option '" + argument + "'");
  }

  if (options.format != "text" && options.format != "json")
    usage_error("--format must be text or json");
  if (options.run_metrics.size() > 256)
    usage_error("a run may contain at most 256 metrics");
  const bool capture_command = options.command == "capture";
  const bool compare_command = options.command == "compare";
  const bool replay_command = options.command == "replay";
  const bool has_run_options =
      options.run_state_directory != ".vektor/runs" ||
      options.replay_directory != ".vektor/replays" ||
      !options.run_id.empty() || !options.outcome.empty() ||
      !options.annotations.empty() || !options.run_metrics.empty() ||
      !options.baseline_run_id.empty() || !options.candidate_run_id.empty();
  if (!capture_command && !compare_command && !replay_command &&
      has_run_options)
    usage_error("run option used with another command");
  if (capture_command) {
    if (!options.baseline_run_id.empty() || !options.candidate_run_id.empty())
      usage_error("comparison option used with capture");
    if (options.capture_action == "start") {
      if (options.config_path.empty())
        usage_error("capture start requires --config");
      if (!options.run_id.empty() || !options.outcome.empty() ||
          !options.annotations.empty() || !options.run_metrics.empty() ||
          !options.output_directory.empty())
        usage_error("capture start reads run fields from --config");
    } else {
      if (!options.config_path.empty())
        usage_error("--config is supported only by capture start");
      if (options.run_id.empty())
        usage_error("capture stop and show require --run-id");
      if ((options.capture_action == "show" ||
           options.capture_action == "export") &&
          (!options.outcome.empty() || !options.annotations.empty() ||
           !options.run_metrics.empty()))
        usage_error("outcome, annotations, and metrics are supported only by "
                    "capture stop");
      if (options.capture_action == "export" &&
          options.output_directory.empty())
        usage_error("capture export requires --output");
      if (options.capture_action != "export" &&
          !options.output_directory.empty())
        usage_error("--output is supported only by capture export");
    }
    if (options.watch || !options.robot_id.empty() ||
        !options.fleet_id.empty() || !options.workload_id.empty() ||
        options.history_path || !options.history || options.insecure ||
        options.tls_certificate || options.tls_private_key ||
        options.tls_client_ca || !options.selectors.empty() || options.limit ||
        options.deployment_state != ".vektor/deployment.yaml" ||
        options.audit_log != ".vektor/audit.jsonl" ||
        options.metrics != ".vektor/metrics.prom" ||
        options.oci_runtime != "docker" ||
        options.runtime_container != "vektor-workload" ||
        options.trust_policy || options.authorization_policy ||
        options.interval != std::chrono::milliseconds(5000) ||
        options.listen_address != "127.0.0.1:50051" ||
        !options.validation_type.empty() || !options.wave.empty() ||
        !options.approval_identity.empty() || !options.issued_at.empty() ||
        !options.expires_at.empty())
      usage_error("option is not supported by capture");
    return options;
  }
  if (compare_command) {
    if (options.baseline_run_id.empty() || options.candidate_run_id.empty())
      usage_error("compare requires --baseline and --candidate");
    if (!options.config_path.empty() || !options.run_id.empty() ||
        !options.outcome.empty() || !options.annotations.empty() ||
        !options.run_metrics.empty() || !options.output_directory.empty() ||
        options.watch || !options.robot_id.empty() ||
        !options.fleet_id.empty() || !options.workload_id.empty() ||
        options.history_path || !options.history || options.insecure ||
        options.tls_certificate || options.tls_private_key ||
        options.tls_client_ca || !options.selectors.empty() || options.limit ||
        options.deployment_state != ".vektor/deployment.yaml" ||
        options.audit_log != ".vektor/audit.jsonl" ||
        options.metrics != ".vektor/metrics.prom" ||
        options.oci_runtime != "docker" ||
        options.runtime_container != "vektor-workload" ||
        options.trust_policy || options.authorization_policy ||
        options.interval != std::chrono::milliseconds(5000) ||
        options.listen_address != "127.0.0.1:50051" ||
        !options.validation_type.empty() || !options.wave.empty() ||
        !options.approval_identity.empty() || !options.issued_at.empty() ||
        !options.expires_at.empty())
      usage_error("option is not supported by compare");
    return options;
  }
  if (replay_command) {
    if (options.config_path.empty())
      usage_error("replay execute requires --config");
    if (!options.run_id.empty() || !options.baseline_run_id.empty() ||
        !options.candidate_run_id.empty() || !options.outcome.empty() ||
        !options.annotations.empty() || !options.run_metrics.empty() ||
        !options.output_directory.empty() || options.watch ||
        !options.robot_id.empty() || !options.fleet_id.empty() ||
        !options.workload_id.empty() || options.history_path ||
        !options.history || options.insecure || options.tls_certificate ||
        options.tls_private_key || options.tls_client_ca ||
        !options.selectors.empty() || options.limit ||
        options.deployment_state != ".vektor/deployment.yaml" ||
        options.audit_log != ".vektor/audit.jsonl" ||
        options.metrics != ".vektor/metrics.prom" ||
        options.oci_runtime != "docker" ||
        options.runtime_container != "vektor-workload" ||
        options.trust_policy || options.authorization_policy ||
        options.interval != std::chrono::milliseconds(5000) ||
        options.listen_address != "127.0.0.1:50051" ||
        !options.validation_type.empty() || !options.wave.empty() ||
        !options.approval_identity.empty() || !options.issued_at.empty() ||
        !options.expires_at.empty())
      usage_error("option is not supported by replay");
    return options;
  }
  if (options.config_path.empty())
    usage_error("--config is required");
  const bool validation_command = options.command == "validate";
  if (!validation_command && !options.validation_type.empty())
    usage_error("--type is supported only by validate");
  if (validation_command) {
    static const std::vector<std::string> validation_types{
        "health",          "fleet",     "rollout", "authorization",
        "approval-policy", "approvals", "trust"};
    if (std::find(validation_types.begin(), validation_types.end(),
                  options.validation_type) == validation_types.end())
      usage_error("validate requires a supported --type");
    if (options.watch || !options.robot_id.empty() ||
        !options.fleet_id.empty() || !options.workload_id.empty() ||
        options.history_path || !options.history || options.insecure ||
        options.tls_certificate || options.tls_private_key ||
        options.tls_client_ca || options.listen_address != "127.0.0.1:50051" ||
        !options.selectors.empty() || options.limit ||
        options.deployment_state != ".vektor/deployment.yaml" ||
        options.audit_log != ".vektor/audit.jsonl" ||
        options.oci_runtime != "docker" ||
        options.runtime_container != "vektor-workload" ||
        options.trust_policy || options.authorization_policy ||
        options.interval != std::chrono::milliseconds(5000))
      usage_error("option is not supported by validate");
  }
  const bool approval_payload_command = options.command == "approval-payload";
  const bool has_approval_payload_options =
      !options.wave.empty() || !options.approval_identity.empty() ||
      !options.issued_at.empty() || !options.expires_at.empty();
  if (!approval_payload_command && has_approval_payload_options)
    usage_error("approval-payload option used with another command");
  if (options.command == "check" &&
      (options.watch || !options.robot_id.empty() ||
       !options.fleet_id.empty() || !options.workload_id.empty() ||
       options.history_path || !options.history || options.insecure ||
       options.tls_certificate || options.tls_private_key ||
       options.tls_client_ca ||
       options.deployment_state != ".vektor/deployment.yaml" ||
       options.audit_log != ".vektor/audit.jsonl" ||
       options.oci_runtime != "docker" ||
       options.runtime_container != "vektor-workload" || options.trust_policy ||
       options.authorization_policy ||
       options.listen_address != "127.0.0.1:50051" ||
       !options.selectors.empty() || options.limit))
    usage_error("status-only option used with check");
  if (options.command == "status" &&
      (!options.fleet_id.empty() || !options.workload_id.empty() ||
       options.insecure || options.tls_certificate || options.tls_private_key ||
       options.tls_client_ca || options.listen_address != "127.0.0.1:50051" ||
       options.deployment_state != ".vektor/deployment.yaml" ||
       options.audit_log != ".vektor/audit.jsonl" ||
       options.oci_runtime != "docker" || !options.selectors.empty() ||
       options.runtime_container != "vektor-workload" || options.trust_policy ||
       options.authorization_policy || options.limit))
    usage_error("agent-only option used with status");
  if (options.command == "agent" &&
      (options.watch || options.format != "text" ||
       !options.selectors.empty() || options.limit))
    usage_error("status-only option used with agent");
  if (options.command == "fleet" &&
      (!options.robot_id.empty() || !options.fleet_id.empty() ||
       !options.workload_id.empty() || options.history_path ||
       !options.history || options.insecure || options.tls_certificate ||
       options.tls_private_key || options.tls_client_ca ||
       options.listen_address != "127.0.0.1:50051" ||
       options.deployment_state != ".vektor/deployment.yaml" ||
       options.audit_log != ".vektor/audit.jsonl" ||
       options.oci_runtime != "docker" ||
       options.runtime_container != "vektor-workload" || options.trust_policy ||
       options.authorization_policy))
    usage_error("option is not supported by fleet");
  const bool rollout_command = options.command == "deploy" ||
                               options.command == "promote" ||
                               options.command == "rollback";
  if (rollout_command &&
      (options.watch || !options.robot_id.empty() ||
       !options.fleet_id.empty() || !options.workload_id.empty() ||
       options.history_path || !options.history || options.insecure ||
       options.tls_certificate || options.tls_private_key ||
       options.tls_client_ca || options.listen_address != "127.0.0.1:50051" ||
       !options.selectors.empty() || options.limit ||
       options.deployment_state != ".vektor/deployment.yaml" ||
       options.audit_log != ".vektor/audit.jsonl" ||
       options.oci_runtime != "docker" ||
       options.runtime_container != "vektor-workload" || options.trust_policy ||
       options.authorization_policy ||
       options.interval != std::chrono::milliseconds(5000)))
    usage_error("option is not supported by rollout commands");
  if (approval_payload_command) {
    if (options.wave.empty() || options.approval_identity.empty() ||
        options.issued_at.empty() || options.expires_at.empty())
      usage_error(
          "approval-payload requires --wave, --identity, --issued-at, and "
          "--expires-at");
    if (options.format != "text" || options.watch ||
        !options.robot_id.empty() || !options.fleet_id.empty() ||
        !options.workload_id.empty() || options.history_path ||
        !options.history || options.insecure || options.tls_certificate ||
        options.tls_private_key || options.tls_client_ca ||
        options.listen_address != "127.0.0.1:50051" ||
        !options.selectors.empty() || options.limit ||
        options.deployment_state != ".vektor/deployment.yaml" ||
        options.audit_log != ".vektor/audit.jsonl" ||
        options.oci_runtime != "docker" ||
        options.runtime_container != "vektor-workload" ||
        options.trust_policy || options.authorization_policy ||
        options.interval != std::chrono::milliseconds(5000))
      usage_error("option is not supported by approval-payload");
  }
  return options;
}

int run_check(const CliOptions &options, const vektor::CheckConfig &config,
              const rclcpp::Node::SharedPtr &node) {
  const auto results = vektor::HealthInspector(node).inspect(config);
  if (options.format == "json")
    vektor::print_results_json(results, std::cout);
  else
    vektor::print_results(results, std::cout);
  return vektor::all_checks_passed(results) ? 0 : 1;
}

int run_validate(const CliOptions &options) {
  if (options.validation_type == "health")
    static_cast<void>(vektor::load_config(options.config_path));
  else if (options.validation_type == "fleet")
    static_cast<void>(vektor::load_fleet_config(options.config_path));
  else if (options.validation_type == "rollout")
    static_cast<void>(vektor::load_rollout_config(options.config_path));
  else if (options.validation_type == "authorization")
    static_cast<void>(vektor::load_authorization_policy(options.config_path));
  else if (options.validation_type == "approval-policy")
    static_cast<void>(vektor::load_approval_policy(options.config_path));
  else if (options.validation_type == "approvals")
    static_cast<void>(vektor::load_approval_records(options.config_path));
  else
    static_cast<void>(vektor::load_trust_policy(options.config_path));

  if (options.format == "json")
    std::cout << "{\"schema_version\":1,\"valid\":true,\"type\":\""
              << options.validation_type << "\"}\n";
  else
    std::cout << "VEKTOR VALIDATION PASSED\ntype: " << options.validation_type
              << '\n';
  return 0;
}

int run_status(const CliOptions &options, const vektor::CheckConfig &config,
               const rclcpp::Node::SharedPtr &node) {
  const auto history_path =
      options.history_path.value_or(vektor::default_status_history_path());
  std::optional<vektor::SnapshotStore> store;
  if (options.history)
    store.emplace(history_path);

  int exit_code = 0;
  bool first = true;
  do {
    const auto started_at = std::chrono::steady_clock::now();
    auto results = vektor::HealthInspector(node).inspect(config);
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    const auto robot_id =
        options.robot_id.empty() ? config.robot_id : options.robot_id;
    const auto snapshot =
        vektor::make_status_snapshot(robot_id, std::move(results), duration);
    if (store)
      store->append(snapshot);

    if (!first && options.format == "text")
      std::cout << "\n---\n\n";
    if (options.format == "json")
      std::cout << vektor::status_to_json(snapshot) << '\n';
    else
      vektor::print_status(snapshot, std::cout);
    std::cout.flush();
    first = false;
    exit_code = snapshot.state == vektor::HealthState::Healthy ||
                        snapshot.state == vektor::HealthState::Degraded
                    ? 0
                    : 1;

    if (options.watch && rclcpp::ok())
      std::this_thread::sleep_for(options.interval);
  } while (options.watch && rclcpp::ok());
  return exit_code;
}

int run_agent(const CliOptions &options, const vektor::CheckConfig &config,
              const rclcpp::Node::SharedPtr &node) {
  vektor::AgentOptions agent_options;
  agent_options.listen_address = options.listen_address;
  agent_options.health_policy_path = options.config_path;
  agent_options.interval = options.interval;
  agent_options.history_path = options.history_path;
  agent_options.history = options.history;
  agent_options.insecure = options.insecure;
  agent_options.tls_certificate = options.tls_certificate;
  agent_options.tls_private_key = options.tls_private_key;
  agent_options.tls_client_ca = options.tls_client_ca;
  agent_options.deployment_state_path = options.deployment_state;
  agent_options.audit_log_path = options.audit_log;
  agent_options.metrics_path = options.metrics;
  agent_options.oci_runtime = options.oci_runtime;
  agent_options.runtime_container = options.runtime_container;
  agent_options.trust_policy_path = options.trust_policy;
  agent_options.authorization_policy_path = options.authorization_policy;
  agent_options.resource_scope = {options.fleet_id, options.workload_id};
  const auto robot_id =
      options.robot_id.empty() ? config.robot_id : options.robot_id;
  return vektor::AgentRunner(node, config, robot_id, std::move(agent_options))
      .run();
}

int run_fleet(const CliOptions &options, const vektor::FleetConfig &config) {
  std::vector<vektor::LabelSelector> selectors;
  selectors.reserve(options.selectors.size());
  for (const auto &selector : options.selectors)
    selectors.push_back(vektor::parse_label_selector(selector));

  int exit_code = 0;
  bool first = true;
  do {
    const auto report = vektor::poll_fleet(config, selectors, options.limit);
    if (!first && options.format == "text")
      std::cout << "\n---\n\n";
    if (options.format == "json")
      std::cout << vektor::fleet_report_to_json(report) << '\n';
    else
      vektor::print_fleet_report(report, std::cout);
    std::cout.flush();
    first = false;
    exit_code = report.state == vektor::HealthState::Healthy ||
                        report.state == vektor::HealthState::Degraded
                    ? 0
                    : 1;
    if (options.watch && rclcpp::ok())
      std::this_thread::sleep_for(options.interval);
  } while (options.watch && rclcpp::ok());
  return exit_code;
}

int run_rollout(const CliOptions &options,
                const vektor::RolloutConfig &config) {
  const auto report =
      options.command == "deploy"    ? vektor::deploy_release(config)
      : options.command == "promote" ? vektor::promote_release(config)
                                     : vektor::rollback_release(config);
  if (options.format == "json")
    std::cout << vektor::rollout_report_to_json(report) << '\n';
  else
    vektor::print_rollout_report(report, std::cout);
  return report.success ? 0 : 1;
}

int run_approval_payload(const CliOptions &options,
                         const vektor::RolloutConfig &config) {
  if (std::none_of(config.waves.begin(), config.waves.end(),
                   [&](const auto &wave) { return wave.name == options.wave; }))
    throw std::invalid_argument("unknown rollout wave '" + options.wave + "'");
  const auto fleet =
      vektor::load_fleet_config(config.fleet_config_path.string());
  const vektor::ApprovalRecord record{{config.deployment_id, config.artifact,
                                       fleet.fleet_id, config.workload_id,
                                       config.environment, options.wave},
                                      options.approval_identity,
                                      options.issued_at,
                                      options.expires_at,
                                      {}};
  std::cout << vektor::approval_signing_payload(record);
  return 0;
}

int run_capture(const CliOptions &options) {
  const vektor::RunStore store(options.run_state_directory);
  if (options.capture_action == "export") {
    store.export_run(options.run_id, options.output_directory);
    if (options.format == "json")
      std::cout << vektor::run_manifest_to_json(store.get(options.run_id))
                << '\n';
    else
      std::cout << "VEKTOR RUN EXPORT: " << options.output_directory << '\n';
    return 0;
  }
  vektor::RunManifest manifest;
  if (options.capture_action == "start") {
    manifest = store.start(vektor::load_run_definition(options.config_path));
    const auto artifact_root = options.run_state_directory.parent_path() /
                               "artifacts" / manifest.run_id;
    const auto bag_path = artifact_root / "rosbag2";
    const auto log_path = artifact_root / "recorder.log";
    const vektor::RosbagRecorder recorder;
    try {
      const auto pid = recorder.start(bag_path, manifest.topics,
                                      manifest.storage_id, log_path);
      try {
        manifest = store.attach_recorder(manifest.run_id, pid, bag_path);
      } catch (...) {
        recorder.stop(pid, bag_path);
        throw;
      }
    } catch (const std::exception &error) {
      static_cast<void>(store.complete_capture(
          manifest.run_id, "capture_failed",
          {std::string("recorder launch failed: ") + error.what()}));
      throw;
    }
  } else if (options.capture_action == "stop") {
    manifest = store.get(options.run_id);
    if (manifest.recorder_pid != 0) {
      const vektor::RosbagRecorder recorder;
      recorder.stop(manifest.recorder_pid, manifest.bag_path);
    }
    std::optional<vektor::RunArtifact> artifact;
    if (!manifest.bag_path.empty() &&
        std::filesystem::exists(manifest.bag_path))
      artifact = vektor::fingerprint_run_artifact(manifest.bag_path, "rosbag2");
    manifest = store.complete_capture(options.run_id, options.outcome,
                                      options.annotations, artifact,
                                      options.run_metrics);
  } else {
    manifest = store.get(options.run_id);
  }
  if (options.format == "json")
    std::cout << vektor::run_manifest_to_json(manifest) << '\n';
  else
    vektor::print_run_manifest(manifest, std::cout);
  return 0;
}

int run_compare(const CliOptions &options) {
  const vektor::RunStore store(options.run_state_directory);
  const auto comparison = vektor::compare_runs(
      store.get(options.baseline_run_id), store.get(options.candidate_run_id));
  if (options.format == "json")
    std::cout << vektor::run_comparison_to_json(comparison) << '\n';
  else
    vektor::print_run_comparison(comparison, std::cout);
  return 0;
}

int run_replay(const CliOptions &options) {
  const auto definition = vektor::load_replay_definition(options.config_path);
  const vektor::RunStore store(options.run_state_directory);
  const vektor::ReplayExecutor executor;
  const auto manifest =
      executor.execute(definition, store.get(definition.source_run_id),
                       options.replay_directory);
  if (options.format == "json")
    std::cout << vektor::replay_manifest_to_json(manifest) << '\n';
  else
    vektor::print_replay_manifest(manifest, std::cout);
  return manifest.status == vektor::ReplayStatus::Completed ? 0 : 1;
}

int run_support_bundle(const CliOptions &options) {
  vektor::create_support_bundle(
      options.output_directory, options.config_path,
      options.history_path.value_or(vektor::default_status_history_path()),
      options.metrics);
  std::cout << "VEKTOR SUPPORT BUNDLE: " << options.output_directory << '\n';
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  try {
    const auto options = parse_cli(argc, argv);
    int exit_code = 0;
    if (options.command == "capture") {
      exit_code = run_capture(options);
    } else if (options.command == "compare") {
      exit_code = run_compare(options);
    } else if (options.command == "replay") {
      exit_code = run_replay(options);
    } else if (options.command == "validate") {
      exit_code = run_validate(options);
    } else if (options.command == "support-bundle") {
      exit_code = run_support_bundle(options);
    } else if (options.command == "approval-payload") {
      exit_code = run_approval_payload(
          options, vektor::load_rollout_config(options.config_path));
    } else if (options.command == "deploy" || options.command == "promote" ||
               options.command == "rollback") {
      exit_code = run_rollout(options,
                              vektor::load_rollout_config(options.config_path));
    } else if (options.command == "fleet") {
      exit_code =
          run_fleet(options, vektor::load_fleet_config(options.config_path));
    } else {
      const auto config = vektor::load_config(options.config_path);
      auto node = std::make_shared<rclcpp::Node>("vektor_" + options.command);
      exit_code = options.command == "check" ? run_check(options, config, node)
                  : options.command == "status"
                      ? run_status(options, config, node)
                      : run_agent(options, config, node);
    }
    rclcpp::shutdown();
    return exit_code;
  } catch (const std::invalid_argument &error) {
    if (std::string(error.what()) != "invalid command line")
      std::cerr << "vektor: " << error.what() << '\n';
    rclcpp::shutdown();
    return 2;
  } catch (const std::exception &error) {
    std::cerr << "vektor: " << error.what() << '\n';
    rclcpp::shutdown();
    return 2;
  }
}
