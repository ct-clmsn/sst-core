#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <map>
#include <memory>
#include <filesystem>

#include <sys/stat.h>

#define USE_MEMPOOL 1

#include "repart.hpp"
#include "sst/core/componentInfo.h"
#include "sst/core/timeVortex.h"
#include "sst/core/timeConverter.h"
#include "sst/core/exit.h"
#include "sst/core/realtime.h"
#include "sst/core/heartbeat.h"
#include "sst/core/statapi/statengine.h"
#include "sst/core/shared/sharedObject.h"
#include "sst/core/mempool.h"
#include "sst/core/mempoolAccessor.h"
#include "sst/core/simulation_impl.h"
#if defined(__STDC_FORMAT_MACROS)
#undef __STDC_FORMAT_MACROS
#endif
#include "sst/core/timeLord.h"
#include "sst/core/timeVortex.h"
#include "sst/core/impl/timevortex/timeVortexPQ.h"
#include "sst/core/impl/timevortex/timeVortexPQ.cc"
#include "sst/core/impl/timevortex/timeVortexBinnedMap.h"
#include "sst/core/impl/timevortex/timeVortexBinnedMap.cc"

namespace fs = std::filesystem;

using namespace SST;
using namespace SST::Shared;
using namespace SST::IMPL;
using namespace SST::Partition;
using namespace SST::IMPL::Partition;
using namespace SST::Repartition;
using namespace SST::Core::Serialization;

using clockMap_t   = std::map<std::pair<SimTime_t, int>, Clock*>;

static inline std::string const input_path_argument = "--input_path";
static inline std::string const output_path_argument = "--output_path";
static inline std::string const repart_scheme_argument = "--part_scheme";
static inline std::string const n_ranks_argument = "--n_ranks";
static inline std::string const n_threads_argument = "--n_threads";

static inline std::map<std::string, std::string> arguments = {
   { input_path_argument, "" },
   { output_path_argument, "" },
   { repart_scheme_argument, "" },
   { n_ranks_argument, "" },
   { n_threads_argument, "" }
};

void store_checkpoint(
    RankInfo const& rank_,
    std::shared_ptr<Simulation_impl> & cur_checkpoint,
    std::shared_ptr<Simulation_impl> & new_checkpoint) {

    std::shared_ptr<Simulation_impl> & sim = cur_checkpoint;

    // Need to create a directory for this checkpoint
    std::string prefix   = sim->checkpoint_prefix_;
    std::string basename = prefix + "_" + std::to_string(sim->checkpoint_id_) + "_" + std::to_string(sim->currentSimCycle);

    // Directory is shared across threads.  Make it a static and make
    // sure we barrier in the right places
    static std::string directory;

    // Only thread 0 will participate in setup
    if ( rank_.thread == 0 ) {
        // Rank 0 will create the directory for this checkpoint
        if ( rank_.rank == 0 ) {
            directory = Checkpointing::createUniqueDirectory(sim->checkpoint_directory_ + "/" + basename);
        }
    }

    //if ( rank_.thread == 0 ) checkpoint_id++;

    std::string filename =
        directory + "/" + basename + "_" + std::to_string(rank_.rank) + "_" + std::to_string(rank_.thread) + ".bin";

    // Write out the checkpoints for the partitions
    sim->checkpoint(filename);

    // Write out the registry.  Rank 0 thread 0 will write the global
    // state and its registry, then each thread will take a turn
    // writing its part of the registry
    RankInfo num_ranks = sim->getNumRanks();

    std::string registry_name = directory + "/" + basename + ".sstcpt";
}

