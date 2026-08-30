#include "vektor/capture.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <regex>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace vektor {
namespace {
void validate_topics(const std::vector<std::string> &topics) {
  if (topics.empty())
    throw std::invalid_argument("capture requires at least one ROS topic");
  if (topics.size() > 64)
    throw std::invalid_argument("capture supports at most 64 ROS topics");
  static const std::regex topic_pattern("^/[A-Za-z0-9_~/]+$");
  for (const auto &topic : topics) {
    if (topic.size() > 256 || !std::regex_match(topic, topic_pattern))
      throw std::invalid_argument("invalid ROS topic '" + topic + "'");
  }
}

void validate_storage_id(const std::string &storage_id) {
  static const std::regex pattern("^[A-Za-z0-9_-]{1,32}$");
  if (!std::regex_match(storage_id, pattern))
    throw std::invalid_argument("invalid rosbag2 storage ID");
}

bool process_exited(pid_t pid) {
  int status = 0;
  const auto result = waitpid(pid, &status, WNOHANG);
  if (result == pid)
    return true;
  if (result == 0)
    return false;
  if (errno != ECHILD)
    throw std::runtime_error("failed to inspect recorder process: " +
                             std::string(std::strerror(errno)));
  if (kill(pid, 0) == 0 || errno == EPERM)
    return false;
  if (errno == ESRCH)
    return true;
  throw std::runtime_error("failed to inspect recorder process: " +
                           std::string(std::strerror(errno)));
}

void wait_for_exit(pid_t pid, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (process_exited(pid))
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  throw std::runtime_error("recorder process did not stop before timeout");
}

void verify_process_identity(pid_t pid,
                             const std::filesystem::path &expected_bag_path) {
  std::ifstream command_line("/proc/" + std::to_string(pid) + "/cmdline",
                             std::ios::binary);
  if (!command_line) {
    if (errno == ENOENT)
      return;
    throw std::runtime_error("cannot verify recorder process identity");
  }
  const std::string command{std::istreambuf_iterator<char>(command_line), {}};
  if (command.find("bag") == std::string::npos ||
      command.find("record") == std::string::npos ||
      command.find(expected_bag_path.string()) == std::string::npos)
    throw std::runtime_error("refusing to signal a process that is not the "
                             "recorded rosbag2 process");
}

using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

void digest_update(EVP_MD_CTX *context, const void *data, std::size_t size) {
  if (EVP_DigestUpdate(context, data, size) != 1)
    throw std::runtime_error("failed to fingerprint capture artifact");
}
} // namespace

RosbagRecorder::RosbagRecorder(std::filesystem::path executable)
    : executable_(std::move(executable)) {
  if (executable_.empty())
    throw std::invalid_argument("ros2 executable cannot be empty");
}

std::int64_t
RosbagRecorder::start(const std::filesystem::path &bag_path,
                      const std::vector<std::string> &topics,
                      const std::string &storage_id,
                      const std::filesystem::path &log_path) const {
  validate_topics(topics);
  validate_storage_id(storage_id);
  if (bag_path.empty() || log_path.empty())
    throw std::invalid_argument("bag and recorder log paths are required");
  if (std::filesystem::exists(bag_path))
    throw std::runtime_error("bag output path already exists: " +
                             bag_path.string());
  std::filesystem::create_directories(bag_path.parent_path());
  std::filesystem::create_directories(log_path.parent_path());

  int error_pipe[2];
  if (pipe2(error_pipe, O_CLOEXEC) != 0)
    throw std::runtime_error("failed to create recorder launch pipe: " +
                             std::string(std::strerror(errno)));
  const auto pid = fork();
  if (pid < 0) {
    const auto error = errno;
    close(error_pipe[0]);
    close(error_pipe[1]);
    throw std::runtime_error("failed to fork rosbag2 recorder: " +
                             std::string(std::strerror(error)));
  }
  if (pid == 0) {
    close(error_pipe[0]);
    if (setsid() < 0) {
      const auto error = errno;
      static_cast<void>(write(error_pipe[1], &error, sizeof(error)));
      _exit(127);
    }
    const int log =
        open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (log < 0 || dup2(log, STDOUT_FILENO) < 0 ||
        dup2(log, STDERR_FILENO) < 0) {
      const auto error = errno;
      static_cast<void>(write(error_pipe[1], &error, sizeof(error)));
      _exit(127);
    }
    if (log != STDOUT_FILENO && log != STDERR_FILENO)
      close(log);
    const int null_input = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null_input < 0 || dup2(null_input, STDIN_FILENO) < 0) {
      const auto error = errno;
      static_cast<void>(write(error_pipe[1], &error, sizeof(error)));
      _exit(127);
    }
    if (null_input != STDIN_FILENO)
      close(null_input);

    std::vector<std::string> arguments{
        executable_.string(), "bag",       "record",  "--output",
        bag_path.string(),    "--storage", storage_id};
    arguments.insert(arguments.end(), topics.begin(), topics.end());
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (auto &argument : arguments)
      argv.push_back(argument.data());
    argv.push_back(nullptr);
    execvp(executable_.c_str(), argv.data());
    const auto error = errno;
    static_cast<void>(write(error_pipe[1], &error, sizeof(error)));
    _exit(127);
  }

  close(error_pipe[1]);
  int launch_error = 0;
  const auto count = read(error_pipe[0], &launch_error, sizeof(launch_error));
  close(error_pipe[0]);
  if (count > 0) {
    static_cast<void>(waitpid(pid, nullptr, 0));
    throw std::runtime_error("failed to launch rosbag2 recorder: " +
                             std::string(std::strerror(launch_error)));
  }
  if (count < 0) {
    const auto error = errno;
    static_cast<void>(kill(pid, SIGKILL));
    static_cast<void>(waitpid(pid, nullptr, 0));
    throw std::runtime_error("failed to confirm rosbag2 launch: " +
                             std::string(std::strerror(error)));
  }
  const auto readiness_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(bag_path) &&
         std::chrono::steady_clock::now() < readiness_deadline) {
    if (process_exited(pid)) {
      std::ifstream log(log_path);
      std::string detail{std::istreambuf_iterator<char>(log), {}};
      if (detail.size() > 4096)
        detail = detail.substr(0, 4096) + " [output truncated]";
      throw std::runtime_error(
          "rosbag2 recorder exited during launch" +
          (detail.empty() ? std::string{} : ": " + detail));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!std::filesystem::exists(bag_path)) {
    static_cast<void>(kill(pid, SIGINT));
    try {
      wait_for_exit(pid, std::chrono::seconds(2));
    } catch (const std::exception &) {
      static_cast<void>(kill(pid, SIGKILL));
      static_cast<void>(waitpid(pid, nullptr, 0));
    }
    throw std::runtime_error("rosbag2 recorder did not create its output "
                             "directory within 5 seconds");
  }
  return static_cast<std::int64_t>(pid);
}

