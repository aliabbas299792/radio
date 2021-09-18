#include "../header/audio_server.h"
#include "../header/web_server/web_server.h"
#include <chrono>
#include <algorithm>
#include <sys/eventfd.h>
#include "../vendor/json/single_include/nlohmann/json.hpp"

using json = nlohmann::json;

// initialise static variable
std::vector<audio_server*> audio_server::audio_servers{};
int audio_server::max_id = 0;
std::unordered_map<std::string, int> audio_server::server_id_map{};
int audio_server::active_instances = 0;

audio_server::audio_server(std::string name, std::string dir_path){ // not thread safe
  if(web_server::basic_web_server<server_type::TLS>::instance_exists || web_server::basic_web_server<server_type::NON_TLS>::instance_exists)
    utility::fatal_error("Audio servers must be initialised before web servers"); // self explanatory

  audio_server_name = utility::to_web_name(name); // only uses the web safe name
  id = max_id++; // (also acts as an index into the vector below)
  audio_servers.push_back(this); // push to static vector
  server_id_map[audio_server_name] = id;

  active_instances++; // used for shutdown

  dir_path = (dir_path.find_last_of("/") == dir_path.size() - 1) ? dir_path : dir_path + "/"; // ensures there's a slash at the end
  this->dir_path = dir_path;

  DIR *audio_directory_ptr = opendir(dir_path.c_str());

  if(audio_directory_ptr == nullptr){
    utility::log_helper_function(std::string(__func__) + " ## " + std::to_string(__LINE__) + " ## " + std::string(__FILE__) + " ## Dir: " + dir_path, true);
    utility::fatal_error("Couldn't open directory");
  }
    
  dirent *file_data_ptr{};

  char *save_ptr{}; //for strtok_r

  bool first_loop = true;
  while((file_data_ptr = readdir(audio_directory_ptr)) != nullptr){
    auto file_name_ptr = file_data_ptr->d_name;
		std::string filename = file_name_ptr;

    if(filename.size() > 5 && filename.substr(filename.size()-5, filename.size()) == ".opus"){
      filename = filename.substr(0, filename.size() - 5);

      audio_file_paths.push_back(dir_path + filename + ".opus"); // saves all the audio file paths

      audio_list.push_back(filename);
      file_set.insert(filename);

      if(first_loop){
        slash_separated_audio_list += filename;
        first_loop = false;
      }else{
        slash_separated_audio_list += "/" + filename;
      }
    }
  }

  audio_thread = std::thread([this] {
    this->run(); // run the audio server in another thread
  });
}

