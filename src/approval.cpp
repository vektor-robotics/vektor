#include "vektor/approval.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <yaml-cpp/yaml.h>

#include <ctime>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

namespace vektor {
namespace {
void reject_unknown(const YAML::Node &node,
                    const std::set<std::string> &allowed,
                    const std::string &field) {
  if (!node || !node.IsMap())
    throw std::runtime_error(field + " must be a mapping");
  for (const auto &entry : node) {
    const auto key = entry.first.as<std::string>();
    if (!allowed.contains(key))
      throw std::runtime_error("unknown " + field + " field '" + key + "'");
  }
}

std::string require_string(const YAML::Node &node, const std::string &field) {
  if (!node || !node.IsScalar())
    throw std::runtime_error(field + " must be a non-empty string");
  const auto value = node.as<std::string>();
  if (value.empty() || value.find('\0') != std::string::npos ||
      value.find('\n') != std::string::npos ||
      value.find('\r') != std::string::npos)
    throw std::runtime_error(field + " must be a non-empty single-line string");
  return value;
}

std::chrono::system_clock::time_point
parse_timestamp(const std::string &value, const std::string &field) {
  std::tm utc{};
  std::istringstream input(value);
  input >> std::get_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  if (input.fail() || input.peek() != std::char_traits<char>::eof())
    throw std::runtime_error(field +
                             " must use UTC format YYYY-MM-DDTHH:MM:SSZ");
  const auto seconds = timegm(&utc);
  if (seconds < 0)
    throw std::runtime_error(field + " is outside the supported range");
  const auto parsed = std::chrono::system_clock::from_time_t(seconds);
  std::ostringstream round_trip;
  round_trip << std::put_time(gmtime(&seconds), "%Y-%m-%dT%H:%M:%SZ");
  if (round_trip.str() != value)
    throw std::runtime_error(field + " is not a valid UTC timestamp");
  return parsed;
}

std::vector<unsigned char> decode_base64(const std::string &value) {
  if (value.empty() || value.size() % 4 != 0)
    throw std::runtime_error("approval signature is not valid base64");
  std::vector<unsigned char> decoded(value.size() / 4 * 3);
  const auto size = EVP_DecodeBlock(
      decoded.data(), reinterpret_cast<const unsigned char *>(value.data()),
      static_cast<int>(value.size()));
  if (size < 0)
    throw std::runtime_error("approval signature is not valid base64");
  auto actual = static_cast<std::size_t>(size);
  if (!value.empty() && value.back() == '=')
    --actual;
  if (value.size() > 1 && value[value.size() - 2] == '=')
    --actual;
  decoded.resize(actual);
  return decoded;
}

bool verify_signature(const std::filesystem::path &key_path,
                      const std::string &payload,
                      const std::string &signature) {
  using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
  using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
  using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  Bio key_file(BIO_new_file(key_path.c_str(), "r"), BIO_free);
  if (!key_file)
    throw std::runtime_error("failed to open approver public key '" +
                             key_path.string() + "'");
  Key key(PEM_read_bio_PUBKEY(key_file.get(), nullptr, nullptr, nullptr),
          EVP_PKEY_free);
  if (!key)
    throw std::runtime_error("failed to parse approver public key '" +
                             key_path.string() + "'");
  Digest digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!digest || EVP_DigestVerifyInit(digest.get(), nullptr, EVP_sha256(),
                                      nullptr, key.get()) != 1)
    throw std::runtime_error("failed to initialize approval verification");
  const auto bytes = decode_base64(signature);
  return EVP_DigestVerify(
             digest.get(), bytes.data(), bytes.size(),
             reinterpret_cast<const unsigned char *>(payload.data()),
             payload.size()) == 1;
}

void append_field(std::ostringstream &output, const std::string &name,
                  const std::string &value) {
  if (value.empty() || value.find('\0') != std::string::npos ||
      value.find('\n') != std::string::npos ||
      value.find('\r') != std::string::npos)
    throw std::runtime_error("approval payload field '" + name +
                             "' must be a non-empty single-line string");
  output << name << ':' << value.size() << ':' << value << '\n';
}
} // namespace

