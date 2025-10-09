#!/bin/bash

echo "=== Complete Test Pipeline ==="
echo

# Step 1: Compile to LLVM IR
echo "Step 1: Compiling to LLVM IR..."
clang++ -S -emit-llvm -O0 -Xclang -disable-O0-optnone \
    test-programs/test_matrix.cpp \
    -o build/test_matrix.ll

echo "Generated functions:"
grep "define.*AdaptiveFuncTest" build/test_matrix.ll | head -1
echo

# Step 2: Apply the adaptive pass
echo "Step 2: Applying SimpleAdaptive pass..."
opt -load-pass-plugin=./llvm-pass/SimpleAdaptive.so -passes=simple-adaptive \
    build/test_matrix.ll \
    -S -o build/test_matrix_adaptive.ll

echo "After transformation:"
grep "define.*AdaptiveFuncTest" build/test_matrix_adaptive.ll
echo
grep "define.*original" build/test_matrix_adaptive.ll
echo
# Step 3: Optimize each version separately
echo "Step 3: Optimizing versions..."
opt -O2 build/test_matrix_adaptive.ll -S -o build/test_matrix_opt.ll

# Step 4: Compile to executable
echo "Step 4: Creating executable..."
clang++ build/test_matrix_opt.ll -o build/test_adaptive

# Step 5: Run the test
echo "Step 5: Running test..."
echo "=============================="
#./build/test_adaptive