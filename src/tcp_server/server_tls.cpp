#include "../header/server.h"
#include "../header/utility.h"

#include <sys/socket.h>
#include <thread>

using namespace tcp_tls_server;

// define static stuff
std::vector<server<server_type::TLS>*> server<server_type::TLS>::tls_servers{};
std::mutex server<server_type::TLS>::tls_server_vector_access{};

void server<server_type::TLS>::kill_all_servers() {
  std::unique_lock<std::mutex> tls_access_lock(tls_server_vector_access);
  for(const auto server : tls_servers)
    server->kill_server();
}

void server<server_type::TLS>::start_closing_connection(int client_idx) {
  auto &client = clients[client_idx];
  
  if(client.num_write_reqs == 0 && (active_connections.count(client_idx) || uninitiated_connections.count(client_idx))){
    clean_up_client_resources(client_idx, active_connections.count(client_idx)); // only trigger the close callback if it is actually active
    client.send_data = {}; //free up all the data we might have wanted to send

    shutdown(client.sockfd, SHUT_WR);

    std::cout << "we've started closing the connection\n";

    client.closing_now = true;
    add_read_req(client_idx, event_type::READ);
  }
}

void server<server_type::TLS>::finish_closing_connection(int client_idx) {
  auto &client = clients[client_idx];
  
  if(client.num_write_reqs == 0 && (active_connections.count(client_idx) || uninitiated_connections.count(client_idx))){
    wolfSSL_shutdown(client.ssl);
    wolfSSL_free(client.ssl);

    client.ssl = nullptr; //so that if we try to close multiple times, free() won't crash on it, inside of wolfSSL_free()

    int shutdwn = shutdown(client.sockfd, SHUT_RD);
    int clse = close(client.sockfd);

    uninitiated_connections.erase(client_idx);
    active_connections.erase(client_idx);
    freed_indexes.insert(freed_indexes.end(), client_idx);

    std::cout << "\t\tfinished shutting down connection (shutdown ## close): (" << shutdwn << " ## " << clse << ")\n";
    std::cout << "\t\t\terrno: " << errno << "\n";
  }
}

void server<server_type::TLS>::write_connection(int client_idx, std::vector<char> &&buff) {
  auto &client = clients[client_idx];
  client.send_data.emplace(std::move(buff));
  const auto &data_ref = client.send_data.front();
  auto &to_write_buff = data_ref.buff;
  
  if(client.send_data.size() == 1) //only do wolfSSL_write() if this is the only thing to write
    wolfSSL_write(client.ssl, &to_write_buff[0], to_write_buff.size()); //writes the data using wolfSSL
}

void server<server_type::TLS>::write_connection(int client_idx, char *buff, size_t length) {
  auto &client = clients[client_idx];
  client.send_data.emplace(buff, length);
  const auto &data_ref = client.send_data.front();
  auto &to_write_buff = data_ref.ptr_buff;
  
  if(client.send_data.size() == 1) //only do wolfSSL_write() if this is the only thing to write
    wolfSSL_write(client.ssl, to_write_buff, length); //writes the data using wolfSSL
}

server<server_type::TLS>::server(
  int listen_port,
  std::string fullchain_location,
  std::string pkey_location,
  void *custom_obj,
  accept_callback<server_type::TLS> a_cb,
  close_callback<server_type::TLS> c_cb,
  read_callback<server_type::TLS> r_cb,
  write_callback<server_type::TLS> w_cb,
  event_callback<server_type::TLS> e_cb,
  custom_read_callback<server_type::TLS> cr_cb
) : server_base<server_type::TLS>(listen_port) { //call parent constructor with the port to listen on
  this->accept_cb = a_cb;
  this->close_cb = c_cb;
  this->read_cb = r_cb;
  this->write_cb = w_cb;
  this->event_cb = e_cb;
  this->custom_read_cb = cr_cb;
  this->custom_obj = custom_obj;

  //initialise wolfSSL
  wolfSSL_Init();

  //create the wolfSSL context
  if((wolfssl_ctx = wolfSSL_CTX_new(wolfTLSv1_2_server_method())) == NULL)
    utility::fatal_error("Failed to create the WOLFSSL_CTX");

  //load the server certificate
  if(wolfSSL_CTX_use_certificate_chain_file(wolfssl_ctx, fullchain_location.c_str()) != SSL_SUCCESS)
    utility::fatal_error("Failed to load the certificate files");

  //load the server's private key
  if(wolfSSL_CTX_use_PrivateKey_file(wolfssl_ctx, pkey_location.c_str(), SSL_FILETYPE_PEM) != SSL_SUCCESS)
    utility::fatal_error("Failed to load the private key file");
  
  //set the wolfSSL callbacks
  wolfSSL_CTX_SetIORecv(wolfssl_ctx, tls_recv);
  wolfSSL_CTX_SetIOSend(wolfssl_ctx, tls_send);

  std::unique_lock<std::mutex> access_lock(tls_server_vector_access);
  tls_servers.push_back(this); // basically so that anything which wants to manage all of the server at once, can
}

void server<server_type::TLS>::tls_accept(int client_idx){
  auto *client = &clients[client_idx];

  WOLFSSL *ssl = wolfSSL_new(wolfssl_ctx);
  wolfSSL_set_fd(ssl, client_idx); //not actually the fd but it's useful to us

  wolfSSL_SetIOReadCtx(ssl, this);
  wolfSSL_SetIOWriteCtx(ssl, this);

  client->ssl = ssl; //sets the ssl connection

  wolfSSL_accept(ssl); //initialise the wolfSSL accept procedure
}

