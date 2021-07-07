#include "../header/utility.h"
#include "../header/web_server/web_server.h"

#include <thread>
#include <malloc.h>
#include <regex>
#include <cmath>
#include <random>

namespace utility{
  void fatal_error(std::string error_message){
    perror(std::string("Fatal Error: " + error_message).c_str());
    exit(1);
  }
  
  std::string replace(std::string s1, std::string s2, std::string pattern){
    std::regex reg(pattern);
    return std::regex_replace(s1, reg, s2);
  }

  std::string remove_from_slash_string(std::string s1, std::string s2){
    std::string output = replace(s1, "", "^"+s2+"/");
    output = replace(output, "", "/"+s2+"$");
    return replace(output, "", "/"+s2);
  }

  void set_timerfd_interval(int timerfd, int ms){
    uint64_t sec = std::floor(ms / 1000);
    uint64_t nsec = (ms - sec*1000) * 1000000;

    itimerspec timer_values;
    timer_values.it_value.tv_sec = 0;
    timer_values.it_value.tv_nsec = 1; // 1ns delay to start, so basically instant
    timer_values.it_interval.tv_sec = sec;
    timer_values.it_interval.tv_nsec = nsec;
    timerfd_settime(timerfd, 0, &timer_values, nullptr);
  }
  
  uint64_t random_number(uint64_t min, uint64_t max){
    static std::random_device random_device{};
    static std::mt19937 generator(random_device());
    std::uniform_int_distribution<int> distribution(min, max);

    return distribution(generator);
  }

  uint64_t get_file_size(int file_fd){
    stat_struct file_stat;

    if(fstat(file_fd, &file_stat) < 0)
      fatal_error("file stat");
    
    if(S_ISBLK(file_stat.st_mode)){
      uint64_t size_bytes;
      if(ioctl(file_fd, BLKGETSIZE64, &size_bytes) != 0)
        fatal_error("file stat ioctl");
      
      return size_bytes;
    }else if(S_ISREG(file_stat.st_mode)){
      return file_stat.st_size;
    }

    return -1;
  }

  std::string to_web_name(std::string name){
    auto new_name = name;
    for(auto &character : new_name){
      character = tolower(character);
      if(character == ' ')
        character = '_';
    }
    return new_name;
  }

  void sigint_handler(int sig_number){
    std::cout << "\nShutting down...\n";

    central_web_server::instance().kill_server(); // the program gracefully exits without needing to explicitly exit
  }
}