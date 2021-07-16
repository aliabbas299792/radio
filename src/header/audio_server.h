#ifndef AUDIO_BROADCAST
#define AUDIO_BROADCAST

#include <chrono>
#include <iostream>
#include <list>

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

enum class audio_events {
	AUDIO_BROADCAST_EVT, FILE_REQUEST, INOTIFY_DIR_CHANGED, AUDIO_LIST, AUDIO_LIST_UPDATE,
	FILE_READY, AUDIO_REQUEST_FROM_PROGRAM, AUDIO_QUEUE, BROADCAST_TIMER, KILL
};

static struct {
  std::array<float, 4> silkOnly{10, 20, 40, 60};
  std::array<float, 2> hybrid{10, 20};
  std::array<float, 4> celtOnly{2.5, 5, 10, 20};
} audio_frame_durations;

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

struct audio_page_data {
  std::vector<char> buff{};
  time_t duration{};
  audio_page_data(std::vector<char> &&buff, time_t duration) : duration(duration), buff(std::move(buff)) {}
};

struct audio_byte_length_duration {
  time_t duration{};
  size_t byte_length{};
  audio_byte_length_duration(time_t duration, size_t byte_length) : duration(duration), byte_length(byte_length) {}
  audio_byte_length_duration() {}
};

struct audio_chunk {
  int duration{};
  time_t start_offset{};
  time_t total_length{};
  std::string title{};
  std::vector<audio_page_data> pages{};
  bool insert_data(audio_page_data &&page){
    if(duration < BROADCAST_INTERVAL_MS){ // less than BROADCAST_INTERVAL_MS
      pages.push_back(page);
      duration += page.duration;
      return true;
    }
    return false;
  }
};

struct combined_data_chunk {
  std::string audio_data{};
  std::string metdata_only{};
  std::string track_name{};
  combined_data_chunk(std::string &&audio_data, std::string &&metdata_only, std::string track_name) : audio_data{audio_data}, metdata_only{metdata_only}, track_name{track_name} {}
  combined_data_chunk() {}
};

struct audio_req_from_program {
	int client_idx{};
	std::string str_data{}; // either file name or response
  int thread_id{}; // what thread is it from
	audio_req_from_program(std::string str_data, int client_idx, int thread_id) : client_idx(client_idx), str_data(str_data), thread_id(thread_id) {}
	audio_req_from_program() {}
};

struct audio_queue_data {
  int client_idx{};
  int thread_id{};
  std::string file_queue{};
  audio_queue_data() {}
  audio_queue_data(int client_idx, int thread_id, std::string file_queue = "") : file_queue(file_queue), thread_id(thread_id), client_idx(client_idx) {} // file_queue is only used in the response, so optional
};

class audio_server {
  std::unordered_set<std::string> file_set{};

  std::vector<std::string> audio_file_paths{};
  std::vector<std::string> audio_list{};
  std::string slash_separated_audio_list{}; // file names are in quotes
  std::list<int> last_10_items{};
  std::deque<audio_chunk> chunks_of_audio{};

	std::vector<std::string> currently_queued_audio{};
	std::queue<std::string> audio_queue{};

  //
  ////communication between threads////
  //

  //lock free queues used to transport data between threads
  moodycamel::ReaderWriterQueue<audio_file_list_data> audio_file_list_data_queue{};

  moodycamel::ReaderWriterQueue<file_transfer_data> file_transfer_queue{};
	moodycamel::ReaderWriterQueue<file_transfer_data> file_req_transfer_queue{};

	moodycamel::ReaderWriterQueue<audio_req_from_program> audio_request_queue{};
	moodycamel::ReaderWriterQueue<audio_req_from_program> audio_request_response_queue{};

  moodycamel::ReaderWriterQueue<combined_data_chunk> broadcast_queue{}; // the audio data chunk and the metadata only chunk

  std::string audio_server_name{};
  std::string dir_path = "";

  void broadcast_routine();
  void process_audio(file_transfer_data &&data);
  
  std::string currently_processing_audio{}; // the name of the current file being processed - it is blank after processing

  std::chrono::system_clock::time_point current_audio_finish_time{};
  std::chrono::system_clock::time_point current_playback_time{}; // is relative to the actual system clock

  static std::vector<audio_server*> audio_servers;

  void fd_read_req(int fd, audio_events event, size_t size = sizeof(uint64_t));

  io_uring ring;

  int get_config_num(int num); // gets the config number from the number provided
  int get_frame_duration_ms(int config); // uses data in the first byte of each segment in a page to get the duration
  audio_byte_length_duration get_ogg_page_info(char *buff); // gets the ogg page length and duration
  std::vector<audio_page_data> get_audio_page_data(std::vector<char> &&buff); // returns a vector of pointers for the pages along with their durations

  static int max_id;

  static int active_instances;

  std::thread audio_thread{};
  void run(); // run the audio server
public:
  audio_server(audio_server &&server) = default;
  audio_server(std::string audio_server_name, std::string dir_path);
  int id = -1;

  static std::unordered_map<std::string, int> server_id_map;
  static audio_server *instance(int id){ return audio_servers[id]; }
  static void audio_server_req_handler(int server_id, int event_fd);
  static std::vector<std::string> audio_server_names; // the names of the audio server are stored here

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

  std::string get_requested_audio();

  void submit_audio_req(std::string filename, int client_idx, int thread_id);
	void submit_audio_req_response(std::string resp, int client_idx, int thread_id);
  audio_req_from_program get_from_audio_req_queue();
  audio_req_from_program get_from_audio_req_response_queue();
	const int audio_req_fd = eventfd(0, 0); // should push data to the audio_req queue
	const int audio_req_response_fd = eventfd(0, 0); // should push some data to the response queue
  
  void send_file_request_to_program(const std::string &path);
  void send_file_back_to_audio_server(const std::string &path, std::vector<char> &&buff);
	file_transfer_data get_from_file_req_transfer_queue();
  file_transfer_data get_from_file_transfer_queue();
  const int file_request_fd = eventfd(0, 0);
	const int file_ready_fd = eventfd(0, 0);
  
  combined_data_chunk get_broadcast_data();
  void broadcast_to_central_server(std::string &&audio_data, std::string &&metadata_only, std::string track_name);
  const int broadcast_fd = eventfd(0, 0);

  // these are updated via the event loop on the main thread, rather than directly since could cause a data race
  struct {
    std::string slash_separated_audio_list{};
    
    std::unordered_map<int, std::string> fd_to_filepath{};

    std::vector<char> last_broadcast_audio_data{};
    std::vector<char> second_last_broadcast_audio_data{};

    std::vector<char> last_broadcast_metadata_only{};
    std::vector<char> second_last_broadcast_metadata_only{};

    std::deque<std::string> queued_audio{};
    
  } main_thread_state;

  ~audio_server(){ // this will be called as soon as it goes out of scope, unlike the web
    kill_server();
    if(audio_thread.joinable())
      audio_thread.join(); // join its own audio thread
  }
};

#endif