#include "../header/web_server/web_server.h"
#include <chrono>

using namespace web_server;

template <server_type T>
bool basic_web_server<T>::instance_exists = false;

template <server_type T>
basic_web_server<T>::basic_web_server() {
  instance_exists = true;
  utility::set_timerfd_interval(ws_ping_timerfd, WS_PING_INTERVAL); // the ping timer should go off every 30 seconds
};

template <server_type T>
auto basic_web_server<T>::get_process(std::string &path, bool accept_bytes, const std::string &sec_websocket_key, int client_idx, std::string ip) -> bool {
  char *path_temp = strdup(path.c_str());

  char *saveptr = nullptr;
  char *token = strtok_r(path_temp, "/", &saveptr);
  std::string subdir = token != nullptr ? token : "";

  if (subdir == "ws" && !sec_websocket_key.empty()) {
    websocket_accept_read_cb(sec_websocket_key, path.substr(2), client_idx);
    free(path_temp);
    return true;
  }

  // not a websocket, so we deal with the path here

  std::vector<std::string> subdirs{};

  while ((token = strtok_r(nullptr, "/", &saveptr)) != nullptr) {
    subdirs.emplace_back(token);
  }
  free(path_temp);

  if (subdir == "skip_track" && subdirs.size() == 1) {
    post_skip_request_to_program(client_idx, subdirs[0], ip); // so we expect the server to respond with true or false
    return true;
  }

  if (subdir == "audio_list" && subdirs.size() == 1) {
    post_audio_list_req_to_program(client_idx, subdirs[0]); // so we expect the server to respond with the list for this station
    return true;
  }

  if (subdir == "audio_req" && subdirs.size() == 2) {
    post_audio_track_req_to_program(client_idx, subdirs[0], subdirs[1]); // { station, track name } request
    return true;
  }

  if (subdir == "broadcast_metadata" && subdirs.empty()) {
    std::string metadata_str = default_plain_text_http_header;
    metadata_str += "BROADCAST_INTERVAL_MS: ";
    metadata_str += std::to_string(BROADCAST_INTERVAL_MS);
    metadata_str += "\nSTART_TIME_S: ";
    metadata_str += std::to_string(std::chrono::time_point_cast<std::chrono::seconds>(time_start).time_since_epoch().count());

    std::vector<char> metadta{metadata_str.begin(), metadata_str.end()};
    tcp_server->write_connection(client_idx, std::move(metadta));
    return true;
  }

  if (subdir == "station_list" && subdirs.empty()) {
    post_server_list_request_to_program(client_idx);
    return true;
  }

  if (subdir == "audio_queue" && subdirs.size() == 1) {
    post_audio_queue_req_to_program(client_idx, subdirs[0]);
    return true;
  }

  if (subdir == "listen") { // the page can be listen/*, the JS side will negotiate what station to connect to
    path = "";
  }

  path = path.empty() ? "public/index.html" : "public/" + path;

  return static_cast<bool>(send_file_request(client_idx, path, accept_bytes, HTTP_200_OK));
}

template <server_type T>
auto basic_web_server<T>::is_valid_http_req(const char *buff, int length) -> bool {
  if (length < HTTP_MIN_REQUEST_SIZE) {
    return false;
  }

  std::array types{"GET ", "POST ", "PUT ", "DELETE ", "PATCH "};
  constexpr size_t DELETE_LEN = 7; // length of "DELETE " is 7 characters
  constexpr byte UNIT_SEPARATOR = 0x1f;
  byte valid = UNIT_SEPARATOR;

  for (int i = 0; i < DELETE_LEN; i++) {
    for (int j = 0; j < types.size(); j++) { // 5 different types
      if (i < strlen(types[j]) && (((valid >> j) & 0x1) != 0) && types[j][i] != buff[i]) {
        valid &= UNIT_SEPARATOR ^ (1 << j);
      }
    }
  }
  return valid != 0U;
}

template <server_type T>
auto basic_web_server<T>::get_content_type(const std::string &filepath) -> std::string {
  char *file_extension_data = (char *)filepath.c_str();
  std::string file_extension;
  char *saveptr = nullptr;
  while ((file_extension_data = strtok_r(file_extension_data, ".", &saveptr)) != nullptr) {
    file_extension = file_extension_data;
    file_extension_data = nullptr;
  }

  if (file_extension == "html" || file_extension == "htm") {
    return "Content-Type: text/html\r\n";
  }
  if (file_extension == "css") {
    return "Content-Type: text/css\r\n";
  }
  if (file_extension == "js") {
    return "Content-Type: text/javascript\r\n";
  }
  if (file_extension == "opus") {
    return "Content-Type: audio/opus\r\n";
  }
  if (file_extension == "mp3") {
    return "Content-Type: audio/mpeg\r\n";
  }
  if (file_extension == "mp4") {
    return "Content-Type: video/mp4\r\n";
  }
  if (file_extension == "gif") {
    return "Content-Type: image/gif\r\n";
  }
  if (file_extension == "png") {
    return "Content-Type: image/png\r\n";
  }
  if (file_extension == "jpg" || file_extension == "jpeg") {
    return "Content-Type: image/jpeg\r\n";
  }
  if (file_extension == "txt") {
    return "Content-Type: text/plain\r\n";
  }
  if (file_extension == "wasm") {
    return "Content-Type: application/wasm\r\n";
  }
  return "Content-Type: application/octet-stream\r\n";
}

