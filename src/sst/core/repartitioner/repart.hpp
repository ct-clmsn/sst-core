#pragma once
#ifndef __SST_REPARTITIONER_HPP__
#define __SST_REPARTITIONER_HPP__

#include "sst/core/configBase.h"
#include "sst/core/configShared.h"
#include "sst/core/configGraph.h"
#include "sst/core/factory.h"
#include "sst/core/sstpart.h"
#include "sst/core/serialization/serializer.h"
#include "sst/core/checkpointAction.h"
#include "sst/core/model/sstmodel.h"
#include "sst/core/impl/partitioners/linpart.h"
#include "sst/core/impl/partitioners/selfpart.h"
#include "sst/core/impl/partitioners/singlepart.h"
#include "sst/core/impl/partitioners/rrobin.h"
#include "sst/core/impl/partitioners/simplepart.h"

using namespace SST;
using namespace SST::Partition;
using namespace SST::IMPL::Partition;

namespace SST { namespace Repartition {

// type tags that correspond to partitioners found in SST::IMPL::Partition
//
struct self {
   using partitioner_type = SSTSelfPartition;
   static inline const std::string identifier = "sst.self";
};

struct linear {
   using partitioner_type = SSTLinearPartition;
   static inline const std::string identifier = "sst.linear";
};

struct simple {
   using partitioner_type = SimplePartitioner;
   static inline const std::string identifier = "sst.simple";
};

struct rrobin {
   using partitioner_type = SSTRoundRobinPartition;
   static inline const std::string identifier = "sst.roundrobin";
};

struct single {
   using partitioner_type = SSTSinglePartition;
   static inline const std::string identifier = "sst.single";
};

struct partitioners {
   static inline const std::vector<std::string> options = {
      self::identifier,
      linear::identifier,
      simple::identifier,
      rrobin::identifier,
      single::identifier
   };
};

/*
// explicit compile-time type checking of partitioner type tags
//
template<typename T>
using is_partitioner =
   typename std::conditional< std::is_same<T, self>::type::value,
      std::true_type,
      typename std::conditional< std::is_same<T, linear>::type::value,
         std::true_type,
         typename std::conditional< std::is_same<T, single>::type::value,
            std::true_type,
            typename std::conditional< std::is_same<T, rrobin>::type::value,
               std::true_type,
               typename std::conditional< std::is_same<T, simple>::type::value,
                  std::true_type,
                  std::false_type
               >::type
            >::type
         >::type
      >::type
   >::type;   
*/

struct Partition {
   std::uint64_t component_count;
   std::map< std::pair<std::uint64_t, std::uint64_t>,  std::vector<ComponentId_t> > assignment;

   Partition() : component_count(0UL), assignment() {}

   std::ostream & summary(std::ostream & os) {

      for(auto itr = assignment.begin(); itr != assignment.end(); ++itr) {
         os << itr->first.first << ' ' << itr->first.second;
         for(auto aitr = itr->second.begin(); aitr != itr->second.end(); ++aitr) {
            os << ' ' << (*aitr);
         }
      }

      return os;
   }

   void collect_assignment( std::vector< std::tuple<ComponentId_t, std::uint64_t, std::uint64_t> > & alloc) {

      alloc.resize(component_count);

      for(auto itr = assignment.begin(); itr != assignment.end(); ++itr) {
         //os << itr->first.first << ' ' << itr->first.second;
         for(auto aitr = itr->second.begin(); aitr != itr->second.end(); ++aitr) {
            alloc.push_back( std::make_tuple((*aitr), itr->first.first, itr->first.second) );
         }
      }

   }
};

struct Repartition {
   Partition before;
   Partition after;

   Repartition() : before(), after() {}

   std::ostream & summary(std::ostream & os) {
      return after.summary(before.summary(os));
   }
};

//template<typename PartitionerTypeTag>
struct Repartitioner {
/*
   // compile-time type checking
   //
   static_assert(
      is_partitioner<PartitionerTypeTag>::value,
      "Invalid PartitionerTypeTag for Repartitioner type instantiation"
   );
   using partitioner_type =
      typename PartitionerTypeTag::partitioner_type;
*/