void audio_server::run(){
  // io_uring
  io_uring_cqe *cqe;
  std::memset(&ring, 0, sizeof(io_uring));
  io_uring_queue_init(QUEUE_DEPTH, &ring, 0); //no flags, setup the queue
  
  // making sure it exits cleanly
  fd_read_req(kill_efd, audio_events::KILL);

  // watching files/updating the server to reflect the directory correctly
  inotify_add_watch(inotify_fd, dir_path.c_str(), IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO); // watch the directory for changes
  fd_read_req(inotify_fd, audio_events::INOTIFY_DIR_CHANGED, inotify_read_size);

  fd_read_req(send_audio_list, audio_events::AUDIO_LIST);
  fd_read_req(file_ready_fd, audio_events::FILE_READY);

  fd_read_req(request_skip_fd, audio_events::REQUEST_SKIP);

  // time stuff
  utility::set_timerfd_interval(timerfd, BROADCAST_INTERVAL_MS);
  fd_read_req(timerfd, audio_events::BROADCAST_TIMER);

	fd_read_req(audio_req_fd, audio_events::AUDIO_REQUEST_FROM_PROGRAM);

  current_audio_finish_time = std::chrono::system_clock::now();
  current_playback_time = current_audio_finish_time;

  char *strtok_saveptr{};

  bool run_server = true;
  while(run_server){
    char ret = io_uring_wait_cqe(&ring, &cqe);

    // std::cout << "event " << errno << "\n";

    if(ret < 0)
      break;
    
    auto *req = reinterpret_cast<audio_req*>(cqe->user_data);
    switch(req->event){
      case audio_events::FILE_READY: {
        auto file_ready_data = get_from_file_transfer_queue();
        std::cout << "File loaded: " << file_ready_data.filepath << std::endl;
        process_audio(std::move(file_ready_data));
        fd_read_req(file_ready_fd, audio_events::FILE_READY);
        break;
      }
      case audio_events::KILL:
        run_server = false;
        break;
      case audio_events::AUDIO_LIST:
        respond_with_file_list();
        fd_read_req(send_audio_list, audio_events::AUDIO_LIST);
        break;
      case audio_events::BROADCAST_TIMER:
        broadcast_routine();

        fd_read_req(timerfd, audio_events::BROADCAST_TIMER);
        break;
      case audio_events::REQUEST_SKIP: {
        auto data = get_request_to_skip_data();

        if(request_skip_ips.count(data.ip)){
          respond_to_request_to_skip(false, data.client_idx, data.thread_id);
        }else{
          request_skip_ips.insert(data.ip);



          respond_to_request_to_skip(true, data.client_idx, data.thread_id);
        }

        fd_read_req(request_skip_fd, audio_events::REQUEST_SKIP);
        break;
      }
  		case audio_events::INOTIFY_DIR_CHANGED: {

        // std::cout << "event dir changed " << errno << "\n";
        auto &buff = req->buff;
        int event_name_length = 0;

        int evs = 0;
        for(char *ptr = &buff[0]; ptr < &buff[0] + cqe->res; ptr += sizeof(inotify_event) + event_name_length){ // loop over all inotify events
          evs++;
          auto data = reinterpret_cast<inotify_event*>(ptr);
          event_name_length = data->len; // updates the amount to increment each time
          auto file_name = data->name;

          std::string filename = file_name;

					// if the filename is not *.opus then just continue to the next inotify event (if there is one)
					if(filename.size() <= 5 || filename.substr(filename.size()-5, filename.size()) != ".opus")
						continue;

					filename = filename.substr(0, filename.size()-5); // remove the .opus extension

          if(data->mask & IN_CREATE || data->mask & IN_MOVED_TO){
            slash_separated_audio_list += "/" + filename;
            audio_list.push_back(filename);
            post_audio_list_update(true, filename); // we've added a file
            file_set.insert(filename);
          }else{
            audio_list.erase(std::remove(audio_list.begin(), audio_list.end(), filename), audio_list.end()); // remove the deleted file
            slash_separated_audio_list = utility::remove_from_slash_string(slash_separated_audio_list, filename);
            post_audio_list_update(false, filename); // we've removed a file
            file_set.erase(filename);
          }
        }

        // std::cout << slash_separated_audio_list << " ## " << evs << " ## is audio list\n";

        fd_read_req(inotify_fd, audio_events::INOTIFY_DIR_CHANGED, inotify_read_size); // guaranteed enough for atleast 1 event
        break;
			}
		  case audio_events::AUDIO_REQUEST_FROM_PROGRAM: {
				audio_req_from_program req = get_from_audio_req_queue();

        // only allow something to be queued if it hasn't been queued already
				if(file_set.count(req.str_data) && std::find(currently_queued_audio.begin(), currently_queued_audio.end(), req.str_data) == currently_queued_audio.end()){
          audio_queue.push(req.str_data);
          currently_queued_audio.push_back(req.str_data);

          submit_audio_req_response(req.str_data, req.client_idx, req.thread_id); // literally just sends back the title
        }else{
          submit_audio_req_response("//FAILURE", req.client_idx, req.thread_id); // putting // at the beginning since a title can't have a / character
        }

				fd_read_req(audio_req_fd, audio_events::AUDIO_REQUEST_FROM_PROGRAM); //rearm the fd
			  break;
	  	}
    }
    
    delete req;
    io_uring_cqe_seen(&ring, cqe); //mark this CQE as seen
  }

  close(kill_efd);
  close(broadcast_fd);
  close(file_ready_fd);
  close(file_request_fd);
  close(inotify_fd);
  close(send_audio_list);
  close(notify_audio_list_available);
  close(audio_list_update);
  close(timerfd);
  io_uring_queue_exit(&ring);
}

combined_data_chunk audio_server::get_broadcast_data(){
  combined_data_chunk output{};
  broadcast_queue.try_dequeue(output);
  return output;
}

