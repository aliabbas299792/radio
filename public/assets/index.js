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
  navigator.mediaSession.setActionHandler('play', function () { toggleAudio(); play_btn_click(); });
  navigator.mediaSession.setActionHandler('pause', function () { toggleAudio(); play_btn_click(); });

  navigator.mediaSession.setPositionState({ // with this we can make it look like a livestream
    duration: 0
  })
}

function updateMetadata(title) {
  if (!is_mediasession_supported) return;
  navigator.mediaSession.metadata = new MediaMetadata({
    title
  });
}

// globals for timing
let last_play_start_time = Date.now();
const time = () => Date.now() - last_play_start_time; // time elapsed since the page loaded
const to_presentable_time = ms => {
  if (ms >= 60000)
    return `${Math.floor(ms / 60000)}m ${Math.floor((ms / 1000) % 60)}s`
  return `${Math.floor(ms / 1000)}s`
}

const to_presentable_time_seconds = s => {
  if (s >= 3600)
    return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m ${Math.floor(s % 60)}s`
  if (s >= 60)
    return `${Math.floor(s / 60)}m ${Math.floor(s % 60)}s`
  return `${Math.floor(s)}s`
}

let broadcast_metadata = undefined;
const stations_el = document.getElementById("stations");
let stations_list = undefined;

function populate_stations_dropdown() {
  const dropdown_items_el = [...stations_el.children].filter(item => item.getAttribute('class') == 'dropdown-menu')[0];

  const button_el = [...stations_el.children].filter(item => item.getAttribute('type') == 'button')[0];
  const station = window.localStorage.getItem("station") ? window.localStorage.getItem("station") : stations_list[0]
  button_el.innerHTML = station.split("_").map(word => word[0].toUpperCase() + word.slice(1)).join(" ")
  set_station(station)

  for (let station of stations_list) {
    station = station.split("_").map(word => word[0].toUpperCase() + word.slice(1)).join(" ")
    dropdown_items_el.innerHTML += `<li><a class="dropdown-item" onclick='set_station_from_el(this)'>${station}</a></li>`;
  }
}

function set_station_from_el(el) {
  const button_el = [...stations_el.children].filter(item => item.getAttribute('type') == 'button')[0];
  const station = el.innerHTML.toLowerCase().replace(/ /g, '_')
  button_el.innerHTML = el.innerHTML;
  if (station != current_station_data.name)
    set_station(station)
}

const time_el = document.getElementById('time');
const broadcast_time_el = document.getElementById('broadcast_time');

const broadcast_elapsed_time = () => Date.now() / 1000 - broadcast_metadata["START_TIME_S"];

function update_broadcast_time() {
  broadcast_time_el.innerHTML = `Running for: ${to_presentable_time_seconds(broadcast_elapsed_time())}`
  setTimeout(update_broadcast_time, 1000)
}

const AudioContext = window.AudioContext // Default
  || window.webkitAudioContext // Safari and older versions of Chrome
  || false;
const createGainNode = (context) => context.createGain() || context.createGainNode();
const createAnalyserNode = (context) => context.createAnalyser() || context.createAnalyserNode();

if (!AudioContext)
  alert("Radio isn't supported on this device.")

const audio_metadata = {
  time_ms: 0,
  title: "",
  total_length: 0,
  relative_start_time: 0, // set from the start_offset property of audio chunks or later from updating the title
  time_in_audio: function () {
    return time() - audio_metadata.relative_start_time
  },
  fraction_complete: function () {
    return Math.min(1, Math.max(0, this.time_in_audio() / audio_metadata.total_length));
  },
  time_left_in_audio: function () {
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
  current_volume: 1,
  buffer_source_nodes: []
}

function playPCM(arrayBuffer) { //plays interleaved linear PCM with 16 bit, bit depth
  const int32array = new Int32Array(arrayBuffer);
  const channels = 2;
  const sampleRate = 48000;
  const length = int32array.length;
  const buffer = player.context.createBuffer(channels, length + 1, sampleRate);

  const positiveSampleDivisor = Math.pow(2, 15) - 1;
  const negativeSampleDivisor = Math.pow(2, 15);
  //I opted to use decimal rather than hexadecimal to represent these for clarities sake
  //Consider how a normal signed byte has a range of values from 127 to -128,
  //i.e 0b01111111 (127) to 0b10000000 (-128),as the most significant bit (leftmost is -128)
  //We do basically the same but considering the fact that WebAudioAPI takes in Float32 values in the range -1 to 1,
  //so we divide Int32 values by those sample divisors to get a number in the necessary range
  const normaliser = (sample) => sample > 0 ? sample / positiveSampleDivisor : sample / negativeSampleDivisor;

  //the +1 used below, and the +1 in the length above fixes the clicking issue - I can only 
  //assume that one sample (if non zero) is distorted if the source stops at that sample
  const floatsL = new Float32Array(int32array.length + 1);
  floatsL.forEach((_, index) => floatsL[index] = 0);
  const floatsR = new Float32Array(int32array.length + 1);
  floatsR.forEach((_, index) => floatsR[index] = 0);

  for (let i = 0; i < int32array.length; i++) {
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

  const current_time = player.context.currentTime;
  if (player.current_page_time < current_time) { //ensures that the current time never lags behind context.currentTime
    player.current_page_time = current_time + 0.1;
  }

  const source = player.context.createBufferSource();
  source.buffer = buffer;
  source.start(player.current_page_time);

  for (let i = 0; i < player.buffer_source_nodes.length; i++) { // removes old elements
    if (player.buffer_source_nodes[i].time < current_time) {
      player.buffer_source_nodes.splice(i, 1)
    }
  }
  player.buffer_source_nodes.push({ time: player.current_page_time, node: source });

  player.current_page_time += Math.round(buffer.duration * 100) / 100; //2dp

  source.connect(player.gain_node)
  player.gain_node.connect(player.analyser_node)
  player.analyser_node.connect(player.context.destination);
}

function clear_queued_chunks() {
  const current_time = player.context.currentTime;
  for (const el of player.buffer_source_nodes) {
    el.node.stop(current_time)
    el.node.disconnect()
  }
  player.current_page_time = player.context.currentTime
  player.buffer_source_nodes = []
}

async function playMusic(typedArrayCurrent) { //takes 1 packet of audio, decode, and then plays it
  let allocatedCurrentBufferPtr = 0, allocatedPreviousBufferPtr = 0;
  let decodedCurrentBufferPtr;

  try {
    allocatedCurrentBufferPtr = Module._malloc(typedArrayCurrent.length * typedArrayCurrent.BYTES_PER_ELEMENT);
    Module.HEAPU8.set(typedArrayCurrent, allocatedCurrentBufferPtr);

    if (player.typed_array_previous) {
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

    // slice creates a copy of the data, so we don't need this ptr after this bit, hence freed below
    const outputArrayBuffer = Module.HEAP8.slice(decodedCurrentBufferPtr, decodedCurrentBufferPtr + output_len).buffer;

    playPCM(outputArrayBuffer); //output array buffer contains decoded audio
  } catch (err) {
    console.log("Error: " + err);
  } finally {
    Module._free(allocatedPreviousBufferPtr);
    Module._free(allocatedCurrentBufferPtr);
    Module._free(decodedCurrentBufferPtr);

    player.typed_array_previous = typedArrayCurrent;
  }
}

let timeout_for_next_track = undefined
function update_playing(text, total_length, force) {
  clearTimeout(timeout_for_next_track)

  if (force) {
    audio_metadata.relative_start_time = time()
    audio_metadata.total_length = total_length
    updateMetadata(text);
    return player.playing_audio_el.innerHTML = text;
  }


  timeout_for_next_track = setTimeout(() => {
    if (player.playing_audio_el.innerHTML != "Loading...")
      audio_metadata.relative_start_time = time()

    audio_metadata.total_length = total_length
    player.playing_audio_el.innerHTML = text;
    updateMetadata(text);
  }, audio_metadata.time_left_in_audio()) // once this has finished, this is the next title
}

function end_audio() {
  player.current_page_time = 0;

  if (player.context)
    player.context.close();
  player.context = undefined

}

toggle_audio_timeout = undefined;
function toggleAudio(force_pause) {
  if (!player.audio_ws && force_pause) return; // return if already paused
  if (player.audio_ws) {
    dummy_audio_el.pause(); // stuff to control mediasession api

    player.audio_ws.close();
    player.audio_ws = undefined;
    player.gain_node.gain.linearRampToValueAtTime(0.00001, player.context.currentTime + 0.1)
    toggle_audio_timeout = setTimeout(() => {
      end_audio()
    }, 2000)
  } else {
    dummy_audio_el.play(); // stuff to control mediasession api

    if (audio_metadata.metadata_ws.readyState === WebSocket.CLOSED) { // if the metadata websocket has timed out for example, this should restart it
      stop_metadata_connection()
      start_metadata_connection()
    }

    clearTimeout(toggle_audio_timeout)
    end_audio()

    player.audio_ws = new WebSocket(`wss://${window.location.host}/ws/radio/${current_station_data.name}/audio_broadcast`)

    player.context = new AudioContext()
    player.analyser_node = createAnalyserNode(player.context)
    player.gain_node = createGainNode(player.context)
    set_volume(player.current_volume)

    player.audio_ws.onmessage = msg => {
      if (msg.data == "INVALID_ENDPOINT" || msg.data == "INVALID_STATION") {
        if (msg.data == "INVALID_STATION") {
          alert("Invalid station selected!");
        } else if (msg.data == "INVALID_ENDPOINT") {
          alert("Invalid websocket endpoint!");
        }
        return;
      }


      const audio_data = JSON.parse(msg.data)
      let prev_durations = 0;
      for (const page of audio_data.pages) {
        // i.e this is saying if the next audio page is up to 500ms before the current playback,
        // or more than 20 seconds in the past (i.e it's from the next track and the time has looped around)
        // then it's a valid page and play it, otherwise it is skipped
        // (this assumes that the tracks are at least around 30 seconds long)
        // this should prevent excessive drift from the metadata timing
        // (and also plays if just had a skip event)
        if (audio_data.start_offset + prev_durations > audio_metadata.time_in_audio() - 1000
          || audio_data.start_offset + prev_durations < audio_metadata.time_in_audio() - 20000) {
          arr = new Uint8Array(page.buff)
          playMusic(arr)
        }
        prev_durations += page.duration;
      }
    }
  }
}

