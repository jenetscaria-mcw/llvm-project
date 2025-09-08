./build_custom_pass.sh Threshold_200 -mllvm -inline-threshold=200 -mllvm -unroll-threshold=200 -mllvm -vectorize-loops=true -fno-semantic-interposition -fvisibility=hidden
./build_custom_pass.sh Threshold_300 -mllvm -inline-threshold=300 -mllvm -unroll-threshold=300 -mllvm -vectorize-loops=true -fno-semantic-interposition -fvisibility=hidden
./build_custom_pass.sh Threshold_400 -mllvm -inline-threshold=400 -mllvm -unroll-threshold=400 -mllvm -vectorize-loops=true -fno-semantic-interposition -fvisibility=hidden
./build_custom_pass.sh Threshold_500 -mllvm -inline-threshold=500 -mllvm -unroll-threshold=500 -mllvm -vectorize-loops=true -fno-semantic-interposition -fvisibility=hidden
./build_custom_pass.sh Threshold_600 -mllvm -inline-threshold=600 -mllvm -unroll-threshold=600 -mllvm -vectorize-loops=true -fno-semantic-interposition -fvisibility=hidden
./build_custom_pass.sh Aggressive -mllvm -inline-threshold=500 -mllvm -inlinehint-threshold=600 -mllvm -hot-callsite-threshold=700 -mllvm -unroll-threshold=600 -fno-semantic-interposition -fvisibility=hidden
./build_custom_pass.sh Balanced -mllvm -inline-threshold=350 -mllvm -hot-callsite-threshold=500 -mllvm -unroll-threshold=400 -fno-semantic-interposition -fvisibility=hidden
./build_pgo_with_fsave.sh stage_work_pgo_fsave -fsave-optimization-record
./build_master_with_fsave.sh stage_work_master_fsave -fsave-optimization-record


Threshold_200 -mllvm -inline-threshold=200 -mllvm -unroll-threshold=200 -mllvm -vectorize-loops=true -fno-semantic-interposition -fvisibility=hidden
Threshold_300 -mllvm -inline-threshold=300 -mllvm -unroll-threshold=300 -mllvm -vectorize-loops=true -fno-semantic-interposition -fvisibility=hidden
Threshold_400 -mllvm -inline-threshold=400 -mllvm -unroll-threshold=400 -mllvm -vectorize-loops=true -fno-semantic-interposition -fvisibility=hidden
Threshold_500 -mllvm -inline-threshold=500 -mllvm -unroll-threshold=500 -mllvm -vectorize-loops=true -fno-semantic-interposition -fvisibility=hidden
Threshold_600 -mllvm -inline-threshold=600 -mllvm -unroll-threshold=600 -mllvm -vectorize-loops=true -fno-semantic-interposition -fvisibility=hidden
Aggressive -mllvm -inline-threshold=500 -mllvm -inlinehint-threshold=600 -mllvm -hot-callsite-threshold=700 -mllvm -unroll-threshold=600 -fno-semantic-interposition -fvisibility=hidden
Balanced -mllvm -inline-threshold=350 -mllvm -hot-callsite-threshold=500 -mllvm -unroll-threshold=400 -fno-semantic-interposition -fvisibility=hidden
