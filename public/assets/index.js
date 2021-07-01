const button = document.getElementById('button');
const selector = document.getElementById('selector');
let context = undefined;
let currentTime = 0;
let arrayBufferPrevious = undefined;

function playPCM(arrayBuffer){ //plays interleaved linear PCM with 16 bit, bit depth
  const int32array = new Int32Array(arrayBuffer);
  const channels = 2;
  const sampleRate = 48000;
  const length = int32array.length;
  const buffer = context.createBuffer(channels, length+1, sampleRate);
  
  const positiveSampleDivisor = Math.pow(2, 15)-1;
  const negativeSampleDivisor = Math.pow(2, 15);
  //I opted to use decimal rather than hexadecimal to represent these for clarities sake
  //Consider how a normal signed byte has a range of values from 127 to -128,
  //i.e 0b01111111 (127) to 0b10000000 (-128),as the most significant bit (leftmost is -128)
  //We do basically the same but considering the fact that WebAudioAPI takes in Float32 values in the range -1 to 1,
  //so we divide Int32 values by those sample divisors to get a number in the necessary range
  const normaliser = (sample) => sample > 0 ? sample / positiveSampleDivisor : sample / negativeSampleDivisor;
  
  //the +1 used below, and the +1 in the length above fixes the clicking issue - I can only 
  //assume that one sample (if non zero) is distorted if the source stops at that sample
  const floatsL = new Float32Array(int32array.length+1);
  floatsL.forEach((sample, index) => floatsL[index] = 0 );
  const floatsR = new Float32Array(int32array.length+1);
  floatsR.forEach((sample, index) => floatsR[index] = 0 );

  for(let i = 0; i < int32array.length; i++){
    const sample = int32array[i];

    //0xFFFF is (2^16 - 1), using & with it applies it as a mask, and gives us only the first 16 bits
    //this loses the sign in js however (js ints are signed 32 bit), so we right shift so most significant bit we care about
    //is the actual most significant bit, then we left shift by the same amount, and the sign is preserved
    //(it does left shift according to 2's complement stuff)
    const sampleLIntermediate = ((sample & 0xFFFF) << 16) >> 16; //least significant byte is left channel
    const sampleRIntermediate = sample >> 16; //most significant byte is right channel

    const sampleL = normaliser(sampleLIntermediate);
    const sampleR = normaliser(sampleRIntermediate);

    floatsL[i] = sampleL;
    floatsR[i] = sampleR;
  }

  buffer.getChannelData(0).set(floatsL);
  buffer.getChannelData(1).set(floatsR);

  if(currentTime < context.currentTime){ //ensures that the current time never lags behind context.currentTime
    currentTime = context.currentTime + 0.05;
  }

  const source = context.createBufferSource();
  source.buffer = buffer;
  source.start(currentTime);
  currentTime += Math.round(buffer.duration*100)/100; //2dp


  source.connect(context.destination);
}

async function getFile(path){ //gets some file via fetch api
  const resp = await fetch(`/assets/audioSamples/${path}`);
  const blob = await resp.blob();
  const arrayBuffer = await blob.arrayBuffer();
  const uint8array = new Uint8Array(arrayBuffer);
  return uint8array;
}

async function playMusic(arrayOfArrayBuffers){ //takes 1 packet of audio, decode, and then plays it
  for(const arrayBuffer of arrayOfArrayBuffers){
    let allocatedCurrentBufferPtr = 0, allocatedPreviousBufferPtr = 0;
    let decodedCurrentBufferPtr;

    try {
      const typedArrayCurrent = new Uint8Array(arrayBuffer);
      const typedArrayPrevious = new Uint8Array(arrayBufferPrevious);
      
      allocatedCurrentBufferPtr = Module._malloc(typedArrayCurrent.length * typedArrayCurrent.BYTES_PER_ELEMENT);
      Module.HEAPU8.set(typedArrayCurrent, allocatedCurrentBufferPtr);

      if(arrayBufferPrevious){
        allocatedPreviousBufferPtr = Module._malloc(typedArrayPrevious.length * typedArrayPrevious.BYTES_PER_ELEMENT);
        Module.HEAPU8.set(typedArrayPrevious, allocatedPreviousBufferPtr);
      }

      const lengthOfOutput = Module.ccall( //will retrieve the total length in bytes required to store the decoded output
        'lengthOfOutput', 
        "number",
        ["number", "number"],
        [allocatedCurrentBufferPtr, typedArrayCurrent.length]
      );

      decodedCurrentBufferPtr = Module._malloc(lengthOfOutput);

      const typedArrayCurrentLength = arrayBuffer ? typedArrayCurrent.length : 0;
      const typedArrayPreviousLength = arrayBufferPrevious ? typedArrayPrevious.length : 0;

      Module.ccall(
        'decodeOggPage',
        "number",
        ["number", "number", "number", "number", "number"],
        [allocatedCurrentBufferPtr, typedArrayCurrentLength, allocatedPreviousBufferPtr, typedArrayPreviousLength, decodedCurrentBufferPtr]
      );

      const outputArrayBuffer = Module.HEAP8.slice(decodedCurrentBufferPtr, decodedCurrentBufferPtr + lengthOfOutput).buffer;

      playPCM(outputArrayBuffer); //output array buffer contains decoded audio
    } catch(err) {
      console.log("Error: " + err);
    } finally {
      Module._free(allocatedPreviousBufferPtr);
      Module._free(allocatedCurrentBufferPtr);
      Module._free(allocatedPreviousBufferPtr);

      arrayBufferPrevious = arrayBuffer;
    }
  }
}

const data = {
  sinetest: 30,
  stereoInstruments: 22
};

async function playOpus(stop){ //plays some number of audio packets from a subdirectory - playMusic is used directly
  if(!context && !stop){ //if the stop flag is set, then only stop
    button.innerText = "Stop Music";
    context = new window.AudioContext();
    currentTime = 0;
    const arrayOfArrayBuffers = [];
    
    const track = selector.value;
    const packetAmount = data[track];

    for(let i = 0; i <= packetAmount; i++){
      arrayOfArrayBuffers.push((await getFile(`${track}/${i}.packet`)).buffer);
    }
    playMusic(arrayOfArrayBuffers);

    setTimeout(() => {
      playOpus(true); //this causes it to stop the music after approximately the entire track has been played
    }, packetAmount*1000);
  }else{
    button.innerText = "Play Music";
    await context.close();
    context = undefined;
  }
}