// for drawing stuff
const canvas = document.getElementById("canvas");
const ctx = canvas.getContext('2d')

// used just to get some native control stuff
const dummy_audio_el = document.getElementById("dummy_audio");

const is_mediasession_supported = 'mediaSession' in navigator

if (is_mediasession_supported) {
  navigator.mediaSession.metadata = new MediaMetadata({
    title: 'Radio'
  });
  navigator.mediaSession.setActionHandler('play', function() { dummy_audio_el.play(); toggleAudio(); play_btn_click(); });
  navigator.mediaSession.setActionHandler('pause', function() { dummy_audio_el.pause(); toggleAudio(); play_btn_click(); });

  navigator.mediaSession.setPositionState({ // with this we can make it look like a livestream
    duration: 0
  })
}

function updateMetadata(title){
  if(!is_mediasession_supported) return;
  navigator.mediaSession.metadata = new MediaMetadata({
    title
  });
}

// globals for timing
let last_play_start_time = Date.now();
const time = () => Date.now() - last_play_start_time; // time elapsed since the page loaded
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

let stations_list = undefined;
fetch("/station_list").then(data => data.json()).then(stations => {
  stations_list = stations["stations"];
  populate_stations_dropdown();
});
const stations_el = document.getElementById("stations");

function populate_stations_dropdown(){
  const dropdown_items_el = [...stations_el.children].filter(item => item.getAttribute('class') == 'dropdown-menu')[0];

  const button_el = [...stations_el.children].filter(item => item.getAttribute('type') == 'button')[0];
  const station = window.localStorage.getItem("station") ? window.localStorage.getItem("station") : stations_list[0]
  button_el.innerHTML = station.split("_").map(word => word[0].toUpperCase() + word.slice(1)).join(" ")
  set_station(station)
  
  for(let station of stations_list){
    station = station.split("_").map(word => word[0].toUpperCase() + word.slice(1)).join(" ")
    dropdown_items_el.innerHTML += `<li><a class="dropdown-item" onclick='set_station_from_el(this)'>${station}</a></li>`;
  }
}

function set_station_from_el(el){
  const button_el = [...stations_el.children].filter(item => item.getAttribute('type') == 'button')[0];
  const station = el.innerHTML.toLowerCase().replace(/ /g, '_')
  button_el.innerHTML = el.innerHTML;
  if(station != current_station_data.name)
    set_station(station)
}

const time_el = document.getElementById('time');
const broadcast_time_el = document.getElementById('broadcast_time');

const broadcast_elapsed_time = () => Date.now()/1000 - broadcast_metadata["START_TIME_S"];

function update_broadcast_time(){
  broadcast_time_el.innerHTML = `Running for: ${to_presentable_time_seconds(broadcast_elapsed_time())}`
  setTimeout(update_broadcast_time, 1000)
}

const AudioContext = window.AudioContext // Default
  || window.webkitAudioContext // Safari and older versions of Chrome
  || false;
const createGainNode = (context) => context.createGain() || context.createGainNode();
const createAnalyserNode = (context) => context.createAnalyser() || context.createAnalyserNode();

if(!AudioContext)
  alert("Radio isn't supported on this device.")

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
  },
  metadata_ws: undefined
};

