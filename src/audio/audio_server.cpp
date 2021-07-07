#include "../header/audio_server.h"
#include <chrono>
#include <algorithm>
#include <regex>
#include "../vendor/json/single_include/nlohmann/json.hpp"

using json = nlohmann::json;

// initialise static variable
std::vector<audio_server*> audio_server::audio_servers{};
int audio_server::max_id = 0;
std::unordered_map<std::string, int> audio_server::server_id_map{};

audio_server::audio_server(std::string dir_path, std::string name){ // not thread safe
  audio_server_name = utility::to_web_name(name); // only uses the web safe name
  id = max_id++; // (also acts as an index into the vector below)
  audio_servers.push_back(this); // push to static vector
  server_id_map[audio_server_name] = id;

  dir_path = (dir_path.find_last_of("/") == dir_path.size() - 1) ? dir_path : dir_path + "/"; // ensures there's a slash at the end
  this->dir_path = dir_path;

  DIR *music_directory_ptr = opendir(dir_path.c_str());

  if(music_directory_ptr == nullptr)
    utility::fatal_error("Couldn't open directory");
  
  dirent *file_data_ptr{};

  char *save_ptr{}; //for strtok_r

  bool first_loop = true;
  while((file_data_ptr = readdir(music_directory_ptr)) != nullptr){
    auto file_name = file_data_ptr->d_name;
    auto original_file_name = file_name;

    const char *file_extension{};
    const char *tmp_file_extension{};
    while((tmp_file_extension = strtok_r(file_name, ".", &save_ptr)) != nullptr){
      file_name = nullptr;
      file_extension = tmp_file_extension;
    }
    
    if(file_extension != nullptr && strcmp(file_extension, "opus") == 0){
      audio_file_paths.push_back(dir_path + std::string(original_file_name) + ".opus"); // saves all the audio file paths

      audio_list.push_back(original_file_name);
      if(first_loop){
        slash_separated_audio_list += std::string(original_file_name);
        first_loop = false;
      }else{
        slash_separated_audio_list += "/" + std::string(original_file_name);
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

  // time stuff
  utility::set_timerfd_interval(timerfd, BROADCAST_INTERVAL_MS);
  fd_read_req(timerfd, audio_events::BROADCAST_TIMER);

  current_audio_finish_time = std::chrono::system_clock::now();
  current_playback_time = current_audio_finish_time;

  char *strtok_saveptr{};

  bool run_server = true;
  while(run_server){
    char ret = io_uring_wait_cqe(&ring, &cqe);

    if(ret < 0)
      break;
    
    auto *req = reinterpret_cast<audio_req*>(cqe->user_data);

    switch(req->event){
      case audio_events::AUDIO_REQUEST_FROM_PROGRAM:
        // req->data.audio_request_from_program_filepath;
        break;
      case audio_events::FILE_READY: {
        auto file_ready_data = get_from_file_transfer_queue();
        std::cout << "FILE IS READY woohoo " << file_ready_data.filepath << "\n";
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
        // std::cout << "it a broadcast timer " << utility::random_number(0, audio_list.size()+100) << "\n";
        broadcast_routine();
        // send_file_request_to_program(dir_path + "audionautix-trippin-coffee.opus");
        fd_read_req(timerfd, audio_events::BROADCAST_TIMER);
        break;
      case audio_events::INOTIFY_DIR_CHANGED:
        auto &buff = req->buff;
        int event_name_length = 0;
        for(char *ptr = &buff[0]; ptr < &buff[0] + cqe->res; ptr += sizeof(inotify_event) + event_name_length){ // loop over all inotify events
          auto data = reinterpret_cast<inotify_event*>(ptr);
          event_name_length = data->len; // updates the amount to increment each time
          auto file_name = data->name;
          auto original_file_name = file_name;

          const char *file_extension{};
          const char *tmp_file_extension{};
          while((tmp_file_extension = strtok_r(file_name, ".", &strtok_saveptr)) != nullptr){
            file_name = nullptr;
            file_extension = tmp_file_extension;
          }

          std::string file_name_str = std::string(original_file_name);

          std::cout <<file_name_str << "\n";

          if(data->mask & IN_CREATE){
            slash_separated_audio_list += "/" + file_name_str;
            audio_list.push_back(file_name_str);
            post_audio_list_update(true, file_name_str); // we've added a file
          }else{
            audio_list.erase(std::remove(audio_list.begin(), audio_list.end(), file_name_str), audio_list.end()); // remove the deleted file
            slash_separated_audio_list = utility::remove_from_slash_string(slash_separated_audio_list, file_name_str);
            post_audio_list_update(false, file_name_str); // we've removed a file
          }
        }

        fd_read_req(inotify_fd, audio_events::INOTIFY_DIR_CHANGED, inotify_read_size); // guaranteed enough for atleast 1 event
        break;
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

std::string audio_server::get_broadcast_audio(){
  std::string output{};
  broadcast_queue.try_dequeue(output);
  return output;
}

void audio_server::broadcast_routine(){
  if(currently_processing_audio == "" && std::chrono::system_clock::now() >= current_audio_finish_time - std::chrono::milliseconds(BROADCAST_INTERVAL_MS)){
    // if there are less than BROADCAST_INTERVAL_MS long till the end of this file, and nothing is currently being
    currently_processing_audio = get_requested_audio();
    if(currently_processing_audio == ""){ // if there was nothing in the requested queue
      if(audio_list.size() == 10)
        utility::fatal_error("There are no opus files in the audio directory " + dir_path);
      if(audio_list.size() < 2)
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

    current_playback_time += std::chrono::milliseconds(chunk.duration); // increase it with each broadcast

    std::cout << "this is how far ahead playback is: " << std::chrono::duration_cast<std::chrono::milliseconds>(current_playback_time - std::chrono::system_clock::now()).count() << "\n";
    std::cout << "\tthis is the last chunk duration: " << chunk.duration << "\n";
    broadcast_to_central_server(data_chunk.dump());
  }
}

void audio_server::broadcast_to_central_server(std::string &&data){
  broadcast_queue.emplace(data);
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

  uint64_t length_ms{};
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
  size_t duration = 0;
  while(read_head < buff.size()){
    auto data = get_ogg_page_info(&buff[read_head]);

    if(iter_num >= 2){
      duration += data.duration;
      page_vec.push_back({ std::vector<char>(&buff[read_head], &buff[read_head] + data.byte_length), data.duration });
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

  bool is_chunks_of_audio_empty = chunks_of_audio.size() == 0;

  uint64_t duration_of_audio = 0;
  for(const auto& page : audio_data)
    duration_of_audio += page.duration;

  while(audio_data_idx < audio_data.size()){
    audio_chunk chunk{};
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

  std::cout << std::floor(float(duration_of_audio)/60000.0) << "m " << std::round((float(duration_of_audio)/60000.0 - std::floor(float(duration_of_audio)/60000.0))*60) << "s is the duration\n";

  current_audio_finish_time += std::chrono::milliseconds(duration_of_audio);
  currently_processing_audio = ""; // we've processed it

  broadcast_routine();

  if(is_chunks_of_audio_empty)
    broadcast_routine(); // extra broadcast routine if it's right at the start of the application
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

void audio_server::wait_for_clean_exit(){
  for(auto server : audio_servers)
    server->audio_thread.join();
}

void audio_server::send_file_request_to_program(const std::string &path){
  file_transfer_queue.emplace(path);
  eventfd_write(file_request_fd, 1); // notify the main program thread
}

void audio_server::request_audio_list(){
  eventfd_write(send_audio_list, 1); // ask for the list
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

void audio_server::send_audio_request_to_audio_server(const std::string &file_name){
  audio_request_queue.emplace(file_name);
}

std::string audio_server::get_requested_audio(){ // returns an empty string if the queue is empty
  std::string req_audio{};
  if(audio_request_queue.try_dequeue(req_audio))
    return req_audio;
  else
    return "";
}