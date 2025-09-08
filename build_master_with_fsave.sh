sleep 5

BUILD_DIR=$1
shift
FLAGS=$@
CWD="$(pwd)"

echo ${FLAGS}

export PATH="/mnt/Data/kamal/new_clone/llvm-project/stage_work_test_1/bin":$PATH

cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_FLAGS="${FLAGS}" -DCMAKE_CXX_FLAGS="${FLAGS}" -DLLVM_ENABLE_PROJECTS=clang -DCMAKE_INSTALL_PREFIX=../install -S llvm -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" --config Release -- -j8 #VERBOSE=1