ApprovalPolicy load_approval_policy(const std::filesystem::path &path) {
  try {
    const auto root = YAML::LoadFile(path.string());
    reject_unknown(root,
                   {"schema_version", "sensitive_environments",
                    "max_wave_size_without_approval", "required_approvals",
                    "max_approval_lifetime_ms", "approvers"},
                   "approval policy");
    if (!root["schema_version"] ||
        root["schema_version"].as<unsigned int>() != 1)
      throw std::runtime_error("approval policy schema_version must be 1");
    ApprovalPolicy policy;
    const auto environments = root["sensitive_environments"];
    if (!environments || !environments.IsSequence())
      throw std::runtime_error("sensitive_environments must be a sequence");
    for (std::size_t index = 0; index < environments.size(); ++index)
      policy.sensitive_environments.insert(
          require_string(environments[index], "sensitive_environments[" +
                                                  std::to_string(index) + "]"));
    if (!root["max_wave_size_without_approval"])
      throw std::runtime_error("max_wave_size_without_approval is required");
    policy.max_wave_size_without_approval =
        root["max_wave_size_without_approval"].as<std::size_t>();
    const auto required = root["required_approvals"]
                              ? root["required_approvals"].as<long long>()
                              : 1;
    if (required <= 0)
      throw std::runtime_error("required_approvals must be greater than zero");
    policy.required_approvals = static_cast<std::size_t>(required);
    const auto lifetime = root["max_approval_lifetime_ms"]
                              ? root["max_approval_lifetime_ms"].as<long long>()
                              : 86400000;
    if (lifetime <= 0 || lifetime > 30LL * 24 * 60 * 60 * 1000)
      throw std::runtime_error(
          "max_approval_lifetime_ms must be between 1 and 2592000000");
    policy.max_approval_lifetime = std::chrono::milliseconds(lifetime);
    const auto approvers = root["approvers"];
    if (!approvers || !approvers.IsSequence() || approvers.size() == 0)
      throw std::runtime_error("approvers must be a non-empty sequence");
    const auto base = std::filesystem::absolute(path).parent_path();
    std::set<std::filesystem::path> public_keys;
    for (std::size_t index = 0; index < approvers.size(); ++index) {
      const auto field = "approvers[" + std::to_string(index) + "]";
      reject_unknown(approvers[index], {"identity", "public_key"}, field);
      const auto identity =
          require_string(approvers[index]["identity"], field + ".identity");
      auto key = std::filesystem::path(require_string(
          approvers[index]["public_key"], field + ".public_key"));
      if (key.is_relative())
        key = base / key;
      key = key.lexically_normal();
      if (!public_keys.insert(key).second)
        throw std::runtime_error(
            "each approver must use a distinct public key path");
      if (!policy.approver_public_keys.emplace(identity, key).second)
        throw std::runtime_error("duplicate approver identity '" + identity +
                                 "'");
    }
    if (policy.required_approvals > policy.approver_public_keys.size())
      throw std::runtime_error(
          "required_approvals exceeds the number of trusted approvers");
    return policy;
  } catch (const std::exception &error) {
    throw std::runtime_error("failed to load approval policy '" +
                             path.string() + "': " + error.what());
  }
}

