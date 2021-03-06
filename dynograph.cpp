/*
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <vector>
#include <map>
#include <iostream>
#include <memory>
#include <string>
#include <sstream>
#include <stdint.h>
#include <cmath>

#include <hooks.h>
#include <dynograph_util.hh>

extern "C" {
#include <stinger_core/stinger.h>
#include <stinger_core/core_util.h>
#include <stinger_alg/bfs.h>
#include <stinger_alg/betweenness.h>
#include <stinger_alg/clustering.h>
#include <stinger_alg/static_components.h>
#include <stinger_alg/kcore.h>
#include <stinger_alg/pagerank.h>
}
#include <stinger_alg/streaming_algorithm.h>
#include <stinger_alg/dynamic_betweenness.h>
#include <stinger_alg/dynamic_bfs.h>
#include <stinger_alg/dynamic_static_components.h>
#include <stinger_alg/dynamic_clustering.h>
#include <stinger_alg/dynamic_kcore.h>
#include <stinger_alg/dynamic_pagerank.h>
#include <stinger_alg/dynamic_simple_communities.h>
#include <stinger_alg/dynamic_simple_communities_updating.h>
#include <stinger_alg/dynamic_streaming_connected_components.h>
#include <stinger_net/stinger_alg.h>

#include <stinger_core/stinger_batch_insert.h>

using std::cerr;
using std::shared_ptr;
using std::make_shared;
using std::string;
using std::stringstream;
using std::vector;
using std::map;

using namespace gt::stinger;
using DynoGraph::msg;

struct StingerGraph
{
    stinger_t * S;

    StingerGraph(int64_t nv)
    {
        stinger_config_t config = generate_stinger_config(nv);
        S = stinger_new_full(&config);
    }
    ~StingerGraph() { stinger_free(S); }

    // Prevent copy
    StingerGraph(const StingerGraph& other)            = delete;
    StingerGraph& operator=(const StingerGraph& other) = delete;

    // Figure out how many edge blocks we can allocate to fill STINGER_MAX_MEMSIZE
    // Assumes we need just enough room for nv vertices and puts the rest into edge blocks
    // Basically implements calculate_stinger_size() in reverse
    static stinger_config_t generate_stinger_config(int64_t nv) {

        // Start with size we will try to fill
        // Scaled by 75% because that's what stinger_new_full does
        uint64_t sz = ((uint64_t)stinger_max_memsize() * 3)/4;

        // Subtract storage for vertices
        sz -= stinger_vertices_size(nv);
        sz -= stinger_physmap_size(nv);

        // Assume just one etype and vtype
        int64_t netypes = 1;
        int64_t nvtypes = 1;
        sz -= stinger_names_size(netypes);
        sz -= stinger_names_size(nvtypes);

        // Leave room for the edge block tracking structures
        sz -= sizeof(stinger_ebpool);
        sz -= sizeof(stinger_etype_array);

        // Finally, calculate how much room is left for the edge blocks themselves
        int64_t nebs = sz / (sizeof(stinger_eb) + sizeof(eb_index_t));

        stinger_config_t config = {
                nv,
                nebs,
                netypes,
                nvtypes,
                0, //size_t memory_size;
                0, //uint8_t no_map_none_etype;
                0, //uint8_t no_map_none_vtype;
                1, //uint8_t no_resize;
        };

        return config;
    }

#ifdef USE_STINGER_BATCH_INSERT

    struct EdgeAdapter : public DynoGraph::Edge
    {
        int64_t result;
        EdgeAdapter(){}
        EdgeAdapter(const DynoGraph::Edge e) : DynoGraph::Edge(e) { result = 0; }
        typedef EdgeAdapter edge;
        static int64_t get_type(const edge &u) { return 0; }
        static void set_type(edge &u, int64_t v) {  }
        static int64_t get_source(const edge &u) { return u.src; }
        static void set_source(edge &u, int64_t v) { u.src = v; }
        static int64_t get_dest(const edge &u) { return u.dst; }
        static void set_dest(edge &u, int64_t v) { u.dst = v; }
        static int64_t get_weight(const edge &u) { return u.weight; }
        static int64_t get_time(const edge &u) { return u.timestamp; }
        static int64_t get_result(const edge& u) { return u.result; }
        static void set_result(edge &u, int64_t v) { u.result = v; }
    };

    void
    insert(DynoGraph::Batch& batch)
    {
        const size_t batch_size = std::distance(batch.begin(), batch.end());
        std::vector<EdgeAdapter> updates(batch_size);
        OMP("omp parallel for")
        for (size_t i = 0; i < updates.size(); ++i)
        {
            DynoGraph::Edge &e = *(batch.begin() + i);
            updates[i] = EdgeAdapter(e);
        }
        Hooks &hooks = Hooks::getInstance();
        hooks.region_begin("insertions");
        hooks.traverse_edges(updates.size());
        if (batch.dataset.isDirected())
        { stinger_batch_incr_edges<EdgeAdapter>(S, updates.begin(), updates.end()); }
        else
        { stinger_batch_incr_edge_pairs<EdgeAdapter>(S, updates.begin(), updates.end()); }
        hooks.region_end();
    }

#else
    void
    insert(DynoGraph::Batch& batch)
    {
        // Insert the edges in parallel
        const int64_t type = 0;
        const bool directed = batch.dataset.isDirected();
        Hooks::getInstance().region_begin("insertions");

        int64_t chunksize = 8192;
        OMP("omp parallel for schedule(dynamic, chunksize)")
        for (auto e = batch.begin(); e < batch.end(); ++e)
        {
            if (directed)
            {
                stinger_incr_edge     (S, type, e->src, e->dst, e->weight, e->timestamp);
            } else { // undirected
                stinger_incr_edge_pair(S, type, e->src, e->dst, e->weight, e->timestamp);
            }
            Hooks::getInstance().traverse_edges(1);
        }
        Hooks::getInstance().region_end();
    }
#endif

    // Deletes edges that haven't been modified recently
    void
    deleteOlderThan(int64_t threshold)
    {
        Hooks &hooks = Hooks::getInstance();
        hooks.region_begin("deletions");
        STINGER_RAW_FORALL_EDGES_OF_ALL_TYPES_BEGIN(S)
        {
            hooks.traverse_edges(1);
            if (STINGER_EDGE_TIME_RECENT < threshold) {
                // Record the deletion
                stinger_edge_update u;
                u.source = STINGER_EDGE_SOURCE;
                u.destination = STINGER_EDGE_DEST;
                u.weight = STINGER_EDGE_WEIGHT;
                u.time = STINGER_EDGE_TIME_RECENT;
                // Delete the edge
                update_edge_data_and_direction (S, current_eb__, i__, ~STINGER_EDGE_DEST, 0, 0, STINGER_EDGE_DIRECTION, EDGE_WEIGHT_SET);
            }
        }
        STINGER_RAW_FORALL_EDGES_OF_ALL_TYPES_END();
        hooks.region_end();
    }

    void printSize()
    {
        size_t stinger_bytes = calculate_stinger_size(S->max_nv, S->max_neblocks, S->max_netypes, S->max_nvtypes).size;
        cerr << DynoGraph::msg <<
             "Initialized stinger with storage for "
             << S->max_nv << " vertices and "
             << S->max_neblocks * STINGER_EDGEBLOCKSIZE << " edges.\n";
        cerr.precision(4);
        cerr << DynoGraph::msg <<
             "Stinger is consuming " << (double)stinger_bytes / (1024*1024*1024) << "GB of RAM\n";
    }
};

class StingerAlgorithm
{
protected:
    shared_ptr<IDynamicGraphAlgorithm> impl;
    stinger_registered_alg data;
    vector<uint8_t> alg_data;
public:
    StingerAlgorithm(stinger_t * S, string name) :
    // Create the implementation
    impl(createImplementation(name)),
    // Zero-initialize the server data
    data{},
    // Allocate data for the algorithm
    alg_data(impl->getDataPerVertex() * S->max_nv)
    {
        // Initialize the "server" data about this algorithm
        strcpy(data.alg_name, impl->getName().c_str());
        data.stinger = S;
        data.alg_data_per_vertex = impl->getDataPerVertex();
        data.alg_data = alg_data.data();
        data.enabled = true;
    }

    static shared_ptr<IDynamicGraphAlgorithm> createImplementation(string name)
    {
        if        (name == "bc") {
            return make_shared<BetweennessCentrality>(128, 0.5, 1);
        } else if (name == "bfs") {
            return make_shared<BreadthFirstSearch>();
        } else if (name == "cc") {
            return make_shared<ConnectedComponents>();
        } else if (name == "clustering") {
            return make_shared<ClusteringCoefficients>();
        } else if (name == "simple_communities") {
            return make_shared<SimpleCommunities>();
        } else if (name == "simple_communities_updating") {
            return make_shared<SimpleCommunitiesUpdating>(false);
        } else if (name == "streaming_cc") {
            return make_shared<StreamingConnectedComponents>();
        } else if (name == "kcore") {
            return make_shared<KCore>();
        } else if (name == "pagerank") {
            return make_shared<PageRank>("", false, true, EPSILON_DEFAULT, DAMPINGFACTOR_DEFAULT, MAXITER_DEFAULT);
        } else {
            cerr << "Algorithm " << name << " not implemented!\n";
            exit(-1);
        }
    }

    void observeInsertions(vector<stinger_edge_update> &recentInsertions)
    {
        data.num_insertions = recentInsertions.size();
        data.insertions = recentInsertions.data();
    }

    void observeDeletions(vector<stinger_edge_update> &recentDeletions)
    {
        data.num_deletions = recentDeletions.size();
        data.deletions = recentDeletions.data();
    }

    void observeVertexCount(int64_t nv)
    {
        data.max_active_vertex = nv;
    }

    // Returns a list of the highest N vertices in the graph
    vector<int64_t> find_high_degree_vertices(int64_t num)
    {
        int64_t n = data.max_active_vertex+1;
        typedef std::pair<int64_t, int64_t> vertex_degree;
        vector<vertex_degree> degrees(n);
        OMP("parallel for")
        for (int i = 0; i < n; ++i) {
            degrees[i] = std::make_pair(i, stinger_outdegree_get(data.stinger, i));
        }

        // order by degree descending, vertex_id ascending
        std::sort(degrees.begin(), degrees.end(),
            [](const vertex_degree &a, const vertex_degree &b)
            {
                if (a.second != b.second) { return a.second > b.second; }
                return a.first < b.first;
            }
        );

        degrees.erase(degrees.begin() + num, degrees.end());
        vector<int64_t> ids(degrees.size());
        std::transform(degrees.begin(), degrees.end(), ids.begin(),
            [](const vertex_degree &d) { return d.first; });
        return ids;
    }

    void pickSources()
    {
        if (auto b = std::dynamic_pointer_cast<BreadthFirstSearch>(impl))
        {
            int64_t source = find_high_degree_vertices(1)[0];
            b->setSource(source);
        } else if (auto b = std::dynamic_pointer_cast<BetweennessCentrality>(impl)) {
            auto samples = find_high_degree_vertices(128);
            b->setSources(samples);
        }
    }

    void onInit(){ impl->onInit(&data); }
    void onPre(){ impl->onPre(&data); }
    void onPost(){ impl->onPost(&data); }
    string name() const { return string(data.alg_name); }
};

class StingerServer
{
private:
    StingerGraph graph;
    vector<StingerAlgorithm> algs;
    vector<stinger_edge_update> recentInsertions;
    vector<stinger_edge_update> recentDeletions;
    int64_t max_active_vertex;

    // Helper functions to split strings
    // http://stackoverflow.com/a/236803/1877086
    void split(const string &s, char delim, vector<string> &elems) {
        stringstream ss(s);
        string item;
        while (getline(ss, item, delim)) {
            elems.push_back(item);
        }
    }
    vector<string> split(const string &s, char delim) {
        vector<string> elems;
        split(s, delim, elems);
        return elems;
    }

public:

    StingerServer(uint64_t nv, std::string alg_names) : graph(nv), max_active_vertex(0)
    {
        graph.printSize();

        // Register algorithms to run
        for (string algName : split(alg_names, ' '))
        {
            cerr << msg << "Initializing " << algName << "...\n";
            algs.emplace_back(graph.S, algName);
            algs.back().onInit();
        }

        onGraphChange();
    }

    void
    prepare(DynoGraph::Batch& batch, int64_t threshold)
    {
        // Store the insertions in the format that the algorithms expect
        assert(batch.dataset.isDirected()); // Not sure what algs expect for undirected batch
        int64_t num_insertions = batch.end() - batch.begin();
        recentInsertions.resize(num_insertions);
        OMP("omp parallel for")
        for (int i = 0; i < num_insertions; ++i)
        {
            DynoGraph::Edge &e = *(batch.begin() + i);
            stinger_edge_update &u = recentInsertions[i];
            u.source = e.src;
            u.destination = e.dst;
            u.weight = e.weight;
            u.time = e.timestamp;
        }

        // Figure out which deletions will actually happen
        recentDeletions.clear();
        // Each thread gets a vector to record deletions
        vector<vector<stinger_edge_update>> myDeletions(omp_get_max_threads());
        // Identical to the deletion loop, but we won't delete anything yet
        STINGER_PARALLEL_FORALL_EDGES_OF_ALL_TYPES_BEGIN(graph.S)
        {
            if (STINGER_EDGE_TIME_RECENT < threshold) {
                // Record the deletion
                stinger_edge_update u;
                u.source = STINGER_EDGE_SOURCE;
                u.destination = STINGER_EDGE_DEST;
                u.weight = STINGER_EDGE_WEIGHT;
                u.time = STINGER_EDGE_TIME_RECENT;
                myDeletions[omp_get_thread_num()].push_back(u);
            }
        }
        STINGER_PARALLEL_FORALL_EDGES_OF_ALL_TYPES_END();

        // Combine each thread's deletions into a single array
        for (int i = 0; i < omp_get_max_threads(); ++i)
        {
            recentDeletions.insert(recentDeletions.end(), myDeletions[i].begin(), myDeletions[i].end());
        }

        // Point all the algorithms to the record of insertions and deletions that will occur
        for (auto &alg : algs)
        {
            alg.observeInsertions(recentInsertions);
            alg.observeDeletions(recentDeletions);
        }
    }

    void
    onGraphChange()
    {
        // Count the number of active vertices
        // Lots of algs need this, so we'll do it in the server to save time
        max_active_vertex = stinger_max_active_vertex(graph.S);
        for (auto &alg : algs)
        {
            alg.observeVertexCount(max_active_vertex);
        }
        recordGraphStats();
    }

    void
    recordGraphStats()
    {
        int64_t nv = max_active_vertex + 1;
        stinger_fragmentation_t stats;
        stinger_fragmentation (graph.S, nv, &stats);
        int64_t num_active_vertices = stinger_num_active_vertices(graph.S);
        DegreeStats d = compute_degree_distribution(graph);

        Hooks &hooks = Hooks::getInstance();
        hooks.set_stat("num_vertices", nv);
        hooks.set_stat("num_active_vertices", num_active_vertices);
        hooks.set_stat("num_edges", stats.num_edges);
        hooks.set_stat("num_empty_edges", stats.num_empty_edges);
        hooks.set_stat("num_fragmented_blocks", stats.num_fragmented_blocks);
        hooks.set_stat("edge_blocks_in_use", stats.edge_blocks_in_use);
        hooks.set_stat("num_empty_blocks", stats.num_empty_blocks);
        hooks.set_stat("degree_mean", d.both.mean);
        hooks.set_stat("degree_max", d.both.max);
        hooks.set_stat("degree_variance", d.both.variance);
        hooks.set_stat("degree_skew", d.both.skew);
    }

    void
    insert(DynoGraph::Batch & b)
    {
        graph.insert(b);
        onGraphChange();
    }

    void
    deleteOlderThan(int64_t threshold) {
        graph.deleteOlderThan(threshold);
        onGraphChange();
    }

    void
    updateAlgorithmsBeforeBatch()
    {
        Hooks &hooks = Hooks::getInstance();
        for (auto &alg : algs)
        {
            hooks.region_begin(alg.name() + "_pre");
            alg.onPre();
            hooks.region_end();
        }
    }

    void
    updateAlgorithmsAfterBatch()
    {
        Hooks &hooks = Hooks::getInstance();
        for (auto &alg : algs)
        {
            // HACK need to pick source vertices outside of timed section
            alg.pickSources();
            hooks.region_begin(alg.name() + "_post");
            alg.onPost();
            hooks.region_end();
        }
    }

    struct DistributionSummary
    {
        double mean;
        double variance;
        int64_t max;
        double skew;
    };

    struct DegreeStats
    {
        DistributionSummary both, in, out;
    };

    /**
     * Computes the mean, variance, max, and skew of the (in/out)degree of all vertices in the graph
     *
     * @return
     */
    //template<std::function<int64_t(int64_t)> get>
    template <typename getter>
    DistributionSummary
    summarize(int64_t n, getter get)
    {
        DistributionSummary d = {};

        // First pass: compute mean and max
        int64_t max = 0;
        int64_t mean_sum = 0;
        OMP("omp parallel for reduction(max : max), reduction(+ : mean_sum)")
        for (int64_t v = 0; v < n; ++v)
        {
            int64_t degree = get(v);
            mean_sum += degree;
            if (degree > max) { max = degree; }
        }
        d.mean = static_cast<double>(mean_sum) / n;
        d.max = max;

        // Second pass: compute second and third central moments about the mean (for variance and skewness)
        double x2_sum = 0;
        double x3_sum = 0;
        OMP("omp parallel for reduction(+ : x2_sum, x3_sum)")
        for (int64_t v = 0; v < n; ++v)
        {
            int64_t degree = get(v);
            x2_sum += pow(d.mean - degree, 2);
            x3_sum += pow(d.mean - degree, 3);
        }
        d.variance = x2_sum / n;
        d.skew = x3_sum / pow(d.variance, 1.5);

        return d;
    }

    DegreeStats
    compute_degree_distribution(StingerGraph& g)
    {
        int64_t n = max_active_vertex + 1;
        DegreeStats stats;
        const stinger_t *S = g.S;

        stats.both = summarize(n, [S](int64_t i) { return stinger_degree_get(S, i); });
        stats.in   = summarize(n, [S](int64_t i) { return stinger_indegree_get(S, i); });
        stats.out  = summarize(n, [S](int64_t i) { return stinger_outdegree_get(S, i); });
        return stats;
    }

    DegreeStats
    compute_degree_distribution(DynoGraph::Batch& b)
    {
        // Calculate the in/out degree of each vertex in the batch
        int64_t max_src = std::max_element(b.begin(), b.end(),
            [](const DynoGraph::Edge& a, const DynoGraph::Edge& b) { return a.src < b.src; }
        )->src;
        int64_t max_dst = std::max_element(b.begin(), b.end(),
            [](const DynoGraph::Edge& a, const DynoGraph::Edge& b) { return a.dst < b.dst; }
        )->dst;
        int64_t n = std::max(max_src, max_dst) + 1;
        vector<int64_t> degree(n);
        vector<int64_t> in_degree(n);
        vector<int64_t> out_degree(n);
        OMP("omp parallel for")
        for (auto e = b.begin(); e < b.end(); ++e)
        {
            stinger_int64_fetch_add(&degree[e->src], 1);
            stinger_int64_fetch_add(&degree[e->dst], 1);
            stinger_int64_fetch_add(&out_degree[e->src], 1);
            stinger_int64_fetch_add(&in_degree[e->dst], 1);
        }

        // Summarize
        DegreeStats stats;
        stats.both = summarize(n, [&degree]    (int64_t i) { return degree[i]; });
        stats.in   = summarize(n, [&in_degree] (int64_t i) { return in_degree[i]; });
        stats.out  = summarize(n, [&out_degree](int64_t i) { return out_degree[i]; });

        return stats;
    }
};

