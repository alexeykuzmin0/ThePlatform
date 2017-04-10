call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Enterprise\VC\Auxiliary\Build\vcvars64"
cd cmake
mkdir build & cd build

mkdir release & cd release
cmake -G "NMake Makefiles" ^
 -DCMAKE_BUILD_TYPE=Release ^
 -DCMAKE_INSTALL_PREFIX=../../../../install ^
 -Dprotobuf_BUILD_TESTS=OFF ^
 ../..
nmake
