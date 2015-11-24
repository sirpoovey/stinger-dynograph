#include <stdint.h>

#include <stinger_bench.h>
#include <hooks.h>

#include <stinger.h>
#include <kcore.h>

int main(int argc, char **argv)
{
    // Create the stinger data structure
    stinger_t * S = stinger_new();

    // Load raw data in from file
    load_graph(S, argv[1]);

    // Get number of vertices
    uint64_t num_vertices = stinger_max_nv(S) + 1;

    // Pre-allocate output data structures
    int64_t *labels = xmalloc (num_vertices * sizeof(int64_t));
    int64_t *counts = xmalloc (num_vertices * sizeof(int64_t));
    int64_t k = 0;

    // Run the benchmark
    bench_start();
    kcore_find(S, labels, counts, num_vertices, &k);
    bench_end();

    // Clean up
    free (labels);
    free (counts);
    stinger_free_all (S);
    return 0;
}