template <server_type T>
auto basic_web_server<T>::send_file_request(int client_idx, const std::string &filepath, bool accept_bytes, int response_code) -> bool {
  const auto file_fd = open(filepath.c_str(), O_RDONLY);

  std::string header_first_line{};
  switch (response_code) {
  case HTTP_200_OK:
    header_first_line = "HTTP/1.0 200 OK\r\n";
    break;
  default:
    header_first_line = "HTTP/1.0 404 Not Found\r\n";
  }

  if (file_fd < 0) {
    return false;
  }

  const auto file_size = utility::get_file_size(file_fd);

  const auto content_length = std::to_string(file_size);
  const auto content_type = get_content_type(filepath);

  const auto cache_data =
      web_cache.fetch_item(filepath, client_idx, tcp_clients[client_idx]);

  std::string headers;
  if (accept_bytes) {
    headers = header_first_line;
    headers += content_type;
    headers += "Accept-Ranges: bytes\r\nContent-Length: ";
    headers += content_length;
    headers += "\r\nRange: bytes=0-";
    headers += content_length;
    headers += "/";
  } else {
    headers = header_first_line;
    headers += content_type;
    headers += "Content-Length: ";
  }
  headers += content_length + "\r\n";
  headers += "Connection: close\r\nKeep-Alive: timeout=0, ";
  headers += "max=0\r\nCache-Control: no-cache, no-store, ";
  headers += "must-revalidate\r\nPragma: no-cache\r\nExpires: 0\r\n";
  headers += "\r\n";

  std::vector<char> send_buffer(file_size + headers.size());

  std::memcpy(&send_buffer[0], headers.c_str(), headers.size());

  if (cache_data.found) {
    tcp_server->write_connection(client_idx, cache_data.buff, cache_data.size);
  } else {
    tcp_clients[client_idx].last_requested_read_filepath = filepath;
    // so that when the file is read, it will be stored with the correct file path
    tcp_server->custom_read_req(file_fd, file_size, true, client_idx, std::move(send_buffer), headers.size()); // true is for using custom_read_req_continued
  }

  return true;
}

template <server_type T>
void basic_web_server<T>::set_tcp_server(tcp_tls_server::server<T> *server) {
  tcp_server = server;
  tcp_server->custom_read_req(web_cache.inotify_fd, inotify_read_size);
  // always read from inotify_fd - we only read size of event, since we monitor files
}

template <server_type T>
void basic_web_server<T>::new_tcp_client(int client_idx) {
  if (client_idx + 1 >= tcp_clients.size()) { // size starts from 1, idx starts from 0
    tcp_clients.resize(client_idx + 1);
  }
  tcp_clients[client_idx] = tcp_client();
}

template <server_type T>
void basic_web_server<T>::kill_client(int client_idx) {
  // be wary of this, I don't think this will cause issues, but maybe it's possible that a new websocket client is at that index already and could be an issue?
  web_cache.finished_with_item(client_idx, tcp_clients[client_idx]);

  int ws_client_idx = tcp_clients[client_idx].ws_client_idx;
  all_websocket_connections.erase(ws_client_idx); // connection definitely closed now

  tcp_clients[client_idx] = tcp_client(); // reset any info about the client

  for (int i = 0; i < broadcast_ws_clients_tcp_client_idxs.size(); i++) {
    if (broadcast_ws_clients_tcp_client_idxs[i].count(client_idx) != 0U) {
      post_radio_client_left_to_server(i); // client has left this channel
      broadcast_ws_clients_tcp_client_idxs[i].erase(client_idx);
    }
  }

  if (active_websocket_connections_client_idxs.count(ws_client_idx) != 0U) {
    active_websocket_connections_client_idxs.erase(client_idx);
    freed_indexes.insert(freed_indexes.end(), ws_client_idx);
  }
}

template <server_type T>
void basic_web_server<T>::close_connection(int client_idx) {
  kill_client(client_idx); // destroy any data related to this request
  tcp_server->start_closing_connection(client_idx);
}

template class web_server::basic_web_server<server_type::TLS>;
template class web_server::basic_web_server<server_type::NON_TLS>;