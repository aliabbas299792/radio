#include "../header/web_server/web_server.h"
#include "../header/audio_server.h"

#include <thread>

#include <sys/timerfd.h>

std::unordered_map<std::string, std::string> central_web_server::config_data_map{};

template<>
void central_web_server::thread_server_runner(web_server::tls_web_server &basic_web_server){
  web_server::tls_server tcp_server(
    std::stoi(config_data_map["TLS_PORT"]),
    config_data_map["FULLCHAIN"],
    config_data_map["PKEY"],
    &basic_web_server,
    tcp_callbacks::accept_cb<server_type::TLS>,
    tcp_callbacks::close_cb<server_type::TLS>,
    tcp_callbacks::read_cb<server_type::TLS>,
    tcp_callbacks::write_cb<server_type::TLS>,
    tcp_callbacks::event_cb<server_type::TLS>,
    tcp_callbacks::custom_read_cb<server_type::TLS>
  ); //pass function pointers and a custom object

  basic_web_server.set_tcp_server(&tcp_server); //required to be called, to give it a pointer to the server

  tcp_server.start();
}

template<>
void central_web_server::thread_server_runner(web_server::plain_web_server &basic_web_server){
  web_server::plain_server tcp_server(
    std::stoi(config_data_map["PORT"]),
    &basic_web_server,
    tcp_callbacks::accept_cb<server_type::NON_TLS>,
    tcp_callbacks::close_cb<server_type::NON_TLS>,
    tcp_callbacks::read_cb<server_type::NON_TLS>,
    tcp_callbacks::write_cb<server_type::NON_TLS>,
    tcp_callbacks::event_cb<server_type::NON_TLS>,
    tcp_callbacks::custom_read_cb<server_type::NON_TLS>
  ); //pass function pointers and a custom object
  
  basic_web_server.set_tcp_server(&tcp_server); //required to be called, to give it a pointer to the server
  
  tcp_server.start();
}

void central_web_server::start_server(const char *config_file_path){
  auto file_fd = open(config_file_path, O_RDONLY);
  if(file_fd == -1)
    utility::fatal_error("Ensure the .config file is in this directory");
  auto file_size = utility::get_file_size(file_fd);
  
  std::vector<char> config(file_size+1);
  int read_amount = 0;
  while(read_amount != file_size)
    read_amount += read(file_fd, &config[0], file_size - read_amount);
  config[read_amount] = '\0';  //sets the final byte to NULL so that strtok_r stops there

  close(file_fd);
  
  std::vector<std::vector<char>> lines;
  char *begin_ptr = &config[0];
  char *line = nullptr;
  char *saveptr = nullptr;
  while((line = strtok_r(begin_ptr, "\n", &saveptr))){
    begin_ptr = nullptr;
    lines.emplace(lines.end(), line, line + strlen(line));
  }
  
  for(auto line : lines){
    int shrink_by = 0;
    const auto length = line.size();
    for(int i = 0; i < length; i++){ //removes whitespace
      if(line[i] ==  ' ')
        shrink_by++;
      else
        line[i-shrink_by] = line[i];
    }
    if(shrink_by)
      line[length-shrink_by] = 0; //sets the byte immediately after the last content byte to NULL so that strtok_r stops there
    if(line[0] == '#') continue; //this is a comment line, so ignore it
    char *saveptr = nullptr;
    std::string key = strtok_r(&line[0], ":", &saveptr);
    std::string value = strtok_r(nullptr, ":", &saveptr);
    config_data_map[key] = value;    
  }

  if(config_data_map.count("TLS") && config_data_map["TLS"] == "yes"){
    if(!config_data_map.count("FULLCHAIN") || !config_data_map.count("PKEY") || !config_data_map.count("TLS_PORT"))
      utility::fatal_error("Please provide FULLCHAIN, PKEY and TLS_PORT settings in the config file");
  }else if(!config_data_map.count("PORT")){
    utility::fatal_error("Please provide the PORT setting in the config file");
  }

  // the below is more like demo code to test out the multithreaded features

  //done reading config
  const auto num_threads = config_data_map.count("SERVER_THREADS") ? std::stoi(config_data_map["SERVER_THREADS"]) : 3; //by default uses 3 threads

  std::cout << "Running server\n";

  if(config_data_map["TLS"] == "yes"){
    std::cout << "TLS will be used\n";
    run<server_type::TLS>(num_threads);
  }else{
    run<server_type::NON_TLS>(num_threads);
  }
}

void central_web_server::add_event_read_req(int event_fd, central_web_server_event event, uint64_t custom_info){
  io_uring_sqe *sqe = io_uring_get_sqe(&ring); //get a valid SQE (correct index and all)
  auto *req = new central_web_server_req(); //enough space for the request struct
  req->buff.resize(sizeof(uint64_t));
  req->event = event;
  req->fd = event_fd;
  req->custom_info = custom_info;
  
  io_uring_prep_read(sqe, event_fd, &(req->buff[0]), sizeof(uint64_t), 0); //don't read at an offset
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring);
}

