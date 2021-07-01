#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opus/include/opus.h"

typedef unsigned char u_char;

struct pageData{
  int length;
  int packetsTableLength;
  int readPointer;
  int residual;
  int preSkip;
  int packetsTable[256]; //256, as the page has up to 255, and then we may add the last frame of the previous frame too
};

void readRaw(unsigned char *dest, unsigned char *src, int offset, int length);
struct pageData parsePageData(unsigned char* buffer, int channels);

int decodeOggPage(unsigned char* input, int inputLength, unsigned char* previous, int previousLength, short* output){ //decodes and writes to output
  const opus_int32 sample_rate = 48000;
  const int channels = 2;
  int error;
  
  struct pageData data;
  struct pageData prevData;

  if(previousLength > 0){
    prevData = parsePageData(previous, channels);
  }else{
    prevData.packetsTableLength = 0;
  }

  data = parsePageData(input, channels);

  char frame;
  
  OpusDecoder *dec;
  dec = opus_decoder_create(sample_rate, 2, &error);

  if(error != OPUS_OK){
    fprintf(stderr, "Cannot create decoder: %s %d\n", opus_strerror(error), error);
    return EXIT_FAILURE;
  }

  int totalLength = data.packetsTableLength;
  if(prevData.packetsTableLength != 0){
    totalLength += prevData.packetsTableLength;
  }

  char processedPrevPacket = 0;
  int writePointer = 0;
  for(int i = 0; i < totalLength; i++){
    int j = 0;

    if(i < prevData.packetsTableLength && previousLength > 0){
      u_char frame[data.packetsTable[i]];
      short decoded_frame[960*channels];
      readRaw(frame, previous, prevData.readPointer, prevData.packetsTable[i]);
      int frame_size = opus_decode(dec, &frame[0], prevData.packetsTable[i], decoded_frame, 960*channels, 0);
      prevData.readPointer += prevData.packetsTable[i];
      continue;
    }
    j = i - prevData.packetsTableLength;

    u_char frame[data.packetsTable[j]];
    short decoded_frame[960*channels];
    readRaw(frame, input, data.readPointer, data.packetsTable[j]);
    int frame_size = opus_decode(dec, frame, data.packetsTable[j], decoded_frame, 960*channels, 0);
    memcpy(&output[writePointer], decoded_frame, frame_size*channels*2); //will write from the correct offset
    data.readPointer += data.packetsTable[j];
    writePointer += frame_size*channels;
  }

  return EXIT_SUCCESS;
}

int lengthOfOutput(unsigned char* input, int inputLength){ //returns the length of what the output would be after decoding
  const opus_int32 sample_rate = 48000;
  const int channels = 2;
  int error;
  
  struct pageData data;

  data = parsePageData(input, channels);

  const int frameSize = 960; //960 samples per segment

  return data.packetsTableLength*frameSize*4; //length in bytes, 4 bytes per element
  //each frame becomes 960 samples, and there are 2 channels
}

void readRaw(unsigned char *dest, unsigned char *src, int offset, int length){
  for(int i = 0; i < length; i++){
    dest[i] = src[offset + i];
  }
}

struct pageData parsePageData(unsigned char* buffer, int channels){ //will parse the Ogg page data
  struct pageData data;

  u_char sig[4];
  u_char preSkip[2];
  u_char absoluteGranulePosition[8];
  u_char streamSerialNumber[4];
  u_char pageSequenceNumber[4];
  u_char pageChecksum[4];
  u_char pageSegements[1];

  readRaw(sig, buffer, 0, 4);
  readRaw(preSkip, buffer, 4, 2); //this is only possible because I've edited the packets to have the preskip amount here
  //readRaw(headerTypeFlag, buffer, 5, 1);
  readRaw(absoluteGranulePosition, buffer, 6, 8);
  readRaw(streamSerialNumber, buffer, 14, 4);
  readRaw(pageChecksum, buffer, 22, 4);
  readRaw(pageSegements, buffer, 26, 1);

  int preSkipNumber = (preSkip[0] << 8) + preSkip[1];
  
  unsigned char pageSegs = pageSegements[0];
  unsigned char segmentsTable[pageSegs];
  readRaw(segmentsTable, buffer, 27, pageSegs);

  int packetsTable[pageSegs]; //up to 255 segments
  int currentIndex = 0;

  int residual = 0; //used to indicat if the final packet overlaps to the next ogg page
  for(int i = 0; i < pageSegs; i++){ //loop through all page segments
    if(segmentsTable[i] == 255){
      int packetSize = 255; //we established the current page segment isn't a complete packet, and is at least 255 long
      for(int j = i+1; j < pageSegs; j++){ //loop until the end (maximum)
        i++; //increment the i variable so that the combined multi segment packet's segments aren't iterated over again in the outer loop
        packetSize += segmentsTable[j]; //increase packet size by however much this segment is
        if(segmentsTable[j] != 255) break; //if this one isn't 255, we can stop this sub loop, otherwise we continue (multi segment - or even multi page - packets are possible)
        if(j == pageSegs-1) residual = 1; //if this is the final segment, and is 255, that means that the final 'packet' is actually only part of one
      }
      packetsTable[currentIndex++] = packetSize; //stores the concatenated packet data in the packetsTable array
    }else{
      packetsTable[currentIndex++] = segmentsTable[i];
    }
    if(i == pageSegs-1){
      break;
    }
  }

  if(residual) currentIndex--;
  
  int readPointer = 27 + pageSegs;
  int length = 960*pageSegs*channels;

  data.length = length;
  memcpy(data.packetsTable, packetsTable, currentIndex*sizeof(int));
  data.packetsTableLength = currentIndex;
  data.residual = residual;
  data.preSkip = preSkipNumber;
  data.readPointer = readPointer;

  return data;
}