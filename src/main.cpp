#include "vektor/agent.hpp"
#include "vektor/config.hpp"
#include "vektor/fleet.hpp"
#include "vektor/health_inspector.hpp"
#include "vektor/reporter.hpp"
#include "vektor/rollout.hpp"
#include "vektor/status.hpp"

#include <rclcpp/rclcpp.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
struct CliOptions {
  std::string command;
  std::string config_path;
  std::string format{"text"};
  std::string robot_id;
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
  std::string oci_runtime{"docker"};
  std::string runtime_container{"vektor-workload"};
};

[[noreturn]] void usage_error(const std::string &message = {}) {
  if (!message.empty())
    std::cerr << "vektor: " << message << "\n\n";
  std::cerr << "Usage:\n"
            << "  vektor check  --config <path> [--format text|json]\n"
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
               "[--deployment-state <path>]\n"
            << "  vektor fleet  --config <path> [--format text|json] "
               "[--selector key=value]...\n"
            << "                [--limit <count>] [--watch] "
               "[--interval-ms <ms>]\n"
            << "  vektor deploy|promote|rollback --config <rollout.yaml> "
               "[--format text|json]\n";
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
      options.command != "deploy" && options.command != "promote" &&
      options.command != "rollback")
    usage_error("unknown command '" + options.command + "'");

  for (int index = 2; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--config")
      options.config_path = next_value(index, argc, argv, argument);
    else if (argument == "--format")
      options.format = next_value(index, argc, argv, argument);
    else if (argument == "--robot-id")
      options.robot_id = next_value(index, argc, argv, argument);
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
    else if (argument == "--oci-runtime")
      options.oci_runtime = next_value(index, argc, argv, argument);
    else if (argument == "--runtime-container")
      options.runtime_container = next_value(index, argc, argv, argument);
    else if (argument == "--selector")
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

  if (options.config_path.empty())
    usage_error("--config is required");
  if (options.format != "text" && options.format != "json")
    usage_error("--format must be text or json");
  if (options.command == "check" &&
      (options.watch || !options.robot_id.empty() || options.history_path ||
       !options.history || options.insecure || options.tls_certificate ||
       options.tls_private_key || options.tls_client_ca ||
       options.deployment_state != ".vektor/deployment.yaml" ||
       options.oci_runtime != "docker" ||
       options.runtime_container != "vektor-workload" ||
       options.listen_address != "127.0.0.1:50051" ||
       !options.selectors.empty() || options.limit))
    usage_error("status-only option used with check");
  if (options.command == "status" &&
      (options.insecure || options.tls_certificate || options.tls_private_key ||
       options.tls_client_ca || options.listen_address != "127.0.0.1:50051" ||
       options.deployment_state != ".vektor/deployment.yaml" ||
       options.oci_runtime != "docker" || !options.selectors.empty() ||
       options.runtime_container != "vektor-workload" || options.limit))
    usage_error("agent-only option used with status");
  if (options.command == "agent" &&
      (options.watch || options.format != "text" ||
       !options.selectors.empty() || options.limit))
    usage_error("status-only option used with agent");
  if (options.command == "fleet" &&
      (!options.robot_id.empty() || options.history_path || !options.history ||
       options.insecure || options.tls_certificate || options.tls_private_key ||
       options.tls_client_ca || options.listen_address != "127.0.0.1:50051" ||
       options.deployment_state != ".vektor/deployment.yaml" ||
       options.oci_runtime != "docker" ||
       options.runtime_container != "vektor-workload"))
    usage_error("option is not supported by fleet");
  const bool rollout_command = options.command == "deploy" ||
                               options.command == "promote" ||
                               options.command == "rollback";
  if (rollout_command &&
      (options.watch || !options.robot_id.empty() || options.history_path ||
       !options.history || options.insecure || options.tls_certificate ||
       options.tls_private_key || options.tls_client_ca ||
       options.listen_address != "127.0.0.1:50051" ||
       !options.selectors.empty() || options.limit ||
       options.deployment_state != ".vektor/deployment.yaml" ||
       options.oci_runtime != "docker" ||
       options.runtime_container != "vektor-workload" ||
       options.interval != std::chrono::milliseconds(5000)))
    usage_error("option is not supported by rollout commands");
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
  agent_options.interval = options.interval;
  agent_options.history_path = options.history_path;
  agent_options.history = options.history;
  agent_options.insecure = options.insecure;
  agent_options.tls_certificate = options.tls_certificate;
  agent_options.tls_private_key = options.tls_private_key;
  agent_options.tls_client_ca = options.tls_client_ca;
  agent_options.deployment_state_path = options.deployment_state;
  agent_options.oci_runtime = options.oci_runtime;
  agent_options.runtime_container = options.runtime_container;
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
} // namespace

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  try {
    const auto options = parse_cli(argc, argv);
    int exit_code = 0;
    if (options.command == "deploy" || options.command == "promote" ||
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
