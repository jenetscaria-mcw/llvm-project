#include <iostream>
#include <chrono>
#include <cstring>
#include <execinfo.h>
#include <cxxabi.h>

extern "C" {  // Use C linkage for simpler function names
    __attribute__((adaptive))
    void AdaptiveFuncTest(float* A, float* B, float* C, int N) {
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
    for (int iter = 0; iter < 10; iter++) {
        memset(C, 0, N*N*sizeof(float));  // Clear C
        
        auto start = std::chrono::high_resolution_clock::now();
        AdaptiveFuncTest(A, B, C, N);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        std::cout << "Call "<< iter+1 <<  " (using v " << iter%2 << "):" << duration.count() << " µs\n";
    }
    
    // Verify result
    std::cout << "\nResult sample: C[0][0] = " << C[0] << "\n";
    
    delete[] A;
    delete[] B;
    delete[] C;
    
    return 0;
}