// globals for timing
const page_start_time = Date.now();
const time = () => Date.now() - page_start_time; // time elapsed since the page loaded
const to_presentable_time = ms => {
  if(ms >= 60000)
      return `${Math.floor(ms/60000)}m ${Math.floor((ms/1000)%60)}s`
  return `${Math.floor(ms/1000)}s`
}

const to_presentable_time_seconds = s => {
  if(s >= 3600)
    return `${Math.floor(s/3600)}h ${Math.floor((s%3600)/60)}m ${Math.floor(s%60)}s`
  if(s >= 60)
    return `${Math.floor(s/60)}m ${Math.floor(s%60)}s`
  return `${Math.floor(s)}s`
}

let broadcast_metadata = undefined;
fetch("/broadcast_metadata").then(data => data.text()).then(metadata => {
  broadcast_metadata = Object.fromEntries(metadata.replace(/ /g, "").split("\n").map(item => [item.split(":")[0], Number(item.split(":")[1])]));
  update_broadcast_time(); // start updating the broadcast time
})

const time_el = document.getElementById('time');
const broadcast_time_el = document.getElementById('broadcast_time');

const broadcast_elapsed_time = () => Date.now()/1000 - broadcast_metadata["START_TIME_S"];

function update_time(){
  time_el.innerHTML = `${to_presentable_time(audio_metadata.time_in_audio())} / ${to_presentable_time(audio_metadata.total_length)}`
  setTimeout(update_time, 1000);
}

function update_broadcast_time(){
  broadcast_time_el.innerHTML = `Running for: ${to_presentable_time_seconds(broadcast_elapsed_time())}`
  setTimeout(update_broadcast_time, 1000)
}

const audio_metadata = {
  time_ms: 0,
  title: "",
  total_length: 0,
  relative_start_time: 0, // set from the start_offset property of audio chunks or later from updating the title
  time_in_audio: function() {
    return time() - audio_metadata.relative_start_time
  },
  fraction_complete: function() {
    return Math.min(1, Math.max(0, this.time_in_audio() / audio_metadata.total_length));
  },
  time_left_in_audio: function() {
    return this.total_length - this.time_in_audio()
  }
};

const player = {
  // elements
  button_el: document.getElementById('button'),
  playing_audio_el: document.getElementById('playing'),
  // user facing stuff
  station: (window.location.pathname.indexOf("/listen/") == 0 ? window.location.pathname.replace("/listen/", "") : "test"),
  audio_ws: undefined,
  // internals
  context: new AudioContext(),
  typed_array_previous: new Uint8Array(),
  current_page_time: 0,
  gain_node: undefined
}

function playPCM(arrayBuffer){ //plays interleaved linear PCM with 16 bit, bit depth
  const int32array = new Int32Array(arrayBuffer);
  const channels = 2;
  const sampleRate = 48000;
  const length = int32array.length;
  const buffer = player.context.createBuffer(channels, length+1, sampleRate);
  
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
  floatsL.forEach((_, index) => floatsL[index] = 0 );
  const floatsR = new Float32Array(int32array.length+1);
  floatsR.forEach((_, index) => floatsR[index] = 0 );

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

  if(player.current_page_time < player.context.currentTime){ //ensures that the current time never lags behind context.currentTime
    player.current_page_time = player.context.currentTime + 0.01;
  }

  const source = player.context.createBufferSource();
  source.buffer = buffer;
  source.start(player.current_page_time);
  player.current_page_time += Math.round(buffer.duration*100)/100; //2dp

  if(!player.gain_node)
    player.gain_node = new GainNode(player.context)

  source.connect(player.gain_node)

  player.gain_node.connect(player.context.destination);
}

