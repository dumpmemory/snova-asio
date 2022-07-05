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

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include "asio.hpp"
#include "snova/io/io.h"
#include "snova/mux/mux_event.h"
#include "snova/mux/mux_stream.h"

namespace snova {

struct RelayContext {
  std::string user;
  std::string remote_host;
  uint16_t remote_port = 0;
  bool is_tls = false;
  bool is_tcp = false;
  bool direct = false;
};

asio::awaitable<void> relay_direct(::asio::ip::tcp::socket&& sock, const Bytes& readed_data,
                                   RelayContext& relay_ctx);
asio::awaitable<void> relay(::asio::ip::tcp::socket&& sock, const Bytes& readed_data,
                            RelayContext& relay_ctx);
asio::awaitable<void> relay(StreamPtr stream, const Bytes& readed_data, RelayContext& relay_ctx);

asio::awaitable<void> relay_handler(const std::string& user, uint64_t client_id,
                                    std::unique_ptr<MuxEvent>&& open_request);
}  // namespace snova