void central_web_server::add_read_req(int fd, size_t size, int custom_info){
  io_uring_sqe *sqe = io_uring_get_sqe(&ring); //get a valid SQE (correct index and all)
  auto *req = new central_web_server_req(); //enough space for the request struct
  req->buff.resize(size);
  req->event = central_web_server_event::READ;
  req->fd = fd;
  req->custom_info = custom_info;
  
  io_uring_prep_read(sqe, fd, &(req->buff[0]), size, 0); //don't read at an offset
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring); //submits the event
}

void central_web_server::add_timer_read_req(int timer_fd){
  io_uring_sqe *sqe = io_uring_get_sqe(&ring); //get a valid SQE (correct index and all)
  auto *req = new central_web_server_req(); //enough space for the request struct
  req->buff.resize(sizeof(uint64_t));
  req->event = central_web_server_event::TIMERFD;
  req->fd = timer_fd;
  
  io_uring_prep_read(sqe, timer_fd, &(req->buff[0]), sizeof(uint64_t), 0); //don't read at an offset
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring);
}

void central_web_server::add_write_req(int fd, const char *buff_ptr, size_t size){
  auto *req = new central_web_server_req();
  req->buff_ptr = buff_ptr;
  req->size = size;
  req->fd = fd;
  req->event = central_web_server_event::WRITE;

  io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  io_uring_prep_write(sqe, fd, buff_ptr, size, 0); //do not write at an offset
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring); //submits the event
}

void central_web_server::read_req_continued(central_web_server_req *req, size_t last_read){
  req->progress_bytes += last_read;
  
  io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  //the fd is stored in the custom info bit
  io_uring_prep_read(sqe, (int)req->fd, &(req->buff[req->progress_bytes]), req->buff.size() - req->progress_bytes, req->progress_bytes);
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring); //submits the event
}

void central_web_server::write_req_continued(central_web_server_req *req, size_t written){
  req->progress_bytes += written;
  
  io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  // again, buff_ptr is used for writing, progress_bytes is how much has been written/read (written in this case)
  io_uring_prep_write(sqe, req->fd, &req->buff_ptr[req->progress_bytes], req->size - req->progress_bytes, 0); //do not write at an offset
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring); //submits the event
}

