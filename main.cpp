#include "server.h"

#include <unistd.h> //read
#include <sys/stat.h> //fstat
#include <fcntl.h> //open
#include <sys/types.h> //O_RDONLY

typedef struct stat stat_struct;

io_uring ring;

void sigint_handler(int sig_number){
  std::cout << " Shutting down...\n";
  exit(0);
}

void sigpipe_handler(int sig_number){}

void accept_callback(int client_fd, server *web_server){
  //std::cout << "accepted client with client_fd: " << client_fd << "\n";
}

long int get_file_size(int file_fd){
  stat_struct file_stat;

  if(fstat(file_fd, &file_stat) < 0)
    fatal_error("file stat");
  
  if(S_ISBLK(file_stat.st_mode)){
    uint long long size_bytes;
    if(ioctl(file_fd, BLKGETSIZE64, &size_bytes) != 0)
      fatal_error("file stat ioctl");
    
    return size_bytes;
  }else if(S_ISREG(file_stat.st_mode)){
    return file_stat.st_size;
  }

  return -1;
}

std::string get_content_type(std::string filepath){
  char *file_extension_data = (char*)filepath.c_str();
  std::string file_extension = "";
  while((file_extension_data = strtok(file_extension_data, "."))){
    file_extension = file_extension_data;
    file_extension_data = nullptr;
  }

  if(file_extension == "html" || file_extension == "htm")
    return "Content-Type: text/html\r\n";
  if(file_extension == "css")
    return "Content-Type: text/css\r\n";
  if(file_extension == "js")
    return "Content-Type: text/javascript\r\n";
  if(file_extension == "opus")
    return "Content-Type: audio/opus\r\n";
  if(file_extension == "mp3")
    return "Content-Type: audio/mpeg\r\n";
  if(file_extension == "mp4")
    return "Content-Type: video/mp4\r\n";
  if(file_extension == "gif")
    return "Content-Type: image/gif\r\n";
  if(file_extension == "png")
    return "Content-Type: image/png\r\n";
  if(file_extension == "jpg" || file_extension == "jpeg")
    return "Content-Type: image/jpeg\r\n";
  if(file_extension == "txt")
    return "Content-Type: text/plain\r\n";
  return "Content-Type: application/octet-stream\r\n";
}

int read_file(std::string filepath, char **buffer, int reserved_bytes = 0){
  int file_fd = open(filepath.c_str(), O_RDONLY);
  if(file_fd < 0) return -1;

  const auto size = get_file_size(file_fd);
  int read_bytes = 0;

  *buffer = (char*)malloc(reserved_bytes + size);
  std::memset(*buffer, 0, reserved_bytes + size);

  while(read_bytes != size){
    io_uring_sqe *sqe = io_uring_get_sqe(&ring); //get a valid SQE (correct index and all)

    io_uring_prep_read(sqe, file_fd, &((char*)*buffer)[reserved_bytes], size, read_bytes); //don't read at an offset
    io_uring_submit(&ring); //submits the event

    io_uring_cqe *cqe;
    char ret = io_uring_wait_cqe(&ring, &cqe);
    read_bytes += cqe->res;

    io_uring_cqe_seen(&ring, cqe); //mark this CQE as seen
  }
  
  return size;
}

int read_file_web(std::string filepath, char **buffer, int responseCode = 200, bool accept_bytes = false){
  auto header_first_line = "";
  std::cout << responseCode << "\n";
  switch(responseCode){
    case 200:
      header_first_line = "HTTP/1.1 200 OK\r\n";
      break;
      break;
    default:
      header_first_line = "HTTP/1.1 404 Not Found\r\n";
  }

  const auto content_type = get_content_type(filepath);
  const auto reserved_bytes = 200; //I'm estimating, header is probably going to be up to 200 bytes
  const auto size = read_file(filepath, buffer, reserved_bytes);
  const auto content_length = std::to_string(size);

  std::string headers = "";
  if(accept_bytes){
    headers = header_first_line + content_type + "Accept-Ranges: bytes\r\nContent-Length: " + content_length + "\r\nRange: bytes=0-" + content_length + "/" + content_length + "\r\nConnection:";
  }else{
    headers = header_first_line + content_type + "Content-Length: " + content_length + "\r\nConnection:";
  }

  const auto header_last = "Keep-Alive\r\n\r\n"; //last part of the header

  if(size < 0) return -1;
  
  std::memset(*buffer, 32, reserved_bytes); //this sets the entire header section in the buffer to be whitespace
  std::memcpy(*buffer, headers.c_str(), headers.size()); //this copies the first bit of the header to the beginning of the buffer
  std::memcpy(&(*buffer)[reserved_bytes-strlen(header_last)], header_last, strlen(header_last)); //this copies the last bit to the end of the reserved section

  /*for(int i = 0; i < size + reserved_bytes; i++){
    fputc((*buffer)[i], stdout);
  }*/

  return size + reserved_bytes; //the total request size
}

void read_callback(int client_fd, char *buffer, unsigned int length, server *web_server){
  std::vector<std::string> headers;

  bool accept_bytes = false;

  char *str = nullptr;
  char *saveptr = nullptr;
  while((str = strtok_r(((char*)buffer), "\r\n", &saveptr))){ //retrieves the headers
    std::string tempStr = std::string(str, strlen(str));
    
    if(tempStr.find("Range: bytes=") != std::string::npos) accept_bytes = true;
    buffer = nullptr;
    headers.push_back(tempStr);
  }
  
  if(!strcmp(strtok_r((char*)headers[0].c_str(), " ", &saveptr), "GET")){
    char *path = strtok_r(nullptr, " ", &saveptr);
    std::string processed_path = std::string(&path[1], strlen(path)-1);
    processed_path = processed_path == "" ? "index.html" : processed_path;

    char *http_version = strtok_r(nullptr, " ", &saveptr);

    char *send_buffer = nullptr;
    int content_length = 0;
    
    if((content_length = read_file_web(processed_path, &send_buffer, 200, accept_bytes)) != -1){
      web_server->add_write_req(client_fd, send_buffer, content_length); //pass the data to the write function
    }else{
      content_length = read_file_web("404.html", &send_buffer, 400);
      web_server->add_write_req(client_fd, send_buffer, content_length);
    }
  }
}

void write_callback(int client_fd, server *web_server){
  std::cout << "closing\n\n";
  close(client_fd); //for web requests you close the socket right after
}

int main(){
  signal(SIGINT, sigint_handler); //signal handler for when Ctrl+C is pressed
  signal(SIGPIPE, sigpipe_handler); //signal handler for when Ctrl+C is pressed

  io_uring_queue_init(QUEUE_DEPTH, &ring, 0); //no flags, setup the queue

  server web_server(accept_callback, read_callback, write_callback);

  return 0;
}