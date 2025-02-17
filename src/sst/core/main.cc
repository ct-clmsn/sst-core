// Copyright 2009-2021 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2021, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include "sst_config.h"

#include "sst/core/warnmacros.h"

DISABLE_WARN_DEPRECATED_REGISTER
#include <Python.h>
REENABLE_WARNING

#ifdef SST_CONFIG_HAVE_MPI
DISABLE_WARN_MISSING_OVERRIDE
#include <mpi.h>
REENABLE_WARNING
#endif

#include "sst/core/activity.h"
#include "sst/core/config.h"
#include "sst/core/configGraph.h"
#include "sst/core/cputimer.h"
#include "sst/core/factory.h"
#include "sst/core/iouse.h"
#include "sst/core/link.h"
#include "sst/core/memuse.h"
#include "sst/core/model/sstmodel.h"
#include "sst/core/objectComms.h"
#include "sst/core/part/sstpart.h"
#include "sst/core/rankInfo.h"
#include "sst/core/simulation_impl.h"
#include "sst/core/statapi/statengine.h"
#include "sst/core/threadsafe.h"
#include "sst/core/timeLord.h"
#include "sst/core/timeVortex.h"

#include <cinttypes>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <signal.h>
#include <sys/resource.h>
#include <time.h>

// Configuration Graph Generation Options
#include "sst/core/cfgoutput/dotConfigOutput.h"
#include "sst/core/cfgoutput/jsonConfigOutput.h"
#include "sst/core/cfgoutput/pythonConfigOutput.h"
#include "sst/core/cfgoutput/xmlConfigOutput.h"
#include "sst/core/configGraphOutput.h"
#include "sst/core/eli/elementinfo.h"

using namespace SST::Core;
using namespace SST::Partition;
using namespace std;
using namespace SST;

static SST::Output g_output;


