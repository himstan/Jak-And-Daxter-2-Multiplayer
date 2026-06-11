#include "lsp_requester.h"

#include <iostream>
#include <chrono>
#include <ctime>

#include "common/log/log.h"
#include "common/util/string_util.h"

#include "lsp/protocol/progress_report.h"

static void safe_lsp_log(const std::string& level, const std::string& message) {
  auto now = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  struct tm buf;
#ifdef _WIN32
  localtime_s(&buf, &time_t_now);
#else
  localtime_r(&time_t_now, &buf);
#endif
  fmt::print(stderr, "[{:02d}:{:02d}] [{}] {}\n", buf.tm_min, buf.tm_sec, level, message);
  fflush(stderr);
}

void LSPRequester::send_request(const json& params, const std::string& method) {
  json req;
  req["id"] = str_util::uuid();
  req["method"] = method;
  req["params"] = params;
  req["jsonrpc"] = "2.0";

  std::string request;
  request.append("Content-Length: " + std::to_string(req.dump().size()) + "\r\n");
  request.append("Content-Type: application/vscode-jsonrpc;charset=utf-8\r\n");
  request.append("\r\n");
  request += req.dump();

  // Send requests immediately, as they may be done during the handling of a client request
  safe_lsp_log("info", fmt::format("Sending Request {}", method));
  std::cout << request.c_str() << std::flush;
}

void LSPRequester::send_notification(const json& params, const std::string& method) {
  json notification;
  notification["method"] = method;
  notification["params"] = params;
  notification["jsonrpc"] = "2.0";

  std::string request;
  request.append("Content-Length: " + std::to_string(notification.dump().size()) + "\r\n");
  request.append("Content-Type: application/vscode-jsonrpc;charset=utf-8\r\n");
  request.append("\r\n");
  request += notification.dump();

  // Send requests immediately, as they may be done during the handling of a client request
  if (method == "$/progress") {
    std::string token = "";
    std::string kind = "";
    std::string message = "";
    int percentage = 0;
    if (params.contains("token") && params["token"].is_string()) {
      token = params["token"].get<std::string>();
    }
    if (params.contains("value") && params["value"].is_object()) {
      auto val = params["value"];
      if (val.contains("kind") && val["kind"].is_string()) {
        kind = val["kind"].get<std::string>();
      }
      if (val.contains("message") && val["message"].is_string()) {
        message = val["message"].get<std::string>();
      }
      if (val.contains("percentage") && val["percentage"].is_number()) {
        percentage = val["percentage"].get<int>();
      }
    }
    if (kind == "begin") {
      safe_lsp_log("info", fmt::format("Sending Notification $/progress kind=begin token={} percentage={} message=\"{}\"",
               token, percentage, message));
    } else if (kind == "report") {
      safe_lsp_log("info", fmt::format("Sending Notification $/progress kind=report token={} percentage={} message=\"{}\"",
               token, percentage, message));
    } else if (kind == "end") {
      safe_lsp_log("info", fmt::format("Sending Notification $/progress kind=end token={} message=\"{}\"",
               token, message));
    } else {
      safe_lsp_log("info", fmt::format("Sending Notification {}", method));
    }
  } else {
    safe_lsp_log("info", fmt::format("Sending Notification {}", method));
  }
  std::cout << request.c_str() << std::flush;
}

void LSPRequester::send_progress_create_request(const std::string& token,
                                                const std::string& title,
                                                const std::string& message,
                                                const int percentage) {
  LSPSpec::WorkDoneProgressCreateParams createRequest;
  createRequest.token = token;
  send_request(createRequest, "window/workDoneProgress/create");
  LSPSpec::WorkDoneProgressBegin beginPayload;
  beginPayload.title = title;
  beginPayload.cancellable = false;  // TODO - maybe one day
  beginPayload.message = message;
  if (percentage >= 0) {
    beginPayload.percentage = percentage;
  }
  LSPSpec::ProgressNotificationPayload notification;
  notification.token = token;
  notification.beginValue = beginPayload;
  send_notification(notification, "$/progress");
}

void LSPRequester::send_progress_update_request(const std::string& token,
                                                const std::string& message,
                                                const int percentage) {
  LSPSpec::WorkDoneProgressReport reportPayload;
  reportPayload.cancellable = false;  // TODO - maybe one day
  reportPayload.message = message;
  if (percentage >= 0) {
    reportPayload.percentage = percentage;
  }
  LSPSpec::ProgressNotificationPayload notification;
  notification.token = token;
  notification.reportValue = reportPayload;
  send_notification(notification, "$/progress");
}

void LSPRequester::send_progress_finish_request(const std::string& token,
                                                const std::string& message) {
  LSPSpec::WorkDoneProgressEnd endPayload;
  endPayload.message = message;
  LSPSpec::ProgressNotificationPayload notification;
  notification.token = token;
  notification.endValue = endPayload;
  send_notification(notification, "$/progress");
}


