#include "vektor/authorization.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <set>
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

AuthorizationRole parse_role(const std::string &value) {
  if (value == "viewer")
    return AuthorizationRole::Viewer;
  if (value == "deployer")
    return AuthorizationRole::Deployer;
  if (value == "operator")
    return AuthorizationRole::Operator;
  if (value == "admin")
    return AuthorizationRole::Admin;
  throw std::runtime_error("unknown authorization role '" + value + "'");
}

bool role_allows(AuthorizationRole role, AuthorizationAction action) {
  if (role == AuthorizationRole::Admin || role == AuthorizationRole::Operator)
    return true;
  if (role == AuthorizationRole::Deployer)
    return action != AuthorizationAction::Rollback;
  return action == AuthorizationAction::Inspect;
}

std::set<std::string> parse_scope_values(const YAML::Node &node,
                                         const std::string &field) {
  if (!node || !node.IsSequence() || node.size() == 0)
    throw std::runtime_error(field + " must be a non-empty sequence");
  std::set<std::string> values;
  for (std::size_t index = 0; index < node.size(); ++index) {
    const auto value =
        require_string(node[index], field + "[" + std::to_string(index) + "]");
    if (!values.insert(value).second)
      throw std::runtime_error("duplicate " + field + " value '" + value + "'");
  }
  return values;
}

bool scope_matches(const std::set<std::string> &allowed,
                   const std::string &requested) {
  return allowed.contains("*") || allowed.contains(requested);
}
} // namespace

const char *authorization_action_name(AuthorizationAction action) {
  switch (action) {
  case AuthorizationAction::Inspect:
    return "inspect";
  case AuthorizationAction::Deploy:
    return "deploy";
  case AuthorizationAction::Promote:
    return "promote";
  case AuthorizationAction::Rollback:
    return "rollback";
  }
  return "unknown";
}

bool authorization_scope_matches(const AuthorizationScope &resource,
                                 const AuthorizationScope &request,
                                 bool require_workload) {
  return !resource.fleet_id.empty() && resource.fleet_id == request.fleet_id &&
         (!require_workload ||
          (!request.workload_id.empty() &&
           (resource.workload_id.empty() ||
            resource.workload_id == request.workload_id)));
}

bool AuthorizationPolicy::allows(const std::string &authenticated_identity,
                                 AuthorizationAction action) const {
  return allows(authenticated_identity, action, {}, false);
}

bool AuthorizationPolicy::allows(const std::string &authenticated_identity,
                                 AuthorizationAction action,
                                 const AuthorizationScope &scope,
                                 bool require_workload) const {
  const auto grant = grants_.find(authenticated_identity);
  if (grant == grants_.end())
    return false;
  const auto role_allowed = std::any_of(
      grant->second.roles.begin(), grant->second.roles.end(),
      [action](AuthorizationRole role) { return role_allows(role, action); });
  if (!role_allowed || !grant->second.scoped)
    return role_allowed;
  if (scope.fleet_id.empty() ||
      !scope_matches(grant->second.fleets, scope.fleet_id))
    return false;
  return !require_workload ||
         (!scope.workload_id.empty() &&
          scope_matches(grant->second.workloads, scope.workload_id));
}

AuthorizationPolicy
load_authorization_policy(const std::filesystem::path &path) {
  try {
    const auto root = YAML::LoadFile(path.string());
    reject_unknown(root, {"schema_version", "identities"},
                   "authorization policy");
    if (!root["schema_version"])
      throw std::runtime_error(
          "authorization policy schema_version is required");
    const auto schema_version = root["schema_version"].as<unsigned int>();
    if (schema_version != 1 && schema_version != 2)
      throw std::runtime_error(
          "authorization policy schema_version must be 1 or 2");
    const auto identities = root["identities"];
    if (!identities || !identities.IsSequence() || identities.size() == 0)
      throw std::runtime_error(
          "authorization policy identities must be a non-empty sequence");

    AuthorizationPolicy policy;
    for (std::size_t index = 0; index < identities.size(); ++index) {
      const auto item = identities[index];
      const auto field = "identities[" + std::to_string(index) + "]";
      reject_unknown(item,
                     schema_version == 1
                         ? std::set<std::string>{"identity", "roles"}
                         : std::set<std::string>{"identity", "roles", "scopes"},
                     field);
      const auto identity =
          require_string(item["identity"], field + ".identity");
      if (policy.grants_.contains(identity))
        throw std::runtime_error("duplicate authorization identity '" +
                                 identity + "'");
      const auto roles = item["roles"];
      if (!roles || !roles.IsSequence() || roles.size() == 0)
        throw std::runtime_error(field + ".roles must be a non-empty sequence");
      auto &grant = policy.grants_[identity];
      for (std::size_t role_index = 0; role_index < roles.size(); ++role_index)
        grant.roles.insert(parse_role(require_string(
            roles[role_index],
            field + ".roles[" + std::to_string(role_index) + "]")));
      if (schema_version == 2) {
        const auto scopes = item["scopes"];
        reject_unknown(scopes, {"fleets", "workloads"}, field + ".scopes");
        grant.fleets =
            parse_scope_values(scopes["fleets"], field + ".scopes.fleets");
        grant.workloads = parse_scope_values(scopes["workloads"],
                                             field + ".scopes.workloads");
        grant.scoped = true;
      }
    }
    return policy;
  } catch (const std::exception &error) {
    throw std::runtime_error("failed to load authorization policy '" +
                             path.string() + "': " + error.what());
  }
}

std::string authorization_denial_json(AuthorizationAction action) {
  return std::string{"{\"schema_version\":1,\"code\":"
                     "\"VEKTOR_AUTHORIZATION_DENIED\",\"action\":\""} +
         authorization_action_name(action) + "\"}";
}

} // namespace vektor
