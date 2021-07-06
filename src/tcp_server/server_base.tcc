#pragma once
#include "../header/server.h"

#include <sys/socket.h>
#include <netinet/tcp.h>

using namespace tcp_tls_server;

//initialise static members
template<server_type T>
std::mutex server_base<T>::init_mutex{};
template<server_type T>
int server_base<T>::shared_ring_fd = -1;

template<server_type T>
void server_base<T>::start(){ //function to run the server
  if(!ran_server){
    ran_server = true;

    io_uring_cqe *cqe;

    add_tcp_accept_req();

    while(true){
      char ret = io_uring_wait_cqe(&ring, &cqe);
      if(ret < 0)
        utility::fatal_error("io_uring_wait_cqe");
      request *req = (request*)cqe->user_data;

      if(req->event != event_type::ACCEPT &&
        req->event != event_type::KILL &&
        req->event != event_type::NOTIFICATION &&
        req->event != event_type::CUSTOM_READ &&
        (cqe->res <= 0 || (req->client_idx > 0 && clients[req->client_idx].id != req->ID)))
      {
        if(req->event == event_type::ACCEPT_WRITE || req->event == event_type::WRITE)
          req->buffer = nullptr; //done with the request buffer
        std::cout << "res: " << cqe->res << "\n";
        if(cqe->res <= 0 && clients[req->client_idx].id == req->ID){ // only do these if the client hasn't been replaced
          auto &client = clients[req->client_idx];
          if(req->event == event_type::WRITE || req->event == event_type::ACCEPT_WRITE)
            client.num_write_reqs--; // a write operation failed, decrement the number of active write operaitons for this client

          std::cout << "client is become death " << req->client_idx << "\n";

          if(client.num_write_reqs == 0){
            while(client.send_data.size()){
              auto &send_data = client.send_data.front();
              
              int broadcast_additional_info = send_data.broadcast ? send_data.custom_info : -1;
              if(close_cb != nullptr) close_cb(req->client_idx, broadcast_additional_info, static_cast<server<T>*>(this), custom_obj); // might have had multiple broadcasts

              client.send_data.pop();
            }

            if(client.send_data.size() == 0) // there was no send_data and no broadcast, so we close it once here
              if(close_cb != nullptr) close_cb(req->client_idx, -1, static_cast<server<T>*>(this), custom_obj);
            
            static_cast<server<T>*>(this)->close_connection(req->client_idx); //making sure to remove any data relating to it as well
          }
        }
      }else if(req->event == event_type::KILL) {
        io_uring_queue_exit(&ring);
        close(listener_fd);
        close(kill_efd);
        close(notification_efd);
        
        is_active = false; // received an exit signal, main server program will now exit, so it's now inactive
        break;
      }else if(req->event == event_type::NOTIFICATION){
        event_read(notification_efd, event_type::NOTIFICATION);

        auto efd_data = *reinterpret_cast<uint64_t*>(&(req->read_data[0]));
        while(efd_data--) // repeat this for the number of times the eventfd has gone off
          if(event_cb != nullptr) event_cb(static_cast<server<T>*>(this), custom_obj);
      }else if(req->event == event_type::CUSTOM_READ){
        if(req->read_data.size() == cqe->res + req->read_amount){
          if(custom_read_cb != nullptr) custom_read_cb(req->client_idx, (int)req->custom_info, std::move(req->read_data), static_cast<server<T>*>(this), custom_obj);
        }else{
          custom_read_req_continued(req, cqe->res);
          req = nullptr; //don't want it to be deleted yet
        }
      }else{
        static_cast<server<T>*>(this)->req_event_handler(req, cqe->res);
      }

      delete req;
      io_uring_cqe_seen(&ring, cqe); //mark this CQE as seen
    }
  }
}

template<server_type T>
void server_base<T>::notify_event(){
  uint64_t data = 1;
  write(notification_efd, &data, sizeof(uint64_t));
}

template<server_type T>
void server_base<T>::kill_server(){
  uint64_t data = 1;
  write(kill_efd, &data, sizeof(uint64_t));
}

