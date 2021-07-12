#Will create and compile a test WASM project, using emsdk version latest-upstream
LIBDIR=$PWD/emscripten_opus
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest-upstream
./emsdk activate latest-upstream
source ./emsdk_env.sh
cd ..
git clone https://github.com/xiph/opus.git
cd opus
./autogen.sh
emconfigure ./configure EMCONFIGURE_JS=1 --disable-intrinsics --disable-rtcd CFAGS='-02' --disable-asm --disable-extra-programs --disable-stack-protector --prefix $LIBDIR
emmake make
emmake make install