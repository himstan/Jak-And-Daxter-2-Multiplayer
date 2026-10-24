/*!
 * @file Deci2Server.cpp
 * Basic implementation of a DECI2 server.
 * Works with deci2.cpp (sceDeci2) to implement the networking on target
 */

// clang-format off
#include "Deci2Server.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "common/cross_sockets/XSocket.h"
#include "common/versions/versions.h"
#include "common/listener_common.h"
#include "game/kernel/common/kprint.h"

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

Deci2Server::~Deci2Server() {
  // Cleanup the accept thread
  if (accept_thread_running) {
    kill_accept_thread = true;
    // NOTE - if we don't want to wait for the roundtrip timeout to exit the game gracefully
    // we should just terminate the thread forcefully
    accept_thread.join();
    accept_thread_running = false;
  }

  close_socket(accepted_socket);
}

void Deci2Server::post_init() {
  lg::info("[Deci2Server:{}] awaiting connections", tcp_port);
  accept_thread_running = true;
  kill_accept_thread = false;
  accept_thread = std::thread(&Deci2Server::accept_thread_func, this);
}

void Deci2Server::accept_thread_func() {
  socklen_t addr_len = sizeof(addr);
  while (!kill_accept_thread) {
    accepted_socket = select_and_accept_socket(listening_socket, (sockaddr*)&addr, &addr_len,
                                               100000);  // 0.1 second timeout
    if (accepted_socket >= 0) {
      set_socket_timeout(accepted_socket, 100000);
      u32 versions[2] = {versions::GOAL_VERSION_MAJOR, versions::GOAL_VERSION_MINOR};
      write_to_socket(accepted_socket, (char*)&versions, 8);  // todo, check result?
      client_connected = true;
      return;
    }
  }
}

bool Deci2Server::is_client_connected() {
  return client_connected;
}

/*!
 * Wait for protocols to become ready.
 * This avoids the case where we receive messages before protocol handlers are set up.
 */
bool Deci2Server::wait_for_protos_ready() {
  if (protocols_ready || want_shutdown) {
    return !want_shutdown;
  }
  std::unique_lock<std::mutex> lk(server_mutex);
  cv.wait(lk, [&] { return protocols_ready || want_shutdown; });
  return !want_shutdown;
}

void Deci2Server::send_shutdown() {
  lock();
  want_shutdown = true;
  unlock();
  cv.notify_all();
}

/*!
 * Inform server that protocol handlers are ready.
 * Will unblock wait_for_protos_ready and incoming messages will be dispatched to these
 * protocols.  You can change the protocol handlers, but you should lock the mutex before
 * doing so.
 */
void Deci2Server::send_proto_ready(Deci2Driver* drivers, int* driver_count) {
  lock();
  d2_drivers = drivers;
  d2_driver_count = driver_count;
  protocols_ready = true;
  unlock();
  cv.notify_all();
}

bool Deci2Server::read_bytes(char* destination, int size) {
  int received = 0;
  while (received < size && !want_exit_callback()) {
    const int count = read_from_socket(accepted_socket, destination + received, size - received);
    if (count == 0 || (count < 0 && !socket_timed_out())) {
      client_connected = false;
      return false;
    }
    if (count > 0) {
      received += count;
    }
  }
  return received == size;
}

void Deci2Server::read_data() {
  if (!is_client_connected()) {
    return;
  }

  ListenerMessageHeader header = {};
  if (!read_bytes(reinterpret_cast<char*>(&header), sizeof(header))) {
    return;
  }
  const auto maximum_size = std::min(buffer.size(), size_t{DEBUG_MESSAGE_BUFFER_SIZE - 1});
  if (!valid_listener_message_header(header, maximum_size) || header.deci2_header.src != 'H' ||
      header.deci2_header.dst != 'E') {
    client_connected = false;
    return;
  }
  std::memcpy(buffer.data(), &header, sizeof(header));
  if (!read_bytes(buffer.data() + sizeof(header), header.msg_size)) {
    return;
  }

  // see what protocol we got:
  std::lock_guard guard(server_mutex);
  if (!d2_drivers || !d2_driver_count) {
    return;
  }
  int handler = -1;
  for (int i = 0; i < *d2_driver_count; i++) {
    auto& prot = d2_drivers[i];
    if (prot.active && prot.protocol == header.deci2_header.proto) {
      if (handler != -1) {
        printf("[DECI2] Warning: more than on protocol handler for this message!\n");
      }
      handler = i;
    }
  }

  if (handler == -1) {
    printf("[DECI2] Warning: no handler for this message, ignoring...\n");
    return;
  }

  auto& driver = d2_drivers[handler];
  u32 sent_to_program = 0;
  while (sent_to_program < header.deci2_header.len && !want_exit_callback()) {
    // send what we have to the program
    driver.recv_buffer = buffer.data() + sent_to_program;
    driver.available_to_receive =
        std::min<u32>(header.deci2_header.len - sent_to_program, std::numeric_limits<u16>::max());
    driver.recv_size = 0;
    (driver.handler)(DECI2_READ, driver.available_to_receive, driver.opt);
    if (driver.recv_size <= 0 || driver.recv_size > driver.available_to_receive) {
      client_connected = false;
      return;
    }
    sent_to_program += driver.recv_size;
  }
  if (!want_exit_callback()) {
    (driver.handler)(DECI2_READDONE, 0, driver.opt);
  }
}

void Deci2Server::send_data(void* buf, u16 len) {
  lock();
  if (!client_connected) {
    printf("[DECI2] send while not connected, not sending!\n");
  } else {
    u32 prog = 0;
    while (prog < len) {
      int wrote = write_to_socket(accepted_socket, (char*)(buf) + prog, len - prog);
      if (wrote <= 0) {
        client_connected = false;
        break;
      }
      prog += wrote;
      if (!client_connected || want_exit_callback()) {
        unlock();
        return;
      }
    }
  }
  unlock();
}

void Deci2Server::lock() {
  server_mutex.lock();
}

void Deci2Server::unlock() {
  server_mutex.unlock();
}