template<server_type T>
void central_web_server::run(int num_threads){
  std::cout << "Using " << num_threads << " threads\n";


  // io_uring stuff
  std::memset(&ring, 0, sizeof(io_uring));
  io_uring_queue_init(QUEUE_DEPTH, &ring, 0); //no flags, setup the queue
  io_uring_cqe *cqe;


  // need to read on the kill efd, and make the server exit cleanly
  add_event_read_req(kill_server_efd, central_web_server_event::KILL_SERVER);
  bool run_server = true;


  // server threads
  std::vector<server_data<T>> thread_data_container{};
  thread_data_container.resize(num_threads);
  int idx = 0;
  for(auto &thread_data : thread_data_container){
    // custom info is the idx of the server in the vector
    add_event_read_req(thread_data.server.central_communication_eventfd, central_web_server_event::SERVER_THREAD_COMMUNICATION, idx++); // add read for all thread events
  }


  // timer stuff - time is relative to process starting time
  int timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
  utility::set_timerfd_interval(timer_fd, 5000); // 5s timer that (fires first event immediately)
  add_timer_read_req(timer_fd); // arm the timer


  // audio server stuff
  auto test_audio_server = audio_server("public/assets/audio/", "Test Server");
  std::thread audio_thread([&test_audio_server] {
    test_audio_server.run(); // run the audio server in another thread
  });
  // read on the various eventfds
  add_event_read_req(test_audio_server.notify_audio_list_available, central_web_server_event::AUDIO_SERVER_COMMUNICATION);
  add_event_read_req(test_audio_server.audio_list_update, central_web_server_event::AUDIO_SERVER_COMMUNICATION);
  add_event_read_req(test_audio_server.file_request_fd, central_web_server_event::AUDIO_SERVER_COMMUNICATION);

  test_audio_server.request_audio_list(); // put in a request for the audio list

  struct {
    std::unordered_set<std::string> file_set{};
    std::string slash_separated_audio_list{};
    
    std::unordered_map<int, std::string> fd_to_filepath{};
  } audio_data;


  // shortcut for make_ws_frame
  const auto make_ws_frame = web_server::basic_web_server<T>::make_ws_frame;

  while(run_server){
    char ret = io_uring_wait_cqe(&ring, &cqe);

    if(ret < 0){
      io_uring_queue_exit(&ring);
      close(event_fd);
      break;
    }

    auto *req = reinterpret_cast<central_web_server_req*>(cqe->user_data);

    if(cqe->res < 0){
      std::cerr << "CQE RES CENTRAL: " << cqe->res << std::endl;
      std::cerr << "ERRNO: " << errno << std::endl;
      std::cerr << "io_uring_wait_cqe ret: " << int(ret) << std::endl;
      io_uring_cqe_seen(&ring, cqe); //mark this CQE as seen
      continue;
    }

    switch (req->event) {
      case central_web_server_event::KILL_SERVER: {
        io_uring_queue_exit(&ring);
        close(event_fd);
        run_server = false;
        break;
      }
      case central_web_server_event::TIMERFD: {
        auto ws_data = make_ws_frame("haha", web_server::websocket_non_control_opcodes::text_frame);

        auto item_data = store.insert_item(std::move(ws_data), num_threads);

        for(auto &thread_data : thread_data_container)
          thread_data.server.post_message_to_server_thread(web_server::message_type::websocket_broadcast, reinterpret_cast<const char*>(item_data.buffer.ptr), item_data.buffer.size, item_data.idx);

        add_timer_read_req(timer_fd); // rearm the timer
        break;
      }
      case central_web_server_event::SERVER_THREAD_COMMUNICATION: {
        if(req->custom_info != -1){ // then the idx is set as custom_info
          auto data = thread_data_container[req->custom_info].server.get_from_to_program_queue();
          store.free_item(data.item_idx);

          add_event_read_req(req->fd, central_web_server_event::SERVER_THREAD_COMMUNICATION, req->custom_info); // rearm the eventfd
        }
        break;
      }
      case central_web_server_event::READ:
        if(req->buff.size() == cqe->res + req->progress_bytes){
          // the entire thing has been read, add it to some local cache or something
          if(audio_data.fd_to_filepath.count(req->fd)){ // send the file to the requester
            test_audio_server.send_file_back_to_audio_server(audio_data.fd_to_filepath[req->fd], std::move(req->buff));
            audio_data.fd_to_filepath.erase(req->fd);
          }
        }else{
          read_req_continued(req, cqe->res);
          req = nullptr;
        }
        break;
      case central_web_server_event::WRITE:
        if(cqe->res + req->progress_bytes < req->size){ // if there is still more to write, then write
          write_req_continued(req, cqe->res);
        }else{
          // we're finished writing otherwise
        }
        break;
      case central_web_server_event::AUDIO_SERVER_COMMUNICATION:
        if(req->fd == test_audio_server.notify_audio_list_available){ // the initial data
          auto data = test_audio_server.get_from_audio_file_list_data_queue();

          for(const auto &file : data.file_list)
            audio_data.file_set.insert(file);

          audio_data.slash_separated_audio_list = data.appropriate_str;

          add_event_read_req(test_audio_server.notify_audio_list_available, central_web_server_event::AUDIO_SERVER_COMMUNICATION);
        }else if(req->fd == test_audio_server.audio_list_update){ // updates the initial data
          auto data = test_audio_server.get_from_audio_file_list_data_queue();

          if(data.addition){
            audio_data.slash_separated_audio_list += "/"+data.appropriate_str;
            audio_data.file_set.insert(data.appropriate_str);
          }else{
            audio_data.slash_separated_audio_list = utility::remove_from_slash_string(audio_data.slash_separated_audio_list, data.appropriate_str);
            audio_data.file_set.erase(data.appropriate_str);
          }

          add_event_read_req(test_audio_server.audio_list_update, central_web_server_event::AUDIO_SERVER_COMMUNICATION);
        }else if(req->fd == test_audio_server.file_request_fd){
          auto data = test_audio_server.get_from_file_transfer_queue();

          int fd = open(data.filepath.c_str(), O_RDONLY);
          size_t size = utility::get_file_size(fd);
          audio_data.fd_to_filepath[fd] = data.filepath;
          add_read_req(fd, size); // add a read request for this file

          add_event_read_req(test_audio_server.file_request_fd, central_web_server_event::AUDIO_SERVER_COMMUNICATION);
        }
        break;
    }

    delete req;
    
    io_uring_cqe_seen(&ring, cqe); //mark this CQE as seen
  }
  
  close(timer_fd);

  // wait for all threads to exit before exiting the program
  for(auto &thread_data : thread_data_container)
    thread_data.thread.join();
  
  audio_thread.join();
}

void central_web_server::kill_server(){
  auto kill_sig = central_web_server_event::KILL_SERVER;
  write(event_fd, &kill_sig, sizeof(kill_sig));

  tcp_tls_server::server<server_type::TLS>::kill_all_servers(); // kills all TLS servers
  tcp_tls_server::server<server_type::NON_TLS>::kill_all_servers(); // kills all non TLS servers
  // this will mean the run() function will exit
}