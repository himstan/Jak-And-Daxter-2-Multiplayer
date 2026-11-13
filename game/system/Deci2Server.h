#pragma once

#include <atomic>
#include <condition_variable>

#include "deci_common.h"

#include "common/cross_sockets/XSocketServer.h"

/// @brief Basic implementation of a DECI2 server.
/// Works with deci2.cpp(sceDeci2) to implement the networking on target
class Deci2Server : public XSocketServer {
 public:
  using XSocketServer::XSocketServer;
  virtual ~Deci2Server();

  void post_init() override;

  void read_data();
  void send_data(void* buf, u16 len);

  bool is_client_connected();
  bool wait_for_protos_ready();  // return true if ready, false if we should shut down.
  void send_proto_ready(Deci2Driver* drivers, int* driver_count);
  void send_shutdown();

  void lock();
  void unlock();

 protected:
  void accept_thread_func();

 private:
  static constexpr size_t kMaxPendingSendBytes = 8 * 1024 * 1024;

  bool read_bytes(char* destination, int size);
  void send_thread_func();
  bool want_shutdown = false;
  bool protocols_ready = false;
  std::condition_variable cv;
  Deci2Driver* d2_drivers = nullptr;
  int* d2_driver_count = nullptr;

  int accepted_socket = -1;
  std::atomic_bool kill_accept_thread = false;
  bool accept_thread_running = false;

  std::thread accept_thread;
  std::mutex server_mutex;

  std::thread send_thread;
  std::mutex send_mutex;
  std::condition_variable send_cv;
  std::vector<char> pending_send;
  bool stop_sending = false;

  std::atomic_bool client_connected = false;
};