std::vector<ApprovalRecord>
load_approval_records(const std::filesystem::path &path) {
  try {
    const auto root = YAML::LoadFile(path.string());
    reject_unknown(root, {"schema_version", "approvals"}, "approval bundle");
    if (!root["schema_version"] ||
        root["schema_version"].as<unsigned int>() != 1)
      throw std::runtime_error("approval bundle schema_version must be 1");
    const auto approvals = root["approvals"];
    if (!approvals || !approvals.IsSequence() || approvals.size() == 0)
      throw std::runtime_error("approvals must be a non-empty sequence");
    std::vector<ApprovalRecord> records;
    for (std::size_t index = 0; index < approvals.size(); ++index) {
      const auto item = approvals[index];
      const auto field = "approvals[" + std::to_string(index) + "]";
      reject_unknown(item,
                     {"identity", "deployment_id", "artifact", "fleet_id",
                      "workload_id", "environment", "wave", "issued_at",
                      "expires_at", "signature"},
                     field);
      ApprovalRecord record;
      record.identity = require_string(item["identity"], field + ".identity");
      record.context.deployment_id =
          require_string(item["deployment_id"], field + ".deployment_id");
      record.context.artifact =
          require_string(item["artifact"], field + ".artifact");
      record.context.fleet_id =
          require_string(item["fleet_id"], field + ".fleet_id");
      record.context.workload_id =
          require_string(item["workload_id"], field + ".workload_id");
      record.context.environment =
          require_string(item["environment"], field + ".environment");
      record.context.wave = require_string(item["wave"], field + ".wave");
      record.issued_at =
          require_string(item["issued_at"], field + ".issued_at");
      record.expires_at =
          require_string(item["expires_at"], field + ".expires_at");
      record.signature =
          require_string(item["signature"], field + ".signature");
      records.push_back(std::move(record));
    }
    return records;
  } catch (const std::exception &error) {
    throw std::runtime_error("failed to load approval bundle '" +
                             path.string() + "': " + error.what());
  }
}

bool approval_required(const ApprovalPolicy &policy,
                       const std::string &environment, std::size_t wave_size) {
  return policy.sensitive_environments.contains(environment) ||
         wave_size > policy.max_wave_size_without_approval;
}

std::string approval_signing_payload(const ApprovalRecord &record) {
  const auto issued = parse_timestamp(record.issued_at, "issued_at");
  const auto expires = parse_timestamp(record.expires_at, "expires_at");
  if (expires <= issued)
    throw std::runtime_error("expires_at must be later than issued_at");
  std::ostringstream output;
  output << "VEKTOR-APPROVAL-V1\n";
  append_field(output, "identity", record.identity);
  append_field(output, "deployment_id", record.context.deployment_id);
  append_field(output, "artifact", record.context.artifact);
  append_field(output, "fleet_id", record.context.fleet_id);
  append_field(output, "workload_id", record.context.workload_id);
  append_field(output, "environment", record.context.environment);
  append_field(output, "wave", record.context.wave);
  append_field(output, "issued_at", record.issued_at);
  append_field(output, "expires_at", record.expires_at);
  return output.str();
}

std::vector<std::string> verify_approval_records(
    const ApprovalPolicy &policy, const std::vector<ApprovalRecord> &records,
    const ApprovalContext &context, std::chrono::system_clock::time_point now) {
  std::vector<std::string> verified;
  std::set<std::string> seen;
  for (const auto &record : records) {
    if (record.context.deployment_id != context.deployment_id ||
        record.context.artifact != context.artifact ||
        record.context.fleet_id != context.fleet_id ||
        record.context.workload_id != context.workload_id ||
        record.context.environment != context.environment ||
        record.context.wave != context.wave)
      continue;
    const auto key = policy.approver_public_keys.find(record.identity);
    if (key == policy.approver_public_keys.end() ||
        !seen.insert(record.identity).second)
      continue;
    const auto issued = parse_timestamp(record.issued_at, "issued_at");
    const auto expires = parse_timestamp(record.expires_at, "expires_at");
    if (issued > now || expires <= now || expires <= issued ||
        expires - issued > policy.max_approval_lifetime)
      continue;
    if (verify_signature(key->second, approval_signing_payload(record),
                         record.signature))
      verified.push_back(record.identity);
  }
  if (verified.size() < policy.required_approvals)
    throw std::runtime_error("VEKTOR_APPROVAL_REQUIRED: wave '" + context.wave +
                             "' has " + std::to_string(verified.size()) +
                             " of " +
                             std::to_string(policy.required_approvals) +
                             " required valid approvals");
  return verified;
}

} // namespace vektor