function resize() {
  const dpi = window.devicePixelRatio * 1 //artificially making the DPI higher seems to make the canvas very high resolution
  const styleWidth = window.innerWidth;
  const styleHeight = (styleWidth * 9 / 21 > window.innerHeight * 0.4) ? window.innerHeight * 0.4 : styleWidth * 9 / 21; //keeps a 21:9 aspect ratio until it covers 40% of the vertical screen

  canvas.setAttribute('height', styleHeight * dpi)
  canvas.setAttribute('width', styleWidth * dpi)
  canvas.setAttribute('style', `width:${styleWidth}px;height:${styleHeight}px;top:${player_bar_holder.getBoundingClientRect().y + 0.1 * window.devicePixelRatio}px`)

  volume_control_container.style.top = `${window.innerHeight - 210}px`
}

window.addEventListener('resize', () => {
  resize();
})

function drawBars() {
  if (!player.analyser_node) return;

  const bufferLength = Math.min(player.analyser_node.frequencyBinCount, canvas.width / 15); // this way get a reasonable number of bars
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

  let font_size = 23 * window.devicePixelRatio;
  if (window.devicePixelRatio != 1)
    font_size = 20 * window.devicePixelRatio;

  ctx.font = `${font_size}px sans-serif`

  ctx.fillStyle = "rgba(255,255,255,0.7)"
  ctx.fillText(`${formatTime(Math.min(totalDuration, currentTime))}/${formatTime(totalDuration)}`, 15, canvas.height - font_size) //displays time and makes sure it's clamped to totalDuration (there are slight deviations)

  ctx.beginPath()
  ctx.moveTo(0, canvas.height)
  ctx.lineTo(canvas.width, canvas.height)
  ctx.strokeStyle = "rgba(0,0,0,0.3)"
  ctx.lineWidth = 20 * window.devicePixelRatio
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
  ctx.lineWidth = 20 * window.devicePixelRatio
  ctx.stroke()
  ctx.closePath()
}