void audio_server::broadcast_routine(){
  if(currently_processing_audio == "" && std::chrono::system_clock::now() >= current_audio_finish_time - std::chrono::milliseconds(BROADCAST_INTERVAL_MS)){
    // if there are less than BROADCAST_INTERVAL_MS long till the end of this file, and nothing is currently being
    currently_processing_audio = get_requested_audio();
    if(currently_processing_audio == ""){ // if there was nothing in the requested queue
      if(audio_list.size() == 0)
        utility::fatal_error("There are no opus files in the audio directory " + dir_path);
      if(audio_list.size() < 10)
        currently_processing_audio = audio_list[utility::random_number(0, audio_list.size()-1)]; // select a file at random
      else{
        // a very rough way of making sure you don't get repeats too often - probably won't get stuck for a long time, not likely
        auto idx = utility::random_number(0, audio_list.size()-1); // select an index which hasn't been used for the previous 10 items
        while(std::find(last_10_items.begin(), last_10_items.end(), idx) != last_10_items.end()) // make sure element isn't in the linked list
          idx = utility::random_number(0, audio_list.size()-1);

        if(last_10_items.size() == 10) // only do this if there are already 10 elements in there=
          last_10_items.pop_back(); // removes the last element of the list
        last_10_items.push_front(idx); // and add this to the exclusion list (at the beginning)

        currently_processing_audio = audio_list[idx];
      }
    }
    // at this point currently_processing_audio is definitely not blank
    send_file_request_to_program(dir_path + currently_processing_audio + ".opus"); // requests a file to be read
  }

  // broadcast data here
  // just get the oldest chunk with chunks_of_audio.pop_front() and broadcast it, as audio_data
  if(chunks_of_audio.size()){
    auto chunk = chunks_of_audio.front();
    chunks_of_audio.pop_front();

    json data_pages{};
    for(const auto &page : chunk.pages){
      json data_page{};
      data_page["duration"] = page.duration;
      data_page["buff"] = page.buff;
      data_pages.push_back(data_page);
    }

    json data_chunk{};
    data_chunk["duration"] = chunk.duration;
    data_chunk["pages"] = data_pages;
    data_chunk["start_offset"] = chunk.start_offset;

    current_playback_time += std::chrono::milliseconds(chunk.duration); // increase it with each broadcast
    
    json metadata_only_chunk{};
    metadata_only_chunk["duration"] = chunk.duration;
    metadata_only_chunk["title"] = chunk.title;
    metadata_only_chunk["start_offset"] = chunk.start_offset;
    metadata_only_chunk["total_length"] = chunk.total_length;
    broadcast_to_central_server(data_chunk.dump(), metadata_only_chunk.dump(), chunk.title);
  }
}

void audio_server::broadcast_to_central_server(std::string &&audio_data, std::string &&metadata_only, std::string track_name){
  broadcast_queue.emplace(std::move(audio_data), std::move(metadata_only), track_name);
  eventfd_write(broadcast_fd, 1);
}

int audio_server::get_config_num(int num){
  return (num >> 3) & 31;
}

int audio_server::get_frame_duration_ms(int config) {
  if (config <= 11) {
    const auto normalisedConfig = config % 4;
    return audio_frame_durations.silkOnly[normalisedConfig];
  }
  else if (config <= 15) {
    const auto normalisedConfig = config % 2;
    return audio_frame_durations.hybrid[normalisedConfig];
  }
  else {
    const auto normalisedConfig = config % 4;
    return audio_frame_durations.celtOnly[normalisedConfig];
  }
}

audio_byte_length_duration audio_server::get_ogg_page_info(char *buff){ // gets the ogg page length and duration
  uint8_t *segments_table = reinterpret_cast<uint8_t*>(&buff[27]);
  uint8_t segments_table_length = buff[26];
  uint32_t segments_total_length{};
  uint8_t *segments = reinterpret_cast<uint8_t*>(&buff[27 + segments_table_length]);

  time_t length_ms{};
  int16_t last_length_extended_page_segment = -1;

  for(int i = 0; i < segments_table_length; i++){
    if(segments_table[i] == 255){
      last_length_extended_page_segment = get_frame_duration_ms(get_config_num(segments[segments_total_length]));
    }else if(last_length_extended_page_segment != -1){
      length_ms += last_length_extended_page_segment;
      last_length_extended_page_segment = -1;
    }else{
      length_ms += get_frame_duration_ms(get_config_num(segments[segments_total_length]));
    }

    segments_total_length += segments_table[i];
  }
  
  return { length_ms, 27 + segments_total_length + segments_table_length };
}

