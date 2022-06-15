/*
 *Copyright (c) 2022, qiyingwang <qiyingwang@tencent.com>
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
#include "snova/mux/mux_connection.h"
#include "asio/experimental/as_tuple.hpp"
#include "snova/util/flags.h"
#include "snova/util/misc_helper.h"
#include "spdlog/fmt/bundled/ostream.h"

namespace snova {
static uint32_t g_mux_conn_num = 0;
static uint32_t g_mux_conn_num_in_loop = 0;
size_t MuxConnection::Size() { return g_mux_conn_num; }
size_t MuxConnection::ActiveSize() { return g_mux_conn_num_in_loop; }
MuxConnection::MuxConnection(::asio::ip::tcp::socket&& sock,
                             std::unique_ptr<CipherContext>&& cipher_ctx, bool is_local)
    : socket_(std::move(sock)),
      write_mutex_(socket_.get_executor()),
      cipher_ctx_(std::move(cipher_ctx)),
      client_id_(0),
      idx_(0),
      expire_at_unix_secs_(0),
      last_unmatch_stream_id_(0),
      last_active_unix_secs_(0),
      recv_bytes_(0),
      send_bytes_(0),
      is_local_(is_local),
      is_authed_(false),
      retired_(false) {
  write_buffer_.resize(kMaxChunkSize + kEventHeadSize + kReservedBufferSize);
  read_buffer_.resize(2 * kMaxChunkSize);
  g_mux_conn_num++;
  expire_at_unix_secs_ = (time(nullptr) + g_connection_expire_secs + random_uint64(0, 60));
}
MuxConnection::~MuxConnection() { g_mux_conn_num--; }

asio::awaitable<ServerAuthResult> MuxConnection::ServerAuth() {
  if (is_local_) {
    SNOVA_ERROR("No need to auth connection for client connection.");
    co_return ServerAuthResult{0, false};
  }
  std::unique_ptr<MuxEvent> auth_req;
  int rc = co_await ReadEvent(auth_req);
  if (0 != rc) {
    SNOVA_ERROR(
        "Oops! Read auth request failed with rc:{}, maybe the client cipher config is invalid.",
        rc);
    co_return ServerAuthResult{0, false};
  }
  AuthRequest* auth_req_event = dynamic_cast<AuthRequest*>(auth_req.get());
  if (nullptr == auth_req_event) {
    SNOVA_ERROR("Recv non auth reqeust with type:{}", auth_req->head.type);
    co_return ServerAuthResult{0, false};
  }
  uint64_t iv = random_uint64(0, 102 * 1024 * 1024LL);
  // absl::BitGen bitgen;
  // uint64_t iv = absl::Uniform(bitgen, 0, 102 * 1024 * 1024LL);
  std::unique_ptr<AuthResponse> auth_res = std::make_unique<AuthResponse>();
  auth_res->success = true;
  auth_res->iv = iv;
  bool write_success = co_await WriteEvent(std::move(auth_res));
  if (write_success) {
    cipher_ctx_->UpdateNonce(iv);
  }
  client_id_ = auth_req_event->client_id;
  co_return ServerAuthResult{auth_req_event->client_id, write_success};
}

asio::awaitable<bool> MuxConnection::ClientAuth(const std::string& user, uint64_t client_id) {
  if (!is_local_) {
    SNOVA_ERROR("No need to client auth for server connection.");
    co_return false;
  }
  client_id_ = client_id;
  std::unique_ptr<AuthRequest> auth = std::make_unique<AuthRequest>();
  auth->user = user;
  auth->client_id = client_id;
  bool write_success = co_await WriteEvent(std::move(auth));
  if (!write_success) {
    SNOVA_ERROR("Write auth request failed.");
    co_return false;
  }
  // SNOVA_INFO("Success to send auth request.");
  std::unique_ptr<MuxEvent> auth_res;
  int rc = co_await ReadEvent(auth_res);
  if (0 != rc) {
    SNOVA_ERROR("Oops! Read auth response failed with rc:{}, maybe the cipher config is invalid.",
                rc);
    co_return false;
  }
  AuthResponse* auth_res_event = dynamic_cast<AuthResponse*>(auth_res.get());
  if (nullptr == auth_res_event) {
    SNOVA_ERROR("Recv non auth response with type:{}", auth_res->head.type);
    co_return false;
  }
  bool success = auth_res_event->success;
  if (!success) {
    SNOVA_ERROR("Recv error auth response.");
  } else {
    cipher_ctx_->UpdateNonce(auth_res_event->iv);
    is_authed_ = true;
    // SNOVA_INFO("Success to recv auth response.");
  }
  co_return success;
}
int MuxConnection::ReadEventFromBuffer(std::unique_ptr<MuxEvent>& event, Bytes& buffer) {
  if (buffer.size() == 0) {
    return ERR_NEED_MORE_INPUT_DATA;
  }
  size_t decrypt_len = 0;
  int rc = cipher_ctx_->Decrypt(buffer, event, decrypt_len);
  if (0 == rc) {
    buffer.remove_prefix(decrypt_len);
    return 0;
  }
  if (rc != ERR_NEED_MORE_INPUT_DATA) {
    SNOVA_ERROR("[{}]Failed to read event with rc:{}", idx_, rc);
    return rc;
  }
  if (read_buffer_.data() != buffer.data()) {
    memmove(read_buffer_.data(), buffer.data(), buffer.size());
  }
  return ERR_NEED_MORE_INPUT_DATA;
}
asio::awaitable<int> MuxConnection::ReadEvent(std::unique_ptr<MuxEvent>& event) {
  Bytes current_read_buffer = readable_data_;
  int rc = 0;
  while (true) {
    rc = ReadEventFromBuffer(event, current_read_buffer);
    if (rc == 0) {
      break;
    }
    if (rc != ERR_NEED_MORE_INPUT_DATA) {
      break;
    }
    size_t read_pos = current_read_buffer.size();
    // SNOVA_INFO("start read {} {}.", read_pos, read_buffer_.size() - read_pos);
    auto [ec, n] = co_await socket_.async_read_some(
        ::asio::buffer(read_buffer_.data() + read_pos, read_buffer_.size() - read_pos),
        ::asio::experimental::as_tuple(::asio::use_awaitable));
    if (ec) {
      SNOVA_ERROR("Failed to read event with error:{}", ec);
      rc = ec.value();
      break;
    }
    if (0 == n) {  // EOF
      SNOVA_ERROR("Failed to read event with EOF.");
      rc = ERR_READ_EOF;
      break;
    }
    recv_bytes_ += n;
    // SNOVA_INFO("Read {} bytes.", n);
    // readable_data_ = Bytes{read_buffer_.data(), read_pos + n};
    current_read_buffer = absl::MakeSpan(read_buffer_.data(), read_pos + n);
  }
  readable_data_ = current_read_buffer;
  co_return rc;
}

asio::awaitable<int> MuxConnection::ProcessReadEvent() {
  std::unique_ptr<MuxEvent> event;
  int rc = co_await ReadEvent(event);
  if (0 != rc) {
    co_return rc;
  }
  switch (event->head.type) {
    case EVENT_RETIRE_CONN_REQ: {
      SNOVA_INFO("[{}]Recv retired request.", idx_);
      retired_ = true;
      if (retire_callback_) {
        retire_callback_(this);
      }
      break;
    }
    case EVENT_STREAM_OPEN: {
      StreamOpenRequest* open_request = dynamic_cast<StreamOpenRequest*>(event.get());
      if (nullptr == open_request) {
        SNOVA_ERROR("null request for EVENT_STREAM_OPEN");
        co_return - 1;
      }
      // SNOVA_INFO("[{}]Recv stream open with {}:{}, tcp:{}", event->head.sid,
      //            open_request->remote_host, open_request->remote_port, open_request->is_tcp);
      if (server_relay_) {
        auto ex = co_await asio::this_coro::executor;
        ::asio::co_spawn(ex, server_relay_(client_id_, std::move(event)), ::asio::detached);
      } else {
        SNOVA_ERROR("No server relay fun to handle EVENT_STREAM_OPEN");
      }
      break;
    }
    case EVENT_STREAM_CLOSE: {
      SNOVA_INFO("[{}][{}]Recv stream close event.", idx_, event->head.sid);
      MuxStreamPtr stream = MuxStream::Get(client_id_, event->head.sid);
      if (!stream) {
        // SNOVA_ERROR("[{}][{}]No stream found to close.", idx_, event->head.sid);
      } else {
        co_await stream->Close(true);
      }
      break;
    }
    case EVENT_STREAM_CHUNK: {
      MuxStreamPtr stream = MuxStream::Get(client_id_, event->head.sid);
      if (!stream) {
        if (last_unmatch_stream_id_ != event->head.sid) {
          SNOVA_ERROR("[{}][{}]No stream found to handle chunk with len:{}", idx_, event->head.sid,
                      event->head.len);
          SNOVA_ERROR("[{}][{}]Force close remote stream.", idx_, event->head.sid);
          last_unmatch_stream_id_ = event->head.sid;
          auto close_ev = std::make_unique<StreamCloseRequest>();
          close_ev->head.sid = event->head.sid;
          co_await WriteEvent(std::move(close_ev));
        }
      } else {
        StreamChunk* chunk = dynamic_cast<StreamChunk*>(event.get());
        if (nullptr == chunk) {
          SNOVA_ERROR("null chunk for EVENT_STREAM_CHUNK");
          co_return - 1;
        }
        co_await stream->Offer(std::move(chunk->chunk), chunk->chunk_len);
      }
      break;
    }
    default: {
      SNOVA_ERROR("[{}][{}]Unknown event type:{}", idx_, event->head.sid, event->head.type);
      break;
    }
  }
  // handle event
  co_return 0;
}

asio::awaitable<void> MuxConnection::ReadEventLoop() {
  g_mux_conn_num_in_loop++;
  while (true) {
    int rc = co_await ProcessReadEvent();
    if (0 != rc && ERR_NEED_MORE_INPUT_DATA != rc) {
      break;
    }
    last_active_unix_secs_ = time(nullptr);
  }
  Close();
  g_mux_conn_num_in_loop--;
  SNOVA_INFO("Close mux connection:{}, retired:{}", idx_, retired_);
}

void MuxConnection::Close() {
  socket_.close();
  write_mutex_.Close();
}

asio::awaitable<bool> MuxConnection::Write(std::unique_ptr<MuxEvent>&& write_ev) {
  // SNOVA_INFO("[{}]Write event:{}", write_ev->head.sid, write_ev->head.type);
  // is_writing_ = true;
  co_await write_mutex_.Lock();
  last_active_unix_secs_ = time(nullptr);
  MutableBytes wbuffer(write_buffer_.data(), write_buffer_.size());
  int rc = cipher_ctx_->Encrypt(write_ev, wbuffer);
  if (0 != rc) {
    SNOVA_ERROR("Encrypt event request:{} with rc:{}", write_ev->head.type, rc);
    co_await write_mutex_.Unlock();
    co_return false;
  }
  auto [ec, n] =
      co_await ::asio::async_write(socket_, ::asio::buffer(wbuffer.data(), wbuffer.size()),
                                   ::asio::experimental::as_tuple(::asio::use_awaitable));
  co_await write_mutex_.Unlock();
  // is_writing_ = false;
  if (ec) {
    SNOVA_ERROR("Write event:{} failed with error:{}", write_ev->head.type, ec);
    co_return false;
  }
  if (n != wbuffer.size()) {
    SNOVA_ERROR("###Expected write {}bytes, but only {} writed.", wbuffer.size(), n);
  }
  send_bytes_ += wbuffer.size();
  co_return true;
}

}  // namespace snova