async function playMusic(typedArrayCurrent){ //takes 1 packet of audio, decode, and then plays it
  let allocatedCurrentBufferPtr = 0, allocatedPreviousBufferPtr = 0;
  let decodedCurrentBufferPtr;

  try {
    allocatedCurrentBufferPtr = Module._malloc(typedArrayCurrent.length * typedArrayCurrent.BYTES_PER_ELEMENT);
    Module.HEAPU8.set(typedArrayCurrent, allocatedCurrentBufferPtr);

    if(player.typed_array_previous){
      allocatedPreviousBufferPtr = Module._malloc(player.typed_array_previous.length * player.typed_array_previous.BYTES_PER_ELEMENT);
      Module.HEAPU8.set(player.typed_array_previous, allocatedPreviousBufferPtr);
    }

    const output_len = Module.ccall( //will retrieve the total length in bytes required to store the decoded output
      'output_len', 
      "number",
      ["number", "number"],
      [allocatedCurrentBufferPtr, typedArrayCurrent.length]
    );

    decodedCurrentBufferPtr = Module._malloc(output_len);

    const typedArrayCurrentLength = typedArrayCurrent ? typedArrayCurrent.length : 0;
    const typedArrayPreviousLength = player.typed_array_previous ? player.typed_array_previous.length : 0;

    Module.ccall(
      'decode_page',
      "number",
      ["number", "number", "number", "number", "number"],
      [allocatedCurrentBufferPtr, typedArrayCurrentLength, allocatedPreviousBufferPtr, typedArrayPreviousLength, decodedCurrentBufferPtr]
    );

    const outputArrayBuffer = Module.HEAP8.slice(decodedCurrentBufferPtr, decodedCurrentBufferPtr + output_len).buffer;

    playPCM(outputArrayBuffer); //output array buffer contains decoded audio
  } catch(err) {
    console.log("Error: " + err);
  } finally {
    Module._free(allocatedPreviousBufferPtr);
    Module._free(allocatedCurrentBufferPtr);

    player.typed_array_previous = typedArrayCurrent;
  }
}

function update_playing(text, total_length){
  setTimeout(() => {
    if(player.playing_audio_el.innerHTML != "Loading...")
      audio_metadata.relative_start_time = time()
    
    audio_metadata.total_length = total_length
    player.playing_audio_el.innerHTML = text;
  }, audio_metadata.time_left_in_audio()) // once this has finished, this is the next title
}

function toggleAudio(){
  if(player.audio_ws){
    player.current_page_time = 0;
    player.button_el.innerHTML = "Play";

    player.audio_ws.close();
    player.context.close();
    player.gain_node = undefined;

    player.audio_ws = undefined;
  }else{
    player.context = new AudioContext()
    player.audio_ws = new WebSocket(`wss://radio.erewhon.xyz/ws/radio/${player.station}/audio_broadcast`)
    player.audio_ws.onmessage = msg => {
      if(msg.data == "INVALID_ENDPOINT" || msg.data == "INVALID_STATION"){
        if(msg.data == "INVALID_STATION"){
          alert("Invalid station selected!");
        }else if(msg.data == "INVALID_ENDPOINT"){
          alert("Invalid websocket endpoint!");
        }
        return;
      }
  
      audio_data = JSON.parse(msg.data)
      for(const page of audio_data.pages){
        arr = new Uint8Array(page.buff)
        playMusic(arr)
      }
    }
    player.button_el.innerText = "Pause";
  }
}

window.addEventListener("load", () => {
  update_time(); // start time updating from now
  let just_started = true;
  const metadata_ws = new WebSocket(`wss://radio.erewhon.xyz/ws/radio/${player.station}/metadata_only`)
  metadata_ws.onmessage = msg => {
    if(msg.data == "INVALID_ENDPOINT" || msg.data == "INVALID_STATION"){
      if(msg.data == "INVALID_STATION"){
        alert("Invalid station selected!");
      }else if(msg.data == "INVALID_ENDPOINT"){
        alert("Invalid websocket endpoint!");
      }
      return;
    }

    metadata = JSON.parse(msg.data)

    if(metadata.start_offset == 0 || just_started) // if the same shows up twice, time isn't reset
      update_playing(metadata.title, metadata.total_length);

    audio_metadata.time_ms = metadata.start_offset
    audio_metadata.title = metadata.title

    if(just_started){
      just_started = false;
      audio_metadata.relative_start_time = -metadata.start_offset;
    }
  }
})