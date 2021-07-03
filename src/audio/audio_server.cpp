#include "../header/audio_server.h"
#include <chrono>
#include <algorithm>
#include <regex>

// initialise static variable
std::vector<audio_server*> audio_server::audio_servers{};

audio_server::audio_server(std::string dir_path, std::string name){ // not thread safe
  audio_server_name = name;

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
}

void audio_server::run(){
  // io_uring
  io_uring_cqe *cqe;
  std::memset(&ring, 0, sizeof(io_uring));
  io_uring_queue_init(QUEUE_DEPTH, &ring, 0); //no flags, setup the queue

  
  // making sure it exits cleanly
  audio_servers.push_back(this); // push to static vector
  fd_read_req(kill_efd, audio_events::KILL);

  // watching files/updating the server to reflect the directory correctly
  inotify_add_watch(inotify_fd, dir_path.c_str(), IN_CREATE | IN_DELETE); // watch the directory for changes
  fd_read_req(inotify_fd, audio_events::INOTIFY_DIR_CHANGED, sizeof(struct inotify_event*) + NAME_MAX + 1); // guaranteed enough for at least 1 event
  fd_read_req(send_audio_list, audio_events::AUDIO_LIST);
  fd_read_req(file_ready_fd, audio_events::FILE_READY);

  // time stuff
  utility::set_timerfd_interval(timerfd, 1000);
  fd_read_req(timerfd, audio_events::BROADCAST_TIMER);
  current_audio_start_time = std::chrono::system_clock::now();
  current_audio_finish_time = current_audio_start_time;
  last_broadcast_time = current_audio_start_time;

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
        auto data = reinterpret_cast<inotify_event*>(req->buff.data());
        auto file_name = data->name;
        auto original_file_name = file_name;

        const char *file_extension{};
        const char *tmp_file_extension{};
        while((tmp_file_extension = strtok_r(file_name, ".", &strtok_saveptr)) != nullptr){
          file_name = nullptr;
          file_extension = tmp_file_extension;
        }

        std::string file_name_str = std::string(original_file_name);

        if(data->mask & IN_CREATE){
          slash_separated_audio_list += "/" + file_name_str;
          audio_list.push_back(file_name_str);
          post_audio_list_update(true, file_name_str); // we've added a file
        }else{
          audio_list.erase(std::remove(audio_list.begin(), audio_list.end(), file_name_str), audio_list.end()); // remove the deleted file
          slash_separated_audio_list = utility::remove_from_slash_string(slash_separated_audio_list, file_name_str);
          post_audio_list_update(false, file_name_str); // we've removed a file
        }

        fd_read_req(inotify_fd, audio_events::INOTIFY_DIR_CHANGED, sizeof(struct inotify_event*) + NAME_MAX + 1); // guaranteed enough for atleast 1 event
        break;
    }
    
    delete req;
    io_uring_cqe_seen(&ring, cqe); //mark this CQE as seen
  }

  close(kill_efd);
  close(broadcast_fd);
  close(file_ready_fd);
  close(file_request_fd);
  close(audio_from_program_request_fd);
  io_uring_queue_exit(&ring);
}

void audio_server::broadcast_routine(){
  if(currently_processing_audio == "" && current_audio_finish_time >= std::chrono::system_clock::now() - std::chrono::seconds(5)){
    // if there are less than 5s till the end of this file, and nothing is currently being
    currently_processing_audio = get_requested_audio();
    if(currently_processing_audio == ""){ // if there was nothing in the requested queue
      if(audio_list.size() < 10)
        currently_processing_audio = audio_list[utility::random_number(0, audio_list.size()-1)]; // select a file at random
      else{
        // a very rough way of making sure you don't get repeats too often
        auto idx = utility::random_number(0, audio_list.size()-1); // select an index which hasn't been used for the previous 10 items
        while(last_10_items.count(idx)){
          idx = utility::random_number(0, audio_list.size()-1);
        }

        if(last_10_items.size() == 10) // only do this if there are already 10 elements in there
          last_10_items.erase(std::prev(last_10_items.end())); // removes the last element of the set
        last_10_items.insert(idx); // and add this to the exclusion list

        currently_processing_audio = audio_list[idx];
      }
    }
    // at this point currently_processing_audio is definitely not blank
    // send_file_request_to_program(dir_path + currently_processing_audio + ".opus"); // requests a file to be read
  }

  static uint64_t count = 0;
  auto now = std::chrono::system_clock::now();
  printf("Difference: %6fms\n", double(int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_broadcast_time).count()-count))/1000000);
  count += 1000000000;

  // broadcast data here
  // just get the oldest chunk with chunks_of_audio.pop_front() and broadcast it, as audio_data

}

void audio_server::process_audio(file_transfer_data &&data){
  //         _______      _______       _______
  // (back) |   1   | -> |   2   | ... |   N   | (front)
  //         -------      -------       -------
  // chunks_of_audio.push_back(NEW_CHUNK_OF_AUDIO)
  // chunks_of_audio.pop_back() to get the most recently pushed chunk
  // chunks_of_audio.pop_front() to get the audio to broadcast

  // firstly get the most recent chunk from chunks_of_audio.pop_back(), get its length and see if you can append some on to the end of it to get 5s chunk
  // then push that chunk, followed by the rest of the chunks
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

void audio_server::kill_all_servers(){
  for(auto server : audio_servers)
    server->kill_server();
}

void audio_server::kill_server(){
  eventfd_write(kill_efd, 1); // kill the server
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