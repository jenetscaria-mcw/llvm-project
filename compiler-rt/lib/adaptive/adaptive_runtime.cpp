#include <cmath>
#include <pthread.h>
#include <signal.h>
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

  // Paired t-test data
  volatile long long *pair_diff_sum[3];    // Sum of (V_i - V0) for i=1,2,3
  volatile long long *pair_diff_sq_sum[3]; // Sum of (V_i - V0)^2
  volatile int *round_count;               // Number of complete rounds
};

static std::vector<AdaptiveFunctionEntry> &get_registry() {
  static std::vector<AdaptiveFunctionEntry> *registry =
      new std::vector<AdaptiveFunctionEntry>();
  return *registry;
}

// Profile File Path Management
static const char *get_profile_path() {
  const char *path = getenv("ADAPTIVE_PROFILE_PATH");
  return path ? path : "/tmp/adaptive_profiles.txt";
}

// Forward declaration
extern "C" void __adaptive_write_profile(const char *func_name,
                                         int best_version);

static int select_best_version(const long long cycles[4],
                               const long long cycles_sq[4],
                               const int runs[4]) {
  double mean[4];
  double cv[4];
  double stderr_arr[4];

  constexpr double MaxVal = 1e308;
  constexpr double CVThreshold = 0.50;
  constexpr double MinImprovement = 0.10; // 10%
  constexpr double TThreshold = 1.96;

  for (int v = 0; v < 4; v++) {
    if (runs[v] <= 0) {
      mean[v] = MaxVal;
      cv[v] = MaxVal;
      stderr_arr[v] = 0.0;
      continue;
    }

    const double runs_f64 = (double)runs[v];
    const double mean_of_cycles = (double)cycles[v] / runs_f64;
    const double mean_of_sq = (double)cycles_sq[v] / runs_f64;
    double variance = mean_of_sq - (mean_of_cycles * mean_of_cycles);

    // Outlier-robust: clamp variance if it exceeds 4x mean^2
    // (indicates context-switch outliers inflating squared-cycle sums)
    const double max_reasonable_var = 4.0 * mean_of_cycles * mean_of_cycles;
    if (variance > max_reasonable_var)
      variance = max_reasonable_var;
    if (variance < 0.0)
      variance = 0.0;

    const double std_dev = sqrt(variance);
    const double std_err = std_dev / sqrt(runs_f64);
    const double safe_mean = mean_of_cycles > 0.0 ? mean_of_cycles : 1.0;

    mean[v] = mean_of_cycles;
    stderr_arr[v] = std_err;
    cv[v] = std_err / safe_mean;
  }

  int best_version = 0;
  double best_mean = mean[0];
  double best_cv = cv[0];
  double best_stderr = stderr_arr[0];

  for (int v = 1; v < 4; v++) {
    const bool current_reliable = cv[v] < CVThreshold;
    const bool best_reliable = best_cv < CVThreshold;
    const bool is_faster = mean[v] < best_mean;
    const double improvement_ratio =
        best_mean > 0.0 ? (best_mean - mean[v]) / best_mean : 0.0;
    const bool significant_improvement = improvement_ratio > MinImprovement;

    const double pooled_stderr =
        sqrt(stderr_arr[v] * stderr_arr[v] + best_stderr * best_stderr);
    const bool has_valid_stderr = pooled_stderr > 0.0;
    const double t_statistic =
        has_valid_stderr ? (best_mean - mean[v]) / pooled_stderr : 0.0;
    const bool welch_significant = has_valid_stderr && t_statistic > TThreshold;

    const bool statutory_significant =
        significant_improvement || welch_significant;
    const bool both_reliable_and_significant =
        current_reliable && best_reliable && is_faster && statutory_significant;
    const bool only_current_reliable = current_reliable && !best_reliable;

    if (both_reliable_and_significant || only_current_reliable) {
      best_version = v;
      best_mean = mean[v];
      best_cv = cv[v];
      best_stderr = stderr_arr[v];
    }
  }

  return best_version;
}