void load_checkpoint_globals(std::string& checkpoint_path_str, std::map<std::string, uint32_t>& keymap, std::vector<std::string>& keymap_rev) {

   std::ifstream fs(checkpoint_path_str);
   if ( !fs.is_open() ) {
      if ( fs.bad() ) {
         std::cout << "bad error" << std::endl;
         return;
      }
      if ( fs.fail() ) {
         std::cout << "fail error" << std::endl;
         return;
      }
      std::cout << "unknown error" << std::endl;
      return;
   }
   std::string line;

   // Look for the line that has the global data file
   std::string globals_filename;
   std::string search_str("** (globals): ");
   while ( std::getline(fs, line) ) {
      // Look for lines starting with "** (globals):", then get the filename.
      size_t pos = line.find(search_str);
      if ( pos == 0 ) {
         // Get the file name
         globals_filename = line.substr(search_str.length());
         break;
      }
   }
   fs.close();

   // Need to open the globals file
   std::ifstream fs_globals(globals_filename);
   if ( !fs_globals.is_open() ) {
      if ( fs_globals.bad() ) {
         fprintf(stderr, "Unable to open checkpoint globals file [%s]: badbit set\n", globals_filename.c_str());
         return;
      }
      if ( fs_globals.fail() ) {
         fprintf(
            stderr, "Unable to open checkpoint globals file [%s]: %s\n", globals_filename.c_str(),
            strerror(errno));
         return;
      }
      fprintf(stderr, "Unable to open checkpoint globals file [%s]: unknown error\n", globals_filename.c_str());
      return;
   }

   size_t size;

   SST::Core::Serialization::serializer ser;
   ser.enable_pointer_tracking();

   fs_globals.read(reinterpret_cast<char*>(&size), sizeof(size));
   char buffer[size];
   fs_globals.read(&buffer[0], size);

   uint32_t cpt_num_threads, cpt_num_ranks;
   std::string                     cpt_lib_path;
   std::string                     cpt_timebase;
   std::string                     cpt_output_directory;
   std::string                     cpt_output_core_prefix;
   std::string                     cpt_debug_file;
   std::string                     cpt_prefix;
   int                             cpt_output_verbose = 0;
   std::map<std::string, uint32_t> & cpt_params_key_map = keymap;
   std::vector<std::string>        & cpt_params_key_map_reverse = keymap_rev;
   uint32_t                        cpt_params_next_key_id;

   ser.start_unpacking(buffer, size);
   ser& cpt_num_ranks;
   ser& cpt_num_threads;
   ser& cpt_lib_path;
   ser& cpt_timebase;
   ser& cpt_output_directory;
   ser& cpt_output_core_prefix;
   ser& cpt_output_verbose;
   ser& cpt_debug_file;
   ser& cpt_prefix;
   ser& cpt_params_key_map;
   ser& cpt_params_key_map_reverse;
   ser& cpt_params_next_key_id;

   fs_globals.close();
}

