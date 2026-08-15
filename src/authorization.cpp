#include "vektor/authorization.hpp"

#include <yaml-cpp/yaml.h>

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

bool AuthorizationPolicy::allows(const std::string &authenticated_identity,
                                 AuthorizationAction action) const {
  const auto grant = grants_.find(authenticated_identity);
  if (grant == grants_.end())
    return false;
  for (const auto role : grant->second) {
    if (role_allows(role, action))
      return true;
  }
  return false;
}

AuthorizationPolicy
load_authorization_policy(const std::filesystem::path &path) {
  try {
    const auto root = YAML::LoadFile(path.string());
    reject_unknown(root, {"schema_version", "identities"},
                   "authorization policy");
    if (!root["schema_version"] ||
        root["schema_version"].as<unsigned int>() != 1)
      throw std::runtime_error("authorization policy schema_version must be 1");
    const auto identities = root["identities"];
    if (!identities || !identities.IsSequence() || identities.size() == 0)
      throw std::runtime_error(
          "authorization policy identities must be a non-empty sequence");

    AuthorizationPolicy policy;
    for (std::size_t index = 0; index < identities.size(); ++index) {
      const auto item = identities[index];
      const auto field = "identities[" + std::to_string(index) + "]";
      reject_unknown(item, {"identity", "roles"}, field);
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
        grant.insert(parse_role(require_string(
            roles[role_index],
            field + ".roles[" + std::to_string(role_index) + "]")));
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