// Paired t-test based selection: uses per-round (V_i - V0) differences
// Provides more statistical power by controlling for input/system variation
static int select_best_version_paired(const long long pair_diff_sum[3],
                                      const long long pair_diff_sq_sum[3],
                                      int rounds) {
  if (rounds < 30)
    return -1; // Not enough rounds for paired test

  constexpr double TThreshold = 1.96;
  constexpr double MinImprovement = 0.10; // 10%

  int best_version = 0; // V0 is the default (baseline)
  double best_t = 0.0;

  for (int i = 0; i < 3; i++) {
    // Paired statistics for (V_{i+1} - V0)
    const double n = (double)rounds;
    const double mean_diff = (double)pair_diff_sum[i] / n;
    const double mean_diff_sq = (double)pair_diff_sq_sum[i] / n;
    double variance = mean_diff_sq - (mean_diff * mean_diff);

    // Outlier-robust: clamp variance
    const double max_var = 4.0 * mean_diff * mean_diff;
    if (variance > max_var && max_var > 0.0)
      variance = max_var;
    if (variance < 0.0)
      variance = 0.0;

    const double std_err = sqrt(variance / n);
    const double t_stat =
        (std_err > 0.0) ? mean_diff / std_err : 0.0;

    // Negative mean_diff means V_{i+1} is FASTER than V0
    // We want t_stat < -TThreshold (significantly faster)
    if (t_stat < -TThreshold && mean_diff < 0.0) {
      // Check minimum improvement threshold against absolute diff
      // mean_diff is negative, so -mean_diff is positive improvement
      if (t_stat < best_t) {
        best_t = t_stat;
        best_version = i + 1;
      }
    }
  }

  return best_version;
}

// Called at program exit via atexit()
static void finalize_profiling() {
  static int finalized = 0;
  pthread_mutex_lock(&registry_mutex);
  if (finalized) {
    pthread_mutex_unlock(&registry_mutex);
    return;
  }
  finalized = 1;

  // fprintf(stderr, "\n=== ADAPTIVE PROFILING FINALIZATION (%zu functions) ===\n",
  //         get_registry().size());

  // Set profilingComplete = 1 for all registered functions
  // This triggers CAS-based finalization in wrappers
  for (auto &entry : get_registry()) {
    *(entry.complete_flag) = 1;
  }

  pthread_mutex_unlock(&registry_mutex);

  // Small delay to let in-flight profiling calls complete
  usleep(100000); // 100ms

  // Now calculate best version and write profile for each function
  pthread_mutex_lock(&registry_mutex);

  for (auto &entry : get_registry()) {
    // Load statistics atomically
    long long cycles[4], cycles_sq[4];
    int runs[4];

    for (int v = 0; v < 4; v++) {
      cycles[v] = __atomic_load_n(entry.cpu_cycles[v], __ATOMIC_ACQUIRE);
      cycles_sq[v] = __atomic_load_n(entry.cpu_cycles_sq[v], __ATOMIC_ACQUIRE);
      runs[v] = __atomic_load_n(entry.run_count[v], __ATOMIC_ACQUIRE);
    }

    int total_runs = 0;
    for (int v = 0; v < 4; v++) {
      total_runs += runs[v];
    }

    if (total_runs == 0)
      continue;

    int best_version = select_best_version(cycles, cycles_sq, runs);

    // Try paired t-test if we have enough round data
    int round_count = __atomic_load_n(entry.round_count, __ATOMIC_ACQUIRE);
    if (round_count >= 30) {
      long long pdiff[3], pdiff_sq[3];
      for (int i = 0; i < 3; i++) {
        pdiff[i] =
            __atomic_load_n(entry.pair_diff_sum[i], __ATOMIC_ACQUIRE);
        pdiff_sq[i] =
            __atomic_load_n(entry.pair_diff_sq_sum[i], __ATOMIC_ACQUIRE);
      }
      int paired_best =
          select_best_version_paired(pdiff, pdiff_sq, round_count);
      if (paired_best >= 0)
        best_version = paired_best; // Prefer paired result
    }

    // Print detailed stats for each version (requested by user)
    for (int v = 0; v < 4; v++) {
      double avg = runs[v] > 0 ? (double)cycles[v] / runs[v] : 0.0;
      double mean_sq = runs[v] > 0 ? (double)cycles_sq[v] / runs[v] : 0.0;
      double var = mean_sq - (avg * avg);
      double std_dev = sqrt(var > 0.0 ? var : 0.0);
      double std_err = runs[v] > 0 ? std_dev / sqrt((double)runs[v]) : 0.0;
      double cv = avg > 0.0 ? std_err / avg : 0.0;

      // fprintf(stderr,
      //         "  [%s] V%d: total_cycles=%llu, runs=%u, avg=%.0f, stderr=%.0f, "
      //         "cv=%.1f%%\n",
      //         entry.name, v, cycles[v], runs[v], avg, std_err, cv * 100.0);
    }

    // Store best version
    __atomic_store_n(entry.best_version, best_version, __ATOMIC_RELEASE);

    // Write to profile file
    __adaptive_write_profile(entry.name, best_version);
    // fprintf(stderr, "  [PROFILE] %s -> V%d\n", entry.name, best_version);
  }

  pthread_mutex_unlock(&registry_mutex);

  // fprintf(stderr, "Profiling finalized. Results in: %s\n", get_profile_path());
  // fprintf(stderr, "=======================================\n\n");
}

