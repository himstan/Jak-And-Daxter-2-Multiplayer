#include <algorithm>
#include <cstring>
#include <limits>

#include "game/kernel/common/kdsnetm.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/ksocket.h"
#include "game/system/Deci2Server.h"
#include "goalc/listener/Listener.h"
#include "gtest/gtest.h"

using namespace listener;

namespace {
bool always_false() {
  return false;
}

struct FramingServer : Deci2Server {
  using Deci2Server::Deci2Server;
  using Deci2Server::listening_socket;

  sockaddr_in address() const {
    sockaddr_in result = {};
    socklen_t size = sizeof(result);
    EXPECT_EQ(getsockname(listening_socket, reinterpret_cast<sockaddr*>(&result), &size), 0);
    return result;
  }
};

template <typename Predicate>
bool wait_for(Predicate ready) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!ready() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return ready();
}

ListenerMessageHeader command_header(u32 payload_size = 0) {
  ListenerMessageHeader header = {};
  header.deci2_header.len = sizeof(header) + payload_size;
  header.deci2_header.proto = DECI2_PROTOCOL;
  header.deci2_header.src = 'H';
  header.deci2_header.dst = 'E';
  header.msg_size = payload_size;
  header.ltt_msg_kind = LTT_MSG_CODE;
  return header;
}

std::vector<ListenerMessageHeader> invalid_headers() {
  std::vector<ListenerMessageHeader> result;
  auto header = command_header();
  header.deci2_header.len = sizeof(header) - 1;
  result.push_back(header);
  header = command_header();
  header.msg_size = 1;
  result.push_back(header);
  header.msg_size = std::numeric_limits<u32>::max();
  result.push_back(header);
  header.deci2_header.len = std::numeric_limits<u32>::max();
  header.msg_size = header.deci2_header.len - sizeof(header);
  result.push_back(header);
  header = command_header();
  header.deci2_header.proto++;
  result.push_back(header);
  return result;
}

void write_all(int socket, const void* data, size_t size) {
  const auto* cursor = static_cast<const char*>(data);
  while (size != 0) {
    const int sent = write_to_socket(socket, cursor, size);
    ASSERT_GT(sent, 0);
    cursor += sent;
    size -= sent;
  }
}

class DeciFraming : public testing::Test {
 protected:
  FramingServer server{always_false, 0};
  int client = -1;

  void SetUp() override {
    ASSERT_TRUE(server.init_server());
    client = open_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client, 0);
    ASSERT_EQ(set_socket_timeout(client, 200000), 0);
    auto address = server.address();
    ASSERT_EQ(connect_socket(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    u32 version[2];
    size_t received = 0;
    while (received < sizeof(version)) {
      const int count = read_from_socket(client, reinterpret_cast<char*>(version) + received,
                                         sizeof(version) - received);
      ASSERT_GT(count, 0);
      received += count;
    }
    ASSERT_TRUE(wait_for([&] { return server.is_client_connected(); }));
  }

  void TearDown() override { close_socket(client); }
};

class InvalidDeciFrame : public DeciFraming,
                         public testing::WithParamInterface<ListenerMessageHeader> {};

class ListenerBuffer : public testing::Test {
 protected:
  GoalProtoBlock saved = protoBlock;
  std::vector<u8> bytes = std::vector<u8>(DEBUG_MESSAGE_BUFFER_SIZE + 1, 0x7f);
  ListenerMessageHeader* header = reinterpret_cast<ListenerMessageHeader*>(bytes.data());

  void SetUp() override {
    protoBlock = {};
    *header = command_header();
    protoBlock.receive_buffer = header;
    protoBlock.last_receive_size = sizeof(*header);
  }

  void TearDown() override { protoBlock = saved; }
};
}  // namespace

TEST_P(InvalidDeciFrame, RejectsBeforeReadingPayload) {
  auto header = GetParam();
  write_all(client, &header, sizeof(header));
  server.read_data();
  EXPECT_FALSE(server.is_client_connected());
}

INSTANTIATE_TEST_SUITE_P(MalformedHeaders, InvalidDeciFrame, testing::ValuesIn(invalid_headers()));

