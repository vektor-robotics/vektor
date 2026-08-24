#include "vektor/metrics.hpp"

#include <fstream>
#include <stdexcept>

namespace vektor {
void OperationalMetrics::record_health(HealthState state) {
  std::lock_guard lock(mutex_);
  switch (state) {
  case HealthState::Healthy: ++health_healthy_; break;
  case HealthState::Degraded: ++health_degraded_; break;
  case HealthState::Unhealthy: ++health_unhealthy_; break;
  case HealthState::Unreachable: ++health_unreachable_; break;
  }
}
void OperationalMetrics::record_authorization_denial() { std::lock_guard lock(mutex_); ++authorization_denials_; }
void OperationalMetrics::record_reconciliation(bool success) { std::lock_guard lock(mutex_); success ? ++reconciliation_successes_ : ++reconciliation_failures_; }
void OperationalMetrics::record_rpc(std::chrono::milliseconds latency) { std::lock_guard lock(mutex_); ++rpc_requests_; rpc_latency_ms_ += static_cast<unsigned long long>(latency.count()); }
std::string OperationalMetrics::prometheus() const {
  std::lock_guard lock(mutex_);
  return "# TYPE vektor_health_inspections_total counter\nvektor_health_inspections_total{state=\"healthy\"} " + std::to_string(health_healthy_) + "\nvektor_health_inspections_total{state=\"degraded\"} " + std::to_string(health_degraded_) + "\nvektor_health_inspections_total{state=\"unhealthy\"} " + std::to_string(health_unhealthy_) + "\nvektor_health_inspections_total{state=\"unreachable\"} " + std::to_string(health_unreachable_) + "\n# TYPE vektor_authorization_denials_total counter\nvektor_authorization_denials_total " + std::to_string(authorization_denials_) + "\n# TYPE vektor_reconciliation_total counter\nvektor_reconciliation_total{outcome=\"success\"} " + std::to_string(reconciliation_successes_) + "\nvektor_reconciliation_total{outcome=\"failure\"} " + std::to_string(reconciliation_failures_) + "\n# TYPE vektor_rpc_requests_total counter\nvektor_rpc_requests_total " + std::to_string(rpc_requests_) + "\n# TYPE vektor_rpc_latency_milliseconds_total counter\nvektor_rpc_latency_milliseconds_total " + std::to_string(rpc_latency_ms_) + "\n";
}
void OperationalMetrics::write_prometheus(const std::filesystem::path &path) const {
  if (path.empty()) return;
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".tmp";
  { std::ofstream output(temporary, std::ios::trunc); if (!output) throw std::runtime_error("cannot write metrics file"); output << prometheus(); }
  std::filesystem::rename(temporary, path);
}
} // namespace vektor
