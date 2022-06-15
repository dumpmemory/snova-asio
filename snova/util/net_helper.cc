/*
 *Copyright (c) 2022, yinqiwen <yinqiwen@gmail.com>
 *All rights reserved.
 *
 *Redistribution and use in source and binary forms, with or without
 *modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of rimos nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 *THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 *BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "snova/util/net_helper.h"
#ifdef __linux__
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <array>
#include <memory>

#include "absl/strings/str_split.h"
#include "asio/experimental/as_tuple.hpp"
#include "snova/io/read_until.h"
#include "snova/log/log_macros.h"
#include "spdlog/fmt/fmt.h"

namespace snova {
PaserEndpointResult parse_endpoint(const std::string& addr) {
  std::vector<absl::string_view> host_ports = absl::StrSplit(addr, ':');
  if (host_ports.size() != 2) {
    return PaserEndpointResult{nullptr, std::make_error_code(std::errc::invalid_argument)};
  }
  int port = -1;
  try {
    port = std::stoi(std::string(host_ports[1]));
  } catch (...) {
    return PaserEndpointResult{nullptr, std::make_error_code(std::errc::invalid_argument)};
  }
  absl::string_view host_view = host_ports[0];
  if (host_view.empty()) {
    host_view = "0.0.0.0";
  }
  auto endpoint =
      std::make_unique<::asio::ip::tcp::endpoint>(::asio::ip::make_address(host_view), port);
  return PaserEndpointResult{std::move(endpoint), std::error_code{}};
}

#define SO_ORIGINAL_DST 80
int get_orig_dst(int fd, ::asio::ip::tcp::endpoint& endpoint) {
#ifdef __linux__
  struct sockaddr_in6 serverAddrV6;
  struct sockaddr_in serverAddr;
  int size = sizeof(serverAddr);
  if (getsockopt(fd, SOL_IP, SO_ORIGINAL_DST, &serverAddr, (socklen_t*)&size) >= 0) {
    endpoint = ::asio::ip::tcp::endpoint(
        ::asio::ip::make_address_v4(ntohl(serverAddr.sin_addr.s_addr)), ntohs(serverAddr.sin_port));
    return 0;
  }
  size = sizeof(serverAddrV6);
  if (getsockopt(fd, SOL_IPV6, SO_ORIGINAL_DST, &serverAddrV6, (socklen_t*)&size) >= 0) {
    std::array<uint8_t, 16> v6_ip;
    memcpy(v6_ip.data(), serverAddrV6.sin6_addr.s6_addr, 16);
    endpoint = ::asio::ip::tcp::endpoint(::asio::ip::make_address_v6(v6_ip),
                                         ntohs(serverAddrV6.sin6_port));
    return 0;
  }
#endif
  return -1;
}

asio::awaitable<SocketPtr> get_connected_socket(const std::string& host, uint16_t port,
                                                bool is_tcp) {
  auto ex = co_await asio::this_coro::executor;
  asio::ip::tcp::resolver r(ex);
  std::string port_str = std::to_string(port);
  auto [ec, results] = co_await r.async_resolve(
      host, port_str, ::asio::experimental::as_tuple(::asio::use_awaitable));
  if (ec || results.size() == 0) {
    SNOVA_ERROR("No endpoint found for {}:{} with error:{}", host, port_str, ec);
    co_return nullptr;
  }
  SocketPtr socket = std::make_unique<::asio::ip::tcp::socket>(ex);
  ::asio::ip::tcp::endpoint select_endpoint = *(results.begin());
  auto [connect_ec] = co_await socket->async_connect(
      select_endpoint, ::asio::experimental::as_tuple(::asio::use_awaitable));
  if (connect_ec) {
    SNOVA_ERROR("Connect {} with error:{}", select_endpoint, connect_ec);
    co_return nullptr;
  }
  co_return socket;
}

asio::awaitable<std::error_code> connect_remote_via_http_proxy(
    ::asio::ip::tcp::socket& socket, const ::asio::ip::tcp::endpoint& remote,
    const std::string& proxy_host, uint32_t proxy_port) {
  auto ex = co_await asio::this_coro::executor;
  asio::ip::tcp::resolver r(ex);
  std::string port_str = std::to_string(proxy_port);
  auto [ec, results] = co_await r.async_resolve(
      proxy_host, port_str, ::asio::experimental::as_tuple(::asio::use_awaitable));

  if (ec || results.size() == 0) {
    SNOVA_ERROR("No endpoint found for {}:{} with error:{}", proxy_host, port_str, ec);
    if (ec) {
      co_return ec;
    }
    co_return std::make_error_code(std::errc::bad_address);
  }
  ::asio::ip::tcp::endpoint proxy_endpoint = *(results.begin());
  auto [connect_ec] = co_await socket.async_connect(
      proxy_endpoint, ::asio::experimental::as_tuple(::asio::use_awaitable));
  if (connect_ec) {
    SNOVA_ERROR("Connect {} with error:{}", proxy_host, connect_ec);
    co_return connect_ec;
  }

  std::string conn_req = fmt::format(
      "CONNECT {}:{} HTTP/1.1\r\nHost: {}:{}\r\nConnection: keep-alive\r\nProxy-Connection: "
      "keep-alive\r\n\r\n",
      remote.address().to_string(), remote.port(), remote.address().to_string(), remote.port());
  auto [wec, wn] =
      co_await ::asio::async_write(socket, ::asio::buffer(conn_req.data(), conn_req.size()),
                                   ::asio::experimental::as_tuple(::asio::use_awaitable));
  if (wec) {
    SNOVA_ERROR("Write CONNECT request failed with error:{}", wec);
    co_return wec;
  }
  std::string_view head_end("\r\n\r\n", 4);
  IOBufPtr read_buffer = get_iobuf(kMaxChunkSize);
  size_t readed_len = 0;
  int end_pos = co_await read_until(socket, *read_buffer, readed_len, head_end);
  if (end_pos < 0) {
    SNOVA_ERROR("Failed to recv CONNECT response.");
    co_return std::make_error_code(std::errc::connection_refused);
  }
  std::string_view response_view((const char*)read_buffer->data(), end_pos);
  if (absl::StartsWith(response_view, "HTTP/1.1 2") ||
      absl::StartsWith(response_view, "HTTP/1.0 2")) {
    co_return std::error_code{};
  }
  SNOVA_ERROR("Recv CONNECT response status:{} from http proxy.", response_view);
  co_return std::make_error_code(std::errc::connection_refused);
}

}  // namespace snova