TEST_F(DeciFraming, RejectsFramesWithoutTerminatorSpace) {
  const auto header = command_header(DEBUG_MESSAGE_BUFFER_SIZE - sizeof(ListenerMessageHeader));
  write_all(client, &header, sizeof(header));
  server.read_data();
  EXPECT_FALSE(server.is_client_connected());
}

TEST_F(DeciFraming, RejectsReversedDirection) {
  auto header = command_header();
  std::swap(header.deci2_header.src, header.deci2_header.dst);
  write_all(client, &header, sizeof(header));
  server.read_data();
  EXPECT_FALSE(server.is_client_connected());
}

TEST_F(DeciFraming, DisconnectsOnTruncatedPayload) {
  const auto header = command_header(4);
  write_all(client, &header, sizeof(header));
  write_all(client, "x", 1);
  close_socket(client);
  client = -1;
  server.read_data();
  EXPECT_FALSE(server.is_client_connected());
}

TEST_F(DeciFraming, DispatchesCompleteFramesInBoundedChunksToMatchingProtocol) {
  struct Capture {
    Deci2Driver* driver;
    std::vector<char> bytes;
    int completed = 0;
  };
  Deci2Driver drivers[2] = {};
  Capture captures[2] = {{&drivers[0]}, {&drivers[1]}};
  int count = 2;
  for (int index = 0; index < count; ++index) {
    auto& driver = drivers[index];
    driver.active = true;
    driver.protocol = DECI2_PROTOCOL + index;
    driver.opt = &captures[index];
    driver.handler = [](s32 event, s32 size, void* context) {
      auto& capture = *static_cast<Capture*>(context);
      if (event == DECI2_READ) {
        EXPECT_LE(size, std::numeric_limits<u16>::max());
        const auto* start = static_cast<const char*>(capture.driver->recv_buffer);
        capture.bytes.insert(capture.bytes.end(), start, start + size);
        capture.driver->recv_size = size;
      } else if (event == DECI2_READDONE) {
        ++capture.completed;
      }
    };
  }
  server.send_proto_ready(drivers, &count);
  const auto header = command_header(65536);
  std::vector<char> frame(header.deci2_header.len, 'x');
  std::memcpy(frame.data(), &header, sizeof(header));
  std::thread writer([&] { write_all(client, frame.data(), frame.size()); });
  server.read_data();
  writer.join();
  EXPECT_EQ(captures[0].bytes, frame);
  EXPECT_EQ(captures[0].completed, 1);
  EXPECT_TRUE(captures[1].bytes.empty());
  EXPECT_EQ(captures[1].completed, 0);
}

TEST_F(ListenerBuffer, RejectsInvalidHeadersWithoutWritingToMemory) {
  for (const auto& invalid : invalid_headers()) {
    *header = invalid;
    const auto before = bytes;
    protoBlock.last_receive_size = sizeof(*header);
    EXPECT_EQ(ReceiveToBuffer(nullptr), std::numeric_limits<u32>::max());
    EXPECT_EQ(protoBlock.last_receive_size, -1);
    EXPECT_EQ(bytes, before);
  }
}

TEST_F(ListenerBuffer, RejectsTruncationAndFullBuffers) {
  *header = command_header(4);
  protoBlock.last_receive_size = sizeof(*header) + 3;
  EXPECT_EQ(ReceiveToBuffer(nullptr), std::numeric_limits<u32>::max());
  *header = command_header(DEBUG_MESSAGE_BUFFER_SIZE - sizeof(*header));
  protoBlock.last_receive_size = DEBUG_MESSAGE_BUFFER_SIZE;
  EXPECT_EQ(ReceiveToBuffer(nullptr), std::numeric_limits<u32>::max());
  EXPECT_EQ(bytes[DEBUG_MESSAGE_BUFFER_SIZE], 0x7f);
}

TEST_F(ListenerBuffer, AcceptsLargestSafePayloadAndTerminatesInsideBuffer) {
  const auto payload_size = DEBUG_MESSAGE_BUFFER_SIZE - sizeof(*header) - 1;
  *header = command_header(payload_size);
  protoBlock.last_receive_size = header->deci2_header.len;
  EXPECT_EQ(ReceiveToBuffer(nullptr), payload_size);
  EXPECT_EQ(bytes[DEBUG_MESSAGE_BUFFER_SIZE - 1], 0);
  EXPECT_EQ(bytes[DEBUG_MESSAGE_BUFFER_SIZE], 0x7f);
  EXPECT_EQ(protoBlock.msg_kind, LTT_MSG_CODE);
}

