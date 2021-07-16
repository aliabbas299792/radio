export CXX=/usr/bin/clang++
SOURCE_FILES=$(find . -type d \( -path ./build -o -path ./src/vendor -o -path ./wasm_audio \) -prune -false -o \( -name *.cpp -o -name *.tcc -o -name *.h \) | sed -E 's:\.\/src\/(.*):\1:g' | tr '\r\n' ' ')
# above will go through all of the directories, except those specified, and find all .cpp, .h and .tcc files,
# and make the output into a space separated string of paths
cd src
rm -rf server
cmake \
  -B ../build \
  -DSOURCE_FILES="$SOURCE_FILES" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=1
cd ..
cd build && make
cp compile_commands.json .. # for clangd
cp -f webserver ../server
cd ..