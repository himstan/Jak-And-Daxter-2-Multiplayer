#include "game/tools/network_simulation/network_simulation_panel.h"

#include <algorithm>

#include "game/multiplayer/platform/runtime/multiplayer_runtime.h"

#include "third-party/imgui/imgui.h"

namespace {
using multiplayer::platform::NetworkSimulationSettings;

NetworkSimulationSettings wifi_preset() {
  NetworkSimulationSettings settings;
  settings.send_lag_ms = 20;
  settings.receive_lag_ms = 20;
  settings.send_loss_percent = 0.2f;
  settings.receive_loss_percent = 0.2f;
  settings.send_jitter_average_ms = 4.0f;
  settings.receive_jitter_average_ms = 4.0f;
  settings.send_jitter_maximum_ms = 15.0f;
  settings.receive_jitter_maximum_ms = 15.0f;
  settings.send_jitter_percent = 50.0f;
  settings.receive_jitter_percent = 50.0f;
  return settings;
}

NetworkSimulationSettings mobile_preset() {
  NetworkSimulationSettings settings;
  settings.send_lag_ms = 55;
  settings.receive_lag_ms = 55;
  settings.send_loss_percent = 1.0f;
  settings.receive_loss_percent = 1.0f;
  settings.send_jitter_average_ms = 15.0f;
  settings.receive_jitter_average_ms = 15.0f;
  settings.send_jitter_maximum_ms = 60.0f;
  settings.receive_jitter_maximum_ms = 60.0f;
  settings.send_jitter_percent = 80.0f;
  settings.receive_jitter_percent = 80.0f;
  settings.send_rate_limit_bytes_per_second = 512 * 1024;
  settings.receive_rate_limit_bytes_per_second = 512 * 1024;
  return settings;
}

NetworkSimulationSettings severe_preset() {
  NetworkSimulationSettings settings;
  settings.send_lag_ms = 150;
  settings.receive_lag_ms = 150;
  settings.send_loss_percent = 8.0f;
  settings.receive_loss_percent = 8.0f;
  settings.send_jitter_average_ms = 60.0f;
  settings.receive_jitter_average_ms = 60.0f;
  settings.send_jitter_maximum_ms = 200.0f;
  settings.receive_jitter_maximum_ms = 200.0f;
  settings.send_jitter_percent = 90.0f;
  settings.receive_jitter_percent = 90.0f;
  settings.send_reorder_percent = 3.0f;
  settings.receive_reorder_percent = 3.0f;
  settings.send_rate_limit_bytes_per_second = 64 * 1024;
  settings.receive_rate_limit_bytes_per_second = 64 * 1024;
  return settings;
}

void clamp_settings(NetworkSimulationSettings& settings) {
  settings.send_rate_limit_bytes_per_second =
      std::clamp(settings.send_rate_limit_bytes_per_second, 0, 1024 * 1024 * 1024);
  settings.receive_rate_limit_bytes_per_second =
      std::clamp(settings.receive_rate_limit_bytes_per_second, 0, 1024 * 1024 * 1024);
  settings.send_rate_limit_burst_bytes =
      std::clamp(settings.send_rate_limit_burst_bytes, 0, 1024 * 1024);
  settings.receive_rate_limit_burst_bytes =
      std::clamp(settings.receive_rate_limit_burst_bytes, 0, 1024 * 1024);
}

void draw_direction_controls(NetworkSimulationSettings& settings) {
  if (!ImGui::BeginTable("network-directions", 3,
                         ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame))
    return;
  ImGui::TableSetupColumn("Condition");
  ImGui::TableSetupColumn("Outbound");
  ImGui::TableSetupColumn("Inbound");
  ImGui::TableHeadersRow();

  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextUnformatted("Fixed latency");
  ImGui::TableNextColumn();
  ImGui::SliderInt("##send-lag", &settings.send_lag_ms, 0, 5000, "%d ms");
  ImGui::TableNextColumn();
  ImGui::SliderInt("##receive-lag", &settings.receive_lag_ms, 0, 5000, "%d ms");

  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextUnformatted("Packet loss");
  ImGui::TableNextColumn();
  ImGui::SliderFloat("##send-loss", &settings.send_loss_percent, 0.0f, 100.0f, "%.1f%%");
  ImGui::TableNextColumn();
  ImGui::SliderFloat("##receive-loss", &settings.receive_loss_percent, 0.0f, 100.0f, "%.1f%%");

  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextUnformatted("Jitter average");
  ImGui::TableNextColumn();
  ImGui::SliderFloat("##send-jitter-average", &settings.send_jitter_average_ms, 0.0f, 2000.0f,
                     "%.1f ms");
  ImGui::TableNextColumn();
  ImGui::SliderFloat("##receive-jitter-average", &settings.receive_jitter_average_ms, 0.0f, 2000.0f,
                     "%.1f ms");

  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextUnformatted("Jitter maximum");
  ImGui::TableNextColumn();
  ImGui::SliderFloat("##send-jitter-maximum", &settings.send_jitter_maximum_ms, 0.0f, 5000.0f,
                     "%.1f ms");
  ImGui::TableNextColumn();
  ImGui::SliderFloat("##receive-jitter-maximum", &settings.receive_jitter_maximum_ms, 0.0f, 5000.0f,
                     "%.1f ms");

  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextUnformatted("Jitter chance");
  ImGui::TableNextColumn();
  ImGui::SliderFloat("##send-jitter-percent", &settings.send_jitter_percent, 0.0f, 100.0f,
                     "%.1f%%");
  ImGui::TableNextColumn();
  ImGui::SliderFloat("##receive-jitter-percent", &settings.receive_jitter_percent, 0.0f, 100.0f,
                     "%.1f%%");

  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextUnformatted("Reordering");
  ImGui::TableNextColumn();
  ImGui::SliderFloat("##send-reorder", &settings.send_reorder_percent, 0.0f, 100.0f, "%.1f%%");
  ImGui::TableNextColumn();
  ImGui::SliderFloat("##receive-reorder", &settings.receive_reorder_percent, 0.0f, 100.0f,
                     "%.1f%%");

  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextUnformatted("Duplication");
  ImGui::TableNextColumn();
  ImGui::SliderFloat("##send-duplicate", &settings.send_duplicate_percent, 0.0f, 100.0f, "%.1f%%");
  ImGui::TableNextColumn();
  ImGui::SliderFloat("##receive-duplicate", &settings.receive_duplicate_percent, 0.0f, 100.0f,
                     "%.1f%%");
  ImGui::EndTable();
}

}  // namespace

