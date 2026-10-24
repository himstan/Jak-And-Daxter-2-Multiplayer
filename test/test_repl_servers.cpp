#include <limits>
#include <string_view>
#include <type_traits>

#include "common/repl/nrepl/ReplServer.h"

#include "goalc/debugger/DebugServer.h"
#include "gtest/gtest.h"

namespace {
template <typename Server>
class SocketServerTest : public testing::Test {
 protected:
  struct TestServer : Server {
    using Server::listening_socket;
    using Server::Server;
  };
  TestServer server{[] { return false; }, 0};
  std::vector<int> clients;

  void SetUp() override { ASSERT_TRUE(server.init_server()); }
  void TearDown() override {
    for (int socket : clients) {
      close_socket(socket);
    }
  }

  std::optional<std::string> poll() {
    if constexpr (std::is_same_v<Server, ReplServer>) {
      return server.get_msg();
    } else {
      server.run_once();
      return std::nullopt;
    }
  }

  sockaddr_in bound_address() {
    sockaddr_in address = {};
    socklen_t size = sizeof(address);
    EXPECT_EQ(getsockname(server.listening_socket, reinterpret_cast<sockaddr*>(&address), &size),
              0);
    return address;
  }

  int connect_client() {
    const int socket = open_socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(socket, 0);
    clients.push_back(socket);
    EXPECT_EQ(set_socket_timeout(socket, 200000), 0);
    int yes = 1;
    EXPECT_EQ(set_socket_option(socket, TCP_SOCKET_LEVEL, TCP_NODELAY, &yes, sizeof(yes)), 0);
    auto address = bound_address();
    EXPECT_EQ(connect_socket(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    poll();
    char greeting[512];
    EXPECT_GT(read_from_socket(socket, greeting, sizeof(greeting)), 0);
    return socket;
  }

  void send(int socket, std::string_view bytes) {
    while (!bytes.empty()) {
      const int sent = write_to_socket(socket, bytes.data(), bytes.size());
      ASSERT_GT(sent, 0);
      bytes.remove_prefix(sent);
    }
  }

  void expect_closed(int socket) {
    char byte;
    const int received = read_from_socket(socket, &byte, 1);
    EXPECT_LE(received, 0);
    if (received < 0) {
      EXPECT_FALSE(socket_timed_out());
    }
  }

  std::string read_line(int socket) {
    std::string line;
    char byte;
    while (read_from_socket(socket, &byte, 1) == 1) {
      if (byte == '\n') {
        return line;
      }
      line += byte;
    }
    ADD_FAILURE() << "No complete response received";
    return "{}";
  }
};

using NReplServerTest = SocketServerTest<ReplServer>;
using DebugServerTest = SocketServerTest<DebugServer>;

std::string frame(std::string_view body, u32 type = EVAL) {
  const ReplServerHeader header = {static_cast<u32>(body.size()), type};
  return std::string(reinterpret_cast<const char*>(&header), sizeof(header)) + std::string(body);
}

TEST_F(NReplServerTest, BindsOnlyToLoopback) {
  EXPECT_EQ(ntohl(bound_address().sin_addr.s_addr), INADDR_LOOPBACK);
}

TEST_F(NReplServerTest, PreservesFragmentedHeadersAndBodies) {
  const int socket = connect_client();
  const auto message = frame("(+ 1 2)");
  send(socket, message.substr(0, 3));
  EXPECT_FALSE(poll());
  send(socket, message.substr(3, sizeof(ReplServerHeader) - 3));
  EXPECT_FALSE(poll());
  send(socket, "(+ ");
  EXPECT_FALSE(poll());
  send(socket, "1 2)");
  EXPECT_EQ(poll(), "(+ 1 2)");
}

TEST_F(NReplServerTest, PreservesPipelinedMessages) {
  const int socket = connect_client();
  send(socket, frame("first") + frame("second"));
  EXPECT_FALSE(poll());
  EXPECT_EQ(poll(), "first");
  EXPECT_FALSE(poll());
  EXPECT_EQ(poll(), "second");
}

TEST_F(NReplServerTest, IncompleteClientDoesNotBlockAnotherClient) {
  const int first = connect_client();
  const int second = connect_client();
  send(first, frame("unfinished").substr(0, sizeof(ReplServerHeader) + 2));
  EXPECT_FALSE(poll());
  EXPECT_FALSE(poll());
  send(second, frame("complete"));
  EXPECT_FALSE(poll());
  EXPECT_EQ(poll(), "complete");
  send(first, "finished");
  EXPECT_EQ(poll(), "unfinished");
}

TEST_F(NReplServerTest, RejectsOversizedAndSignedOverflowLengths) {
  for (u32 length : {1024u * 1024u + 1, 0x80000000u, std::numeric_limits<u32>::max()}) {
    const int socket = connect_client();
    const ReplServerHeader header = {length, EVAL};
    send(socket, {reinterpret_cast<const char*>(&header), sizeof(header)});
    EXPECT_FALSE(poll());
    expect_closed(socket);
  }
}

TEST_F(NReplServerTest, AcceptsMessageAtSizeLimit) {
  const int socket = connect_client();
  const std::string chunk(16 * 1024, 'x');
  const ReplServerHeader header = {1024 * 1024, EVAL};
  send(socket, {reinterpret_cast<const char*>(&header), sizeof(header)});
  EXPECT_FALSE(poll());
  for (int index = 0; index < 64; ++index) {
    send(socket, chunk);
    const auto message = poll();
    if (index != 63) {
      EXPECT_FALSE(message);
    } else {
      ASSERT_TRUE(message);
      EXPECT_EQ(*message, std::string(1024 * 1024, 'x'));
    }
  }
}

TEST_F(NReplServerTest, RejectsUnknownTypesAndKeepsOtherClientsUsable) {
  const int bad = connect_client();
  send(bad, frame("", 999));
  EXPECT_FALSE(poll());
  expect_closed(bad);
  const int good = connect_client();
  send(good, frame("", PING));
  EXPECT_FALSE(poll());
  char greeting[512];
  EXPECT_GT(read_from_socket(good, greeting, sizeof(greeting)), 0);
  send(good, frame(""));
  EXPECT_EQ(poll(), "");
}

TEST_F(NReplServerTest, DiscardsIncompleteMessagesOnDisconnect) {
  const int first = connect_client();
  send(first, frame("unfinished").substr(0, sizeof(ReplServerHeader) + 2));
  EXPECT_FALSE(poll());
  EXPECT_FALSE(poll());
  close_socket(first);
  clients.front() = -1;
  EXPECT_FALSE(poll());
  const int second = connect_client();
  send(second, frame("new request"));
  EXPECT_FALSE(poll());
  EXPECT_EQ(poll(), "new request");
}

TEST_F(DebugServerTest, BindsOnlyToLoopback) {
  EXPECT_EQ(ntohl(bound_address().sin_addr.s_addr), INADDR_LOOPBACK);
}

TEST_F(DebugServerTest, RejectsMalformedRequestsAndRecovers) {
  const int socket = connect_client();
  for (const std::string_view request :
       {"null", "[]", "{\"cmd\":1}", "{\"cmd\":\"status\",\"args\":[]}",
        "{\"cmd\":\"status\",\"seq\":\"bad\"}"}) {
    send(socket, std::string(request) + "\n");
    poll();
    const auto response = json::parse(read_line(socket));
    EXPECT_EQ(response["ok"], false);
    EXPECT_EQ(response["error"], "invalid request fields");
  }
  for (const auto& request : {std::string("{"), std::string(100, '[') + std::string(100, ']')}) {
    send(socket, request + "\n");
    poll();
    const auto response = json::parse(read_line(socket));
    EXPECT_EQ(response["ok"], false);
    EXPECT_EQ(response["error"], "malformed json request");
  }
  send(socket, "{\"cmd\":\"status\",\"seq\":42}\n");
  poll();
  const auto response = json::parse(read_line(socket));
  EXPECT_EQ(response["seq"], 42);
  EXPECT_EQ(response["error"], "compiler is not available");
}

TEST_F(DebugServerTest, KeepsFragmentedRequestsSeparateBetweenClients) {
  const int first = connect_client();
  const int second = connect_client();
  send(first, "{\"cmd\":\"sta");
  poll();
  send(second, "{\"cmd\":\"status\",\"seq\":2}\n");
  poll();
  EXPECT_EQ(json::parse(read_line(second))["seq"], 2);
  send(first, "tus\",\"seq\":1}\n");
  poll();
  EXPECT_EQ(json::parse(read_line(first))["seq"], 1);
}

TEST_F(DebugServerTest, ProcessesPipelinedRequests) {
  const int socket = connect_client();
  send(socket, "{\"cmd\":\"status\",\"seq\":1}\n{\"cmd\":\"status\",\"seq\":2}\n");
  poll();
  EXPECT_EQ(json::parse(read_line(socket))["seq"], 1);
  EXPECT_EQ(json::parse(read_line(socket))["seq"], 2);
}

TEST_F(DebugServerTest, DiscardsIncompleteRequestsOnDisconnect) {
  const int first = connect_client();
  send(first, "{\"cmd\":");
  poll();
  close_socket(first);
  clients.front() = -1;
  poll();
  const int second = connect_client();
  send(second, "{\"cmd\":\"status\",\"seq\":3}\n");
  poll();
  EXPECT_EQ(json::parse(read_line(second))["seq"], 3);
}

TEST_F(DebugServerTest, AcceptsRequestAtSizeLimit) {
  const int socket = connect_client();
  const std::string chunk(16 * 1024, ' ');
  const std::string request = "{\"cmd\":\"status\",\"seq\":4}";
  for (int index = 0; index < 63; ++index) {
    send(socket, chunk);
    poll();
  }
  send(socket, std::string(chunk.size() - request.size(), ' ') + request + "\n");
  poll();
  EXPECT_EQ(json::parse(read_line(socket))["seq"], 4);
}

TEST_F(DebugServerTest, DisconnectsOversizedRequestsWithAndWithoutNewline) {
  for (const std::string_view ending : {"x", "x\n"}) {
    const int socket = connect_client();
    const std::string chunk(16 * 1024, ' ');
    for (int index = 0; index < 64; ++index) {
      send(socket, chunk);
      poll();
    }
    send(socket, ending);
    poll();
    expect_closed(socket);
  }
  const int socket = connect_client();
  send(socket, "{\"cmd\":\"status\",\"seq\":3}\n");
  poll();
  EXPECT_EQ(json::parse(read_line(socket))["seq"], 3);
}
}  // namespace