void load_checkpoint(std::string& checkpoint_path_str, std::shared_ptr<Factory> & factory, std::vector<ComponentId_t> & components, int const argc, char **argv, bool const initfactory) {
   // Need to open the globals file
   std::ifstream fs(checkpoint_path_str, std::ios::in | std::ios::binary);
   if ( !fs.is_open() ) {
      if ( fs.bad() ) {
         fprintf(stderr, "Unable to open checkpoint globals file [%s]: badbit set\n", checkpoint_path_str.c_str());
         return;
      }
      if ( fs.fail() ) {
         fprintf(
            stderr, "Unable to open checkpoint globals file [%s]: %s\n", checkpoint_path_str.c_str(),
            strerror(errno));
         return;
      }
      fprintf(stderr, "Unable to open checkpoint globals file [%s]: unknown error\n", checkpoint_path_str.c_str());
      return;
   }

   size_t size;
   RankInfo world_size(1, 1);
   RankInfo my_rank(0, 0);

   Config cfg(world_size.rank, my_rank.rank == 0);

   std::size_t const arguments_count = arguments.size();

    // All ranks parse the command line
    //auto ret_value = cfg.parseCmdLine(argc-(arguments_count*2), argv+(arguments_count*2), true);
    auto ret_value = cfg.parseCmdLine(argc-(8), argv+(8), true);
    if ( ret_value == -1 ) {
        // Error in command line arguments
        return;
    }
    else if ( ret_value == 1 ) {
        // Just asked for info, clean exit
        return;
    }

   my_rank.rank = 0;
   my_rank.thread = 0;

   world_size.rank = 1;
   world_size.thread = 1;

   static SST::Output g_output;
   if(initfactory) {
      factory = 
         std::make_shared<Factory>(cfg.getLibPath());
   }

   SST::Core::Serialization::serializer ser;
   ser.enable_pointer_tracking();

   {
      /* Begin deserialization, libraries */
      fs.read(reinterpret_cast<char*>(&size), sizeof(size));

      std::vector<char> buffer(size);
      fs.read(&buffer[0], size);

      ser.start_unpacking(&buffer[0], size);

      std::set<std::string> libnames;
      ser&                  libnames;

      /* Load libraries before anything else */
      factory->loadUnloadedLibraries(libnames);	   
   }

   RankInfo num_ranks;
   {
      fs.read(reinterpret_cast<char*>(&size), sizeof(size));
      std::vector<char> buffer(size);
      fs.read(&buffer[0], size);

      ser.start_unpacking(&buffer[0], size);

      ser& num_ranks;
      ser& my_rank;
   }

   Simulation_impl::factory = factory.get();
   Simulation_impl::sim_output = g_output;
   Simulation_impl::resizeBarriers(world_size.thread);

   CheckpointAction::barrier.resize(world_size.thread);   

   SST::Core::MemPoolAccessor::initializeGlobalData(world_size.thread, cfg.cache_align_mempools());
   SST::Core::MemPoolAccessor::initializeLocalData(0);

   std::size_t const count_rankwthread = num_ranks.rank * num_ranks.thread;
   std::vector<std::shared_ptr<Simulation_impl>> sims;
   sims.reserve(count_rankwthread);

   sims.emplace_back(
      Simulation_impl::createSimulation(&cfg, my_rank, world_size, false)
   );

   Simulation_impl::getTimeLord()->init(cfg.timeBase());

   sims.back()->setupSimActions(&cfg);

   sims.back()->checkpoint_directory_ = Checkpointing::initializeCheckpointInfrastructure(
      &cfg, sims.back()->real_time_->canInitiateCheckpoint(), my_rank.rank
   );

   sims.back()->restart(&cfg);

   for(std::size_t i = 1 ; i < count_rankwthread; ++i) {
      RankInfo ri;
      ri.rank = i % world_size.rank;
      ri.thread = i / (world_size.thread+1);
      sims.emplace_back(
         Simulation_impl::createSimulation(&cfg, ri, world_size, false)
      );

      sims.back()->setupSimActions(&cfg);

      sims.back()->checkpoint_directory_ = Checkpointing::initializeCheckpointInfrastructure(
         &cfg, sims.back()->real_time_->canInitiateCheckpoint(), my_rank.rank
      );

      sims.back()->restart(&cfg);
   }

   fs.close();
}