TEST_F(ListenerBuffer, RejectsMissingReceiveBuffer) {
  protoBlock.receive_buffer = nullptr;
  EXPECT_EQ(ReceiveToBuffer(nullptr), std::numeric_limits<u32>::max());
}

TEST(Listener, RejectsMalformedRuntimeHeaders) {
  auto headers = invalid_headers();
  auto oversized_ack = command_header(512);
  oversized_ack.msg_kind = ListenerMessageKind::MSG_ACK;
  headers.push_back(oversized_ack);
  auto invalid_id = command_header();
  invalid_id.msg_kind = ListenerMessageKind::MSG_PRINT;
  invalid_id.msg_id = 1;
  headers.push_back(invalid_id);
  for (auto header : headers) {
    FramingServer server(always_false, 0);
    ASSERT_TRUE(server.init_server());
    Listener listener;
    ASSERT_TRUE(listener.connect_to_target(1, "127.0.0.1", ntohs(server.address().sin_port)));
    ASSERT_TRUE(wait_for([&] { return server.is_client_connected(); }));
    header.deci2_header.src = 'E';
    header.deci2_header.dst = 'H';
    server.send_data(&header, sizeof(header));
    EXPECT_TRUE(wait_for([&] { return !listener.is_connected(); }));
  }
}

TEST(Listener, ReceivesValidRuntimeOutput) {
  FramingServer server(always_false, 0);
  ASSERT_TRUE(server.init_server());
  Listener listener;
  ASSERT_TRUE(listener.connect_to_target(1, "127.0.0.1", ntohs(server.address().sin_port)));
  ASSERT_TRUE(wait_for([&] { return server.is_client_connected(); }));
  listener.record_messages(ListenerMessageKind::MSG_PRINT);
  auto header = command_header(5);
  header.deci2_header.src = 'E';
  header.deci2_header.dst = 'H';
  header.msg_kind = ListenerMessageKind::MSG_PRINT;
  server.send_data(&header, 5);
  server.send_data(reinterpret_cast<char*>(&header) + 5, sizeof(header) - 5);
  char body[] = "hello";
  server.send_data(body, 5);
  ASSERT_TRUE(wait_for([&] { return listener.get_received_message_count() == 1; }));
  EXPECT_EQ(listener.stop_recording_messages(), std::vector<std::string>{"hello"});
}

TEST(Listener, DisconnectsOnTruncatedRuntimeOutput) {
  Listener listener;
  {
    FramingServer server(always_false, 0);
    ASSERT_TRUE(server.init_server());
    ASSERT_TRUE(listener.connect_to_target(1, "127.0.0.1", ntohs(server.address().sin_port)));
    ASSERT_TRUE(wait_for([&] { return server.is_client_connected(); }));
    auto header = command_header(4);
    header.deci2_header.src = 'E';
    header.deci2_header.dst = 'H';
    header.msg_kind = ListenerMessageKind::MSG_PRINT;
    server.send_data(&header, sizeof(header));
    char body = 'x';
    server.send_data(&body, 1);
  }
  EXPECT_TRUE(wait_for([&] { return !listener.is_connected(); }));
}

TEST(Listener, RejectsMalformedRuntimeMetadataWithoutAborting) {
  for (const std::string body : {"reset\n", "load\n", "load \"file\" code\n",
                                 "load \"file\" code #xBAD #xBAD #xBAD #xBAD #xBAD invalid\n"}) {
    FramingServer server(always_false, 0);
    ASSERT_TRUE(server.init_server());
    Listener listener;
    ASSERT_TRUE(listener.connect_to_target(1, "127.0.0.1", ntohs(server.address().sin_port)));
    ASSERT_TRUE(wait_for([&] { return server.is_client_connected(); }));
    auto header = command_header(body.size());
    header.deci2_header.src = 'E';
    header.deci2_header.dst = 'H';
    header.msg_kind = ListenerMessageKind::MSG_OUTPUT;
    server.send_data(&header, sizeof(header));
    server.send_data(const_cast<char*>(body.data()), body.size());
    EXPECT_TRUE(wait_for([&] { return !listener.is_connected(); }));
  }
}

