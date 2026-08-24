#pragma once

#include "vektor/status.hpp"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>

namespace vektor {

class OperationalMetrics {
public:
  void record_health(HealthState state);
  void record_authorization_denial();
  void record_reconciliation(bool success);
  void record_rollout(bool success);
  void record_rpc(std::chrono::milliseconds latency);
  void write_prometheus(const std::filesystem::path &path) const;
  std::string prometheus() const;

private:
  mutable std::mutex mutex_;
  unsigned long long health_healthy_{0};
  unsigned long long health_degraded_{0};
  unsigned long long health_unhealthy_{0};
  unsigned long long health_unreachable_{0};
  unsigned long long authorization_denials_{0};
  unsigned long long reconciliation_successes_{0};
  unsigned long long reconciliation_failures_{0};
  unsigned long long rollout_successes_{0};
  unsigned long long rollout_failures_{0};
  unsigned long long rpc_requests_{0};
  unsigned long long rpc_latency_ms_{0};
};

} // namespace vektor
