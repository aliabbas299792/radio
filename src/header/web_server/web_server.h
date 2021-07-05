#ifndef BASIC_WEB_SERVER
#define BASIC_WEB_SERVER

#include "../server.h"
#include "../utility.h"
#include "../callbacks.h"
#include "../data_store.h"

#include "../utility.h"

#include "common_structs_enums.h"
#include "cache.h"

#include "../../vendor/readerwriterqueue/atomicops.h"
#include "../../vendor/readerwriterqueue/readerwriterqueue.h"

#include <thread>

#include <openssl/sha.h>
#include <openssl/evp.h>

using uchar = unsigned char;

using namespace web_cache;

namespace web_server{
  struct receiving_data_info{
    receiving_data_info(int length = -1, std::vector<char> buffer = {}) : length(length), buffer(buffer) {}
    int length = -1;
    std::vector<char> buffer{};
  };

  struct ws_client {
    int currently_writing = 0; //items it is currently writing
    bool close = false; //should this socket be closed
    std::vector<char> websocket_frames{};
    receiving_data_info receiving_data{};
    int id = 0; //in case we use io_uring later
    int client_idx{}; //for the TCP/TLS layer
  };

  struct message_post_data {
    message_post_data(message_type msg_type = message_type::websocket_broadcast, const char *buff_ptr = nullptr, size_t length = 0, int item_idx = 0, uint64_t additional_info = 0, std::string additional_str = "")
       : msg_type(msg_type), buff_ptr(buff_ptr), length(length), item_idx(item_idx), additional_info(additional_info), additional_str(additional_str) {}
    message_type msg_type;
    const char *buff_ptr;
    uint64_t length;
    int item_idx;
    uint64_t additional_info;
    std::string additional_str;
  };

  struct broadcast_data_items {
    const char* buff_ptr{};
    size_t data_len{};
    size_t uses{};
    broadcast_data_items(const char* buff_ptr = nullptr, size_t data_len = -1, uint64_t uses = -1) : buff_ptr(buff_ptr), data_len(data_len), uses(uses) {}
  };

  template<server_type T>
  class basic_web_server{
    //
    ////generally useful functions and variables
    //
    tcp_tls_server::server<T> *tcp_server = nullptr;

    std::string get_content_type(std::string filepath);

    //
    ////websocket stuff////
    //
    
    //reading data from connections
    ulong get_ws_frame_length(const char *buffer); //helper function which reads the websocket header to get the length of the message
    std::pair<int, std::vector<char>> decode_websocket_frame(std::vector<char> &&data); //decodes a single full websocket frame
    std::pair<int, std::vector<std::vector<char>>> get_ws_frames(char *buffer, int length, int ws_client_idx); //gets any full websocket frames possible

    //writing data to connections
    void websocket_write(int ws_client_idx, std::vector<char> &&buff);
    
    //related to opening/closing connections
    std::string get_accept_header_value(std::string input); //gets the appropriate header value from the websocket connection request
    int new_ws_client(int client_idx); //makes a new websocket client
    bool close_ws_connection_req(int ws_client_idx, bool client_already_closed = false); //puts in a request to close this websocket connection
    bool close_ws_connection_potential_confirm(int ws_client_idx); //actually closes the websocket connection (it's sent a close notification)

    //where data about connections is stored
    std::set<int> freed_indexes{}; //set of free indexes for websocket client stuff
    std::vector<ws_client> websocket_clients{};
    
    //
    ////communication between threads////
    //

    //lock free queues used to transport data between threads
    moodycamel::ReaderWriterQueue<message_post_data> to_server_queue{};
    moodycamel::ReaderWriterQueue<message_post_data> to_program_queue{};

  public:
    static std::vector<char> make_ws_frame(const std::string &packet_msg, websocket_non_control_opcodes opcode);
    
    basic_web_server(basic_web_server &&server) = default;
    basic_web_server() {};

    void set_tcp_server(tcp_tls_server::server<T> *tcp_server); //required to be called to ensure pointer to TCP server is present

    void new_tcp_client(int client_idx);
    void kill_client(int client_idx);

    void close_connection(int client_idx);

    std::vector<tcp_client> tcp_clients{}; //storing additional data related to the client_idxs passed to this layer

    //thread stuff
    int memory_release_fd = eventfd(0, 0);
    int new_radio_client_fd = eventfd(0, 0);
    
    std::vector<broadcast_data_items> broadcast_data{}; // data from any broadcasts sent from the program thread

    void post_message_to_server_thread(message_type msg_type, const char *buff_ptr, size_t length, int item_idx, uint64_t additional_info = -1){ //called from the program thread, to notify the server thread
      if(!tcp_server) return; // need this set before posting any messages
      to_server_queue.emplace(msg_type, buff_ptr, length, item_idx, additional_info);
      tcp_server->notify_event();
    }

    void post_memory_release_message_to_program(message_type msg_type, const char *buff_ptr, size_t length, int item_idx, uint64_t additional_info = -1){
      if(!tcp_server) return; // need this stuff set before posting any messages
      to_program_queue.emplace(msg_type, buff_ptr, length, item_idx, additional_info);
      eventfd_write(memory_release_fd, 1); //notify the program thread using our eventfd
    }

