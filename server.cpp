#include "server.h"

void fatal_error(std::string error_message){
  perror(std::string("Fatal Error: " + error_message).c_str());
  exit(1);
}

server::server(void (*accept_callback)(int client_fd, server *web_server), void (*read_callback)(int client_fd, int iovec_count, iovec iovecs[], server *web_server), void (*write_callback)(int client_fd, server *web_server)) : accept_callback(accept_callback), read_callback(read_callback), write_callback(write_callback){
  //above just sets the callbacks

  io_uring_queue_init(QUEUE_DEPTH, &ring, 0); //no flags, setup the queue

  this->listener_fd = setup_listener(PORT);
  
  this->serverLoop();
}

int server::setup_listener(int port) {
  int listener_fd;
  int yes = 1;
  addrinfo hints, *server_info, *traverser;

  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET; //IPv4 or IPv6
  hints.ai_socktype = SOCK_STREAM; //tcp
  hints.ai_flags = AI_PASSIVE; //use local IP

  if(getaddrinfo(NULL, std::to_string(port).c_str(), &hints, &server_info) != 0)
    fatal_error("getaddrinfo");

  for(traverser = server_info; traverser != NULL; traverser = traverser->ai_next){
    if((listener_fd = socket(traverser->ai_family, traverser->ai_socktype, traverser->ai_protocol)) == -1) //ai_protocol may be usefulin the future I believe, only UDP/TCP right now, may
      fatal_error("socket construction");

    if(setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) //2nd param (SOL_SOCKET) is saying to do it at the socket protocol level, not TCP or anything else, just for the socket
      fatal_error("setsockopt SO_REUSEADDR");
      
    if(setsockopt(listener_fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) == -1)
      fatal_error("setsockopt SO_REUSEPORT");

    if(bind(listener_fd, traverser->ai_addr, traverser->ai_addrlen) == -1){ //try to bind the socket using the address data supplied, has internet address, address family and port in the data
      perror("bind");
      continue; //not fatal, we can continue
    }

    break; //we got here, so we've got a working socket - so break
  }

  freeaddrinfo(server_info); //free the server_info linked list

  if(traverser == NULL) //means we didn't break, so never got a socket made successfully
    fatal_error("no socket made");

  if(listen(listener_fd, BACKLOG) == -1)
    fatal_error("listen");

  return listener_fd;
}

int server::add_accept_req(int listener_fd, sockaddr_storage *client_address, socklen_t *client_address_length){
  io_uring_sqe *sqe = io_uring_get_sqe(&ring); //get a valid SQE (correct index and all)
  io_uring_prep_accept(sqe, listener_fd, (sockaddr*)client_address, client_address_length, 0); //no flags set, prepares an SQE

  request *req = (request*)std::malloc(sizeof(request));
  req->event = event_type::ACCEPT;

  io_uring_sqe_set_data(sqe, req); //sets the SQE data
  io_uring_submit(&ring); //submits the event

  return 0; //maybe return is required for something else later
}

int server::add_read_req(int client_fd){
  io_uring_sqe *sqe = io_uring_get_sqe(&ring); //get a valid SQE (correct index and all)
  request *req = (request*)std::malloc(sizeof(request) + sizeof(iovec)); //enough space for the request struct, and 1 iovec struct (we are using one to do the read request)
  
  req->iovecs[0].iov_base = std::malloc(READ_SIZE); //malloc enough space for the data to be read
  req->iovecs[0].iov_len = READ_SIZE;
  req->iovec_count = 1;
  req->event = event_type::READ;
  req->client_socket = client_fd;
  std::memset(req->iovecs[0].iov_base, 0, sizeof(req->iovecs[0].iov_base));
  
  io_uring_prep_readv(sqe, client_fd, &req->iovecs[0], 1, 0); //don't read at an offset, and read with one iovec structure
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring); //submits the event

  return 0;
}

int server::add_write_req(int client_fd, iovec *iovecs, int iovec_count) {
  request *req = (request*)std::malloc(sizeof(request) + sizeof(iovec) * iovec_count);
  req->client_socket = client_fd;
  req->iovec_count = iovec_count;
  req->event = event_type::WRITE;

  std::cout << iovec_count << "\n";
  std::memcpy(req->iovecs, iovecs, iovec_count * sizeof(iovec));
  std::cout << iovec_count << "aaaaaaa\n";

  io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  io_uring_prep_writev(sqe, req->client_socket, req->iovecs, req->iovec_count, 0); //do not write at an offset
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring); //submits the event

  return 0;
}

void server::serverLoop(){
  io_uring_cqe *cqe;
  sockaddr_storage client_address;
  socklen_t client_address_length = sizeof(client_address);

  add_accept_req(listener_fd, &client_address, &client_address_length);

  while(true){
    char ret = io_uring_wait_cqe(&ring, &cqe);
    request *req = (request*)cqe->user_data;

    if(ret < 0)
      fatal_error("io_uring_wait_cqe");
    if(cqe->res < 0){
      fprintf(stderr, "async request failed: %s\n", strerror(-cqe->res));
      exit(1);
    }
    
    switch(req->event){
      case event_type::ACCEPT:
        if(accept_callback != nullptr) accept_callback(cqe->res, this);
        add_accept_req(listener_fd, &client_address, &client_address_length); //add another accept request
        add_read_req(cqe->res); //also need to read whatever request it sends immediately
        free(req); //cleanup from the malloc in add_accept_req
        break;
      case event_type::READ:
        if(read_callback != nullptr) read_callback(req->client_socket, req->iovec_count, req->iovecs, this);
        //below is cleaning up from the malloc stuff
        free(req->iovecs[0].iov_base);
        free(req);
        break;
      case event_type::WRITE:
        if(write_callback != nullptr) write_callback(req->client_socket, this);
        //below is cleaning up from the malloc stuff
        for(int i = 0; i < req->iovec_count; i++) free(req->iovecs[i].iov_base);
        free(req);
        break;
    }

    io_uring_cqe_seen(&ring, cqe); //mark this CQE as seen
  }
}