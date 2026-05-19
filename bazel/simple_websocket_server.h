#pragma once

#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vlpweb {

class SimpleWebsocketServer {
 public:
  SimpleWebsocketServer(int port, std::string html_path);
  ~SimpleWebsocketServer();

  SimpleWebsocketServer(const SimpleWebsocketServer&) = delete;
  SimpleWebsocketServer& operator=(const SimpleWebsocketServer&) = delete;

  void Start();
  void Stop();
  void BroadcastText(const std::string& payload);

 private:
  void Loop();

  int port_ = 0;
  std::string html_path_;
  int listen_fd_ = -1;
  bool stopping_ = false;
  std::thread thread_;
  std::mutex mu_;
  std::vector<int> clients_;
};

}  // namespace vlpweb
