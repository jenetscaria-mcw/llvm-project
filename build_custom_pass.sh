
sleep 5
#if [ $# -ne 2 ]; then
#    echo "Usage: $0 <build_dir> <flag to be passed>"
#    exit 1
#fi


PROCESS_NAME=$1
BUILD_DIR=$2
CWD="$(pwd)"



export PATH="${CWD}/${PROCESS_NAME}_test_1/bin/":$PATH
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DLLVM_PROFDATA_FILE="${CWD}/${PROCESS_NAME}.prof" -DLLVM_ENABLE_PROJECTS=clang -DCMAKE_INSTALL_PREFIX=../install -S llvm -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" --config Release -- -j8 VERBOSE=1