function animationLoop() {
  ctx.clearRect(0, 0, canvas.width, canvas.height)

  drawBars()
  updateProgressBar()

  requestAnimationFrame(animationLoop)
}

function set_volume(vol) {
  const original_vol = vol;
  const normalised_vol = original_vol / 100;
  let volume = normalised_vol;

  // piecewise functions used for gradually changing volume
  // y = { 200 >= x > 150 : 1.04^(x-150)}
  // y = { 150 >= x >= 60 : x/100 - 0.5}
  // y = { 60 > x >= 0 : 0.0104566441123 * (1.04^x - 1)}

  if (original_vol > 150) {
    volume = Math.pow(1.04, original_vol - 150); // above 100, exponentially higher
  } else if (original_vol < 60) {
    volume = 0.0104566441123 * (Math.pow(1.04, original_vol) - 1);
  } else {
    volume -= 0.5;
  }

  if (player.context && player.gain_node && player.audio_ws != undefined)
    player.gain_node.gain.linearRampToValueAtTime(volume, player.context.currentTime + 0.1)

  player.current_volume = original_vol;
  window.localStorage.setItem("volume", original_vol);

  if (volume_control.value != original_vol) {
    volume_control.value = original_vol;
  }
}

function change_volume_by_value(value) {
  const current_vol = player.current_volume;
  const new_vol = Math.min(200, Math.max(0.001, Number(current_vol) + Number(value)));
  set_volume(new_vol);
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

  // some people were having issues since I changed the audio, this will mean the default audio is set to the correct value on their device
  let is_new_update = true;
  if (window.localStorage.getItem("update02oct2021") != "true") {
    is_new_update = false;
    window.localStorage.setItem("update02oct2021", "true");
  }

  // also volume doesn't default to 0
  // volume is set to 150 by default
  player.current_volume = window.localStorage.getItem("volume") && is_new_update ? Number(window.localStorage.getItem("volume")) : 150 // get volume from the local storage
  volume_control.value = Math.max(1, player.current_volume) // sets the volume_control element's value

  fetch("/broadcast_metadata").then(data => data.text()).then(metadata => {
    broadcast_metadata = Object.fromEntries(metadata.replace(/ /g, "").split("\n").map(item => [item.split(":")[0], Number(item.split(":")[1])]));
    update_broadcast_time(); // start updating the broadcast time
  })

  fetch("/station_list").then(data => data.json()).then(stations => {
    stations_list = stations["stations"];
    populate_stations_dropdown();
  });

  requestAnimationFrame(animationLoop);
})

