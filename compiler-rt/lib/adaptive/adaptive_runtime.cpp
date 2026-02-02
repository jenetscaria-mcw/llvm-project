#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <vector>

// Thread Safety: Mutexes for File I/O
static pthread_mutex_t profile_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;

// Function Registry
struct AdaptiveFunctionEntry
{
    const char *name;
    volatile int *complete_flag;
};

static std::vector<AdaptiveFunctionEntry> registered_functions;

// Profile File Path Management
static const char *get_profile_path()
{
    const char *path = getenv("ADAPTIVE_PROFILE_PATH");
    return path ? path : "/tmp/adaptive_profiles.txt";
}

// Profiling Initialization and Finalization
// Called at program exit via atexit()
static void finalize_profiling()
{
    pthread_mutex_lock(&registry_mutex);

    printf("\n=== ADAPTIVE PROFILING FINALIZATION ===\n");
    printf("Setting completion flag for %zu function(s)...\n",
           registered_functions.size());

    // Set profilingComplete = 1 for all registered functions
    // This triggers CAS-based finalization in wrappers
    for (auto &entry : registered_functions)
    {
        *(entry.complete_flag) = 1;
    }

    pthread_mutex_unlock(&registry_mutex);

    // Small delay to let in-flight profiling calls complete
    usleep(100000); // 100ms

    printf("Profiling finalized. Results in: %s\n", get_profile_path());
    printf("=======================================\n\n");
}

// Initialize profiling system (called from __adaptive_init)
extern "C" void __adaptive_init_profiling()
{
    // Register cleanup handler to run at program exit
    atexit(finalize_profiling);

    printf("\n=== ADAPTIVE PROFILING MODE ===\n");
    printf("Environment: ADAPTIVE_MODE=PROFILE\n");
    printf("Profile file: %s\n", get_profile_path());
    printf("===============================\n\n");
}

// Register a function for profiling
extern "C" void __adaptive_register_function(const char *name,
                                             volatile int *complete_flag)
{
    pthread_mutex_lock(&registry_mutex);

    AdaptiveFunctionEntry entry;
    entry.name = name;
    entry.complete_flag = complete_flag;
    registered_functions.push_back(entry);

    pthread_mutex_unlock(&registry_mutex);

#if ADAPTIVE_DEBUG
    printf("Registered function: %s\n", name);
#endif
}

// Profile File I/O (Thread-Safe)
// Write profile entry (called from wrapper after CAS succeeds)
// THREAD-SAFE: Protected by mutex
extern "C" void __adaptive_write_profile(const char *func_name, int best_version)
{
    pthread_mutex_lock(&profile_mutex);

    const char *profile_path = get_profile_path();
    FILE *file = fopen(profile_path, "a");

    if (file)
    {
        fprintf(file, "%s:%d\n", func_name, best_version);
        fflush(file); // Ensure immediate write
        fclose(file);

        printf("[PROFILE] %s -> V%d\n", func_name, best_version);
    }
    else
    {
        fprintf(stderr, "[ERROR] Cannot write to %s\n", profile_path);
        perror("fopen");
    }

    pthread_mutex_unlock(&profile_mutex);
}

// Read best version from profile file (for production mode)
// THREAD-SAFE: Protected by mutex
extern "C" int __adaptive_read_profile(const char *func_name)
{
    pthread_mutex_lock(&profile_mutex);

    const char *profile_path = get_profile_path();
    FILE *file = fopen(profile_path, "r");
    int best_version = -1; // -1 = not found

    if (file)
    {
        char line[256];
        while (fgets(line, sizeof(line), file))
        {
            char name[200];
            int version;

            // Parse line format: "function_name:version"
            if (sscanf(line, "%199[^:]:%d", name, &version) == 2)
            {
                if (strcmp(name, func_name) == 0)
                {
                    best_version = version;
                    break; // Found it
                }
            }
        }
        fclose(file);

        if (best_version >= 0)
        {
            printf("[LOAD] %s -> V%d (from profile)\n", func_name, best_version);
        }
    }

    pthread_mutex_unlock(&profile_mutex);
    return best_version;
}

// Force finalization (useful for testing)
extern "C" void __adaptive_force_finalize()
{
    finalize_profiling();
}