std::vector<audio_page_data> audio_server::get_audio_page_data(std::vector<char> &&buff){
  std::vector<audio_page_data> page_vec{};

  int read_head = 0;
  int iter_num = 0;
  time_t duration = 0;
  while(read_head < buff.size()){
    auto data = get_ogg_page_info(&buff[read_head]);

    if(iter_num >= 2){
      page_vec.push_back({ std::vector<char>(&buff[read_head], &buff[read_head] + data.byte_length), data.duration });
      duration += data.duration;
    }else{
      iter_num++;
    }

    read_head += data.byte_length;
  }

  return page_vec;
}

void audio_server::process_audio(file_transfer_data &&data){
  //         _______      _______       _______
  // (back) |   1   | -> |   2   | ... |   N   | (front)
  //         -------      -------       -------
  // chunks_of_audio.push_back(NEW_CHUNK_OF_AUDIO)
  // chunks_of_audio.pop_back() to get the most recently pushed chunk
  // chunks_of_audio.pop_front() to get the audio to broadcast

  // firstly get the most recent chunk from chunks_of_audio.pop_back(), get its length and see if you can append some on to the end of it to get a BROADCAST_INTERVAL_MS long chunk
  // then push that chunk, followed by the rest of the chunks
  auto audio_data = get_audio_page_data(std::move(data.data));

  int audio_data_idx = 0;
  
  std::string filename = "";
  char *save_ptr{};
  char *path_dup = strdup(data.filepath.c_str());
  char *path_dup_original = path_dup;
  char *path_part{};
  while((path_part = strtok_r(path_dup, "/", &save_ptr)) != nullptr){
    path_dup = nullptr;
    filename = path_part;
  }
  free(path_dup_original);
  filename = filename.substr(0, filename.size() - 5); // we know the extension is .opus, so we remove the last 5 characters

  bool is_chunks_of_audio_empty = chunks_of_audio.size() == 0;

  uint64_t duration_of_audio = 0;
  for(const auto& page : audio_data)
    duration_of_audio += page.duration;

  int chunk_num = 0;
  while(audio_data_idx < audio_data.size()){
    audio_chunk chunk{};
    chunk.start_offset = chunk_num * BROADCAST_INTERVAL_MS; // offset will always be a multiple of BROADCAST_INTERVAL_MS
    chunk_num++;

    chunk.total_length = duration_of_audio;

    chunk.title = filename; // set the title
    while(chunk.insert_data(std::move(audio_data[audio_data_idx++]))){ // fill the chunk up as much as possible
      if(audio_data_idx == audio_data.size()) break; // don't want to segfault by overstepping the boundaries
    }
    if(audio_data_idx != audio_data.size())
      audio_data_idx--; // the reason that while loop broke was because the chunk was filled, so lower the idx again

    if(audio_data_idx == audio_data.size() && chunk.duration < BROADCAST_INTERVAL_MS){ // if the last chunk is too small, concatenate the last 2
      auto &last_chunk = chunks_of_audio.back();
      last_chunk.pages.insert(last_chunk.pages.end(), chunk.pages.begin(), chunk.pages.end());
      last_chunk.duration += chunk.duration;
    }else
      chunks_of_audio.push_back(chunk);
  }

  std::cout << "" << std::floor(float(duration_of_audio)/60000.0) << "m " << std::round((float(duration_of_audio)/60000.0 - std::floor(float(duration_of_audio)/60000.0))*60) << "s is the duration" << std::endl;

  current_audio_finish_time += std::chrono::milliseconds(duration_of_audio);
  currently_processing_audio = ""; // we've processed it

  broadcast_routine();

  if(is_chunks_of_audio_empty)
    broadcast_routine(); // extra broadcast routine if there is not enough data
}