template<server_type T>
void server_base<T>::event_read(int event_fd, event_type event){
  io_uring_sqe *sqe = io_uring_get_sqe(&ring); //get a valid SQE (correct index and all)
  request *req = new request(); //enough space for the request struct
  req->read_data.resize(sizeof(uint64_t));
  req->event = event;
  
  io_uring_prep_read(sqe, event_fd, &(req->read_data[0]), sizeof(uint64_t), 0); //don't read at an offset
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring); //submits the event
}

template<server_type T>
server_base<T>::server_base(int listen_port){
  std::unique_lock<std::mutex> init_lock(init_mutex);

  if(shared_ring_fd == -1){
    std::memset(&ring, 0, sizeof(io_uring));
    io_uring_queue_init(QUEUE_DEPTH, &ring, 0); //no flags, setup the queue
    shared_ring_fd = ring.ring_fd;
  }else{ //all subsequent threads therefore share the same backend
    std::memset(&ring, 0, sizeof(io_uring));
    io_uring_params params{};
    params.wq_fd = shared_ring_fd;
    params.flags = IORING_SETUP_ATTACH_WQ;
    io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params);
  }
  
  event_read(kill_efd, event_type::KILL); //sets a read request for the signal eventfd
  event_read(notification_efd, event_type::NOTIFICATION); //sets a read request for the normal eventfd
  
  listener_fd = setup_listener(listen_port); //setup the listener socket
}

template<server_type T>
int server_base<T>::setup_client(int client_socket){ //returns index into clients array
  auto index = 0;

  if(freed_indexes.size()){ //if there's a free index, give that
    index = *freed_indexes.begin(); //get first element in set
    freed_indexes.erase(index); //erase first element in set

    auto &freed_client = clients[index];

    const auto new_id = (freed_client.id + 1) % 100; //ID loops every 100
    freed_client = client<T>();
    freed_client.id = new_id;
  }else{
    clients.push_back(client<T>()); //otherwise give a new one
    index = clients.size()-1;
  }
  
  clients[index].sockfd = client_socket;

  return index;
}

template<server_type T>
void server_base<T>::read_connection(int client_idx) {
  add_read_req(client_idx, event_type::READ);
}

template<server_type T>
int server_base<T>::setup_listener(int port) {
  int listener_fd;
  int yes = 1;
  addrinfo hints, *server_info, *traverser;

  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET; //IPv4 or IPv6
  hints.ai_socktype = SOCK_STREAM; //tcp
  hints.ai_flags = AI_PASSIVE; //use local IP

  if(getaddrinfo(NULL, std::to_string(port).c_str(), &hints, &server_info) != 0)
    utility::fatal_error("getaddrinfo");

  for(traverser = server_info; traverser != NULL; traverser = traverser->ai_next){
    if((listener_fd = socket(traverser->ai_family, traverser->ai_socktype, traverser->ai_protocol)) == -1) //ai_protocol may be usefulin the future I believe, only UDP/TCP right now, may
      utility::fatal_error("socket construction");

    if(setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) //2nd param (SOL_SOCKET) is saying to do it at the socket protocol level, not TCP or anything else, just for the socket
      utility::fatal_error("setsockopt SO_REUSEADDR");
      
    if(setsockopt(listener_fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) == -1)
      utility::fatal_error("setsockopt SO_REUSEPORT");
      
    if(setsockopt(listener_fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) == -1)
      utility::fatal_error("setsockopt SO_KEEPALIVE");
    
    int keep_idle = 1000; // The time (in seconds) the connection needs to remain idle before TCP starts sending keepalive probes, if the socket option SO_KEEPALIVE has been set on this socket.  This option should not be used in code intended to be portable.
    if(setsockopt(listener_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle)) == -1)
      utility::fatal_error("setsockopt TCP_KEEPIDLE");
    
    int keep_interval = 1000; // The time (in seconds) between individual keepalive probes. This option should not be used in code intended to be portable.
    if(setsockopt(listener_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keep_interval, sizeof(keep_interval)) == -1)
      utility::fatal_error("setsockopt TCP_KEEPINTVL");
    
    int keep_count = 10; // The maximum number of keepalive probes TCP should send before dropping the connection.  This option should not be used in code intended to be portable.
    if(setsockopt(listener_fd, IPPROTO_TCP, TCP_KEEPCNT, &keep_count, sizeof(keep_count)) == -1)
      utility::fatal_error("setsockopt TCP_KEEPCNT");

    if(bind(listener_fd, traverser->ai_addr, traverser->ai_addrlen) == -1){ //try to bind the socket using the address data supplied, has internet address, address family and port in the data
      perror("bind");
      continue; //not fatal, we can continue
    }

    break; //we got here, so we've got a working socket - so break
  }

  freeaddrinfo(server_info); //free the server_info linked list

  if(traverser == NULL) //means we didn't break, so never got a socket made successfully
    utility::fatal_error("no socket made");

  if(listen(listener_fd, BACKLOG) == -1)
    utility::fatal_error("listen");

  return listener_fd;
}

