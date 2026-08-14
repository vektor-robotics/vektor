#include "vektor/audit.hpp"

#include <cerrno>
#include <chrono>
#include <ctime>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vektor {
namespace {
std::string utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

std::string json_escape(std::string_view value) {
  std::ostringstream output;
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\b':
      output << "\\b";
      break;
    case '\f':
      output << "\\f";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (character < 0x20)
        output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
               << static_cast<unsigned int>(character) << std::dec;
      else
        output << static_cast<char>(character);
    }
  }
  return output.str();
}

std::string serialize(const AuditEvent &event) {
  std::ostringstream output;
  output << "{\"schema_version\":1,\"timestamp\":\""
         << json_escape(utc_timestamp()) << "\",\"actor\":\""
         << json_escape(event.actor) << "\",\"action\":\""
         << json_escape(event.action) << "\",\"outcome\":\""
         << json_escape(event.outcome) << "\",\"deployment_id\":\""
         << json_escape(event.deployment_id) << "\",\"artifact\":\""
         << json_escape(event.artifact) << "\",\"phase\":\""
         << json_escape(event.phase) << "\",\"operation\":\""
         << json_escape(event.operation) << "\",\"message\":\""
         << json_escape(event.message) << "\"}\n";
  return output.str();
}

void close_checked(int descriptor, const std::filesystem::path &path) {
  if (close(descriptor) != 0)
    throw std::runtime_error("failed to close audit log '" + path.string() +
                             "': " + std::strerror(errno));
}
} // namespace

JsonLinesAuditLog::JsonLinesAuditLog(std::filesystem::path path)
    : path_(std::move(path)) {
  if (path_.empty())
    throw std::invalid_argument("audit log path cannot be empty");
}

void JsonLinesAuditLog::append(const AuditEvent &event) {
  const auto line = serialize(event);
  std::lock_guard lock(mutex_);
  const auto parent = path_.parent_path();
  if (!parent.empty())
    std::filesystem::create_directories(parent);
  const int descriptor =
      open(path_.c_str(),
           O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor < 0)
    throw std::runtime_error("failed to open audit log '" + path_.string() +
                             "': " + std::strerror(errno));
  struct stat metadata {};
  const auto stat_result = fstat(descriptor, &metadata);
  if (stat_result != 0 || !S_ISREG(metadata.st_mode)) {
    const auto error = stat_result == 0 ? 0 : errno;
    close(descriptor);
    throw std::runtime_error("audit log is not a regular file '" +
                             path_.string() + "'" +
                             (error == 0 ? std::string{}
                                         : ": " + std::string(std::strerror(error))));
  }
  if (flock(descriptor, LOCK_EX) != 0) {
    const auto error = errno;
    close(descriptor);
    throw std::runtime_error("failed to lock audit log '" + path_.string() +
                             "': " + std::strerror(error));
  }
  std::size_t written = 0;
  while (written < line.size()) {
    const auto count =
        write(descriptor, line.data() + written, line.size() - written);
    if (count > 0) {
      written += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    const auto error = errno;
    close(descriptor);
    throw std::runtime_error("failed to append audit log '" + path_.string() +
                             "': " + std::strerror(error));
  }
  if (fsync(descriptor) != 0) {
    const auto error = errno;
    close(descriptor);
    throw std::runtime_error("failed to sync audit log '" + path_.string() +
                             "': " + std::strerror(error));
  }
  close_checked(descriptor, path_);
}

} // namespace vektor
