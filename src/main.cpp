#include "vektor/config.hpp"
#include "vektor/health_inspector.hpp"
#include "vektor/reporter.hpp"
#include "vektor/status.hpp"

#include <rclcpp/rclcpp.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

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
};

[[noreturn]] void usage_error(const std::string &message = {}) {
  if (!message.empty())
    std::cerr << "vektor: " << message << "\n\n";
  std::cerr << "Usage:\n"
            << "  vektor check  --config <path> [--format text|json]\n"
            << "  vektor status --config <path> [--format text|json] "
               "[--robot-id <id>]\n"
            << "                [--watch] [--interval-ms <ms>] "
               "[--history <path>|--no-history]\n";
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
  if (options.command != "check" && options.command != "status")
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
        options.interval = std::chrono::milliseconds(std::stoll(value));
      } catch (const std::exception &) {
        usage_error("invalid value for --interval-ms");
      }
      if (options.interval.count() <= 0)
        usage_error("--interval-ms must be greater than zero");
    } else if (argument == "--history")
      options.history_path = next_value(index, argc, argv, argument);
    else if (argument == "--no-history")
      options.history = false;
    else
      usage_error("unknown option '" + argument + "'");
  }

  if (options.config_path.empty())
    usage_error("--config is required");
  if (options.format != "text" && options.format != "json")
    usage_error("--format must be text or json");
  if (options.command == "check" &&
      (options.watch || !options.robot_id.empty() || options.history_path ||
       !options.history))
    usage_error("status-only option used with check");
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
} // namespace

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  try {
    const auto options = parse_cli(argc, argv);
    const auto config = vektor::load_config(options.config_path);
    auto node = std::make_shared<rclcpp::Node>("vektor_" + options.command);
    const int exit_code = options.command == "check"
                              ? run_check(options, config, node)
                              : run_status(options, config, node);
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
