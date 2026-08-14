#include "vektor/config.hpp"
#include "vektor/health_inspector.hpp"
#include "vektor/reporter.hpp"

#include <rclcpp/rclcpp.hpp>

#include <iostream>
#include <stdexcept>

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  try {
    if (argc < 2 || std::string(argv[1]) != "check") {
      std::cerr << "Usage: vektor check --config <path>\n";
      rclcpp::shutdown();
      return 2;
    }
    std::string config_path;
    std::string format{"text"};
    for (int i = 2; i < argc; ++i) {
      if (std::string(argv[i]) == "--config" && i + 1 < argc) {
        config_path = argv[++i];
      } else if (std::string(argv[i]) == "--format" && i + 1 < argc) {
        format = argv[++i];
      }
    }
    if (config_path.empty()) {
      std::cerr << "Usage: vektor check --config <path>\n";
      rclcpp::shutdown();
      return 2;
    }
    if (format != "text" && format != "json") {
      throw std::runtime_error("unsupported output format '" + format +
                               "'; expected text or json");
    }
    auto node = std::make_shared<rclcpp::Node>("vektor_check");
    const auto results =
        vektor::HealthInspector(node).inspect(vektor::load_config(config_path));
    if (format == "json")
      vektor::print_results_json(results, std::cout);
    else
      vektor::print_results(results, std::cout);
    const bool passed = vektor::all_checks_passed(results);
    rclcpp::shutdown();
    return passed ? 0 : 1;
  } catch (const std::exception &error) {
    std::cerr << "vektor: " << error.what() << '\n';
    rclcpp::shutdown();
    return 2;
  }
}