void repartition(std::shared_ptr<Factory> & factory, RankInfo & num_ranks, int const argc, char ** argv, std::uint8_t const part, std::shared_ptr<ConfigGraph> & cfgraph) {

   RankInfo world_size(1, 1);
   RankInfo my_rank(0, 0);

   Config cfg(world_size.rank, my_rank.rank == 0);

   std::size_t const arguments_count = arguments.size();

    // All ranks parse the command line
    auto ret_value = cfg.parseCmdLine(argc-(8), argv+(8), true);
    if ( ret_value == -1 ) {
        // Error in command line arguments
        return;
    }
    else if ( ret_value == 1 ) {
        // Just asked for info, clean exit
        return;
    }

   my_rank.rank = 0;
   my_rank.thread = 0;

   world_size.rank = num_ranks.rank;
   world_size.thread = num_ranks.thread;

   SST::Output g_output;

   Simulation_impl::resizeBarriers(world_size.thread);

   CheckpointAction::barrier.resize(world_size.thread);   

   SST::Core::MemPoolAccessor::initializeGlobalData(world_size.thread, cfg.cache_align_mempools());
   SST::Core::MemPoolAccessor::initializeLocalData(0);

   std::size_t const count_rankwthread = num_ranks.rank * num_ranks.thread;
   std::vector<std::shared_ptr<Simulation_impl>> sims;
   sims.reserve(count_rankwthread);

   sims.emplace_back(
      Simulation_impl::createSimulation(&cfg, my_rank, world_size, false)
   );

   Simulation_impl::getTimeLord()->init(cfg.timeBase());

   sims.back()->setupSimActions(&cfg);

   sims.back()->checkpoint_directory_ = Checkpointing::initializeCheckpointInfrastructure(
      &cfg, sims.back()->real_time_->canInitiateCheckpoint(), my_rank.rank
   );

   sims.back()->restart(&cfg);

   for(std::size_t i = 1 ; i < count_rankwthread; ++i) {
      RankInfo ri;
      ri.rank = i % world_size.rank;
      ri.thread = i / (world_size.thread+1);
      sims.emplace_back(
         Simulation_impl::createSimulation(&cfg, ri, world_size, false)
      );

      sims.back()->setupSimActions(&cfg);

      sims.back()->checkpoint_directory_ = Checkpointing::initializeCheckpointInfrastructure(
         &cfg, sims.back()->real_time_->canInitiateCheckpoint(), my_rank.rank
      );

      sims.back()->restart(&cfg);
   }

   /*

   std::vector<SimThreadInfo_t> threadInfo(world_size.thread);
   for ( uint32_t i = 0; i < world_size.thread; i++ ) {
      threadInfo[i].myRank        = myRank;
      threadInfo[i].myRank.thread = i;
      threadInfo[i].world_size    = world_size;
      threadInfo[i].config        = &cfg;
      threadInfo[i].graph         = graph;
      threadInfo[i].min_part      = min_part;
   }

   Core::MemPoolAccessor::initializeLocalData(tid);
   info.myRank.thread = tid;

   bool restart = info.config->load_from_checkpoint();

   ////// Create Simulation Objects //////
   SST::Simulation_impl* sim = Simulation_impl::createSimulation(info.config, info.myRank, info.world_size, restart);    

   */   

   // Repartition here
   //
   // Repartitioner(Config & config, RankInfo const& total_ranks, RankInfo const& my_rank, Factory & factry, std::uint8_t const partitioner_t, int const verbose=0) :
   //
   Repartitioner rep(cfg, world_size, my_rank, *factory.get(), part, false);
   cfgraph = rep.cfgraph;
   rep();

   std::vector<std::shared_ptr<Simulation_impl>> sims_new;
   sims_new.reserve(count_rankwthread);

   sims_new.emplace_back(
      Simulation_impl::createSimulation(&cfg, my_rank, world_size, false)
   );

   std::shared_ptr<Simulation_impl> & si_back = sims_new.back();

   Simulation_impl::getTimeLord()->init(cfg.timeBase());

   SimTime_t min_part = 0xffffffffffffffffl;

   si_back->setupSimActions(&cfg);

   si_back->checkpoint_directory_ = Checkpointing::initializeCheckpointInfrastructure(
      &cfg, si_back->real_time_->canInitiateCheckpoint(), my_rank.rank
   );

   si_back->processGraphInfo(*cfgraph.get(), my_rank, min_part);
   si_back->prepareLinks(*cfgraph.get(), my_rank, min_part);
   si_back->performWireUp(*cfgraph.get(), my_rank, min_part);
   si_back->exchangeLinkInfo();

   si_back->processGraphInfo(*cfgraph.get(), my_rank, min_part);
   si_back->prepareLinks(*cfgraph.get(), my_rank, min_part);
   si_back->performWireUp(*cfgraph.get(), my_rank, min_part);
   si_back->exchangeLinkInfo();

   for(std::size_t i = 1 ; i < count_rankwthread; ++i) {
      RankInfo ri;
      ri.rank = i % world_size.rank;
      ri.thread = i / (world_size.thread+1);
      sims_new.emplace_back(
         Simulation_impl::createSimulation(&cfg, ri, world_size, false)
      );

      si_back = sims_new.back(); 
      si_back->setupSimActions(&cfg);

      si_back->processGraphInfo(*cfgraph.get(), my_rank, min_part);
      si_back->prepareLinks(*cfgraph.get(), my_rank, min_part);
      si_back->performWireUp(*cfgraph.get(), my_rank, min_part);

      if(ri.thread == 0) {
         si_back->exchangeLinkInfo();
      }
   }

   std::vector< std::shared_ptr<ConfigGraph> > per_rank_subgraphs;
   per_rank_subgraphs.reserve(num_ranks.rank);

   for(std::size_t j = 0; j < num_ranks.rank; ++j) {
      per_rank_subgraphs.emplace_back(
         // get subgraph is closed-range [i,j] 
         cfgraph->getSubGraph(j, j)
      );
   }

   //new_checkpoint(sims, sims_new, per_rank_subgraphs);

   Simulation_impl::shutdown();
}

