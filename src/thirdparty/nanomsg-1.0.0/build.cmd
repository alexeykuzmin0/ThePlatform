call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Enterprise\VC\Auxiliary\Build\vcvars64"
mkdir build
cd build
cmake ..
cmake --build .
ctest -C Debug .
cmake --build . --target install
