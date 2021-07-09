#ifndef COMMON_STRUCTS_ENUMS
#define COMMON_STRUCTS_ENUMS

#include "../server_metadata.h"
#include <string>

namespace web_server {
  template<server_type T>
  class basic_web_server;

  using tls_server = tcp_tls_server::server<server_type::TLS>;
  using plain_server = tcp_tls_server::server<server_type::NON_TLS>;
  using tls_web_server = basic_web_server<server_type::TLS>;
  using plain_web_server = basic_web_server<server_type::NON_TLS>;

  enum websocket_non_control_opcodes {
    text_frame = 0x01,
    binary_frame = 0x02,
    close_connection = 0x08,
    ping = 0x09,
    pong = 0xA
  };

  enum class message_type {
    websocket_broadcast,
    broadcast_finished,
    new_radio_client,
    new_radio_client_response,
    request_audio_list,
    request_audio_list_response,
    request_audio,
    request_audio_response,
    request_station_list,
    request_station_list_response
  };

  struct tcp_client {
    std::string last_requested_read_filepath{}; //the last filepath it was asked to read
    int ws_client_idx{};
    bool using_file = false;
  };
}

#endif