void server<server_type::TLS>::tls_accepted_routine(const int client_idx){
  auto &client = clients[client_idx];

  if(accept_cb != nullptr) accept_cb(client_idx, this, custom_obj);
  uninitiated_connections.erase(client_idx);
  active_connections.insert(client_idx);

  auto &data = client.recv_data; //the data vector
  const auto recvd_amount = data.size();
  std::vector<char> buffer(READ_SIZE);
  auto amount_read = wolfSSL_read(client.ssl, &buffer[0], READ_SIZE);

  //above will either add in a read request, or get whatever is left in the local buffer (as we might have got the HTTP request with the handshake)

  client.recv_data = std::vector<char>{};
  if(amount_read > -1){
    clients[client_idx].read_req_active = false;
    if(read_cb != nullptr) read_cb(client_idx, &buffer[0], amount_read, this, custom_obj);
  }
}

void server<server_type::TLS>::req_event_handler(request *&req, int cqe_res){
  switch(req->event){
    case event_type::ACCEPT: {
      auto client_idx = setup_client(cqe_res);
      uninitiated_connections.insert(client_idx);

      add_tcp_accept_req();
      tls_accept(client_idx);
      break;
    }
    case event_type::ACCEPT_READ: {
      auto &client = clients[req->client_idx];
      client.read_req_active = false;

      if(client.recv_data.size() == 0) { //if there is no data in the buffer, add it
        client.recv_data = std::move(req->read_data);
        client.recv_data.resize(cqe_res);
      }else{ //otherwise copy the new data to the end of the old data
        auto *vec = &client.recv_data;
        vec->insert(vec->end(), &(req->read_data[0]), &(req->read_data[0]) + cqe_res);
      }
      if(wolfSSL_accept(client.ssl) == 1) //that means the connection was successfully established
        tls_accepted_routine(req->client_idx);
      break;
    }
    case event_type::ACCEPT_WRITE: { //used only for when wolfSSL needs to write data during the TLS handshake
      auto &client = clients[req->client_idx];
      client.num_write_reqs--; // decrement number of active write requests
      client.accept_last_written = cqe_res; //this is the amount that was last written, used in the tls_write callback
      if(wolfSSL_accept(client.ssl) == 1) //that means the connection was successfully established
        tls_accepted_routine(req->client_idx);
      break;
    }
    case event_type::WRITE: { //used for generally writing over TLS
      int broadcast_additional_info = -1; // only used for broadcast messages
      auto &client = clients[req->client_idx];
      client.num_write_reqs--; // decrement number of active write requests
      if(client.send_data.size() > 0){ //ensure this connection is still active
        auto &data_ref = client.send_data.front();
        auto write_data_stuff = data_ref.get_ptr_and_size();
        data_ref.last_written = cqe_res;

        int written = wolfSSL_write(client.ssl, write_data_stuff.buff, write_data_stuff.length);
        if(written > -1){ //if it's not negative, it's all been written, so this write call is done
          if(client.send_data.front().broadcast) //if it's broadcast, then custom_info must be some data we want to pass to the read callback (such as in our case the item_idx)
            broadcast_additional_info = client.send_data.front().custom_info;

          client.send_data.pop();
          if(write_cb != nullptr) write_cb(req->client_idx, broadcast_additional_info, this, custom_obj);
          if(client.send_data.size()){ //if the write queue isn't empty, then write that as well
            auto &data_ref = client.send_data.front();
            auto write_data_stuff = data_ref.get_ptr_and_size();
            wolfSSL_write(client.ssl, write_data_stuff.buff, write_data_stuff.length);
          }
        }
      }
      break;
    }
    case event_type::READ: { //used for reading over TLS
      auto &client = clients[req->client_idx];
      client.read_req_active = false;

      if(client.closing_now && cqe_res == 0){ // close if it read 0
        finish_closing_connection(req->client_idx);
        break;
      }

      int to_read_amount = cqe_res; //the default read size
      if(client.recv_data.size()) { //will correctly deal with needing to call wolfSSL_read multiple times
        auto &vec_member = client.recv_data;
        vec_member.insert(vec_member.end(), &(req->read_data[0]), &(req->read_data[0]) + cqe_res);
        to_read_amount = vec_member.size(); //the read amount has got to be bigger, since the pending data could be more than READ_SIZE
      }else{
        client.recv_data = std::move(req->read_data);
        client.recv_data.resize(cqe_res);
      }

      std::vector<char> buffer(to_read_amount);
      int total_read = 0;

      while(client.recv_data.size()){
        int this_time = wolfSSL_read(client.ssl, &buffer[total_read], to_read_amount - total_read);
        if(this_time <= 0) break;
        total_read += this_time;
      }

      if(total_read == 0) add_read_req(req->client_idx, event_type::READ); //total_read of 0 implies that data must be read into the recv_data buffer
      
      if(total_read > 0){
        if(read_cb != nullptr) read_cb(req->client_idx, &buffer[0], total_read, this, custom_obj);
        if(!client.recv_data.size())
          client.recv_data = {};
      }
      break;
    }
  }
}
