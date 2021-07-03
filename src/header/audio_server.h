#ifndef AUDIO_BROADCAST
#define AUDIO_BROADCAST

#include <chrono>
#include <iostream>

#include <dirent.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

#include "utility.h"
#include "web_server/web_server.h"

#include "../vendor/readerwriterqueue/atomicops.h"
#include "../vendor/readerwriterqueue/readerwriterqueue.h"

#include <liburing.h> //for liburing

constexpr const char *extension_opus = ".opus";

struct audio_info {
  std::string name{};
  std::string artists{}; // comma separated if multiple
  std::string album{};
  std::string album_art_path{};
  std::string release_data{};
};

struct audio_data {
  std::vector<char> audio_data{};

  audio_info audio1_info{};
  // if another piece of audio starts during this interval
  // just include it here at the correct offset
  audio_info audio2_info{};
  float audio2_start_offset{};
};

enum class audio_events { AUDIO_BROADCAST_EVT, FILE_REQUEST, INOTIFY_DIR_CHANGED, AUDIO_LIST, AUDIO_LIST_UPDATE, FILE_READY, AUDIO_REQUEST_FROM_PROGRAM, BROADCAST_TIMER, KILL };

struct audio_req {
  audio_events event{};
  std::vector<char> buff{};
  int fd{};
};

struct audio_file_list_data {
  bool addition{}; // adding it or removing it
  std::vector<std::string> file_list{};
  std::string appropriate_str{}; // either slash separated file list, or just a new file name
  audio_file_list_data(std::string appropriate_str = "", bool addition = false, std::vector<std::string> file_list = {}) : file_list(file_list), appropriate_str(appropriate_str), addition(addition) {}
};

struct file_transfer_data {
  std::string filepath{};
  std::vector<char> data{};
  file_transfer_data(const std::string filepath = "", std::vector<char> &&buff = {}) : filepath(filepath), data(std::move(buff)) {}
};

class audio_server {
  std::vector<std::string> audio_file_paths{};
  std::vector<std::string> audio_list{};
  std::string slash_separated_audio_list{}; // file names are in quotes
  std::set<int> last_10_items{};
  std::deque<audio_data> chunks_of_audio{};

  //
  ////communication between threads////
  //

  //lock free queues used to transport data between threads
  moodycamel::ReaderWriterQueue<audio_file_list_data> audio_file_list_data_queue{};
  moodycamel::ReaderWriterQueue<file_transfer_data> file_transfer_queue{};
  moodycamel::ReaderWriterQueue<std::string> audio_request_queue{};

  std::string audio_server_name{};
  std::string dir_path = "";

  void broadcast_routine();
  void process_audio(file_transfer_data &&data);
  
  std::string currently_processing_audio{}; // the name of the current file being processed - it is blank after processing

  std::chrono::system_clock::time_point current_audio_start_time{};
  std::chrono::system_clock::time_point current_audio_finish_time{};
  std::chrono::system_clock::time_point last_broadcast_time{};

  static std::vector<audio_server*> audio_servers;

  void fd_read_req(int fd, audio_events event, size_t size = sizeof(uint64_t));

  io_uring ring;
public:
  audio_server(std::string dir_path, std::string audio_server_name);
  void run(); // run the audio server

  static void kill_all_servers();
  void kill_server();
  int kill_efd = eventfd(0, 0);

  // these deal with keeping the files in the specified audio file directory syncd with what we've loaded in (so no need to restart the program)
  const int inotify_fd = inotify_init();
  void respond_with_file_list();
  int send_audio_list = eventfd(0, 0); // asks the audio_server to send the audio list
  int notify_audio_list_available = eventfd(0, 0); // tells the program that we've queued some data
  int audio_list_update = eventfd(0, 0); // tell the program there is a change in the audio
  void post_audio_list_update(const bool addition, const std::string &file_name);
  void request_audio_list();
  audio_file_list_data get_from_audio_file_list_data_queue();
  
  // relating to the actual broadcast
  const int timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
  void send_file_request_to_program(const std::string &path);
  const int file_request_fd = eventfd(0, 0);
  file_transfer_data get_from_file_transfer_queue();
  void send_file_back_to_audio_server(const std::string &path, std::vector<char> &&buff);
  void send_audio_request_to_audio_server(const std::string &file_name);
  std::string get_requested_audio();

  // audio_evt_req get_from_to_audio_server_queue(); // so used in the audio server
  // audio_evt_req get_from_to_program_queue(); // so used in the main central web server
  void broadcast_audio_to_program(audio_data &&data);



  const int broadcast_fd = eventfd(0, 0);
  const int file_ready_fd = eventfd(0, 0);
  const int audio_from_program_request_fd = eventfd(0, 0);

  const int send_audio_file = eventfd(0, 0); // notify thread that we want a file
  const int notify_audio_server_file_ready = eventfd(0, 0);
};

#endif