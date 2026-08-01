#ifdef _WIN32

#include "tools/multiplayer_netem/netem_core.h"
#include "tools/multiplayer_netem/netem_relay.h"

#include <windows.h>

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using multiplayer_netem::DirectionSettings;

struct ParsedArguments {
  std::unordered_map<std::string, std::string> values;
  bool help = false;
};

std::atomic_bool* g_stop_requested = nullptr;

BOOL WINAPI console_handler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT || signal == CTRL_BREAK_EVENT ||
      signal == CTRL_SHUTDOWN_EVENT) {
    if (g_stop_requested) {
      g_stop_requested->store(true);
    }
    return TRUE;
  }
  return FALSE;
}

template <typename T>
bool parse_integer(std::string_view text, T& output) {
  if (text.empty()) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), output);
  return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool parse_double(std::string_view text, double& output) {
  if (text.empty()) {
    return false;
  }
  const std::string value(text);
  char* end = nullptr;
  output = std::strtod(value.c_str(), &end);
  return end && *end == '\0';
}

std::optional<std::string_view> get_value(const ParsedArguments& arguments, std::string_view name) {
  const auto found = arguments.values.find(std::string(name));
  if (found == arguments.values.end()) {
    return std::nullopt;
  }
  return found->second;
}

bool parse_arguments(int argc, char** argv, ParsedArguments& output, std::string& error) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      output.help = true;
      continue;
    }
    if (!argument.starts_with("--") || index + 1 >= argc) {
      error = "every option must have a value: " + std::string(argument);
      return false;
    }
    const std::string key(argument);
    const std::string value(argv[++index]);
    if (!output.values.emplace(key, value).second) {
      error = "option was specified more than once: " + key;
      return false;
    }
  }
  return true;
}

bool parse_nonnegative_int(const ParsedArguments& arguments,
                           std::string_view name,
                           int& output,
                           std::string& error) {
  const auto value = get_value(arguments, name);
  if (!value) {
    return true;
  }
  if (!parse_integer(*value, output) || output < 0) {
    error = std::string(name) + " must be a non-negative integer";
    return false;
  }
  return true;
}

bool parse_nonnegative_uint32(const ParsedArguments& arguments,
                              std::string_view name,
                              uint32_t& output,
                              std::string& error) {
  const auto value = get_value(arguments, name);
  if (!value) {
    return true;
  }
  if (!parse_integer(*value, output)) {
    error = std::string(name) + " must be a non-negative 32-bit integer";
    return false;
  }
  return true;
}

bool parse_percent_option(const ParsedArguments& arguments,
                          std::string_view name,
                          double& output,
                          std::string& error) {
  const auto value = get_value(arguments, name);
  if (!value) {
    return true;
  }
  if (!parse_double(*value, output) || output < 0.0 || output > 100.0) {
    error = std::string(name) + " must be between 0 and 100";
    return false;
  }
  return true;
}

bool apply_direction_overrides(const ParsedArguments& arguments,
                               std::string_view prefix,
                               DirectionSettings& settings,
                               std::string& error) {
  const std::string prefix_text(prefix);
  if (!parse_nonnegative_int(arguments, "--" + prefix_text + "-latency-ms", settings.latency_ms,
                             error) ||
      !parse_nonnegative_int(arguments, "--" + prefix_text + "-jitter-ms", settings.jitter_ms,
                             error) ||
      !parse_percent_option(arguments, "--" + prefix_text + "-loss-percent",
                            settings.loss_percent, error) ||
      !parse_nonnegative_uint32(arguments, "--" + prefix_text + "-burst-length",
                                settings.burst_length, error) ||
      !parse_percent_option(arguments, "--" + prefix_text + "-reorder-percent",
                            settings.reorder_percent, error) ||
      !parse_nonnegative_int(arguments, "--" + prefix_text + "-reorder-delay-ms",
                             settings.reorder_delay_ms, error) ||
      !parse_percent_option(arguments, "--" + prefix_text + "-duplicate-percent",
                            settings.duplicate_percent, error)) {
    return false;
  }
  return true;
}

bool validate_known_options(const ParsedArguments& arguments, std::string& error) {
  static const std::unordered_set<std::string> known_options = {
      "--profile",           "--listen-port",          "--target",           "--seed",
      "--log-file",          "--max-queue-packets",    "--max-queue-bytes",
      "--up-latency-ms",     "--up-jitter-ms",         "--up-loss-percent",
      "--up-burst-length",   "--up-reorder-percent",  "--up-reorder-delay-ms",
      "--up-duplicate-percent", "--down-latency-ms",  "--down-jitter-ms",
      "--down-loss-percent", "--down-burst-length",   "--down-reorder-percent",
      "--down-reorder-delay-ms", "--down-duplicate-percent",
  };
  for (const auto& entry : arguments.values) {
    if (!known_options.contains(entry.first)) {
      error = "unknown option: " + entry.first;
      return false;
    }
  }
  return true;
}

