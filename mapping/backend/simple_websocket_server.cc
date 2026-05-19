#include "mapping/backend/simple_websocket_server.h"

#include <glog/logging.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace vlpweb {
namespace {

std::string Base64Encode(const uint8_t* data, size_t len) {
  static constexpr char kB64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  for (; i + 2 < len; i += 3) {
    const uint32_t n =
        (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) |
        static_cast<uint32_t>(data[i + 2]);
    out.push_back(kB64[(n >> 18) & 0x3F]);
    out.push_back(kB64[(n >> 12) & 0x3F]);
    out.push_back(kB64[(n >> 6) & 0x3F]);
    out.push_back(kB64[n & 0x3F]);
  }
  if (i < len) {
    const uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    out.push_back(kB64[(n >> 18) & 0x3F]);
    if (i + 1 < len) {
      const uint32_t n2 = n | (static_cast<uint32_t>(data[i + 1]) << 8);
      out.push_back(kB64[(n2 >> 12) & 0x3F]);
      out.push_back(kB64[(n2 >> 6) & 0x3F]);
      out.push_back('=');
    } else {
      out.push_back(kB64[(n >> 12) & 0x3F]);
      out.push_back('=');
      out.push_back('=');
    }
  }
  return out;
}

std::string Sha1Base64(const std::string& s) {
  uint8_t digest[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const uint8_t*>(s.data()), s.size(), digest);
  return Base64Encode(digest, SHA_DIGEST_LENGTH);
}

bool SendAll(int fd, const uint8_t* data, size_t n) {
  size_t sent = 0;
  while (sent < n) {
    const ssize_t s = send(fd, data + sent, n - sent, MSG_NOSIGNAL);
    if (s <= 0) return false;
    sent += static_cast<size_t>(s);
  }
  return true;
}

bool SendWsText(int fd, const std::string& msg) {
  std::vector<uint8_t> frame;
  frame.push_back(0x81);
  const size_t n = msg.size();
  if (n < 126) {
    frame.push_back(static_cast<uint8_t>(n));
  } else if (n <= 0xFFFF) {
    frame.push_back(126);
    frame.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(n & 0xFF));
  } else {
    frame.push_back(127);
    for (int i = 7; i >= 0; --i) frame.push_back(static_cast<uint8_t>((n >> (i * 8)) & 0xFF));
  }
  frame.insert(frame.end(), msg.begin(), msg.end());
  return SendAll(fd, frame.data(), frame.size());
}

bool Handshake(int fd, const std::string& req) {
  const std::string key_tag = "Sec-WebSocket-Key:";
  const size_t p = req.find(key_tag);
  if (p == std::string::npos) return false;
  size_t ks = p + key_tag.size();
  while (ks < req.size() && (req[ks] == ' ' || req[ks] == '\t')) ++ks;
  size_t ke = req.find("\r\n", ks);
  if (ke == std::string::npos) return false;
  const std::string key = req.substr(ks, ke - ks);
  const std::string accept = Sha1Base64(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  std::ostringstream oss;
  oss << "HTTP/1.1 101 Switching Protocols\r\n"
      << "Upgrade: websocket\r\n"
      << "Connection: Upgrade\r\n"
      << "Sec-WebSocket-Accept: " << accept << "\r\n\r\n";
  const std::string resp = oss.str();
  return SendAll(fd, reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
}

bool SendHttp(int fd, const std::string& status, const std::string& content_type,
              const std::string& body) {
  std::ostringstream oss;
  oss << "HTTP/1.1 " << status << "\r\n"
      << "Content-Type: " << content_type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n\r\n"
      << body;
  const std::string resp = oss.str();
  return SendAll(fd, reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
}

}  // namespace

SimpleWebsocketServer::SimpleWebsocketServer(int port, std::string html_path)
    : port_(port), html_path_(std::move(html_path)) {}

SimpleWebsocketServer::~SimpleWebsocketServer() { Stop(); }

void SimpleWebsocketServer::Start() { thread_ = std::thread(&SimpleWebsocketServer::Loop, this); }

void SimpleWebsocketServer::Stop() {
  stopping_ = true;
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (int fd : clients_) {
      close(fd);
    }
    clients_.clear();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

void SimpleWebsocketServer::BroadcastText(const std::string& payload) {
  std::vector<int> bad;
  std::lock_guard<std::mutex> lock(mu_);
  for (int fd : clients_) {
    if (!SendWsText(fd, payload)) {
      bad.push_back(fd);
    }
  }
  if (!bad.empty()) {
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                  [&](int fd) {
                                    if (std::find(bad.begin(), bad.end(), fd) != bad.end()) {
                                      close(fd);
                                      return true;
                                    }
                                    return false;
                                  }),
                   clients_.end());
  }
}

void SimpleWebsocketServer::Loop() {
  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    LOG(ERROR) << "websocket socket create failed";
    return;
  }
  int opt = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(static_cast<uint16_t>(port_));
  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    LOG(ERROR) << "websocket bind failed on port " << port_;
    return;
  }
  if (listen(listen_fd_, 8) < 0) {
    LOG(ERROR) << "websocket listen failed";
    return;
  }
  LOG(INFO) << "websocket server listening on :" << port_;
  while (!stopping_) {
    sockaddr_in cli{};
    socklen_t cl = sizeof(cli);
    const int fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&cli), &cl);
    if (fd < 0) {
      if (stopping_) break;
      continue;
    }

    char buf[8192];
    const ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
      close(fd);
      continue;
    }
    buf[n] = '\0';
    const std::string req(buf);
    const bool is_ws =
        req.find("Upgrade: websocket") != std::string::npos && req.find("GET /ws ") != std::string::npos;
    if (!is_ws) {
      size_t p0 = req.find("GET ");
      size_t p1 = (p0 == std::string::npos) ? std::string::npos : req.find(' ', p0 + 4);
      const std::string path =
          (p0 == std::string::npos || p1 == std::string::npos) ? "/" : req.substr(p0 + 4, p1 - (p0 + 4));
      if (!(path == "/" || path == "/index.html")) {
        SendHttp(fd, "404 Not Found", "text/plain; charset=utf-8", "Not Found\n");
      } else {
        std::ifstream in(html_path_);
        if (!in.good()) {
          SendHttp(fd, "500 Internal Server Error", "text/plain; charset=utf-8",
                   "web_client.html not found\n");
        } else {
          std::string html((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
          SendHttp(fd, "200 OK", "text/html; charset=utf-8", html);
        }
      }
      close(fd);
      continue;
    }

    if (!Handshake(fd, req)) {
      close(fd);
      continue;
    }
    std::lock_guard<std::mutex> lock(mu_);
    clients_.push_back(fd);
    LOG(INFO) << "websocket client connected, total=" << clients_.size();
  }
}

}  // namespace vlpweb