function start_metadata_connection() {
  let just_started = true;
  audio_metadata.metadata_ws = new WebSocket(`wss://${window.location.host}/ws/radio/${current_station_data.name}/metadata_only`)
  audio_metadata.metadata_ws.onmessage = msg => {
    if (msg.data == "INVALID_ENDPOINT" || msg.data == "INVALID_STATION") {
      if (msg.data == "INVALID_STATION") {
        alert("Invalid station selected!");
      } else if (msg.data == "INVALID_ENDPOINT") {
        alert("Invalid websocket endpoint!");
      }
      return;
    }

    metadata = JSON.parse(msg.data)

    if (metadata.start_offset == 0) { // if the same shows up twice, time isn't reset
      update_playing(metadata.title, metadata.total_length);
      refresh_voted_for_stuff(); // if for example the voting box is still open
    }

    if (just_started)
      update_playing(metadata.title, metadata.total_length, true);

    if (metadata.skipped_track) {
      update_playing(metadata.title, metadata.total_length, true);
      if (pre_skip_gain_value != -1)
        set_volume(pre_skip_gain_value)
    }

    audio_metadata.time_ms = metadata.start_offset
    audio_metadata.title = metadata.title

    update_num_listeners(metadata.num_listeners);

    if (just_started) {
      just_started = false;
      audio_metadata.relative_start_time = -metadata.start_offset;
    }
  }
}

