#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace vektor {

enum class AuthorizationRole { Viewer, Deployer, Operator, Admin };
enum class AuthorizationAction { Inspect, Deploy, Promote, Rollback };

const char *authorization_action_name(AuthorizationAction action);

class AuthorizationPolicy {
public:
  bool allows(const std::string &authenticated_identity,
              AuthorizationAction action) const;

private:
  friend AuthorizationPolicy
  load_authorization_policy(const std::filesystem::path &path);
  std::map<std::string, std::set<AuthorizationRole>> grants_;
};

AuthorizationPolicy
load_authorization_policy(const std::filesystem::path &path);
std::string authorization_denial_json(AuthorizationAction action);

} // namespace vektor
