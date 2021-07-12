#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opus/include/opus.h"

typedef unsigned int uint32_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;

#define true 1;
#define false 0;

struct page_data {
  uint32_t length;
  uint32_t packets_table_length;

  uint32_t read_idx;
  uint32_t residual;

  uint32_t packets_table[256];
};

struct page_data parse_page_data(uint8_t *buff, uint32_t channels){
  struct page_data data;

  uint8_t page_segments = buff[26];
  uint8_t segments_table[page_segments];
  memcpy(&segments_table[0], &buff[27], page_segments);

  uint32_t packets_table[page_segments];
  uint32_t current_idx = 0;

  char residual = false; // true only if the final packet crosses over into the next ogg page
  
  for(int i = 0; i < page_segments; i++){

    // this bit deals with multi segment packets
    if(segments_table[i] == 255){

      // at least this long since it's a continuing packet
      int packet_size = 255;

      for(int j = i+1; j < page_segments; j++){
        //increment the i variable so that the combined multi segment
        // packet's segments aren't iterated over again in the outer loop
        i++; 
        //increase packet size by however much this segment is
        packet_size += segments_table[j]; 

        //if this one isn't 255, we can stop this sub loop, otherwise we continue
        // (multi segment - or even multi page - packets are possible)
        if(segments_table[j] != 255) break;
        //if this is the final segment, and is 255, that means that the final
        // 'packet' is actually only part of one
        if(j == page_segments-1) residual = true;
      }

      //stores the concatenated packet data in the packets_table array
      packets_table[current_idx++] = packet_size;
    }else{
      packets_table[current_idx++] = segments_table[i];
    }

    if(i == page_segments-1)
      break;
  }

  if(residual) current_idx--;
  
  size_t read_idx = 27 + page_segments;
  size_t length = 960 * page_segments * channels;

  data.length = length;
  data.packets_table_length = current_idx;
  data.residual = residual;
  data.read_idx = read_idx;
  
  memcpy(&data.packets_table[0], &packets_table[0], current_idx * sizeof(uint32_t));

  return data;
}

size_t output_len(uint8_t *input, int input_len){ //returns the length of what the output would be after decoding
  const opus_int32 sample_rate = 48000;
  const int channels = 2;
  int error;
  
  struct page_data data;

  data = parse_page_data(input, channels);

  const int frameSize = 960; //960 samples per segment

  return data.packets_table_length*frameSize*4; //length in bytes, 4 bytes per element
  //each frame becomes 960 samples, and there are 2 channels
}

int decode_page(uint8_t* input, size_t input_len, uint8_t* previous, size_t prev_len, uint16_t* output){ //decodes and writes to output
  const opus_int32 sample_rate = 48000;
  const int channels = 2;
  int error;
  

  struct page_data data;
  struct page_data prev_data;

  if(prev_len != 0)
    prev_data = parse_page_data(previous, channels);
  
  data = parse_page_data(input, channels);

  
  OpusDecoder *dec;
  dec = opus_decoder_create(sample_rate, channels, &error);

  if(error != OPUS_OK){
    printf("Couldn't create Opus decoder (error: %d )\n", error);
    return error;
  }
  

  size_t total_len = data.packets_table_length;

  if(prev_len != 0)
    total_len += prev_data.packets_table_length;

  
  uint32_t write_idx = 0;
  const uint32_t decoded_frame_len = 960*channels;

  for(int i = 0; i < total_len; i++){
    int j = 0;
    int16_t decoded_frame[960*channels*2];

    if(prev_len > 0 && prev_data.packets_table_length > i){ // the previous data is discarded - only used for initialising the state of the Opus decoder

      uint32_t raw_frame_len = prev_data.packets_table[i];

      // decode the frame
      int frame_size = opus_decode(
        dec,
        &previous[prev_data.read_idx],
        raw_frame_len,
        &decoded_frame[0],
        decoded_frame_len,
        0
      );

      prev_data.read_idx += raw_frame_len;
      continue;
    }
    
    j = i - prev_data.packets_table_length;

    int frame_size = opus_decode(
      dec,
      &input[data.read_idx],
      data.packets_table[j],
      &decoded_frame[0],
      decoded_frame_len,
      0
    );

    if(frame_size < 0) { // this is an error
      printf("frame size: %d\n", frame_size);
      continue;
    }
    
    //will copy to the correct offset
    memcpy(&output[write_idx], &decoded_frame[0], frame_size * channels * 2);
    data.read_idx += data.packets_table[j];
    write_idx += frame_size * channels;
  }

  opus_decoder_destroy(dec);

  return OPUS_OK;
}