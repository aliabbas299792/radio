LIBDIR=$PWD/emscripten_opus
source ./emsdk/emsdk_env.sh #expects emsdk to be in this directory
emcc \
  decode.c \
  -I$LIBDIR/include/opus \
  -lopus \
  -L$LIBDIR/lib \
  -O3 \
  -s EXPORTED_RUNTIME_METHODS='["cwrap", "ccall"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s EXPORTED_FUNCTIONS="['_decode_page', '_output_len', '_malloc', '_free']" \
  -o "main.js"
cp main.js ../public
cp main.wasm ../public