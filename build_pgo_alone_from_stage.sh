
sleep 5
#if [ $# -ne 2 ]; then
#    echo "Usage: $0 <build_dir> <flag to be passed>"
#    exit 1
#fi


PROCESS_NAME=$1
shift
FLAGS=$@
CWD="$(pwd)"


export PATH="${CWD}/${PROCESS_NAME}_test_1/bin/":$PATH
llvm-profdata merge -output="${CWD}/${PROCESS_NAME}.prof" ${PROCESS_NAME}_test_2/profiles/*.profraw

export PATH="${CWD}/${PROCESS_NAME}_test_1/bin/":$PATH
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DLLVM_PROFDATA_FILE="${CWD}/${PROCESS_NAME}.prof" -DLLVM_ENABLE_PROJECTS=clang -DCMAKE_INSTALL_PREFIX=../install -S llvm -B "${PROCESS_NAME}_test_pgo"
cmake --build "${PROCESS_NAME}_test_pgo" --config Release -- -j8 #VERBOSE=1
