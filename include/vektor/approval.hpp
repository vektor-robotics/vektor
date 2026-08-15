#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace vektor {

struct ApprovalPolicy {
  std::set<std::string> sensitive_environments;
  std::size_t max_wave_size_without_approval{0};
  std::size_t required_approvals{1};
  std::chrono::milliseconds max_approval_lifetime{86400000};
  std::map<std::string, std::filesystem::path> approver_public_keys;
};

struct ApprovalContext {
  std::string deployment_id;
  std::string artifact;
  std::string fleet_id;
  std::string workload_id;
  std::string environment;
  std::string wave;
};

struct ApprovalRecord {
  ApprovalContext context;
  std::string identity;
  std::string issued_at;
  std::string expires_at;
  std::string signature;
};

ApprovalPolicy load_approval_policy(const std::filesystem::path &path);
std::vector<ApprovalRecord>
load_approval_records(const std::filesystem::path &path);
bool approval_required(const ApprovalPolicy &policy,
                       const std::string &environment, std::size_t wave_size);
std::string approval_signing_payload(const ApprovalRecord &record);
std::vector<std::string>
verify_approval_records(const ApprovalPolicy &policy,
                        const std::vector<ApprovalRecord> &records,
                        const ApprovalContext &context,
                        std::chrono::system_clock::time_point now =
                            std::chrono::system_clock::now());

} // namespace vektor