void NetworkSimulationPanel::apply() {
  clamp_settings(settings_);
  const bool queued = multiplayer::platform::multiplayer_runtime()
                          .enqueue<multiplayer::platform::SetNetworkSimulationCommand>(settings_);
  status_ = queued ? "Settings queued on the multiplayer worker."
                   : "Multiplayer runtime is not available.";
}

void NetworkSimulationPanel::draw(bool* open) {
  if (!ImGui::Begin("Network Simulation", open)) {
    ImGui::End();
    return;
  }

  ImGui::TextColored({1.0f, 0.72f, 0.2f, 1.0f},
                     "Developer tool: these settings affect every GNS connection in this process.");
  if (ImGui::Button("Normal")) {
    settings_ = {};
    apply();
  }
  ImGui::SameLine();
  if (ImGui::Button("Wi-Fi")) {
    settings_ = wifi_preset();
    apply();
  }
  ImGui::SameLine();
  if (ImGui::Button("Mobile")) {
    settings_ = mobile_preset();
    apply();
  }
  ImGui::SameLine();
  if (ImGui::Button("Severe")) {
    settings_ = severe_preset();
    apply();
  }

  ImGui::SeparatorText("Packet Conditions");
  draw_direction_controls(settings_);

  ImGui::SeparatorText("Timing Details");
  ImGui::SliderInt("Reorder delay", &settings_.reorder_delay_ms, 0, 5000, "%d ms");
  ImGui::SliderInt("Maximum duplicate delay", &settings_.duplicate_delay_maximum_ms, 0, 5000,
                   "%d ms");

  ImGui::SeparatorText("Rate Limits");
  ImGui::TextDisabled("A rate of 0 B/s disables that limiter.");
  ImGui::InputInt("Outbound rate (B/s)", &settings_.send_rate_limit_bytes_per_second, 1024,
                  64 * 1024);
  ImGui::InputInt("Outbound burst (B)", &settings_.send_rate_limit_burst_bytes, 1024, 16 * 1024);
  ImGui::InputInt("Inbound rate (B/s)", &settings_.receive_rate_limit_bytes_per_second, 1024,
                  64 * 1024);
  ImGui::InputInt("Inbound burst (B)", &settings_.receive_rate_limit_burst_bytes, 1024, 16 * 1024);

  if (ImGui::Button("Apply"))
    apply();
  if (!status_.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", status_.c_str());
  }
  ImGui::End();
}
