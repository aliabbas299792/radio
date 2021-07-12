#include "../header/web_server/web_server.h"
#include "../header/audio_server.h"

#include <thread>

#include <sys/timerfd.h>

std::unordered_map<std::string, std::string> central_web_server::config_data_map{}; // config options
std::chrono::system_clock::time_point time_start = std::chrono::system_clock::now(); // global start time

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
  tcp_server.custom_read_req(basic_web_server.ws_ping_timerfd, sizeof(uint64_t)); // start reading on the ping timerfd
  
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
  tcp_server.custom_read_req(basic_web_server.ws_ping_timerfd, sizeof(uint64_t)); // start reading on the ping timerfd
  
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
  this->num_threads = num_threads;

  std::cout << "Running server\n";

  if(config_data_map["TLS"] == "yes"){
    std::cout << "TLS will be used\n";
    run<server_type::TLS>();
  }else{
    run<server_type::NON_TLS>();
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
void central_web_server::run(){
  std::cout << "Using " << num_threads << " threads\n";

  // io_uring stuff
  std::memset(&ring, 0, sizeof(io_uring));
  io_uring_queue_init(QUEUE_DEPTH, &ring, 0); //no flags, setup the queue
  io_uring_cqe *cqe;


  // need to read on the kill efd, and make the server exit cleanly
  add_event_read_req(kill_server_efd, central_web_server_event::KILL_SERVER);
  bool run_server = true;


  // audio server, automatically registers with the eventfds - assumption is that this setup takes place before server threads are setup
  audio_server test_audio_server("public/assets/audio/", "Test");
  audio_server_initialise_reads(test_audio_server);

  audio_server test2_audio_server("public/assets/audio2/", "Test2");
  audio_server_initialise_reads(test2_audio_server);


  // server threads
  std::vector<server_data<T>> thread_data_container{};
  thread_data_container.resize(num_threads);
  int idx = 0;
  for(auto &thread_data : thread_data_container){
    // custom info is the idx of the server in the vector
    add_event_read_req(thread_data.server.central_communication_fd, central_web_server_event::SERVER_THREAD_COMMUNICATION, idx); // add read for all thread events
    idx++;
  }


  // timer stuff - time is relative to process starting time
  int timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
  utility::set_timerfd_interval(timer_fd, 5000); // 5s timer that (fires first event immediately)
  add_timer_read_req(timer_fd); // arm the timer


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
        add_timer_read_req(timer_fd); // rearm the timer
        break;
      }
      case central_web_server_event::SERVER_THREAD_COMMUNICATION: {
        add_event_read_req(req->fd, central_web_server_event::SERVER_THREAD_COMMUNICATION, req->custom_info); // rearm the eventfd

        if(req->custom_info != -1){ // then the idx is set as custom_info in the add_event_read_req call above
          auto efd_count = *reinterpret_cast<uint64_t*>(req->buff.data()); // we only write a value of 1 to the efd, so if greater than 1, multiple items in queue
          
          while(efd_count--){ // possibly deal with multiple items in the queue
            auto &server = thread_data_container[req->custom_info].server;
            auto data = server.get_from_to_program_queue();

            switch(data.msg_type){
              case web_server::message_type::request_station_list: {
                std::string response_str = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n\r\n{\"stations\": [";

                for(auto &pair : audio_server::server_id_map)
                  response_str += "\"" + pair.first + "\",";
                response_str = response_str.substr(0, response_str.size() - 1); // gets rid of the trailing comma
                response_str += "]}";

                std::vector<char> response{response_str.begin(), response_str.end()};

                server.post_server_list_response_to_server(data.item_idx, std::move(response));
                break;
              }
              case web_server::message_type::broadcast_finished:
                store.free_item(data.item_idx);
                break;
              case web_server::message_type::new_radio_client:
                std::vector<char> response_data_first{};
                std::vector<char> response_data_second{};

                char *saveptr{};
                char *temp_str = strdup(data.additional_str.c_str()); // expecting something like "test_server/endpoint"
                std::string station_name = strtok_r(temp_str, "/", &saveptr);
                std::string connection_type = strtok_r(nullptr, "", &saveptr); // so either audio_broadcast or metadata_only in this case
                free(temp_str);

                // broadcast_channel_id = server_id*2 for connection_type == "audio_broadcast"
                // broadcast_channel_id = server_id*2 + 1 for connection_type == "metadata_only"
                int broadcast_channel_id = -1; // the default, used to indicate the connectio should be closed

                if(audio_server::server_id_map.count(station_name) && ( connection_type == "audio_broadcast" || connection_type == "metadata_only" )){ // so must be a valid station and connection_type
                  // send the latest cached data for this specific server to this user, data.item_idx is the ws_client_idx and data.additional_info is the ws_client_id
                  auto server_id = audio_server::server_id_map[station_name];
                  
                  broadcast_channel_id = server_id * 2; // the server ID

                  audio_server *inst = audio_server::instance(server_id);

                  if(connection_type == "audio_broadcast"){
                    response_data_first = inst->main_thread_state.second_last_broadcast_audio_data; // since this would be the 2nd last item, so send it first
                    response_data_second = inst->main_thread_state.last_broadcast_audio_data; // and this would be the last item, so send it after that
                  }else if(connection_type == "metadata_only"){
                    response_data_first = inst->main_thread_state.second_last_broadcast_metadata_only; // since this would be the 2nd last item, so send it first
                    response_data_second = inst->main_thread_state.last_broadcast_metadata_only; // and this would be the last item, so send it after that
                    broadcast_channel_id += 1; // since broadcast channel is server_id*2 + 1
                  }
                }

                server.post_new_radio_client_response_to_server(data.item_idx, data.additional_info, std::move(response_data_first), broadcast_channel_id);

                if(broadcast_channel_id != -1)
                  server.post_new_radio_client_response_to_server(data.item_idx, data.additional_info, std::move(response_data_second), broadcast_channel_id);
                break;
            }
          }
        }
        break;
      }
      case central_web_server_event::READ:
        if(req->buff.size() == cqe->res + req->progress_bytes){
          audio_server_read_req_handler(req->fd, req->custom_info, std::move(req->buff)); // the audio server ID is stored in req->custom_info
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
      case central_web_server_event::AUDIO_SERVER_COMMUNICATION: {
        add_event_read_req(req->fd, central_web_server_event::AUDIO_SERVER_COMMUNICATION, req->custom_info); // the eventfd is in req->fd
        audio_server_event_req_handler<T>(req->fd, req->custom_info, thread_data_container); // the audio server ID is stored in req->custom_info
        break;
      }
    }

    delete req;
    
    io_uring_cqe_seen(&ring, cqe); //mark this CQE as seen
  }
  
  // make sure to close sockets
  close(timer_fd);
}