int main(int argc, char **argv)
{
    // Process command line arguments
    DynoGraph::Args args(argc, argv);
    // Load graph data in from the file in batches
    DynoGraph::Dataset dataset(args);
    Hooks& hooks = Hooks::getInstance();

    for (int64_t trial = 0; trial < args.num_trials; trial++)
    {
        hooks.set_attr("trial", trial);
        // Create the stinger data structure
        StingerServer server(dataset.getMaxNumVertices(), args.alg_name);

        // Run the algorithm(s) after each inserted batch
        for (int64_t i = 0; i < dataset.batches.size(); ++i)
        {
            hooks.set_attr("batch", i);
            hooks.region_begin("preprocess");
            std::shared_ptr<DynoGraph::Batch> batch = dataset.getBatch(i);
            hooks.region_end();

            int64_t threshold = dataset.getTimestampForWindow(i);
            server.prepare(*batch, threshold);

            cerr << msg << "Running algorithms (pre-processing step)\n";
            server.updateAlgorithmsBeforeBatch();

            if (args.enable_deletions)
            {
                cerr << msg << "Deleting edges older than " << threshold << "\n";
                server.deleteOlderThan(threshold);
            }

            StingerServer::DegreeStats stats = server.compute_degree_distribution(*batch);
            hooks.set_stat("batch_degree_mean", stats.both.mean);
            hooks.set_stat("batch_degree_max", stats.both.max);
            hooks.set_stat("batch_degree_variance", stats.both.variance);
            hooks.set_stat("batch_degree_skew", stats.both.skew);
            hooks.set_stat("batch_num_vertices", batch->num_vertices_affected());

            cerr << msg << "Inserting batch " << i << "\n";
            server.insert(*batch);

            //TODO re-enable filtering at some point cerr << msg << "Filtering on >= " << threshold << "\n";
            cerr << msg << "Running algorithms (post-processing step)\n";
            server.updateAlgorithmsAfterBatch();

            // Clear out the graph between batches in snapshot mode
            if (args.sort_mode == DynoGraph::Args::SNAPSHOT)
            {
                // server = StingerServer() is no good here,
                // because this would allocate a new stinger before deallocating the old one.
                // We probably won't have enough memory for that.
                // Instead, use an explicit destructor call followed by placement new
                server.~StingerServer();
                new(&server) StingerServer(dataset.getMaxNumVertices(), args.alg_name);
            }
        }
    }

    return 0;
}
