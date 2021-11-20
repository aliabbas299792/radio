#ifndef SERVER
#define SERVER

#include <liburing.h> //for liburing
#include <memory>
#include <sys/eventfd.h> // for eventfd
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <mutex>
#include <queue>
#include <set>
#include <unordered_set>

#include "server_metadata.h"
#include "utility.h"

class server_base;

struct server_data {
  server_base *server{};
  void *user_data{};
};

using accept_callback = void (*)(server_data data);
using close_callback = void (*)(CLOSE_CB_PARAMS);
using read_callback = void (*)(READ_CB_PARAMS);
using write_callback = void (*)(WRITE_CB_PARAMS);
using event_callback = void (*)(EVENT_CB_PARAMS);
using custom_read_callback = void (*)(CUSTOM_READ_CB_PARAMS);

struct server_callbacks {
};

struct client {
  int id = 0; // id is only used to ensure the connection is unique
  int sockfd = -1;
  std::queue<char> send_data{};
  bool closing_now = false; // marked as true when closing is initiated

  bool read_req_active = false;
  int num_write_reqs = 0; // if this is non zero, then do not proceed with the close callback, wait for other requests to finish
};

struct multi_write {
  //this should be decremented each time you would normally delete this object, when it reaches 0, then delete
  multi_write(std::vector<char> &&buff, int uses) : buff(std::move(buff)), uses(uses) {}
  std::vector<char> buff;
  int uses{};
};

class server_base {
  std::vector<client> clients{};

public:
  virtual void read(int client_idx);
  virtual void write_connection(int client_idx, std::vector<char> &&buff);
  virtual void write_connection(int client_idx, char *buff, size_t length);

  template <typename U>
  void broadcast_message(U begin, U end, int num_clients, std::vector<char> &&buff) {
    if (num_clients > 0) {
      auto *data = new multi_write(std::move(buff), num_clients);

      for (auto client_idx_ptr = begin; client_idx_ptr != end; client_idx_ptr++) {
        auto &client = clients[(int)*client_idx_ptr];
        client.send_data.emplace(data);
        if (client.send_data.size() == 1) { //only adds a write request in the case that the queue was empty before this
          add_write_req(*client_idx_ptr, event_type::WRITE, &(data->buff[0]), data->buff.size());
        }
      }
    }
  }

  template <typename U>
  void broadcast_message(U begin, U end, int num_clients, const char *buff, size_t length, uint64_t custom_info = -1) { //if the buff pointer is ever invalidated, it will just fail to write - so sort of unsafe on its own
    if (num_clients > 0) {
      for (auto client_idx_ptr = begin; client_idx_ptr != end; client_idx_ptr++) {
        auto &client = clients[(int)*client_idx_ptr];
        client.send_data.emplace(buff, length, true, custom_info);
        if (client.send_data.size() == 1) { //only adds a write request in the case that the queue was empty before this
          add_write_req(*client_idx_ptr, event_type::WRITE, buff, length);
        }
      }
    }
  }

  void custom_read(int fd, size_t to_read, bool auto_retry = false, int client_idx = -1, std::vector<char> &&buff = {}, size_t read_amount = 0);
  void notify_event();
  void kill_server();             // will kill the server
  static void kill_all_servers(); // will kill all tls server threads

  void start_closing_connection(int client_idx);  //start closing a connection
  void finish_closing_connection(int client_idx); //finish closing a connection
  void force_close_connection(int client_idx);    //force close a connection
};

class server_nontls : public server_base {
};

class server_tls : public server_base {
};

#endif