void central_web_server::audio_server_initialise_reads(audio_server &server){
  // read on the various eventfds, supplying the IDs as well
  add_event_read_req(server.notify_audio_list_available, central_web_server_event::AUDIO_SERVER_COMMUNICATION, server.id);
  add_event_read_req(server.audio_list_update, central_web_server_event::AUDIO_SERVER_COMMUNICATION, server.id);
  add_event_read_req(server.file_request_fd, central_web_server_event::AUDIO_SERVER_COMMUNICATION, server.id);
  add_event_read_req(server.broadcast_fd, central_web_server_event::AUDIO_SERVER_COMMUNICATION, server.id);
  server.request_audio_list(); // put in a request for the audio list
}

void central_web_server::audio_server_read_req_handler(int readfd, int server_id, std::vector<char> &&buff){
  // the audio server
  auto server = audio_server::instance(server_id);

  if(server->main_thread_state.fd_to_filepath.count(readfd)){ // send the file to the requester
    server->send_file_back_to_audio_server(server->main_thread_state.fd_to_filepath[readfd], std::move(buff));
    server->main_thread_state.fd_to_filepath.erase(readfd);
  }

  close(readfd); // make sure to close the file descriptor
}

template<server_type T>
void central_web_server::audio_server_event_req_handler(int eventfd, int server_id, std::vector<server_data<T>> &thread_data_container){
  // shortcut for make_ws_frame
  const auto make_ws_frame = web_server::basic_web_server<T>::make_ws_frame;
  // the audio server
  auto server = audio_server::instance(server_id);

  if(eventfd == server->notify_audio_list_available){ // the initial data
    auto data = server->get_from_audio_file_list_data_queue();

    for(const auto &file : data.file_list)
      server->main_thread_state.file_set.insert(file);

    server->main_thread_state.slash_separated_audio_list = data.appropriate_str;
  }else if(eventfd == server->audio_list_update){ // updates the initial data
    auto data = server->get_from_audio_file_list_data_queue();

    if(data.addition){
      server->main_thread_state.slash_separated_audio_list += "/"+data.appropriate_str;
      server->main_thread_state.file_set.insert(data.appropriate_str);
    }else{
      server->main_thread_state.slash_separated_audio_list = utility::remove_from_slash_string(server->main_thread_state.slash_separated_audio_list, data.appropriate_str);
      server->main_thread_state.file_set.erase(data.appropriate_str);
    }
  }else if(eventfd == server->file_request_fd){
    auto data = server->get_from_file_transfer_queue();

    int fd = open(data.filepath.c_str(), O_RDONLY);
    size_t size = utility::get_file_size(fd);
    server->main_thread_state.fd_to_filepath[fd] = data.filepath;
    add_read_req(fd, size, server_id); // add a read request for this file, setting the server_id correctly - need it to locate the server
  }else if(eventfd == server->broadcast_fd){
    //
    // audio data broadcast
    //
    auto data = server->get_broadcast_data();
    auto &main_thread_state = server->main_thread_state;

    auto ws_audio_data = make_ws_frame(data.audio_data, web_server::websocket_non_control_opcodes::text_frame);
    main_thread_state.second_last_broadcast_audio_data = main_thread_state.last_broadcast_audio_data;
    main_thread_state.last_broadcast_audio_data = ws_audio_data;

    auto item_audio_data = store.insert_item(std::move(ws_audio_data), num_threads);

    // as mentioned above, broadcast_channel_id == server_id*2 when the subscribed endpoint is audio_broadcast, so we just send the server_id
    for(server_data<T> &thread_data : thread_data_container)
      thread_data.server.post_message_to_server_thread(
        web_server::message_type::websocket_broadcast, 
        reinterpret_cast<const char*>(item_audio_data.buffer.ptr),
        item_audio_data.buffer.size,
        item_audio_data.idx,
        server_id*2
      );
    
    
    //
    // metadata only broadcast
    //
    auto ws_metadata_only = make_ws_frame(data.metdata_only, web_server::websocket_non_control_opcodes::text_frame);
    main_thread_state.second_last_broadcast_metadata_only = main_thread_state.last_broadcast_metadata_only;
    main_thread_state.last_broadcast_metadata_only = ws_metadata_only;

    auto item_metadata_only = store.insert_item(std::move(ws_metadata_only), num_threads);

    // as mentioned above, broadcast_channel_id == server_id*2+1 when the subscribed endpoint is metadata_only, so we just send the server_id
    for(server_data<T> &thread_data : thread_data_container)
      thread_data.server.post_message_to_server_thread(
        web_server::message_type::websocket_broadcast, 
        reinterpret_cast<const char*>(item_metadata_only.buffer.ptr),
        item_metadata_only.buffer.size,
        item_metadata_only.idx,
        server_id*2+1
      );
  }
}

void central_web_server::kill_server(){
  auto kill_sig = central_web_server_event::KILL_SERVER;
  write(event_fd, &kill_sig, sizeof(kill_sig));

  tcp_tls_server::server<server_type::TLS>::kill_all_servers(); // kills all TLS servers
  tcp_tls_server::server<server_type::NON_TLS>::kill_all_servers(); // kills all non TLS servers
  // this will mean the run() function will exit
}