static void signal_handler(int sig) {
  fprintf(stderr, "\nCaught signal %d, finalizing profiling...\n", sig);
  finalize_profiling();
  _exit(128 + sig);
}

// Initialize profiling system (called from __adaptive_init)
extern "C" void __adaptive_init_profiling() {
  static int initialized = 0;
  pthread_mutex_lock(&registry_mutex);
  if (!initialized) {
    // Register cleanup handler to run at program exit
    atexit(finalize_profiling);

    // Register signal handlers for common termination signals
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Truncate the profile file so each profiling run starts fresh.
    // This prevents stale/duplicate entries from accumulating across runs.
    {
      const char *profile_path = get_profile_path();
      FILE *f = fopen(profile_path, "w");
      if (f)
        fclose(f);
    }

    initialized = 1;

    // fprintf(stderr, "\n=== ADAPTIVE PROFILING MODE ENABLED ===\n");
  }
  pthread_mutex_unlock(&registry_mutex);

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
    volatile int *runs2, volatile int *runs3, volatile int *best_version,
    volatile long long *pdiff0, volatile long long *pdiff1,
    volatile long long *pdiff2, volatile long long *pdiffsq0,
    volatile long long *pdiffsq1, volatile long long *pdiffsq2,
    volatile int *round_count) {
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
  entry.pair_diff_sum[0] = pdiff0;
  entry.pair_diff_sum[1] = pdiff1;
  entry.pair_diff_sum[2] = pdiff2;
  entry.pair_diff_sq_sum[0] = pdiffsq0;
  entry.pair_diff_sq_sum[1] = pdiffsq1;
  entry.pair_diff_sq_sum[2] = pdiffsq2;
  entry.round_count = round_count;
  get_registry().push_back(entry);

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
    char line[4096];
    while (fgets(line, sizeof(line), file)) {
      line[strcspn(line, "\r\n")] = '\0';
      char *sep = strrchr(line, ':');
      if (!sep)
        continue;
      *sep = '\0';
      int version = atoi(sep + 1);
      if (version < 0 || version > 3)
        continue;
      if (strcmp(line, func_name) == 0)
        best_version = version;
    }
    fclose(file);

    // if (best_version >= 0) {
    //   fprintf(stderr, "[LOAD] %s -> V%d (from profile)\n", func_name,
    //           best_version);
    // }
  }

  pthread_mutex_unlock(&profile_mutex);
  return best_version;
}

// Force finalization (useful for testing)
extern "C" void __adaptive_force_finalize() { finalize_profiling(); }
