
sleep 5
if [ $# -ne 2 ]; then
    echo "Usage: $0 <build_dir> <flag to be passed>"
    exit 1
fi


PROCESS_NAME=$1
shift
FLAGS=$@
CWD="$(pwd)"

#clang stage 1
cmake -G "Unix Makefiles" -DLLVM_ENABLE_PROJECTS="clang;compiler-rt" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=../install -S ./llvm -B "${PROCESS_NAME}_test_1"
cmake --build "${PROCESS_NAME}_test_1" --config Release -- -j12

#clang stage 2
export PATH="${CWD}/${PROCESS_NAME}_test_1/bin/":$PATH
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DLLVM_BUILD_INSTRUMENTED=IR -DLLVM_BUILD_RUNTIME=No -DLLVM_ENABLE_PROJECTS=clang -DCMAKE_INSTALL_PREFIX=../install -S llvm -B "${PROCESS_NAME}_test_2"
cmake --build "${PROCESS_NAME}_test_2" --config Release -- -j12

#clang stage 3
export PATH="${CWD}/${PROCESS_NAME}_test_2/bin/":$PATH
cmake -G "Unix Makefiles" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_FLAGS="-ftime-trace" -DCMAKE_CXX_FLAGS="-ftime-trace" -DCMAKE_BUILD_TYPE=Release -DLLVM_BUILD_RUNTIME=No -DLLVM_ENABLE_PROJECTS=clang -DCMAKE_INSTALL_PREFIX=../install -S llvm -B "${PROCESS_NAME}_test_3"
cmake --build "${PROCESS_NAME}_test_3" --config Release -- -j12

make -C "${PROCESS_NAME}_test_3" check-llvm
make -C "${PROCESS_NAME}_test_3" check-clang

#perf_data generation
llvm-profdata merge -output="${CWD}/${PROCESS_NAME}.prof" "${PROCESS_NAME}_test_2/profiles/*.profraw"

export PATH="${CWD}/${PROCESS_NAME}_test_1/bin/":$PATH
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_FLAGS="${FLAGS}" -DCMAKE_CXX_FLAGS="${FLAGS}" -DLLVM_PROFDATA_FILE="${CWD}/${PROCESS_NAME}.prof" -DLLVM_ENABLE_PROJECTS=clang -DCMAKE_INSTALL_PREFIX=../install -S llvm -B "${PROCESS_NAME}_test_pgo"
cmake --build "${PROCESS_NAME}_test_pgo" --config Release -- -j12