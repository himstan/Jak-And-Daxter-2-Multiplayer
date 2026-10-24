// clang-format off
#include "ReplServer.h"

#include <algorithm>
#include <utility>

#include "common/cross_sockets/XSocket.h"
#include "common/versions/versions.h"

#include "fmt/format.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#endif
#include "common/log/log.h"
// clang-format on

// TODO - The server also needs to eventually return the result of the evaluation

ReplServer::~ReplServer() {
  // Close all our client sockets!
  for (const auto& [socket, client] : clients) {
    close_socket(socket);
  }
}

void ReplServer::post_init() {
  lg::debug("[nREPL:{}:{}] awaiting connections", tcp_port, listening_socket);
}

void ReplServer::close_client(int socket) {
  // Cleanup the socket and remove it from our clients
  close_socket(socket);
  clients.erase(socket);
}

void ReplServer::ping_response(int socket) {
  const auto ping = fmt::format("Connected to OpenGOAL v{}.{} nREPL!", versions::GOAL_VERSION_MAJOR,
                                versions::GOAL_VERSION_MINOR);
  if (write_to_socket(socket, ping.data(), ping.size()) != (int)ping.size()) {
    close_client(socket);
  }
}

std::optional<std::string> ReplServer::read_client(int socket) {
  auto& client = clients.at(socket);
  if (client.header_received < sizeof(ReplServerHeader)) {
    // Attempt to read a header
    const int received =
        read_from_socket(socket, reinterpret_cast<char*>(&client.header) + client.header_received,
                         sizeof(ReplServerHeader) - client.header_received);
    if (received <= 0) {
      close_client(socket);
      return std::nullopt;
    }
    client.header_received += received;
    if (client.header_received != sizeof(ReplServerHeader)) {
      return std::nullopt;
    }
    constexpr size_t kMaximumMessageSize = 1024 * 1024;
    if (client.header.length > std::min(buffer.size(), kMaximumMessageSize) ||
        (client.header.type != PING && client.header.type != EVAL)) {
      close_client(socket);
      return std::nullopt;
    }
    if (client.header.length != 0) {
      return std::nullopt;
    }
  } else {
    // get the body of the message
    const int received =
        read_from_socket(socket, buffer.data(), client.header.length - client.body.size());
    if (received <= 0) {
      close_client(socket);
      return std::nullopt;
    }
    client.body.append(buffer.data(), received);
    if (client.body.size() != client.header.length) {
      return std::nullopt;
    }
  }

  // Otherwise, process the message
  const auto type = client.header.type;
  auto message = std::move(client.body);
  client = {};
  if (type == PING) {
    ping_response(socket);
    return std::nullopt;
  }
  return message;
}

std::optional<std::string> ReplServer::get_msg() {
  fd_set read_sockets;
  // Clear the sockets we are listening on
  FD_ZERO(&read_sockets);

  // Add the server's main listening socket (where we accept clients from)
  FD_SET(listening_socket, &read_sockets);
  int max_sd = listening_socket;
  for (const auto& [socket, client] : clients) {
    FD_SET(socket, &read_sockets);
    max_sd = std::max(max_sd, socket);
  }
  // Wait for activity on _something_, with a timeout so we don't get stuck here on exit.
  timeval timeout = {0, 100000};
  const auto activity = select(max_sd + 1, &read_sockets, nullptr, nullptr, &timeout);
  if (activity <= 0 || want_exit_callback()) {
    return std::nullopt;
  }

  // If something happened on the master socket - it's a new connection
  if (FD_ISSET(listening_socket, &read_sockets)) {
    socklen_t addr_len = sizeof(addr);
    const int socket = accept_socket(listening_socket, (sockaddr*)&addr, &addr_len);
    if (socket >= 0) {
      if ((int)clients.size() >= max_clients) {
        // Reject the connection and close the socket
        close_socket(socket);
      } else {
        // Track the new socket
        clients.emplace(socket, Client{});
        // Say hello
        ping_response(socket);
      }
    }
  }

  // Check all clients for activity
  for (auto it = clients.begin(); it != clients.end();) {
    const int socket = (it++)->first;
    if (FD_ISSET(socket, &read_sockets)) {
      if (auto message = read_client(socket)) {
        return message;
      }
    }
  }
  return std::nullopt;
}