int main(int argc, char ** argv) {

   std::size_t const arguments_count = arguments.size();

   if( argc == static_cast<int>(arguments.size() << 1) ) {
     std::cerr << "repartitioner requires " << arguments_count << " arguments." << std::endl;
     return -1;
   }

   auto args_end = arguments.end();
   auto arg_itr = arguments.begin();

   for(int i = 1; i < 10; i+=2) {
      arg_itr = arguments.find(argv[i]);

      if(arg_itr == args_end) {
         std::cerr << "invalid argument found\t" << argv[i] << std::endl;
	 continue;
      }

      arg_itr->second = std::string{argv[i+1]};
   }

   if(arguments.find(input_path_argument)->second.size() < 1) {
      std::cerr << "missing --input_path argument" << std::endl;
      return -1;
   }

   if(arguments.find(output_path_argument)->second.size() < 1) {
      std::cerr << "missing --output_path argument" << std::endl;
      return -1;
   }

   std::map<std::string, std::string>::iterator scheme_itr =
      arguments.find(repart_scheme_argument);

   bool is_self = false;
   bool is_linear = false;
   bool is_simple = false;
   bool is_rrobin = false;
   bool is_single = false;

   if(scheme_itr->second.size() < 1) {
      std::cerr << "missing --part_scheme argument" << std::endl;
      return -1;
   }
   else {
      is_self =
         scheme_itr->second.find("self") != std::string::npos;
      is_linear =
         scheme_itr->second.find("linear") != std::string::npos;
      is_simple =
         scheme_itr->second.find("simple") != std::string::npos;
      is_rrobin =
         scheme_itr->second.find("rrobin") != std::string::npos;
      is_single =
         scheme_itr->second.find("single") != std::string::npos;

      if(!(is_self || is_linear || is_simple || is_rrobin || is_single)) {
         std::cerr << "user provided " << scheme_itr->second << " invalid `--scheme` argument. options are \"self, linear, simple, rrobin, single\"" << std::endl;
         return -1;
      }
   }

   std::map<std::string, uint32_t> keymap;
   std::vector<std::string> keymap_rev;

   std::map<std::string, std::string>::iterator input_path_itr =
      arguments.find(input_path_argument);

   std::string & input_path_argval = input_path_itr->second;
   if(!fs::exists(fs::path{input_path_argval})) {
      std::cerr << input_path_argument << ' ' << input_path_argval << ' ' << " does not exist." << std::endl;
   }

   std::shared_ptr<Factory> factory;
   load_checkpoint_globals(input_path_argval, keymap, keymap_rev);

   fs::path dir_path(input_path_argval);
   std::vector<std::string> records;
   for(auto dir_entry : fs::directory_iterator(dir_path.parent_path())) {
      std::string const fs_str = dir_entry.path().string();
      if(fs_str.find("_globals.bin") == std::string::npos && fs_str.find(".bin") != std::string::npos ) {
	      std::cout << fs_str << std::endl;
         records.push_back(fs_str);
      }
   }

   std::uint8_t part_t = 0;
   if(is_self) { part_t = 0; }
   else if(is_linear) { part_t = (1 << 0); }
   else if(is_simple) { part_t = (1 << 1); }
   else if(is_rrobin) { part_t = (1 << 2); }
   else if(is_single) { part_t = (1 << 3); }

   RankInfo ri;
   ri.rank = std::stoi(arguments.find(n_ranks_argument)->second);
   ri.thread = std::stoi(arguments.find(n_threads_argument)->second);

   std::vector<ComponentId_t> components;
   bool factoryinit = true;
   for(auto & path : records) {
      load_checkpoint(path, factory, components, argc, argv, factoryinit);
      factoryinit = false;
   }

   std::shared_ptr<ConfigGraph> cfgraph;
   repartition(factory, ri, argc, argv, part_t, cfgraph);

   return 0;
}
