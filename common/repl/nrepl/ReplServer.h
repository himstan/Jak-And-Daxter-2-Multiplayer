#pragma once

#include <map>
#include <optional>

#include "common/cross_sockets/XSocketServer.h"

enum ReplServerMessageType { PING = 0, EVAL = 10, SHUTDOWN = 20 };

struct ReplServerHeader {
  u32 length;
  u32 type;
};

class ReplServer : public XSocketServer {
 public:
  using XSocketServer::XSocketServer;
  virtual ~ReplServer();

  void post_init() override;

  std::optional<std::string> get_msg();

 private:
  int max_clients = 50;
  struct Client {
    ReplServerHeader header = {};
    size_t header_received = 0;
    std::string body;
  };
  std::map<int, Client> clients;

  void close_client(int socket);
  std::optional<std::string> read_client(int socket);
  void ping_response(int socket);
};
