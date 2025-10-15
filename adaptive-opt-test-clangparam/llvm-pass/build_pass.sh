#!/bin/bash

echo "Building SimpleAdaptive Pass..."

# Get LLVM configuration
LLVM_CONFIG=$(which llvm-config)
if [ -z "$LLVM_CONFIG" ]; then
    echo "Error: llvm-config not found!"
    exit 1
fi

LLVM_CXXFLAGS=$($LLVM_CONFIG --cxxflags)
LLVM_LDFLAGS=$($LLVM_CONFIG --ldflags)
LLVM_SYSTEM_LIBS=$($LLVM_CONFIG --system-libs)

# Compile the pass
echo "Compiling..."
clang++ -fPIC -shared \
    $LLVM_CXXFLAGS \
    $LLVM_LDFLAGS \
    -o SimpleAdaptive.so \
    SimpleAdaptive.cpp \
    $LLVM_SYSTEM_LIBS

# Link into shared library
echo "Built plugin: SimpleAdaptive.so"

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

echo
echo "Usage examples:"
echo "  opt -load-pass-plugin=./SimpleAdaptive.so -passes=simple-adaptive -disable-output input.ll"
echo "  clang -fpass-plugin=./SimpleAdaptive.so your.cpp -O2 -S -emit-llvm -o - | \\
        opt -passes=simple-adaptive -disable-output -"