function stop_metadata_connection() {
  // radio connection to the other station starts now
  last_play_start_time = Date.now()
  player.playing_audio_el.innerHTML = "Loading...";
  if (audio_metadata.metadata_ws)
    audio_metadata.metadata_ws.close()
}

///// for switching stations

const current_station_data = {
  tracks: [],
  queued: [],
  name: window.localStorage.getItem("station")
};

function set_station(name) {
  fetch(`/audio_list/${name}`).then(data => data.text()).then(res => {
    current_station_data.tracks = res != "" ? res.split("/") : []
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
  if (e.target.id != "volume_control_container" && e.target.id != "volume_control" && volume_control_container.style.opacity == 1)
    toggle_volume_control()
})

const vote_modal_tracks = document.getElementById("vote_tracks");

document.getElementById("vote_btn").onclick = refresh_voted_for_stuff

function refresh_voted_for_stuff(data = undefined) {
  const func_body = (data) => {
    current_station_data.queued = data.split("/")?.filter(item => item != "").reverse()

    for (const el of vote_modal_tracks.children) {
      el.innerHTML = el.innerHTML.replace(/ <b style="float:right">\(Position: \d+\)<\/b>$/, "")

      if (current_station_data.queued.indexOf(el.innerText) != -1) {
        el.classList.add("voted_for");
        el.classList.remove("vote_track_item");
        el.innerHTML += ` <b style="float:right">(Position: ${current_station_data.queued.indexOf(el.innerText) + 1})</b>`
      } else {
        el.classList.remove("voted_for");
        el.classList.add("vote_track_item");
      }
    }
  }

  if (typeof data == "string" && data != "") {
    func_body(data);
  } else {
    fetch(`/audio_queue/${current_station_data.name}`).then(res => res.text()).then(data => {
      func_body(data);
    })
  }
}

function update_vote_modal() {
  vote_modal_tracks.innerHTML = current_station_data.tracks.map(track => `<div class='vote_track_item' onclick='vote_for(this.innerText)'>${track}</div>`).join("")
}

function vote_for(track_name) {
  fetch(`/audio_req/${current_station_data.name}/${track_name}`).then(res => res.text()).then(data => {
    if (data != "FAILURE")
      refresh_voted_for_stuff(data)
  })
}

const skip_button = document.getElementById("skip_button");
let pre_skip_gain_value = -1;
skip_button.addEventListener('click', () => [
  fetch(`/skip_track/${current_station_data.name}`).then(res => res.text()).then(data => {
    if (data.indexOf("FAILURE") == 0) {
      successful_action_popup(data.replace("FAILURE:", "Failure: "), true)
    } else {
      successful_action_popup("Succesfully skipped");
      pre_skip_gain_value = player.current_volume;
      set_volume(0); // to prevent clipping
      clear_queued_chunks();
      audio_metadata.total_length = 1;
    }
  })
])