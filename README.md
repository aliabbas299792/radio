# Radio
### What it is
Using [this](https://github.com/aliabbas299792/web_server) web server, and [this](https://github.com/aliabbas299792/wasmOpusDecoder) WASM audio player along with some other assets from past projects this is a web based radio.<br>
You should be able to join in the browser and listen to a selection of seamlessly streamed audio files (streamed via websockets).

## Config
Put a `.config` file in the same directory as the server, the server is configured as such:
```
TLS: yes
FULLCHAIN: /home/me/ssl/fullchain.cer
PKEY: /home/me/ssl/example.abc.key

PORT: 80
TLS_PORT: 443

SERVER_THREADS: 10

RADIOS: { "station 1 name", "station 1 directory" }, { "station 2 name", "station 2 directory" }
```

## Stuff used
JSON (https://github.com/nlohmann/json.git)<br>
Thread safe queue (https://github.com/cameron314/readerwriterqueue)<br>
libcurl (for parsing URLs)<br>
OpenSSL (for base64 encoding)<br>
liburing (for the wrapper over io_uring)<br>
WolfSSL (for TLS)<br>
Opus (https://github.com/xiph/opus) (compiled to WebAssembly)<br>
Bootstrap 5.0 (for some modals/dropdowns in the UI)<br>
Google Fonts (for the icons in the UI)<br>
emsdk (https://github.com/emscripten-core/emsdk) (for the WebAssembly stuff)

## Known issues
- Sometimes requests don't seem to get through, though they do get right to the point of writing to a connection, but they fail to write completely - not sure if it's due to my nginx reverse proxy, browser or the actual server
- Sometimes it may crash when moving audio to a directory it is watching (due to a bad file descriptor - haven't seen it enough times to be able to find out where exactly the issue is)