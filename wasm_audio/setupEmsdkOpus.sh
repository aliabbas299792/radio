#Will create and compile a test WASM project, using emsdk version 1.38.45
LIBDIR=$PWD/emscripten_opus
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install 1.38.45
./emsdk activate 1.38.45
source ./emsdk_env.sh
cd ..
git clone https://github.com/xiph/opus.git
cd opus
./autogen.sh
emconfigure ./configure EMCONFIGURE_JS=1 --disable-intrinsics --disable-rtcd CFAGS='-02' --disable-asm --disable-extra-programs --prefix $LIBDIR
emmake make
emmake make install