template<server_type T>
int server_base<T>::add_accept_req(int listener_fd, sockaddr_storage *client_address, socklen_t *client_address_length){
  io_uring_sqe *sqe = io_uring_get_sqe(&ring); //get a valid SQE (correct index and all)
  io_uring_prep_accept(sqe, listener_fd, (sockaddr*)client_address, client_address_length, 0); //no flags set, prepares an SQE

  request *req = new request();
  req->event = event_type::ACCEPT;

  io_uring_sqe_set_data(sqe, req); //sets the SQE data
  io_uring_submit(&ring); //submits the event

  return 0; //maybe return is required for something else later
}

template<server_type T>
int server_base<T>::add_read_req(int client_idx, event_type event){
  if(!clients[client_idx].read_req_active){
    io_uring_sqe *sqe = io_uring_get_sqe(&ring); //get a valid SQE (correct index and all)
    request *req = new request(); //enough space for the request struct
    req->total_length = READ_SIZE;;
    req->event = event;
    req->client_idx = client_idx;
    req->ID = clients[client_idx].id;
    req->read_data.resize(READ_SIZE);

    io_uring_prep_read(sqe, clients[client_idx].sockfd, &(req->read_data[0]), READ_SIZE, 0); //don't read at an offset
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring); //submits the event
    
    clients[client_idx].read_req_active = true;
    
    return 0;
  }else{
    return -1;
  }
}

template<server_type T>
int server_base<T>::add_write_req(int client_idx, event_type event, const char *buffer, unsigned int length) {
  request *req = new request();
  req->client_idx = client_idx;
  req->total_length = length;
  req->buffer = buffer;
  req->event = event;
  req->ID = clients[client_idx].id;

  clients[client_idx].num_write_reqs++; // another write request is now active
  
  io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  io_uring_prep_write(sqe, clients[client_idx].sockfd, buffer, length, 0); //do not write at an offset
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring); //submits the event

  return 0;
}

template<server_type T>
void server_base<T>::custom_read_req(int fd, size_t to_read, int client_idx, std::vector<char> &&buff, size_t read_amount){
  request *req = new request();
  req->client_idx = client_idx;
  req->total_length = to_read;
  req->read_amount = read_amount;
  req->read_data = buff;
  req->custom_info = fd;
  req->event = event_type::CUSTOM_READ;

  req->read_data.resize(to_read + read_amount); //needs this much at least

  io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  io_uring_prep_read(sqe, fd, &(req->read_data[read_amount]), READ_SIZE, 0);
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring); //submits the event
}

template<server_type T>
void server_base<T>::custom_read_req_continued(request *req, size_t last_read){
  req->read_amount += last_read;

  const auto initial_offset = req->read_data.size() - req->total_length;
  //the buffer is big enough to hold header data, but the amount we want to read is the total_length
  //but the initial read_amount is the offset of the header data, so we find the initial offset like this

  io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  //the fd is stored in the custom info bit
  io_uring_prep_read(sqe, (int)req->custom_info, &(req->read_data[req->read_amount]), READ_SIZE, req->read_amount - initial_offset);
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring); //submits the event
}

template<server_type T>
void server_base<T>::add_tcp_accept_req(){
  add_accept_req(listener_fd, &client_address, &client_address_length);
}