void RosbagRecorder::stop(std::int64_t raw_pid,
                          const std::filesystem::path &expected_bag_path,
                          std::chrono::milliseconds timeout) const {
  if (raw_pid <= 1 || raw_pid > std::numeric_limits<pid_t>::max())
    throw std::invalid_argument("invalid recorder PID");
  const auto pid = static_cast<pid_t>(raw_pid);
  if (process_exited(pid))
    return;
  verify_process_identity(pid, expected_bag_path);
  if (kill(pid, SIGINT) != 0 && errno != ESRCH)
    throw std::runtime_error("failed to stop rosbag2 recorder: " +
                             std::string(std::strerror(errno)));
  try {
    wait_for_exit(pid, timeout);
  } catch (const std::runtime_error &) {
    if (kill(pid, SIGTERM) != 0 && errno != ESRCH)
      throw;
    try {
      wait_for_exit(pid, std::chrono::seconds(2));
    } catch (const std::runtime_error &) {
      static_cast<void>(kill(pid, SIGKILL));
      wait_for_exit(pid, std::chrono::seconds(2));
    }
  }
}

RunArtifact fingerprint_run_artifact(const std::filesystem::path &path,
                                     const std::string &kind) {
  if (!std::filesystem::exists(path) || !std::filesystem::is_directory(path))
    throw std::runtime_error("capture artifact directory does not exist: " +
                             path.string());
  std::vector<std::filesystem::path> files;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(path)) {
    if (entry.is_symlink())
      throw std::runtime_error("capture artifacts cannot contain symlinks");
    if (entry.is_regular_file())
      files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());

  DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("failed to initialize artifact fingerprint");
  std::uintmax_t total_size = 0;
  std::array<char, 65536> buffer{};
  for (const auto &file : files) {
    const auto relative =
        std::filesystem::relative(file, path).generic_string();
    digest_update(context.get(), relative.data(), relative.size());
    const char separator = '\0';
    digest_update(context.get(), &separator, 1);
    std::ifstream input(file, std::ios::binary);
    if (!input)
      throw std::runtime_error("failed to read capture artifact: " +
                               file.string());
    while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
      digest_update(context.get(), buffer.data(),
                    static_cast<std::size_t>(input.gcount()));
      total_size += static_cast<std::uintmax_t>(input.gcount());
    }
    if (!input.eof())
      throw std::runtime_error("failed to read capture artifact: " +
                               file.string());
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1)
    throw std::runtime_error("failed to finalize artifact fingerprint");
  std::ostringstream fingerprint;
  fingerprint << "sha256:" << std::hex << std::setfill('0');
  for (unsigned int index = 0; index < digest_size; ++index)
    fingerprint << std::setw(2) << static_cast<unsigned int>(digest[index]);
  return {kind, std::filesystem::absolute(path).lexically_normal().string(),
          fingerprint.str(), total_size};
}

} // namespace vektor