void print_help() {
  std::cout << "Usage: multiplayer-netem [options]\n\n"
               "Opaque UDP relay for local multiplayer network testing.\n\n"
               "Required/connection options:\n"
               "  --listen-port PORT       Relay port (default: 26212)\n"
               "  --target IPV4:PORT       Host endpoint (default: 127.0.0.1:26210)\n"
               "  --profile NAME           lan, wifi, 4g, poor-4g, or stress (default: wifi)\n"
               "  --seed NUMBER            Deterministic random seed (default: 1)\n"
               "  --log-file PATH          Append relay statistics to this file\n\n"
               "Direction overrides use --up-* for client-to-host and --down-* for host-to-client:\n"
               "  latency-ms, jitter-ms, loss-percent, burst-length,\n"
               "  reorder-percent, reorder-delay-ms, duplicate-percent\n\n"
               "Queue options:\n"
               "  --max-queue-packets N    Maximum delayed datagrams (default: 4096)\n"
               "  --max-queue-bytes N      Maximum delayed payload bytes (default: 4194304)\n";
}

bool build_config(const ParsedArguments& arguments,
                  multiplayer_netem::RelayConfig& config,
                  std::string& profile_name,
                  std::string& log_file,
                  std::string& error) {
  profile_name = get_value(arguments, "--profile").value_or("wifi");
  const auto profile = multiplayer_netem::find_profile(profile_name);
  if (!profile) {
    error = "unknown profile: " + profile_name;
    return false;
  }
  config.client_to_host = profile->client_to_host;
  config.host_to_client = profile->host_to_client;

  uint32_t listen_port = config.listen_port;
  if (!parse_nonnegative_uint32(arguments, "--listen-port", listen_port, error) ||
      listen_port > UINT16_MAX) {
    error = "--listen-port must be between 0 and 65535";
    return false;
  }
  config.listen_port = static_cast<uint16_t>(listen_port);

  const auto target = get_value(arguments, "--target").value_or("127.0.0.1:26210");
  if (!multiplayer_netem::parse_endpoint(target, config.target, error)) {
    error = "--target: " + error;
    return false;
  }

  if (const auto seed = get_value(arguments, "--seed")) {
    if (!parse_integer(*seed, config.seed)) {
      error = "--seed must be a 64-bit unsigned integer";
      return false;
    }
  }
  if (const auto queue_packets = get_value(arguments, "--max-queue-packets")) {
    if (!parse_integer(*queue_packets, config.max_queue_packets) || config.max_queue_packets == 0) {
      error = "--max-queue-packets must be greater than zero";
      return false;
    }
  }
  if (const auto queue_bytes = get_value(arguments, "--max-queue-bytes")) {
    if (!parse_integer(*queue_bytes, config.max_queue_bytes) || config.max_queue_bytes == 0) {
      error = "--max-queue-bytes must be greater than zero";
      return false;
    }
  }

  if (!apply_direction_overrides(arguments, "up", config.client_to_host, error) ||
      !apply_direction_overrides(arguments, "down", config.host_to_client, error)) {
    return false;
  }
  if (!multiplayer_netem::validate_settings(config.client_to_host, error) ||
      !multiplayer_netem::validate_settings(config.host_to_client, error)) {
    return false;
  }

  log_file = std::string(get_value(arguments, "--log-file").value_or(""));
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  ParsedArguments arguments;
  std::string error;
  if (!parse_arguments(argc, argv, arguments, error) ||
      !validate_known_options(arguments, error)) {
    std::cerr << "multiplayer-netem: " << error << '\n';
    return 2;
  }
  if (arguments.help) {
    print_help();
    return 0;
  }

  multiplayer_netem::RelayConfig config;
  std::string profile_name;
  std::string log_file;
  if (!build_config(arguments, config, profile_name, log_file, error)) {
    std::cerr << "multiplayer-netem: " << error << '\n';
    return 2;
  }

  std::ofstream log_stream;
  std::ostream* log = &std::cout;
  if (!log_file.empty()) {
    log_stream.open(log_file, std::ios::out | std::ios::app);
    if (!log_stream) {
      std::cerr << "multiplayer-netem: could not open log file: " << log_file << '\n';
      return 1;
    }
    log = &log_stream;
  }
  *log << "[Netem] profile=" << profile_name << " seed=" << config.seed << " target="
       << multiplayer_netem::endpoint_to_string(config.target) << '\n';

  std::atomic_bool stop_requested = false;
  g_stop_requested = &stop_requested;
  SetConsoleCtrlHandler(console_handler, TRUE);
  multiplayer_netem::UdpRelay relay(std::move(config));
  const int result = relay.run(stop_requested, *log);
  SetConsoleCtrlHandler(console_handler, FALSE);
  g_stop_requested = nullptr;
  return result;
}

#else

int main() {
  return 1;
}

#endif
