#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace vektor {

enum class AuthorizationRole { Viewer, Deployer, Operator, Admin };
enum class AuthorizationAction { Inspect, Deploy, Promote, Rollback };

struct AuthorizationScope {
  std::string fleet_id;
  std::string workload_id;
};

const char *authorization_action_name(AuthorizationAction action);
bool authorization_scope_matches(const AuthorizationScope &resource,
                                 const AuthorizationScope &request,
                                 bool require_workload);

class AuthorizationPolicy {
public:
  bool allows(const std::string &authenticated_identity,
              AuthorizationAction action) const;
  bool allows(const std::string &authenticated_identity,
              AuthorizationAction action, const AuthorizationScope &scope,
              bool require_workload) const;

private:
  struct Grant {
    std::set<AuthorizationRole> roles;
    std::set<std::string> fleets;
    std::set<std::string> workloads;
    bool scoped{false};
  };
  friend AuthorizationPolicy
  load_authorization_policy(const std::filesystem::path &path);
  std::map<std::string, Grant> grants_;
};

AuthorizationPolicy
load_authorization_policy(const std::filesystem::path &path);
std::string authorization_denial_json(AuthorizationAction action);

} // namespace vektor