const player = {
  // elements
  button_el: document.getElementById('button'),
  playing_audio_el: document.getElementById('playing'),
  audio_ws: undefined,
  // internals
  context: new AudioContext(),
  typed_array_previous: new Uint8Array(),
  current_page_time: 0,
  gain_node: undefined,
  analyser_node: undefined,
  current_volume: 1
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

  source.connect(player.gain_node)
  player.gain_node.connect(player.analyser_node)
  player.analyser_node.connect(player.context.destination);
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

function update_playing(text, total_length, force){
  if(force){
    audio_metadata.total_length = total_length
    updateMetadata(text);
    return player.playing_audio_el.innerHTML = text;
  }
    

  setTimeout(() => {
    if(player.playing_audio_el.innerHTML != "Loading...")
      audio_metadata.relative_start_time = time()
    
    audio_metadata.total_length = total_length
    player.playing_audio_el.innerHTML = text;
    updateMetadata(text);
  }, audio_metadata.time_left_in_audio()) // once this has finished, this is the next title
}

function end_audio(){
  player.current_page_time = 0;

  if(player.context)
    player.context.close();
  player.context = undefined

}

toggle_audio_timeout = undefined;
function toggleAudio(force_pause){
  if(!player.audio_ws && force_pause) return; // return if already paused
  if(player.audio_ws){
    dummy_audio_el.pause(); // stuff to control mediasession api

    player.audio_ws.close();
    player.audio_ws = undefined;
    player.gain_node.gain.linearRampToValueAtTime(0.00001, player.context.currentTime + 0.1)
    toggle_audio_timeout = setTimeout(() => {
      end_audio()
    }, 1000)
  }else{
    dummy_audio_el.play(); // stuff to control mediasession api

    if(audio_metadata.metadata_ws.readyState === WebSocket.CLOSED){ // if the metadata websocket has timed out for example, this should restart it
      stop_metadata_connection()
      start_metadata_connection()
    }
    
    clearTimeout(toggle_audio_timeout)
    end_audio()

    player.context = new AudioContext()
    player.analyser_node = createAnalyserNode(player.context)
    player.gain_node = createGainNode(player.context)
    player.gain_node.gain.value = player.current_volume;
    
    player.audio_ws = new WebSocket(`wss://radio.erewhon.xyz/ws/radio/${current_station_data.name}/audio_broadcast`)
    player.audio_ws.onmessage = msg => {
      if(msg.data == "INVALID_ENDPOINT" || msg.data == "INVALID_STATION"){
        if(msg.data == "INVALID_STATION"){
          alert("Invalid station selected!");
        }else if(msg.data == "INVALID_ENDPOINT"){
          alert("Invalid websocket endpoint!");
        }
        return;
      }

  
      const audio_data = JSON.parse(msg.data)
      let prev_durations = 0;
      for(const page of audio_data.pages){
        // i.e this is saying if the next audio page is up to 200ms before the current playback,
        // or more than 20 seconds in the past (i.e it's from the next track and the time has looped around)
        // then it's a valid page and play it, otherwise it is skipped
        // (this assumes that the tracks are at least around 30 seconds long)
        // this should prevent excessive drift from the metadata timing
        if(audio_data.start_offset + prev_durations > audio_metadata.time_in_audio() - 200 
          || audio_data.start_offset + prev_durations < audio_metadata.time_in_audio() - 20000){
          arr = new Uint8Array(page.buff)
          playMusic(arr)
        }
        prev_durations += page.duration;
      }
    }
  }
}

function resize(){
  const dpi = window.devicePixelRatio * 2 //artificially making the DPI higher seems to make the canvas very high resolution
  const styleWidth = window.innerWidth;
  const styleHeight = (styleWidth * 9 / 21 > window.innerHeight * 0.4) ? window.innerHeight * 0.4 : styleWidth * 9 / 21; //keeps a 21:9 aspect ratio until it covers 40% of the vertical screen

  canvas.setAttribute('height', styleHeight * dpi)
  canvas.setAttribute('width', styleWidth * dpi)
  canvas.setAttribute('style', `width:${styleWidth}px;height:${styleHeight}px;top:${player_bar_holder.getBoundingClientRect().y+0.1*window.devicePixelRatio}px`)

  volume_control_container.style.top = `${window.innerHeight-210}px`
}

window.addEventListener('resize', () => {
  resize();
})

function drawBars() {
  if(!player.analyser_node) return;

  const bufferLength = Math.min(player.analyser_node.frequencyBinCount, canvas.width/20); // this way get a reasonable number of bars
  const dataArray = new Uint8Array(bufferLength)
  player.analyser_node.getByteFrequencyData(dataArray)

  let barWidth = canvas.width / (dataArray.length - 1) //if you don't do -1 then there is an empty bar on the left (because one bar is 0*width - so we don't count for that)
  let barHeight = canvas.height * 0.98
  for (let i = 0; i < dataArray.length; i++) {
    const colour = interpolateColours(interpolateColoursColours.top, interpolateColoursColours.bottom, 1 - (i / dataArray.length))
    ctx.beginPath()
    ctx.fillStyle = colour
    ctx.fillRect(canvas.width - i * barWidth, canvas.height * 0.1 + canvas.height - barHeight * dataArray[i] / 255, barWidth - 1, barHeight * dataArray[i] / 255)
    ctx.fill()
    ctx.closePath()
  }
}

function formatTime(ms) { //give a formatted time string
  ms /= 1000
  const hours = Math.floor(ms / 3600)
  const minutes = Math.floor((ms - hours * 3600) / 60)
  const seconds = Math.floor(ms - hours * 3600 - minutes * 60) % 60
  const s = String(seconds).padStart(2, '0')
  const m = String(minutes).padStart(2, '0')
  const h = String(hours).padStart(2, '0')

  if (h === '00') return `${m}:${s}`
  return `${h}:${m}:${s}`
}

function updateProgressBar() {
  const currentTime = Math.max(0, audio_metadata.time_in_audio())
  const totalDuration = Math.max(0, audio_metadata.total_length)

  if (!totalDuration || !currentTime) return;

  const percentProgress = Math.max(0, Math.min(1, currentTime / totalDuration))

  ctx.font = `${40*window.devicePixelRatio}px sans-serif`

  ctx.fillStyle = "rgba(255,255,255,0.7)"
  ctx.fillText(`${formatTime(Math.min(totalDuration, currentTime))}/${formatTime(totalDuration)}`, 15, canvas.height - 40) //displays time and makes sure it's clamped to totalDuration (there are slight deviations)

  ctx.beginPath()
  ctx.moveTo(0, canvas.height)
  ctx.lineTo(canvas.width, canvas.height)
  ctx.strokeStyle = "rgba(0,0,0,0.3)"
  ctx.lineWidth = 20*window.devicePixelRatio
  ctx.stroke()
  ctx.closePath()

  const colourBottom = interpolateColours(interpolateColoursColours.top, interpolateColoursColours.bottom, 0)
  const colourTop = interpolateColours(interpolateColoursColours.top, interpolateColoursColours.bottom, percentProgress)

  const grad = ctx.createLinearGradient(0, canvas.height - 5, canvas.width * percentProgress, canvas.height - 5)
  grad.addColorStop(0, colourBottom)
  grad.addColorStop(1, colourTop)

  ctx.beginPath()
  ctx.moveTo(0, canvas.height)
  ctx.lineTo(canvas.width * percentProgress, canvas.height)

  ctx.strokeStyle = grad
  ctx.lineWidth = 20*window.devicePixelRatio
  ctx.stroke()
  ctx.closePath()
}

function animationLoop() {
  ctx.clearRect(0, 0, canvas.width, canvas.height)

  drawBars()
  updateProgressBar()

  requestAnimationFrame(animationLoop)
}

function set_volume(vol){
  if(player.context && player.gain_node)
    player.gain_node.gain.linearRampToValueAtTime(vol, player.context.currentTime + 0.1)
  player.current_volume = vol
  window.localStorage.setItem("volume", vol)
}

const interpolateColoursColours = {
  top: "#e74c3c",
  bottom: "#f39c12"
}

function interpolateColours(top, bottom, ratio) { //https://stackoverflow.com/a/16360660/3605868
  const hex = (x) => {
    x = x.toString(16)
    return (x.length === 1) ? '0' + x : x
  }

  top = top.slice(1, 7)
  bottom = bottom.slice(1, 7)

  const r = Math.ceil(parseInt(top.substring(0, 2), 16) * ratio + parseInt(bottom.substring(0, 2), 16) * (1 - ratio))
  const g = Math.ceil(parseInt(top.substring(2, 4), 16) * ratio + parseInt(bottom.substring(2, 4), 16) * (1 - ratio))
  const b = Math.ceil(parseInt(top.substring(4, 6), 16) * ratio + parseInt(bottom.substring(4, 6), 16) * (1 - ratio))

  return "#" + hex(r) + hex(g) + hex(b)
}

window.addEventListener("load", () => {
  resize();

  // also volume doesn't default to 0
  player.current_volume = window.localStorage.getItem("volume") ? Number(window.localStorage.getItem("volume")) : 1 // get volume from the local storage
  volume_control.value = Math.max(1, player.current_volume*100) // sets the volume_control element's value

  
  requestAnimationFrame(animationLoop);
})

function start_metadata_connection(){
  let just_started = true;
  audio_metadata.metadata_ws = new WebSocket(`wss://radio.erewhon.xyz/ws/radio/${current_station_data.name}/metadata_only`)
  audio_metadata.metadata_ws.onmessage = msg => {
    if(msg.data == "INVALID_ENDPOINT" || msg.data == "INVALID_STATION"){
      if(msg.data == "INVALID_STATION"){
        alert("Invalid station selected!");
      }else if(msg.data == "INVALID_ENDPOINT"){
        alert("Invalid websocket endpoint!");
      }
      return;
    }

    metadata = JSON.parse(msg.data)

    if(metadata.start_offset == 0){ // if the same shows up twice, time isn't reset
      update_playing(metadata.title, metadata.total_length);
      refresh_voted_for_stuff(); // if for example the voting box is still open
    }

    if(just_started)
      update_playing(metadata.title, metadata.total_length, true);

    audio_metadata.time_ms = metadata.start_offset
    audio_metadata.title = metadata.title

    if(just_started){
      just_started = false;
      audio_metadata.relative_start_time = -metadata.start_offset;
    }
  }
}

function stop_metadata_connection(){
  // radio connection to the other station starts now
  last_play_start_time = Date.now()
  player.playing_audio_el.innerHTML = "Loading...";
  if(audio_metadata.metadata_ws)
    audio_metadata.metadata_ws.close()
}

///// for switching stations

const current_station_data = {
  tracks: [],
  queued: [],
  name: window.localStorage.getItem("station")
};

function set_station(name){
  fetch(`/audio_list/${name}`).then(data => data.text()).then(res => {
    current_station_data.tracks = res.split("/")
    update_vote_modal()
  })

  current_station_data.name = name
  window.localStorage.setItem("station", name)

  fetch(`/audio_list/${name}`).then(data => data.text()).then(text => {
    const list = text.split("/");
    current_station_data.tracks = list;
  })

  // force pause
  toggleAudio(true)
  play_btn_click(true)
  stop_metadata_connection()
  setTimeout(() => {
    start_metadata_connection(); // 200ms should be enough to switch
  }, 200)
}

window.addEventListener('click', e => {
  if(e.target.id != "volume_control_container" && e.target.id != "volume_control" && volume_control_container.style.opacity == 1)
    toggle_volume_control()
})

const vote_modal_tracks = document.getElementById("vote_tracks");

document.getElementById("vote_btn").onclick = refresh_voted_for_stuff

function refresh_voted_for_stuff(data = undefined) {
  const func_body = (data) => {
    current_station_data.queued = data.split("/")?.filter(item => item != "").reverse()

    for(const el of vote_modal_tracks.children){
      el.innerHTML = el.innerHTML.replace(/ <b style="float:right">\(Position: \d+\)<\/b>$/, "")
      
      if(current_station_data.queued.indexOf(el.innerText) != -1){
        el.classList.add("voted_for");
        el.classList.remove("vote_track_item");
        el.innerHTML += ` <b style="float:right">(Position: ${current_station_data.queued.indexOf(el.innerText)+1})</b>`
      }else{
        el.classList.remove("voted_for");
        el.classList.add("vote_track_item");
      }
    }
  }

  if(typeof data ==	"string" && data != ""){
    func_body(data);
  }else{
    fetch(`/audio_queue/${current_station_data.name}`).then(res => res.text()).then(data => {
      func_body(data);
    })
  }
}

function update_vote_modal(){
  vote_modal_tracks.innerHTML = current_station_data.tracks.map(track => `<div class='vote_track_item' onclick='vote_for(this.innerText)'>${track}</div>`).join("")
}

function vote_for(track_name){
  fetch(`/audio_req/${current_station_data.name}/${track_name}`).then(res => res.text()).then(data => {
    if(data != "FAILURE")
      refresh_voted_for_stuff(data)
  })
}