// Functions to force initialization stages of simulation to execute
// one rank at a time.  Put force_rank_sequential_start() before the
// serialized section and force_rank_sequential_stop() after.  These
// calls must be used in matching pairs.
static void
force_rank_sequential_start(const Config& cfg, const RankInfo& myRank, const RankInfo& world_size)
{
    if ( !cfg.rank_seq_startup() || world_size.rank == 1 || myRank.thread != 0 ) return;

#ifdef SST_CONFIG_HAVE_MPI
    // Start off all ranks with a barrier so none enter the serialized
    // region until they are all there
    MPI_Barrier(MPI_COMM_WORLD);

    // Rank 0 will proceed immediately.  All others will wait
    if ( myRank.rank == 0 ) return;

    // Ranks will wait for notice from previous rank before proceeding
    int32_t buf = 0;
    MPI_Recv(&buf, 1, MPI_INT32_T, myRank.rank - 1, MPI_ANY_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
#endif
}


// Functions to force initialization stages of simulation to execute
// one rank at a time.  Put force_rank_sequential_start() before the
// serialized section and force_rank_sequential_stop() after.  These
// calls must be used in matching pairs.
static void
force_rank_sequential_stop(const Config& cfg, const RankInfo& myRank, const RankInfo& world_size)
{
    if ( !cfg.rank_seq_startup() || world_size.rank == 1 || myRank.thread != 0 ) return;

#ifdef SST_CONFIG_HAVE_MPI
    // After I'm through the serialized region, notify the next
    // sender, then barrier.  The last rank does not need to do a
    // send.
    if ( myRank.rank != world_size.rank - 1 ) {
        uint32_t buf = 0;
        MPI_Send(&buf, 1, MPI_INT32_T, myRank.rank + 1, 0, MPI_COMM_WORLD);
    }
    MPI_Barrier(MPI_COMM_WORLD);
#endif
}

static void
SimulationSigHandler(int sig)
{
    Simulation_impl::setSignal(sig);
    if ( sig == SIGINT || sig == SIGTERM ) {
        signal(sig, SIG_DFL); // Restore default handler
    }
}

static void
setupSignals(uint32_t threadRank)
{
    if ( 0 == threadRank ) {
        if ( SIG_ERR == signal(SIGUSR1, SimulationSigHandler) ) {
            g_output.fatal(CALL_INFO, 1, "Installation of SIGUSR1 signal handler failed.\n");
        }
        if ( SIG_ERR == signal(SIGUSR2, SimulationSigHandler) ) {
            g_output.fatal(CALL_INFO, 1, "Installation of SIGUSR2 signal handler failed\n");
        }
        if ( SIG_ERR == signal(SIGINT, SimulationSigHandler) ) {
            g_output.fatal(CALL_INFO, 1, "Installation of SIGINT signal handler failed\n");
        }
        if ( SIG_ERR == signal(SIGALRM, SimulationSigHandler) ) {
            g_output.fatal(CALL_INFO, 1, "Installation of SIGALRM signal handler failed\n");
        }
        if ( SIG_ERR == signal(SIGTERM, SimulationSigHandler) ) {
            g_output.fatal(CALL_INFO, 1, "Installation of SIGTERM signal handler failed\n");
        }

        g_output.verbose(CALL_INFO, 1, 0, "Signal handler registration is completed\n");
    }
    else {
        /* Other threads don't want to receive the signal */
        sigset_t maskset;
        sigfillset(&maskset);
        pthread_sigmask(SIG_BLOCK, &maskset, nullptr);
    }
}

static void
dump_partition(Config& cfg, ConfigGraph* graph, const RankInfo& size)
{

    ///////////////////////////////////////////////////////////////////////
    // If the user asks us to dump the partitioned graph.
    if ( cfg.component_partition_file() != "" ) {
        if ( cfg.verbose() ) {
            g_output.verbose(
                CALL_INFO, 1, 0, "# Dumping partitioned component graph to %s\n",
                cfg.component_partition_file().c_str());
        }

        ofstream              graph_file(cfg.component_partition_file().c_str());
        ConfigComponentMap_t& component_map = graph->getComponentMap();

        for ( uint32_t i = 0; i < size.rank; i++ ) {
            for ( uint32_t t = 0; t < size.thread; t++ ) {
                graph_file << "Rank: " << i << "." << t << " Component List:" << std::endl;

                RankInfo r(i, t);
                for ( ConfigComponentMap_t::const_iterator j = component_map.begin(); j != component_map.end(); ++j ) {
                    auto c = *j;
                    if ( c->rank == r ) {
                        graph_file << "   " << c->name << " (ID=" << c->id << ")" << std::endl;
                        graph_file << "      -> type      " << c->type << std::endl;
                        graph_file << "      -> weight    " << c->weight << std::endl;
                        graph_file << "      -> linkcount " << c->links.size() << std::endl;
                        graph_file << "      -> rank      " << c->rank.rank << std::endl;
                        graph_file << "      -> thread    " << c->rank.thread << std::endl;
                    }
                }
            }
        }

        graph_file.close();

        if ( cfg.verbose() ) { g_output.verbose(CALL_INFO, 2, 0, "# Dump of partition graph is complete.\n"); }
    }
}

static void
do_graph_wireup(ConfigGraph* graph, SST::Simulation_impl* sim, const RankInfo& myRank, SimTime_t min_part)
{

    if ( !graph->containsComponentInRank(myRank) ) {
        g_output.output("WARNING: No components are assigned to rank: %u.%u\n", myRank.rank, myRank.thread);
    }

    sim->performWireUp(*graph, myRank, min_part);
}

static void
do_link_preparation(ConfigGraph* graph, SST::Simulation_impl* sim, const RankInfo& myRank, SimTime_t min_part)
{

    sim->prepareLinks(*graph, myRank, min_part);
}

// Returns the extension, or an empty string if there was no extension
static std::string
addRankToFileName(std::string& file_name, int rank)
{
    auto        index = file_name.find_last_of(".");
    std::string base;
    std::string ext;
    // If there is an extension, add it before the extension
    if ( index != std::string::npos ) {
        base = file_name.substr(0, index);
        ext  = file_name.substr(index);
    }
    else {
        base = file_name;
    }
    file_name = base + std::to_string(rank) + ext;
    return ext;
}

static void
doGraphOutput(SST::Config* cfg, ConfigGraph* graph, const RankInfo& myRank, const RankInfo& world_size)
{
    std::vector<ConfigGraphOutput*> graphOutputs;

    // User asked us to dump the config graph to a file in Python
    if ( cfg->output_config_graph() != "" ) {
        // See if we are doing parallel output
        std::string file_name(cfg->output_config_graph());
        if ( cfg->parallel_output() && world_size.rank != 1 ) {
            // Append rank number to base filename
            std::string ext = addRankToFileName(file_name, myRank.rank);
            if ( ext != ".py" ) {
                g_output.fatal(CALL_INFO, 1, "--output-config requires a filename with a .py extension\n");
            }
        }
        graphOutputs.push_back(new PythonConfigGraphOutput(file_name.c_str()));
    }

    // user asked us to dump the config graph in dot graph format
    if ( cfg->output_dot() != "" ) { graphOutputs.push_back(new DotConfigGraphOutput(cfg->output_dot().c_str())); }

    // User asked us to dump the config graph in XML format
    if ( cfg->output_xml() != "" ) { graphOutputs.push_back(new XMLConfigGraphOutput(cfg->output_xml().c_str())); }

    // User asked us to dump the config graph in JSON format
    if ( cfg->output_json() != "" ) {
        std::string file_name(cfg->output_json());
        if ( cfg->parallel_output() ) {
            // Append rank number to base filename
            std::string ext = addRankToFileName(file_name, myRank.rank);
            if ( ext != ".json" ) {
                g_output.fatal(CALL_INFO, 1, "--output-json requires a filename with a .json extension\n");
            }
        }
        graphOutputs.push_back(new JSONConfigGraphOutput(file_name.c_str()));
    }

    ConfigGraph* myGraph = graph;
    if ( cfg->parallel_output() ) {
        // Get a graph that only includes components important to this
        // rank
        myGraph = graph->getSubGraph(myRank.rank, myRank.rank);
    }
    for ( size_t i = 0; i < graphOutputs.size(); i++ ) {
        graphOutputs[i]->generate(cfg, myGraph);
        delete graphOutputs[i];
    }
    if ( cfg->parallel_output() ) delete myGraph;
}

typedef struct
{
    RankInfo     myRank;
    RankInfo     world_size;
    Config*      config;
    ConfigGraph* graph;
    SimTime_t    min_part;

    // Time / stats information
    double      build_time;
    double      run_time;
    UnitAlgebra simulated_time;
    uint64_t    max_tv_depth;
    uint64_t    current_tv_depth;
    uint64_t    sync_data_size;

} SimThreadInfo_t;

void
finalize_statEngineConfig(void)
{
    StatisticProcessingEngine::getInstance()->finalizeInitialization();
}

static void
start_simulation(uint32_t tid, SimThreadInfo_t& info, Core::ThreadSafe::Barrier& barrier)
{
    info.myRank.thread = tid;
    double start_build = sst_get_cpu_time();

    if ( tid ) {
        /* already did Thread Rank 0 in main() */
        setupSignals(tid);
    }

    ////// Create Simulation Objects //////
    SST::Simulation_impl* sim = Simulation_impl::createSimulation(info.config, info.myRank, info.world_size);

    barrier.wait();

    sim->processGraphInfo(*info.graph, info.myRank, info.min_part);

    barrier.wait();

    force_rank_sequential_start(*info.config, info.myRank, info.world_size);

    barrier.wait();

    // Perform the wireup.  Do this one thread at a time for now.  If
    // this ever changes, then need to put in some serialization into
    // performWireUp.
    for ( uint32_t i = 0; i < info.world_size.thread; ++i ) {
        if ( i == info.myRank.thread ) { do_link_preparation(info.graph, sim, info.myRank, info.min_part); }
        barrier.wait();
    }

    for ( uint32_t i = 0; i < info.world_size.thread; ++i ) {
        if ( i == info.myRank.thread ) { do_graph_wireup(info.graph, sim, info.myRank, info.min_part); }
        barrier.wait();
    }

    if ( tid == 0 ) {
        finalize_statEngineConfig();
        delete info.graph;
    }

    double start_run = sst_get_cpu_time();
    info.build_time  = start_run - start_build;

    force_rank_sequential_stop(*info.config, info.myRank, info.world_size);

    barrier.wait();

#ifdef SST_CONFIG_HAVE_MPI
    if ( tid == 0 && info.world_size.rank > 1 ) { MPI_Barrier(MPI_COMM_WORLD); }
#endif

    barrier.wait();

    if ( info.config->runMode() == Simulation::RUN || info.config->runMode() == Simulation::BOTH ) {
        if ( info.config->verbose() && 0 == tid ) {
            g_output.verbose(CALL_INFO, 1, 0, "# Starting main event loop\n");

            time_t     the_time = time(nullptr);
            struct tm* now      = localtime(&the_time);

            g_output.verbose(
                CALL_INFO, 1, 0, "# Start time: %04u/%02u/%02u at: %02u:%02u:%02u\n", (now->tm_year + 1900),
                (now->tm_mon + 1), now->tm_mday, now->tm_hour, now->tm_min, now->tm_sec);

            if ( info.config->exit_after() > 0 ) {
                time_t     stop_time = the_time + info.config->exit_after();
                struct tm* end       = localtime(&stop_time);
                g_output.verbose(
                    CALL_INFO, 1, 0, "# Will end by: %04u/%02u/%02u at: %02u:%02u:%02u\n", (end->tm_year + 1900),
                    (end->tm_mon + 1), end->tm_mday, end->tm_hour, end->tm_min, end->tm_sec);

                /* Set the alarm */
                alarm(info.config->exit_after());
            }
        }
        // g_output.output("info.config.stopAtCycle = %s\n",info.config->stopAtCycle.c_str());
        sim->setStopAtCycle(info.config);

        if ( tid == 0 && info.world_size.rank > 1 ) {
            // If we are a MPI_parallel job, need to makes sure that all used
            // libraries are loaded on all ranks.
#ifdef SST_CONFIG_HAVE_MPI
            set<string> lib_names;
            set<string> other_lib_names;
            Factory::getFactory()->getLoadedLibraryNames(lib_names);
            // vector<set<string> > all_lib_names;

            // Send my lib_names to the next lowest rank
            if ( info.myRank.rank == (info.world_size.rank - 1) ) {
                Comms::send(info.myRank.rank - 1, 0, lib_names);
                lib_names.clear();
            }
            else {
                Comms::recv(info.myRank.rank + 1, 0, other_lib_names);
                for ( auto iter = other_lib_names.begin(); iter != other_lib_names.end(); ++iter ) {
                    lib_names.insert(*iter);
                }
                if ( info.myRank.rank != 0 ) {
                    Comms::send(info.myRank.rank - 1, 0, lib_names);
                    lib_names.clear();
                }
            }

            Comms::broadcast(lib_names, 0);
            Factory::getFactory()->loadUnloadedLibraries(lib_names);
#endif
        }
        barrier.wait();

        sim->initialize();
        barrier.wait();

        /* Run Set */
        sim->setup();
        barrier.wait();

        /* Run Simulation */
        sim->run();
        barrier.wait();

        sim->complete();
        barrier.wait();

        sim->finish();
        barrier.wait();
    }

    barrier.wait();

    info.simulated_time = sim->getFinalSimTime();
    // g_output.output(CALL_INFO,"Simulation time = %s\n",info.simulated_time.toStringBestSI().c_str());

    double end_time = sst_get_cpu_time();
    info.run_time   = end_time - start_run;

    info.max_tv_depth     = sim->getTimeVortexMaxDepth();
    info.current_tv_depth = sim->getTimeVortexCurrentDepth();

    delete sim;
}

int
main(int argc, char* argv[])
{
#ifdef SST_CONFIG_HAVE_MPI
    MPI_Init(&argc, &argv);

    int myrank = 0;
    int mysize = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &myrank);
    MPI_Comm_size(MPI_COMM_WORLD, &mysize);

    RankInfo world_size(mysize, 1);
    RankInfo myRank(myrank, 0);
#else
    int      myrank = 0;
    RankInfo world_size(1, 1);
    RankInfo myRank(0, 0);
#endif
    Config cfg(world_size);

    // All ranks parse the command line
    auto ret_value = cfg.parseCmdLine(argc, argv);
    if ( ret_value == -1 ) {
        // Error in command line arguments
        return -1;
    }
    else if ( ret_value == 1 ) {
        // Just asked for info, clean exit
        return 0;
    }
    world_size.thread = cfg.num_threads();

    if ( cfg.parallel_load() && world_size.rank != 1 ) { addRankToFileName(cfg.configFile_, myRank.rank); }
    cfg.checkConfigFile();

    // Create the factory.  This may be needed to load an external model definition
    Factory* factory = new Factory(cfg.getLibPath());

    // Get a list of all the available SSTModelDescriptions
    std::vector<std::string> models = ELI::InfoDatabase::getRegisteredElementNames<SSTModelDescription>();

    // Create a map of extensions to the model that supports them
    std::map<std::string, std::string> extension_map;
    for ( auto x : models ) {
        // auto extensions = factory->getSimpleInfo<SSTModelDescription, 1, std::vector<std::string>>(x);
        auto extensions = SSTModelDescription::getElementSupportedExtensions(x);
        for ( auto y : extensions ) {
            extension_map[y] = x;
        }
    }


    // Create the model generator
    SSTModelDescription* modelGen = nullptr;

    // Get the memory before we create the graph
    const uint64_t pre_graph_create_rss = maxGlobalMemSize();

    force_rank_sequential_start(cfg, myRank, world_size);

    double start = sst_get_cpu_time();

    if ( cfg.configFile() != "NONE" ) {
        // Get the file extension by finding the last .
        std::string extension = cfg.configFile().substr(cfg.configFile().find_last_of("."));

        std::string model_name;
        try {
            model_name = extension_map.at(extension);
        }
        catch ( exception& e ) {
            std::cerr << "Unsupported SDL file type: \"" << extension << "\"" << std::endl;
            return -1;
        }

        // If doing parallel load, make sure this model is parallel capable
        if ( cfg.parallel_load() && !SSTModelDescription::isElementParallelCapable(model_name) ) {
            std::cerr << "Model type for extension: \"" << extension << "\" does not support parallel loading."
                      << std::endl;
            return -1;
        }

        modelGen = factory->Create<SSTModelDescription>(model_name, cfg.configFile(), cfg.verbose(), &cfg, start);
    }

    /* Build objects needed for startup */
    Output::setWorldSize(world_size, myrank);
    g_output = Output::setDefaultObject(cfg.output_core_prefix(), cfg.verbose(), 0, Output::STDOUT);

    g_output.verbose(
        CALL_INFO, 1, 0, "#main() My rank is (%u.%u), on %u/%u nodes/threads\n", myRank.rank, myRank.thread,
        world_size.rank, world_size.thread);

    ////// Start ConfigGraph Creation //////
    ConfigGraph* graph = nullptr;

    double start_graph_gen = sst_get_cpu_time();
    graph                  = new ConfigGraph();

    // Only rank 0 will populate the graph, unless we are using
    // parallel load.  In this case, all ranks will load the graph
    if ( myRank.rank == 0 || cfg.parallel_load() ) {
        try {
            graph = modelGen->createConfigGraph();
        }
        catch ( std::exception& e ) {
            g_output.fatal(CALL_INFO, -1, "Error encountered during config-graph generation: %s\n", e.what());
        }
    }

    force_rank_sequential_stop(cfg, myRank, world_size);

#ifdef SST_CONFIG_HAVE_MPI
    // Config is done - broadcast it, unless we are parallel loading
    if ( world_size.rank > 1 && !cfg.parallel_load() ) {
        try {
            Comms::broadcast(cfg, 0);
        }
        catch ( std::exception& e ) {
            g_output.fatal(CALL_INFO, -1, "Error encountered broadcasting configuration object: %s\n", e.what());
        }
    }


#endif

    // Need to initialize TimeLord
    Simulation_impl::getTimeLord()->init(cfg.timeBase());

    if ( myRank.rank == 0 || cfg.parallel_load() ) {
        graph->postCreationCleanup();

        // Check config graph to see if there are structural errors.
        if ( graph->checkForStructuralErrors() ) {
            g_output.fatal(CALL_INFO, 1, "Structure errors found in the ConfigGraph.\n");
        }
    }

    // Delete the model generator
    delete modelGen;
    modelGen = nullptr;

    double end_graph_gen = sst_get_cpu_time();

    if ( myRank.rank == 0 ) {
        g_output.verbose(CALL_INFO, 1, 0, "# ------------------------------------------------------------\n");
        g_output.verbose(CALL_INFO, 1, 0, "# Graph construction took %f seconds.\n", (end_graph_gen - start_graph_gen));
    }

    ////// End ConfigGraph Creation //////

    ////// Start Partitioning //////
    double start_part = sst_get_cpu_time();

    if ( !cfg.parallel_load() ) {
        // Normal partitioning

        // If this is a serial job, just use the single partitioner,
        // but the same code path
        if ( world_size.rank == 1 && world_size.thread == 1 ) cfg.partitioner_ = "sst.single";

        // Get the partitioner.  Built in partitioners are in the "sst" library.
        SSTPartitioner* partitioner = factory->CreatePartitioner(cfg.partitioner(), world_size, myRank, cfg.verbose());

        try {
            if ( partitioner->requiresConfigGraph() ) { partitioner->performPartition(graph); }
            else {
                PartitionGraph* pgraph;
                if ( myRank.rank == 0 ) { pgraph = graph->getCollapsedPartitionGraph(); }
                else {
                    pgraph = new PartitionGraph();
                }

                if ( myRank.rank == 0 || partitioner->spawnOnAllRanks() ) {
                    partitioner->performPartition(pgraph);

                    if ( myRank.rank == 0 ) graph->annotateRanks(pgraph);
                }

                delete pgraph;
            }
        }
        catch ( std::exception& e ) {
            g_output.fatal(CALL_INFO, -1, "Error encountered during graph partitioning phase: %s\n", e.what());
        }

        delete partitioner;
    }
    else {
        // Need to make sure all the cross rank links have matching
        // IDs on both sides.

        // We'll do this by hashing the link name and then checking
        // for clashes.  For now, a clash will casue a fatal.  Later,
        // we'll add a fallback.

        std::set<uint32_t> clashes;

        ConfigComponentMap_t& comps = graph->getComponentMap();
        ConfigLinkMap_t&      links = graph->getLinkMap();
        for ( ConfigLinkMap_t::iterator iter = links.begin(); iter != links.end(); ++iter ) {
            ConfigLink& clink = *iter;
            RankInfo    rank[2];
            rank[0] = comps[COMPONENT_ID_MASK(clink.component[0])]->rank;
            rank[1] = comps[COMPONENT_ID_MASK(clink.component[1])]->rank;
            if ( rank[0].rank == rank[1].rank || (rank[0].rank != myRank.rank && rank[1].rank != myRank.rank) )
                continue;

            // Found link that goes from my rank to another one.  Get
            // the name and generate the hash
            const char* key  = clink.name.c_str();
            int         len  = ::strlen(key);
            uint32_t    hash = 0;
            for ( int i = 0; i < len; ++i ) {
                hash += key[i];
                hash += (hash << 10);
                hash ^= (hash >> 6);
            }
            hash += (hash << 3);
            hash ^= (hash >> 11);
            hash += (hash << 15);

            // Hashed id's will always have a 1 in highest order bit
            // to avoid a class with autogenerated links
            hash = hash | (1 << 31);

            auto ret = clashes.insert(hash);
            if ( !ret.second ) {
                g_output.fatal(
                    CALL_INFO, 1, "ERROR: Found clash in hashes for two link names that cross rank partitions.\n");
            }

            clink.remote_tag = hash;
        }
    }

    // Check the partitioning to make sure it is sane
    if ( myRank.rank == 0 || cfg.parallel_load() ) {
        if ( !graph->checkRanks(world_size) ) {
            g_output.fatal(CALL_INFO, 1, "ERROR: Bad partitioning; partition included unknown ranks.\n");
        }
    }
    double         end_part              = sst_get_cpu_time();
    const uint64_t post_graph_create_rss = maxGlobalMemSize();

    if ( myRank.rank == 0 ) {
        g_output.verbose(CALL_INFO, 1, 0, "# Graph partitioning took %lg seconds.\n", (end_part - start_part));
        g_output.verbose(
            CALL_INFO, 1, 0, "# Graph construction and partition raised RSS by %" PRIu64 " KB\n",
            (post_graph_create_rss - pre_graph_create_rss));
        g_output.verbose(CALL_INFO, 1, 0, "# ------------------------------------------------------------\n");

        // Output the partition information if user requests it
        dump_partition(cfg, graph, world_size);
    }

    ////// End Partitioning //////

    ////// Calculate Minimum Partitioning //////
    SimTime_t local_min_part = 0xffffffffffffffffl;
    SimTime_t min_part       = 0xffffffffffffffffl;
    if ( world_size.rank > 1 ) {
        // Check the graph for the minimum latency crossing a partition boundary
        if ( myRank.rank == 0 || cfg.parallel_load() ) {
            ConfigComponentMap_t& comps = graph->getComponentMap();
            ConfigLinkMap_t&      links = graph->getLinkMap();
            // Find the minimum latency across a partition
            for ( ConfigLinkMap_t::iterator iter = links.begin(); iter != links.end(); ++iter ) {
                ConfigLink& clink = *iter;
                RankInfo    rank[2];
                rank[0] = comps[COMPONENT_ID_MASK(clink.component[0])]->rank;
                rank[1] = comps[COMPONENT_ID_MASK(clink.component[1])]->rank;
                if ( rank[0].rank == rank[1].rank ) continue;
                if ( clink.getMinLatency() < local_min_part ) { local_min_part = clink.getMinLatency(); }
            }
        }
#ifdef SST_CONFIG_HAVE_MPI

        // Fix for case that probably doesn't matter in practice, but
        // does come up during some specific testing.  If there are no
        // links that cross the boundary and we're a multi-rank job,
        // we need to put in a sync interval to look for the exit
        // conditions being met.
        // if ( min_part == MAX_SIMTIME_T ) {
        //     // std::cout << "No links cross rank boundary" << std::endl;
        //     min_part = Simulation_impl::getTimeLord()->getSimCycles("1us","");
        // }

        MPI_Allreduce(&local_min_part, &min_part, 1, MPI_UINT64_T, MPI_MIN, MPI_COMM_WORLD);
        // Comms::broadcast(min_part, 0);
#endif
    }
    ////// End Calculate Minimum Partitioning //////

    if ( cfg.enable_sig_handling() ) {
        g_output.verbose(CALL_INFO, 1, 0, "Signal handlers will be registered for USR1, USR2, INT and TERM...\n");
        setupSignals(0);
    }
    else {
        // Print out to say disabled?
        g_output.verbose(CALL_INFO, 1, 0, "Signal handlers are disabled by user input\n");
    }

    ////// Broadcast Graph //////
#ifdef SST_CONFIG_HAVE_MPI
    if ( world_size.rank > 1 && !cfg.parallel_load() ) {
        try {
            Comms::broadcast(Params::keyMap, 0);
            Comms::broadcast(Params::keyMapReverse, 0);
            Comms::broadcast(Params::nextKeyID, 0);
            Comms::broadcast(Params::global_params, 0);

            std::set<uint32_t> my_ranks;
            std::set<uint32_t> your_ranks;

            if ( 0 == myRank.rank ) {
                // Split the rank space in half
                for ( uint32_t i = 0; i < world_size.rank / 2; i++ ) {
                    my_ranks.insert(i);
                }

                for ( uint32_t i = world_size.rank / 2; i < world_size.rank; i++ ) {
                    your_ranks.insert(i);
                }

                // Need to send the your_ranks set and the proper
                // subgraph for further distribution
                ConfigGraph* your_graph = graph->getSubGraph(your_ranks);
                int          dest       = *your_ranks.begin();
                Comms::send(dest, 0, your_ranks);
                Comms::send(dest, 0, *your_graph);
                your_ranks.clear();
            }
            else {
                Comms::recv(MPI_ANY_SOURCE, 0, my_ranks);
                Comms::recv(MPI_ANY_SOURCE, 0, *graph);
            }

            while ( my_ranks.size() != 1 ) {
                // This means I have more data to pass on to other ranks
                std::set<uint32_t>::iterator mid = my_ranks.begin();
                for ( unsigned int i = 0; i < my_ranks.size() / 2; i++ ) {
                    ++mid;
                }

                your_ranks.insert(mid, my_ranks.end());
                my_ranks.erase(mid, my_ranks.end());

                ConfigGraph* your_graph = graph->getSubGraph(your_ranks);
                uint32_t     dest       = *your_ranks.begin();

                Comms::send(dest, 0, your_ranks);
                Comms::send(dest, 0, *your_graph);
                your_ranks.clear();
                delete your_graph;
            }
        }
        catch ( std::exception& e ) {
            g_output.fatal(CALL_INFO, -1, "Error encountered during graph broadcast: %s\n", e.what());
        }
    }
#endif
    ////// End Broadcast Graph //////

    if ( myRank.rank == 0 || cfg.parallel_output() ) { doGraphOutput(&cfg, graph, myRank, world_size); }


    // // Print the graph
    // if ( myRank.rank == 0 ) {
    //     std::cout << "Rank 0 graph:" << std::endl;
    //     graph->print(std::cout);
    // }
    // Simulation_impl::barrier.wait();
    // if ( myRank.rank == 1 ) {
    //     std::cout << "Rank 1 graph:" << std::endl;
    //     graph->print(std::cout);
    // }
    // Simulation_impl::barrier.wait();

    ///// Set up StatisticEngine /////

    SST::Statistics::StatisticProcessingEngine::init(graph);

    ///// End Set up StatisticEngine /////

    ////// Create Simulation //////
    Core::ThreadSafe::Barrier mainBarrier(world_size.thread);

    Simulation_impl::factory    = factory;
    Simulation_impl::sim_output = g_output;
    Simulation_impl::resizeBarriers(world_size.thread);
#ifdef USE_MEMPOOL
    /* Estimate that we won't have more than 128 sizes of events */
    Activity::memPools.reserve(world_size.thread * 128);
#endif

    std::vector<std::thread>     threads(world_size.thread);
    std::vector<SimThreadInfo_t> threadInfo(world_size.thread);
    for ( uint32_t i = 0; i < world_size.thread; i++ ) {
        threadInfo[i].myRank        = myRank;
        threadInfo[i].myRank.thread = i;
        threadInfo[i].world_size    = world_size;
        threadInfo[i].config        = &cfg;
        threadInfo[i].graph         = graph;
        threadInfo[i].min_part      = min_part;
    }

    double end_serial_build = sst_get_cpu_time();

    try {
        Output::setThreadID(std::this_thread::get_id(), 0);
        for ( uint32_t i = 1; i < world_size.thread; i++ ) {
            threads[i] = std::thread(start_simulation, i, std::ref(threadInfo[i]), std::ref(mainBarrier));
            Output::setThreadID(threads[i].get_id(), i);
        }

        start_simulation(0, threadInfo[0], mainBarrier);
        for ( uint32_t i = 1; i < world_size.thread; i++ ) {
            threads[i].join();
        }

        Simulation_impl::shutdown();
    }
    catch ( std::exception& e ) {
        g_output.fatal(CALL_INFO, -1, "Error encountered during simulation: %s\n", e.what());
    }

    double total_end_time = sst_get_cpu_time();

    for ( uint32_t i = 1; i < world_size.thread; i++ ) {
        threadInfo[0].simulated_time = std::max(threadInfo[0].simulated_time, threadInfo[i].simulated_time);
        threadInfo[0].run_time       = std::max(threadInfo[0].run_time, threadInfo[i].run_time);
        threadInfo[0].build_time     = std::max(threadInfo[0].build_time, threadInfo[i].build_time);

        threadInfo[0].max_tv_depth = std::max(threadInfo[0].max_tv_depth, threadInfo[i].max_tv_depth);
        threadInfo[0].current_tv_depth += threadInfo[i].current_tv_depth;
        threadInfo[0].sync_data_size += threadInfo[i].sync_data_size;
    }

    double build_time = (end_serial_build - start) + threadInfo[0].build_time;
    double run_time   = threadInfo[0].run_time;
    double total_time = total_end_time - start;

    double max_run_time = 0, max_build_time = 0, max_total_time = 0;

    uint64_t local_max_tv_depth      = threadInfo[0].max_tv_depth;
    uint64_t global_max_tv_depth     = 0;
    uint64_t local_current_tv_depth  = threadInfo[0].current_tv_depth;
    uint64_t global_current_tv_depth = 0;

    uint64_t global_max_sync_data_size = 0, global_sync_data_size = 0;

    uint64_t mempool_size = 0, max_mempool_size = 0, global_mempool_size = 0;
    uint64_t active_activities = 0, global_active_activities = 0;
#ifdef USE_MEMPOOL
    Activity::getMemPoolUsage(mempool_size, active_activities);
#endif

#ifdef SST_CONFIG_HAVE_MPI
    uint64_t local_sync_data_size = threadInfo[0].sync_data_size;

    MPI_Allreduce(&run_time, &max_run_time, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&build_time, &max_build_time, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&total_time, &max_total_time, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&local_max_tv_depth, &global_max_tv_depth, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&local_current_tv_depth, &global_current_tv_depth, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(&local_sync_data_size, &global_max_sync_data_size, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&local_sync_data_size, &global_sync_data_size, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(&mempool_size, &max_mempool_size, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&mempool_size, &global_mempool_size, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(&active_activities, &global_active_activities, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
#else
    max_build_time            = build_time;
    max_run_time              = run_time;
    max_total_time            = total_time;
    global_max_tv_depth       = local_max_tv_depth;
    global_current_tv_depth   = local_current_tv_depth;
    global_max_sync_data_size = 0;
    global_max_sync_data_size = 0;
    max_mempool_size          = mempool_size;
    global_mempool_size       = mempool_size;
    global_active_activities  = active_activities;
#endif

    const uint64_t local_max_rss     = maxLocalMemSize();
    const uint64_t global_max_rss    = maxGlobalMemSize();
    const uint64_t local_max_pf      = maxLocalPageFaults();
    const uint64_t global_pf         = globalPageFaults();
    const uint64_t global_max_io_in  = maxInputOperations();
    const uint64_t global_max_io_out = maxOutputOperations();

    if ( myRank.rank == 0 && (cfg.verbose() || cfg.print_timing()) ) {
        char ua_buffer[256];
        sprintf(ua_buffer, "%" PRIu64 "KB", local_max_rss);
        UnitAlgebra max_rss_ua(ua_buffer);

        sprintf(ua_buffer, "%" PRIu64 "KB", global_max_rss);
        UnitAlgebra global_rss_ua(ua_buffer);

        sprintf(ua_buffer, "%" PRIu64 "B", global_max_sync_data_size);
        UnitAlgebra global_max_sync_data_size_ua(ua_buffer);

        sprintf(ua_buffer, "%" PRIu64 "B", global_sync_data_size);
        UnitAlgebra global_sync_data_size_ua(ua_buffer);

        sprintf(ua_buffer, "%" PRIu64 "B", max_mempool_size);
        UnitAlgebra max_mempool_size_ua(ua_buffer);

        sprintf(ua_buffer, "%" PRIu64 "B", global_mempool_size);
        UnitAlgebra global_mempool_size_ua(ua_buffer);

        g_output.output("\n");
        g_output.output("\n");
        g_output.output("------------------------------------------------------------\n");
        g_output.output("Simulation Timing Information:\n");
        g_output.output("Build time:                      %f seconds\n", max_build_time);
        g_output.output("Simulation time:                 %f seconds\n", max_run_time);
        g_output.output("Total time:                      %f seconds\n", max_total_time);
        g_output.output("Simulated time:                  %s\n", threadInfo[0].simulated_time.toStringBestSI().c_str());
        g_output.output("\n");
        g_output.output("Simulation Resource Information:\n");
        g_output.output("Max Resident Set Size:           %s\n", max_rss_ua.toStringBestSI().c_str());
        g_output.output("Approx. Global Max RSS Size:     %s\n", global_rss_ua.toStringBestSI().c_str());
        g_output.output("Max Local Page Faults:           %" PRIu64 " faults\n", local_max_pf);
        g_output.output("Global Page Faults:              %" PRIu64 " faults\n", global_pf);
        g_output.output("Max Output Blocks:               %" PRIu64 " blocks\n", global_max_io_in);
        g_output.output("Max Input Blocks:                %" PRIu64 " blocks\n", global_max_io_out);
        g_output.output("Max mempool usage:               %s\n", max_mempool_size_ua.toStringBestSI().c_str());
        g_output.output("Global mempool usage:            %s\n", global_mempool_size_ua.toStringBestSI().c_str());
        g_output.output("Global active activities:        %" PRIu64 " activities\n", global_active_activities);
        g_output.output("Current global TimeVortex depth: %" PRIu64 " entries\n", global_current_tv_depth);
        g_output.output("Max TimeVortex depth:            %" PRIu64 " entries\n", global_max_tv_depth);
        g_output.output("Max Sync data size:              %s\n", global_max_sync_data_size_ua.toStringBestSI().c_str());
        g_output.output("Global Sync data size:           %s\n", global_sync_data_size_ua.toStringBestSI().c_str());
        g_output.output("------------------------------------------------------------\n");
        g_output.output("\n");
        g_output.output("\n");
    }

#ifdef USE_MEMPOOL
    if ( cfg.event_dump_file() != "" ) {
        Output out("", 0, 0, Output::FILE, cfg.event_dump_file());
        if ( cfg.event_dump_file() == "STDOUT" || cfg.event_dump_file() == "stdout" )
            out.setOutputLocation(Output::STDOUT);
        if ( cfg.event_dump_file() == "STDERR" || cfg.event_dump_file() == "stderr" )
            out.setOutputLocation(Output::STDERR);
        Activity::printUndeletedActivities("", out, MAX_SIMTIME_T);
    }
#endif

#ifdef SST_CONFIG_HAVE_MPI
    if ( 0 == myRank.rank ) {
#endif
        // Print out the simulation time regardless of verbosity.
        g_output.output(
            "Simulation is complete, simulated time: %s\n", threadInfo[0].simulated_time.toStringBestSI().c_str());
#ifdef SST_CONFIG_HAVE_MPI
    }
#endif

#ifdef SST_CONFIG_HAVE_MPI
    MPI_Finalize();
#endif

    return 0;
}
