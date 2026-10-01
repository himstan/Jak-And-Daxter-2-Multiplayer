#include "game/tools/network_debug/network_debug_panel.h"

#include <algorithm>
#include <cfloat>
#include <string>

#include "game/multiplayer/platform/core/limits.h"

#include "fmt/format.h"
#include "third-party/imgui/imgui.h"

namespace {
using namespace multiplayer::platform;

constexpr auto kSampleInterval = std::chrono::milliseconds(250);
constexpr int kPingWarningMs = 100;
constexpr int kPingCriticalMs = 200;
constexpr float kJitterWarningMs = 10.0f;
constexpr float kJitterCriticalMs = 30.0f;
constexpr float kQualityWarning = 0.95f;
constexpr float kQualityCritical = 0.80f;
constexpr int kBacklogWarningBytes = kReliableBacklogLimitBytes / 4;
constexpr int kBacklogCriticalBytes = kReliableBacklogLimitBytes * 3 / 4;

enum class Health { NEUTRAL, GOOD, WARNING, CRITICAL };

ImVec4 health_color(const Health health) {
  switch (health) {
    case Health::GOOD:
      return {0.35f, 0.85f, 0.45f, 1.0f};
    case Health::WARNING:
      return {1.0f, 0.72f, 0.2f, 1.0f};
    case Health::CRITICAL:
      return {1.0f, 0.3f, 0.3f, 1.0f};
    case Health::NEUTRAL:
      return ImGui::GetStyleColorVec4(ImGuiCol_Text);
  }
  return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

Health ping_health(const int ping_ms) {
  if (ping_ms >= kPingCriticalMs)
    return Health::CRITICAL;
  if (ping_ms >= kPingWarningMs)
    return Health::WARNING;
  return Health::GOOD;
}

Health jitter_health(const float jitter_ms) {
  if (jitter_ms >= kJitterCriticalMs)
    return Health::CRITICAL;
  if (jitter_ms >= kJitterWarningMs)
    return Health::WARNING;
  return Health::GOOD;
}

Health quality_health(const float quality) {
  if (quality < 0.0f)
    return Health::NEUTRAL;
  if (quality < kQualityCritical)
    return Health::CRITICAL;
  if (quality < kQualityWarning)
    return Health::WARNING;
  return Health::GOOD;
}

Health backlog_health(const int bytes) {
  if (bytes >= kBacklogCriticalBytes)
    return Health::CRITICAL;
  if (bytes >= kBacklogWarningBytes)
    return Health::WARNING;
  return Health::GOOD;
}

std::string format_bytes(const double bytes) {
  const double safe_bytes = std::max(bytes, 0.0);
  if (safe_bytes >= 1024.0 * 1024.0)
    return fmt::format("{:.2f} MiB", safe_bytes / (1024.0 * 1024.0));
  if (safe_bytes >= 1024.0)
    return fmt::format("{:.1f} KiB", safe_bytes / 1024.0);
  return fmt::format("{:.0f} B", safe_bytes);
}

std::string format_rate(const double bytes_per_second) {
  return fmt::format("{}/s", format_bytes(bytes_per_second));
}

std::string format_quality(const float quality) {
  return quality < 0.0f ? "—" : fmt::format("{:.1f}%", quality * 100.0f);
}

std::string format_player(const PlayerId player_id) {
  return player_id == kInvalidPlayerId ? "—" : fmt::format("{}", player_id);
}

const char* session_status_name(const SessionStatus status) {
  switch (status) {
    case SessionStatus::IDLE:
      return "Idle";
    case SessionStatus::CONNECTING:
      return "Connecting";
    case SessionStatus::LOBBY:
      return "Lobby";
    case SessionStatus::GAME_STARTING:
      return "Game Starting";
    case SessionStatus::IN_GAME:
      return "In Game";
    case SessionStatus::RECONNECTING:
      return "Reconnecting";
    case SessionStatus::HOST_LEFT:
      return "Host Left";
    case SessionStatus::FAILED:
      return "Failed";
  }
  return "Unknown";
}

const char* command_error_name(const CommandError error) {
  switch (error) {
    case CommandError::NONE:
      return "None";
    case CommandError::RUNTIME_INACTIVE:
      return "Runtime Inactive";
    case CommandError::QUEUE_FULL:
      return "Queue Full";
    case CommandError::INVALID_REQUEST:
      return "Invalid Request";
    case CommandError::INVALID_STATE:
      return "Invalid State";
    case CommandError::NOT_ALLOWED:
      return "Not Allowed";
    case CommandError::UNAVAILABLE:
      return "Unavailable";
    case CommandError::START_FAILED:
      return "Start Failed";
    case CommandError::INTERNAL_ERROR:
      return "Internal Error";
  }
  return "Unknown";
}

const char* role_name(const SessionRole role) {
  switch (role) {
    case SessionRole::NONE:
      return "None";
    case SessionRole::HOST:
      return "Host";
    case SessionRole::CLIENT:
      return "Client";
  }
  return "Unknown";
}

const char* character_name(const PlayerCharacter character) {
  switch (character) {
    case PlayerCharacter::JAK:
      return "Jak";
    case PlayerCharacter::DAXTER:
      return "Daxter";
    case PlayerCharacter::UNKNOWN:
      return "Unknown";
  }
  return "Unknown";
}

const PlayerProfile* find_profile(const SessionSnapshot& session, const PlayerId player_id) {
  const auto found = std::ranges::find(session.players, player_id, &PlayerProfile::player_id);
  return found == session.players.end() ? nullptr : &*found;
}

void draw_text(const std::string& text, const Health health = Health::NEUTRAL) {
  ImGui::TextColored(health_color(health), "%s", text.c_str());
}

void draw_metric(const char* label, const std::string& value, const Health health) {
  ImGui::TableNextColumn();
  ImGui::TextDisabled("%s", label);
  draw_text(value, health);
}

template <typename Series>
float history_value(void* data, const int index) {
  return static_cast<const Series*>(data)->at(static_cast<size_t>(index));
}

template <typename Series>
void draw_plot(const char* label, const Series& series, const std::string& overlay) {
  ImGui::PlotLines(label, history_value<Series>, const_cast<Series*>(&series),
                   static_cast<int>(series.count), 0, overlay.c_str(), FLT_MAX, FLT_MAX,
                   ImVec2(-1.0f, 48.0f));
}

void draw_summary(const RuntimeSnapshot& snapshot) {
  const auto& state = snapshot.session.state;
  if (ImGui::BeginTable("session-summary", 4, ImGuiTableFlags_SizingStretchSame)) {
    draw_metric("Session", session_status_name(state.status), Health::NEUTRAL);
    draw_metric("Discovery", fmt::format("{}", static_cast<int>(snapshot.discovery.status)),
                Health::NEUTRAL);
    draw_metric("Role", role_name(state.role), Health::NEUTRAL);
    draw_metric("Local ID", format_player(state.local_player_id), Health::NEUTRAL);
    draw_metric("Host ID", format_player(state.host_player_id), Health::NEUTRAL);
    draw_metric("Players",
                fmt::format("{} / {}", snapshot.session.players.size(), state.player_limit),
                Health::NEUTRAL);
    draw_metric("Command", command_error_name(snapshot.connection_result.error),
                snapshot.connection_result.error == CommandError::NONE ? Health::NEUTRAL
                                                                       : Health::CRITICAL);
    ImGui::EndTable();
  }
}

void draw_aggregate(const AggregateSnapshot& stats) {
  const float jitter_ms = static_cast<float>(stats.worst_jitter_us) / 1000.0f;
  const int reliable_bytes = stats.pending_reliable_bytes + stats.sent_unacked_reliable_bytes;
  if (ImGui::BeginTable("aggregate-health", 4,
                        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
    draw_metric("Inbound", format_rate(stats.receive_bytes_per_second), Health::NEUTRAL);
    draw_metric("Outbound", format_rate(stats.send_bytes_per_second), Health::NEUTRAL);
    draw_metric("Send Capacity", format_rate(stats.send_rate_bytes_per_second), Health::NEUTRAL);
    draw_metric("Reliable Pressure", format_bytes(reliable_bytes), backlog_health(reliable_bytes));
    draw_metric("Worst Ping", fmt::format("{} ms", stats.worst_ping_ms),
                ping_health(stats.worst_ping_ms));
    draw_metric("Maximum Jitter", fmt::format("{:.2f} ms", jitter_ms), jitter_health(jitter_ms));
    draw_metric("Minimum Local Quality", format_quality(stats.minimum_local_quality),
                quality_health(stats.minimum_local_quality));
    draw_metric("Minimum Remote Quality", format_quality(stats.minimum_remote_quality),
                quality_health(stats.minimum_remote_quality));
    draw_metric("Realtime Queue", format_bytes(stats.pending_unreliable_bytes), Health::NEUTRAL);
    draw_metric("Reliable Queue", format_bytes(stats.pending_reliable_bytes),
                backlog_health(reliable_bytes));
    draw_metric("Reliable Unacked", format_bytes(stats.sent_unacked_reliable_bytes),
                backlog_health(reliable_bytes));
    draw_metric("Backlog Limit", format_bytes(kReliableBacklogLimitBytes), Health::NEUTRAL);
    ImGui::EndTable();
  }
}

void draw_connection_table(const SessionSnapshot& session) {
  constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX |
                                    ImGuiTableFlags_SizingFixedFit;
  if (!ImGui::BeginTable("connection-stats", 13, flags, ImVec2(0.0f, 220.0f)))
    return;
  ImGui::TableSetupColumn("Player");
  ImGui::TableSetupColumn("Player");
  ImGui::TableSetupColumn("Connection");
  ImGui::TableSetupColumn("Character");
  ImGui::TableSetupColumn("Ping");
  ImGui::TableSetupColumn("Jitter");
  ImGui::TableSetupColumn("Local Quality");
  ImGui::TableSetupColumn("Remote Quality");
  ImGui::TableSetupColumn("Inbound");
  ImGui::TableSetupColumn("Outbound");
  ImGui::TableSetupColumn("Capacity");
  ImGui::TableSetupColumn("Realtime Queue");
  ImGui::TableSetupColumn("Reliable Pressure");
  ImGui::TableHeadersRow();
  for (const auto& connection : session.connections) {
    const auto& stats = connection.network;
    const auto* profile = find_profile(session, connection.player_id);
    const float jitter_ms = static_cast<float>(stats.jitter_us) / 1000.0f;
    const int reliable_bytes = stats.pending_reliable_bytes + stats.sent_unacked_reliable_bytes;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(profile ? profile->display_name.c_str() : "Pending admission");
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(format_player(connection.player_id).c_str());
    ImGui::TableNextColumn();
    ImGui::Text("%u", stats.connection_id);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(profile ? character_name(profile->character) : "—");
    ImGui::TableNextColumn();
    draw_text(fmt::format("{} ms", stats.ping_ms), ping_health(stats.ping_ms));
    ImGui::TableNextColumn();
    draw_text(fmt::format("{:.2f} ms", jitter_ms), jitter_health(jitter_ms));
    ImGui::TableNextColumn();
    draw_text(format_quality(stats.local_quality), quality_health(stats.local_quality));
    ImGui::TableNextColumn();
    draw_text(format_quality(stats.remote_quality), quality_health(stats.remote_quality));
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(format_rate(stats.receive_bytes_per_second).c_str());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(format_rate(stats.send_bytes_per_second).c_str());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(format_rate(stats.send_rate_bytes_per_second).c_str());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(format_bytes(stats.pending_unreliable_bytes).c_str());
    ImGui::TableNextColumn();
    draw_text(format_bytes(reliable_bytes), backlog_health(reliable_bytes));
  }
  ImGui::EndTable();
}

}  // namespace

void NetworkDebugPanel::HistorySeries::clear() {
  values.fill(0.0f);
  count = 0;
  next = 0;
}

void NetworkDebugPanel::HistorySeries::push(const float value) {
  values[next] = value;
  next = (next + 1) % values.size();
  count = std::min(count + 1, values.size());
}

float NetworkDebugPanel::HistorySeries::at(const size_t index) const {
  const size_t first = count == values.size() ? next : 0;
  return values[(first + index) % values.size()];
}

void NetworkDebugPanel::reset_history() {
  receive_rate_.clear();
  send_rate_.clear();
  ping_.clear();
  jitter_.clear();
  reliable_pressure_.clear();
  last_sample_ = {};
  has_session_ = false;
}

void NetworkDebugPanel::update_history(const RuntimeSnapshot& snapshot) {
  const auto& state = snapshot.session.state;
  if (state.role == SessionRole::NONE) {
    reset_history();
    return;
  }
  if (!has_session_ || session_role_ != state.role || local_player_id_ != state.local_player_id ||
      host_player_id_ != state.host_player_id) {
    reset_history();
    has_session_ = true;
    session_role_ = state.role;
    local_player_id_ = state.local_player_id;
    host_player_id_ = state.host_player_id;
  }
  const auto now = std::chrono::steady_clock::now();
  if (last_sample_ != std::chrono::steady_clock::time_point{} &&
      now - last_sample_ < kSampleInterval) {
    return;
  }
  last_sample_ = now;
  const auto& stats = snapshot.session.statistics;
  receive_rate_.push(stats.receive_bytes_per_second / 1024.0f);
  send_rate_.push(stats.send_bytes_per_second / 1024.0f);
  ping_.push(static_cast<float>(stats.worst_ping_ms));
  jitter_.push(static_cast<float>(stats.worst_jitter_us) / 1000.0f);
  reliable_pressure_.push(
      static_cast<float>(stats.pending_reliable_bytes + stats.sent_unacked_reliable_bytes) /
      1024.0f);
}

void NetworkDebugPanel::draw(const RuntimeSnapshot& snapshot, bool* open) {
  update_history(snapshot);
  if (!ImGui::Begin("Network Diagnostics", open)) {
    ImGui::End();
    return;
  }
  draw_summary(snapshot);

  if (snapshot.session.state.role == SessionRole::NONE) {
    ImGui::Spacing();
    ImGui::TextDisabled("No active multiplayer session.");
    ImGui::End();
    return;
  }

  ImGui::SeparatorText("Aggregate Health");
  draw_aggregate(snapshot.session.statistics);

  ImGui::SeparatorText("Recent History (60 seconds)");
  draw_plot("Inbound KiB/s", receive_rate_,
            format_rate(snapshot.session.statistics.receive_bytes_per_second));
  draw_plot("Outbound KiB/s", send_rate_,
            format_rate(snapshot.session.statistics.send_bytes_per_second));
  draw_plot("Worst Ping (ms)", ping_,
            fmt::format("{} ms", snapshot.session.statistics.worst_ping_ms));
  draw_plot("Maximum Jitter (ms)", jitter_,
            fmt::format("{:.2f} ms",
                        static_cast<float>(snapshot.session.statistics.worst_jitter_us) / 1000.0f));
  const int reliable_bytes = snapshot.session.statistics.pending_reliable_bytes +
                             snapshot.session.statistics.sent_unacked_reliable_bytes;
  draw_plot("Reliable Pressure (KiB)", reliable_pressure_, format_bytes(reliable_bytes));

  ImGui::SeparatorText("Connections");
  if (snapshot.session.connections.empty())
    ImGui::TextDisabled("No active connections.");
  else
    draw_connection_table(snapshot.session);
  ImGui::End();
}
