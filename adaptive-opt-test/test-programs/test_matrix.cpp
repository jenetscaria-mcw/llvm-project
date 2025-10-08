#include <iostream>
#include <chrono>
#include <cstring>
#include <execinfo.h>
#include <cxxabi.h>

/* void print_current_function() {
    void* addr[1];
    backtrace(addr, 1);           // get return address
    char** syms = backtrace_symbols(addr, 1);
    // syms[0] has mangled symbol + offset + module
    int status;
    char* demangled = abi::__cxa_demangle(syms[0], 0, 0, &status);
    std::cout << (status == 0 ? demangled : syms[0]) << "\n";
    free(demangled);
    free(syms);
} */

extern "C" {  // Use C linkage for simpler function names
    void matrix_multiply(float* A, float* B, float* C, int N) {
        //print_current_function();
        for (int i = 0; i < N; i++) {
            for (int j = 0; j < N; j++) {
                float sum = 0;
                for (int k = 0; k < N; k++) {
                    sum += A[i*N + k] * B[k*N + j];
                }
                C[i*N + j] = sum;
            }
        }
    }
    
    // Declare the performance summary function created by the LLVM pass
    void print_performance_summary();
}

int main() {
    const int N = 50;
    float *A = new float[N*N];
    float *B = new float[N*N];
    float *C = new float[N*N];
    
    // Initialize
    for (int i = 0; i < N*N; i++) {
        A[i] = i * 0.01f;
        B[i] = i * 0.02f;
    }
    
    std::cout << "Testing Adaptive Optimization\n";
    std::cout << "=============================\n\n";
    
    // Call 200 times to trigger version switch at 100
    for (int iter = 0; iter < 200; iter++) {
        memset(C, 0, N*N*sizeof(float));  // Clear C
        
        auto start = std::chrono::high_resolution_clock::now();
        matrix_multiply(A, B, C, N);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        if (iter == 0) {
            std::cout << "Call 1 (using v0): " << duration.count() << " µs\n";
        } else if (iter == 99) {
            std::cout << "Call 100 (last v0): " << duration.count() << " µs\n";
        } else if (iter == 100) {
            std::cout << "Call 101 (first v1): " << duration.count() << " µs\n";
        } else if (iter == 199) {
            std::cout << "Call 200 (using v1): " << duration.count() << " µs\n";
        }
    }
    
    // Verify result
    std::cout << "\nResult sample: C[0][0] = " << C[0] << "\n";
    
    // Print performance summary
    print_performance_summary();
    
    delete[] A;
    delete[] B;
    delete[] C;
    
    return 0;
}