TEST(Listener, ListenerCreation) {
  listener::Listener l;
}

TEST(Listener, DeciCreation) {
  Deci2Server s(always_false, DECI2_PORT);
}

TEST(Listener, DeciInit) {
  Deci2Server s(always_false, DECI2_PORT);
  EXPECT_TRUE(s.init_server());
}

TEST(Listener, DeciBindsOnlyToLoopback) {
  struct TestServer : Deci2Server {
    using Deci2Server::Deci2Server;
    using Deci2Server::listening_socket;
  };
  TestServer s(always_false, 0);
  ASSERT_TRUE(s.init_server());
  sockaddr_in bound_address = {};
  socklen_t address_length = sizeof(bound_address);
  ASSERT_EQ(
      getsockname(s.listening_socket, reinterpret_cast<sockaddr*>(&bound_address), &address_length),
      0);
  EXPECT_EQ(bound_address.sin_family, AF_INET);
  EXPECT_EQ(ntohl(bound_address.sin_addr.s_addr), INADDR_LOOPBACK);
}

/*!
 * Try to connect when no Deci2Server is running
 */
TEST(Listener, ListenToNothing) {
  Listener l;
  if (l.connect_to_target()) {
    printf(
        "~~~~~~ Test connected to a runtime when there shouldn't be anything running! Check that "
        "you don't have gk running in the background!\n");
  }
  EXPECT_FALSE(l.connect_to_target());
  l.disconnect();
}

TEST(Listener, DeciCheckNoListener) {
  Deci2Server s(always_false, DECI2_PORT);
  EXPECT_TRUE(s.init_server());
  EXPECT_FALSE(s.is_client_connected());
  EXPECT_FALSE(s.is_client_connected());
  EXPECT_FALSE(s.is_client_connected());
}

TEST(Listener, CheckConnectionStaysAlive) {
  Deci2Server s(always_false, DECI2_PORT);
  EXPECT_TRUE(s.init_server());
  EXPECT_FALSE(s.is_client_connected());
  Listener l;
  EXPECT_FALSE(s.is_client_connected());
  bool connected = l.connect_to_target();
  EXPECT_TRUE(connected);
  // TODO - some sort of backoff and retry would be better
  while (connected && !s.is_client_connected()) {
  }

  EXPECT_TRUE(s.is_client_connected());
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT_TRUE(s.is_client_connected());
  EXPECT_TRUE(l.is_connected());
}

TEST(Listener, DeciThenListener) {
  for (int i = 0; i < 3; i++) {
    Deci2Server s(always_false, DECI2_PORT);
    EXPECT_TRUE(s.init_server());
    EXPECT_FALSE(s.is_client_connected());
    EXPECT_FALSE(s.is_client_connected());
    EXPECT_FALSE(s.is_client_connected());
    Listener l;
    EXPECT_FALSE(s.is_client_connected());
    EXPECT_FALSE(s.is_client_connected());
    bool connected = l.connect_to_target();
    EXPECT_TRUE(connected);
    // TODO - some sort of backoff and retry would be better
    while (connected && !s.is_client_connected()) {
    }

    EXPECT_TRUE(s.is_client_connected());
  }
}

TEST(Listener, DeciThenListener2) {
  for (int i = 0; i < 3; i++) {
    Deci2Server s(always_false, DECI2_PORT);
    EXPECT_TRUE(s.init_server());
    EXPECT_FALSE(s.is_client_connected());
    EXPECT_FALSE(s.is_client_connected());
    EXPECT_FALSE(s.is_client_connected());
    Listener l;
    EXPECT_FALSE(s.is_client_connected());
    EXPECT_FALSE(s.is_client_connected());
    EXPECT_TRUE(l.connect_to_target());
  }
}

TEST(Listener, ListenerThenDeci) {
  for (int i = 0; i < 3; i++) {
    Listener l;
    EXPECT_FALSE(l.connect_to_target());
    Deci2Server s(always_false, DECI2_PORT);
    EXPECT_TRUE(s.init_server());
    EXPECT_FALSE(s.is_client_connected());
    bool connected = l.connect_to_target();
    EXPECT_TRUE(connected);
    // TODO - some sort of backoff and retry would be better
    while (connected && !s.is_client_connected()) {
    }
  }
}