   Config & cfg;
   RankInfo const& totalRanks;
   RankInfo const& myRank;
   Factory & factory;
   std::uint8_t partitioner_;
   int const verbosity;
   std::shared_ptr<ConfigGraph> cfgraph;

   Repartitioner(Config & config, RankInfo const& total_ranks, RankInfo const& my_rank, Factory & factry, std::uint8_t const partitioner_t, int const verbose=0) :
      cfg(config), totalRanks(total_ranks), myRank(my_rank), factory(factry), partitioner_(partitioner_t), verbosity(verbose), cfgraph() {
   }

   void operator()() {

      std::vector<std::string> models = ELI::InfoDatabase::getRegisteredElementNames<SSTModelDescription>();

      // Create a map of extensions to the model that supports them
      std::map<std::string, std::string> extension_map;
      for ( auto x : models ) {
         auto extensions = SST::SSTModelDescription::getElementSupportedExtensions(x);
         for ( auto y : extensions ) {
            extension_map[y] = x;
         }
      }

      std::unique_ptr<SSTModelDescription> modelGen;
      std::string model_name;

      if ( cfg.configFile() != "NONE" ) {
         // Get the file extension by finding the last .
         std::string extension =
	    cfg.configFile().substr(cfg.configFile().find_last_of("."));

         try {
            model_name = extension_map.at(extension);
         }
         catch ( std::exception& e ) {
            std::cerr << "Unsupported SDL file type: \"" << extension << "\"" << std::endl;
         }

         // If doing parallel load, make sure this model is parallel capable
         if ( cfg.parallel_load() && !SST::SSTModelDescription::isElementParallelCapable(model_name) ) {
            std::cerr << "Model type for extension: \"" << extension << "\" does not support parallel loading."
                      << std::endl;
         }

         if ( myRank.rank == 0 || cfg.parallel_load() ) {
            modelGen.reset(factory.Create<SSTModelDescription>(
                model_name, cfg.configFile(), cfg.verbose(), &cfg, sst_get_cpu_time()));
         }
      }

      modelGen.reset(factory.Create<SSTModelDescription>(
         model_name, cfg.configFile(), cfg.verbose(), &cfg, sst_get_cpu_time())
      );

      cfgraph.reset(modelGen->createConfigGraph());

      SSTPartitioner * partitioner = nullptr;
      if(partitioner_ == 0) {
         partitioner = 
            factory.CreatePartitioner(self::identifier, totalRanks, myRank, verbosity);
      }
      else if(partitioner_ == (1 << 0)) {
         partitioner =
            factory.CreatePartitioner(linear::identifier, totalRanks, myRank, verbosity);
      }
      else if(partitioner_ == (1 << 1)) {
         partitioner =
            factory.CreatePartitioner(simple::identifier, totalRanks, myRank, verbosity);
      }
      else if(partitioner_ == (1 << 2)) {
         partitioner =
            factory.CreatePartitioner(rrobin::identifier, totalRanks, myRank, verbosity);
      }
      else if(partitioner_ == (1 << 3)) {
         partitioner =
            factory.CreatePartitioner(single::identifier, totalRanks, myRank, verbosity);
      }

      try {
         if ( partitioner->requiresConfigGraph() ) { partitioner->performPartition(cfgraph.get()); }
         else {
	    PartitionGraph  * pgraph = nullptr;
            if ( myRank.rank == 0 ) { pgraph = cfgraph->getCollapsedPartitionGraph(); }
            else {
               pgraph = new PartitionGraph();
            }

            if ( myRank.rank == 0 || partitioner->spawnOnAllRanks() ) {
               partitioner->performPartition(pgraph);

               if ( myRank.rank == 0 ) { cfgraph->annotateRanks(pgraph); }
            }
	    delete pgraph;
         }
      }
      catch ( std::exception& e ) {
	      std::cerr << "Error encountered during graph partitioning phase: " << e.what() << std::endl;
      }

      delete partitioner;

      // Check the partitioning to make sure it is sane
      if ( myRank.rank == 0 || cfg.parallel_load() ) {
         if ( !cfgraph->checkRanks(totalRanks) ) {
		 std::cerr << "ERROR: Bad partitioning; partition included unknown ranks." << std::endl;
         }
      }
   }
};

} /* end Repartition */ } // end SST

#endif
