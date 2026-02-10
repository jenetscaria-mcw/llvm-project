#include <cmath>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

// Thread Safety: Mutexes for File I/O
static pthread_mutex_t profile_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;

// Function Registry
struct AdaptiveFunctionEntry {
  const char *name;
  volatile int *complete_flag;
  volatile long long *cpu_cycles[4];    // Pointers to cycle counters
  volatile long long *cpu_cycles_sq[4]; // Pointers to squared cycle counters
  volatile int *run_count[4];           // Pointers to run counters
  volatile int *best_version;           // Pointer to best version storage
};

static std::vector<AdaptiveFunctionEntry> registered_functions;

// Profile File Path Management
static const char *get_profile_path() {
  const char *path = getenv("ADAPTIVE_PROFILE_PATH");
  return path ? path : "/tmp/adaptive_profiles.txt";
}

// Forward declaration
extern "C" void __adaptive_write_profile(const char *func_name,
                                         int best_version);

// Profiling Initialization and Finalization
// Called at program exit via atexit()
static void finalize_profiling() {
  pthread_mutex_lock(&registry_mutex);

  // fprintf(stderr, "\n=== ADAPTIVE PROFILING FINALIZATION ===\n");
  // fprintf(stderr, "Setting completion flag for %zu function(s)...\n",
  //         registered_functions.size());

  // Set profilingComplete = 1 for all registered functions
  // This triggers CAS-based finalization in wrappers
  for (auto &entry : registered_functions) {
    *(entry.complete_flag) = 1;
  }

  pthread_mutex_unlock(&registry_mutex);

  // Small delay to let in-flight profiling calls complete
  usleep(100000); // 100ms

  // Now calculate best version and write profile for each function
  pthread_mutex_lock(&registry_mutex);

  for (auto &entry : registered_functions) {
    // Load statistics atomically
    long long cycles[4], cycles_sq[4];
    int runs[4];

    for (int v = 0; v < 4; v++) {
      cycles[v] = __atomic_load_n(entry.cpu_cycles[v], __ATOMIC_ACQUIRE);
      cycles_sq[v] = __atomic_load_n(entry.cpu_cycles_sq[v], __ATOMIC_ACQUIRE);
      runs[v] = __atomic_load_n(entry.run_count[v], __ATOMIC_ACQUIRE);
    }

    // Calculate best version (lowest mean with CV < 50%)
    int best_version = 0;
    double best_mean = 1e308; // max double
    const double cv_threshold = 0.50;

    for (int v = 0; v < 4; v++) {
      if (runs[v] > 0) {
        double mean = (double)cycles[v] / runs[v];
        double mean_of_sq = (double)cycles_sq[v] / runs[v];
        double variance = mean_of_sq - (mean * mean);
        double std_dev = sqrt(variance > 0 ? variance : 0);
        double std_err = std_dev / sqrt((double)runs[v]);
        double cv = std_err / mean;

        if (cv < cv_threshold && mean < best_mean) {
          best_mean = mean;
          best_version = v;
        }
      }
    }

    // Store best version
    __atomic_store_n(entry.best_version, best_version, __ATOMIC_RELEASE);

    // Write to profile file
    __adaptive_write_profile(entry.name, best_version);
  }

  pthread_mutex_unlock(&registry_mutex);

  // fprintf(stderr, "Profiling finalized. Results in: %s\n",
  // get_profile_path()); fprintf(stderr,
  // "=======================================\n\n");
}

// Initialize profiling system (called from __adaptive_init)
extern "C" void __adaptive_init_profiling() {
  // Register cleanup handler to run at program exit
  atexit(finalize_profiling);

  // fprintf(stderr, "\n=== ADAPTIVE PROFILING MODE ===\n");
  // fprintf(stderr, "Environment: ADAPTIVE_MODE=PROFILE\n");
  // fprintf(stderr, "Profile file: %s\n", get_profile_path());
  // fprintf(stderr, "===============================\n\n");
}

// Register a function for profiling
extern "C" void __adaptive_register_function(
    const char *name, volatile int *complete_flag, volatile long long *cycles0,
    volatile long long *cycles1, volatile long long *cycles2,
    volatile long long *cycles3, volatile long long *cycles_sq0,
    volatile long long *cycles_sq1, volatile long long *cycles_sq2,
    volatile long long *cycles_sq3, volatile int *runs0, volatile int *runs1,
    volatile int *runs2, volatile int *runs3, volatile int *best_version) {
  pthread_mutex_lock(&registry_mutex);

  AdaptiveFunctionEntry entry;
  entry.name = name;
  entry.complete_flag = complete_flag;
  entry.cpu_cycles[0] = cycles0;
  entry.cpu_cycles[1] = cycles1;
  entry.cpu_cycles[2] = cycles2;
  entry.cpu_cycles[3] = cycles3;
  entry.cpu_cycles_sq[0] = cycles_sq0;
  entry.cpu_cycles_sq[1] = cycles_sq1;
  entry.cpu_cycles_sq[2] = cycles_sq2;
  entry.cpu_cycles_sq[3] = cycles_sq3;
  entry.run_count[0] = runs0;
  entry.run_count[1] = runs1;
  entry.run_count[2] = runs2;
  entry.run_count[3] = runs3;
  entry.best_version = best_version;
  registered_functions.push_back(entry);

  pthread_mutex_unlock(&registry_mutex);

#if ADAPTIVE_DEBUG
  fprintf(stderr, "Registered function: %s\n", name);
#endif
}

// Profile File I/O (Thread-Safe)
// Write profile entry (called from wrapper after CAS succeeds)
// THREAD-SAFE: Protected by mutex
extern "C" void __adaptive_write_profile(const char *func_name,
                                         int best_version) {
  pthread_mutex_lock(&profile_mutex);

  const char *profile_path = get_profile_path();
  FILE *file = fopen(profile_path, "a");

  if (file) {
    fprintf(file, "%s:%d\n", func_name, best_version);
    fflush(file); // Ensure immediate write
    fclose(file);

    // fprintf(stderr, "[PROFILE] %s -> V%d\n", func_name, best_version);
  } else {
    fprintf(stderr, "[ERROR] Cannot write to %s\n", profile_path);
    perror("fopen");
  }

  pthread_mutex_unlock(&profile_mutex);
}

// Read best version from profile file (for production mode)
// THREAD-SAFE: Protected by mutex
extern "C" int __adaptive_read_profile(const char *func_name) {
  pthread_mutex_lock(&profile_mutex);

  const char *profile_path = get_profile_path();
  FILE *file = fopen(profile_path, "r");
  int best_version = -1; // -1 = not found

  if (file) {
    char line[256];
    while (fgets(line, sizeof(line), file)) {
      char name[200];
      int version;

      // Parse line format: "function_name:version"
      if (sscanf(line, "%199[^:]:%d", name, &version) == 2) {
        if (strcmp(name, func_name) == 0) {
          best_version = version;
          break; // Found it
        }
      }
    }
    fclose(file);

    if (best_version >= 0) {
      fprintf(stderr, "[LOAD] %s -> V%d (from profile)\n", func_name,
              best_version);
    }
  }

  pthread_mutex_unlock(&profile_mutex);
  return best_version;
}

// Force finalization (useful for testing)
extern "C" void __adaptive_force_finalize() { finalize_profiling(); }