    void post_new_radio_client_to_program(std::string station, int ws_client_idx, int ws_client_id){
      if(!tcp_server) return; // need this stuff set before posting any messages
      to_program_queue.emplace(message_type::new_radio_client, nullptr, 0, ws_client_idx, ws_client_id, station); // ws_client_idx is item_idx, and additional_str is used for station
      // we aren't using the buff_ptr and buff_size obviously, so just pass as nullptr and 0
      eventfd_write(new_radio_client_fd, 1); //notify the program thread using our eventfd
    }
    
    message_post_data get_from_to_program_queue(){ // so called from main program thread
      message_post_data data{};
      to_program_queue.try_dequeue(data);
      return data;
    }

    message_post_data get_from_to_server_queue(){ // so called from associated server thread
      message_post_data data{};
      to_server_queue.try_dequeue(data);
      return data;
    }

    //
    ////http public methods
    //

    //responding to get requests
    bool get_process(std::string &path, bool accept_bytes, const std::string& sec_websocket_key, int client_idx);
    //sending files
    bool send_file_request(int client_idx, const std::string &filepath, bool accept_bytes, int response_code);
    //checking if it's a valid HTTP request
    bool is_valid_http_req(const char* buff, int length);
    //the cache
    cache<5> web_cache{}; //cache of 5 items
    
    //
    ////public websocket stuff
    //

    //websocket public methods
    void websocket_process_read_cb(int client_idx, char *buffer, int length);
    bool websocket_process_write_cb(int client_idx); //returns whether or not this was used
    void websocket_accept_read_cb(const std::string& sec_websocket_key, const std::string &path, int client_idx); //used in the read callback to accept web sockets

    //websocket data
    std::unordered_set<int> all_websocket_connections{}; //this is used for the duration of the connection (even after we've sent the close request)
    std::unordered_set<int> active_websocket_connections_client_idxs{}; //this is only active up until we call a close request, has client_idx

    ~basic_web_server(){
      close(web_cache.inotify_fd);
    }
  };

  #include "../../web_server/web_server.tcc"
  #include "../../web_server/websockets.tcc"
}

template<server_type T>
struct server_data;

class audio_server;

// AUDIO_SERVER_COMMUNICATION is a helper enum to distinguish the events from that class, for any other classes, just add a similar enum
enum class central_web_server_event { TIMERFD, READ, WRITE, SERVER_THREAD_COMMUNICATION, AUDIO_SERVER_COMMUNICATION , KILL_SERVER };

struct central_web_server_req {
  central_web_server_event event{};
  std::vector<char> buff{};
  
  const char *buff_ptr{};
  size_t size = -1; // if this is -1, buff_ptr is unused

  size_t progress_bytes{}; // how much has been read/written
  int fd{};

  uint64_t custom_info = -1;
};

class central_web_server {
private:
  std::unordered_map<char*, int> buff_ptr_to_uses_map{};
  
  template<server_type T>
  friend struct server_data;

  static std::unordered_map<std::string, std::string> config_data_map;

  template<server_type T>
  static void thread_server_runner(web_server::basic_web_server<T> &basic_web_server);

  central_web_server() {};

  void run();

  template<server_type T>
  void run();

  int num_threads = 0;

  int event_fd = eventfd(0, 0);
  int kill_server_efd = eventfd(0, 0);
  
  io_uring ring;

  data_store_namespace::data_store store{}; // the data store

  void add_timer_read_req(int timerfd); // adds io_uring read request for the timerfd
  void add_read_req(int fd, size_t size, int custom_info = -1); // adds normal read request on io_uring
  void add_write_req(int fd, const char *buff_ptr, size_t size); // adds normal write request on io_uring

  // to finish off the requests
  void read_req_continued(central_web_server_req *req, size_t last_read);
  void write_req_continued(central_web_server_req *req, size_t written);

  // helper functions to deal with the audio servers
  template<server_type T>
  void audio_server_event_req_handler(int eventfd, int server_id, std::vector<server_data<T>> &thread_data_container);
  void audio_server_read_req_handler(int readfd, int server_id, std::vector<char> &&buff);
  void audio_server_initialise_reads(audio_server &server);
public:
  void start_server(const char *config_file_path);
  void add_event_read_req(int eventfd, central_web_server_event event, uint64_t custom_info = -1); // adds io_uring read request for the eventfd

  central_web_server(central_web_server const&) = delete;
  void operator=(central_web_server const&) = delete;

  static central_web_server& instance(){
    static central_web_server inst;
    return inst;
  }
  
  void kill_server();
};

template<server_type T>
struct server_data {
  std::thread thread{};
  web_server::basic_web_server<T> server{};
  server_data(){
    thread = std::thread(central_web_server::thread_server_runner<T>, std::ref(server));
  }
  server_data(server_data &&data) = default;
  ~server_data() {
    thread.join();
  }
};

#endif