void audio_server::respond_with_file_list(){
  audio_file_list_data_queue.emplace(slash_separated_audio_list, true, audio_list); // we are updating with a new list and stuff
  eventfd_write(notify_audio_list_available, 1); // notify the main thread we've pushed something
}

void audio_server::post_audio_list_update(const bool addition, const std::string &file_name){
  audio_file_list_data_queue.emplace(file_name, addition); // either adding or removing a file
  eventfd_write(audio_list_update, 1); // notify the main thread that we've pushed an update
}

audio_file_list_data audio_server::get_from_audio_file_list_data_queue(){
  audio_file_list_data data{};
  audio_file_list_data_queue.try_dequeue(data);
  return data;
}

void audio_server::fd_read_req(int fd, audio_events event, size_t size){
  io_uring_sqe *sqe = io_uring_get_sqe(&ring); //get a valid SQE (correct index and all)
  auto *req = new audio_req(); //enough space for the request struct
  req->buff.resize(size);
  req->event = event;
  req->fd = fd;
  
  io_uring_prep_read(sqe, fd, &(req->buff[0]), size, 0); //don't read at an offset
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring);
}

void audio_server::kill_server(){
  eventfd_write(kill_efd, 1); // kill the server
}

void audio_server::send_file_request_to_program(const std::string &path){
  file_req_transfer_queue.emplace(path);
  eventfd_write(file_request_fd, 1); // notify the main program thread
}

void audio_server::request_audio_list(){
  eventfd_write(send_audio_list, 1); // ask for the list
}

file_transfer_data audio_server::get_from_file_req_transfer_queue(){
  file_transfer_data data{};
  file_req_transfer_queue.try_dequeue(data);
  return data;
}

file_transfer_data audio_server::get_from_file_transfer_queue(){
	file_transfer_data data{};
	file_transfer_queue.try_dequeue(data);
	return data;
}

void audio_server::send_file_back_to_audio_server(const std::string &path, std::vector<char> &&buff){
  file_transfer_queue.emplace(path, std::move(buff));
  eventfd_write(file_ready_fd, 1); // the file is ready to be received on the audio thread
}

std::string audio_server::get_requested_audio(){ // returns an empty string if the queue is empty
  if(audio_queue.size() == 0)
    return "";

  const std::string audio = audio_queue.front();
  audio_queue.pop();

  currently_queued_audio.erase(std::remove(currently_queued_audio.begin(), currently_queued_audio.end(), audio), currently_queued_audio.end());

  if(file_set.count(audio)) // i.e if the file has been erased
    return audio;
  return "";
}

void audio_server::submit_audio_req(std::string filename, int client_idx, int thread_id){ // returns an empty string if the queue is empty
  audio_request_queue.emplace(filename, client_idx, thread_id);
	eventfd_write(audio_req_fd, 1);
}

void audio_server::submit_audio_req_response(std::string resp, int client_idx, int thread_id){
	audio_request_response_queue.emplace(resp, client_idx, thread_id);
	eventfd_write(audio_req_response_fd, 1);
}

audio_req_from_program audio_server::get_from_audio_req_queue(){
  audio_req_from_program data{};
  audio_request_queue.try_dequeue(data);
  return data;
}

audio_req_from_program audio_server::get_from_audio_req_response_queue(){
  audio_req_from_program data{};
  audio_request_response_queue.try_dequeue(data);
  return data;
}

void audio_server::send_request_to_skip_to_audio_server(const std::string &ip, int client_idx, int thread_id) {
	request_to_skip_queue.emplace(client_idx, thread_id, ip);
  eventfd_write(request_skip_fd, 1);
}

void audio_server::respond_to_request_to_skip(bool success, int client_idx, int thread_id){
	request_to_skip_response_queue.emplace(client_idx, thread_id, "", success);
	eventfd_write(request_skip_response_fd, 1);
}

request_skip_data audio_server::get_request_to_skip_data(){
  request_skip_data data{};
  request_to_skip_queue.try_dequeue(data);
  return data;
}

request_skip_data audio_server::get_request_to_skip_response_data(){
  request_skip_data data{};
  request_to_skip_response_queue.try_dequeue(data);
  return data;
}

template class web_server::basic_web_server<server_type::TLS>;
template class web_server::basic_web_server<server_type::NON_TLS>;