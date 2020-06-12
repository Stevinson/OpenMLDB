#! /bin/sh
#
# compile.sh
PWD=`pwd`

if $(uname -a | grep -q Darwin); then
    JOBS=$(sysctl -n machdep.cpu.core_count)
else
    JOBS=$(grep -c ^processor /proc/cpuinfo 2>/dev/null)
fi

export PATH=${PWD}/thirdparty/bin:$PATH
rm -rf build
mkdir -p build && cd build && \
cmake .. -DCMAKE_BUILD_TYPE=Release -DBENCHMARK_ENABLE_LTO=true -DCOVERAGE_ENABLE=OFF -DTESTING_ENABLE=ON && \
make fesql_proto && make fesql_parser && \
make -j${JOBS} && 
make test -j${JOBS}
