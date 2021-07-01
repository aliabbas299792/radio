LIBDIR=$PWD/emscripten_opus
source ./emsdk/emsdk_env.sh #expects emsdk to be in this directory
emcc \
  main.c \
  -I$LIBDIR/include/opus \
  -lopus \
  -L$LIBDIR/lib \
  -s EXTRA_EXPORTED_RUNTIME_METHODS='["cwrap", "ccall"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s EXPORTED_FUNCTIONS="['_decodeOggPage', '_lengthOfOutput']" \
  -o "main.js"
cp main.js ../public
cp main.wasm ../public