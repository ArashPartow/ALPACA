/*****************************************************************************************
*                                                                                        *
* This file is part of ALPACA                                                            *
*                                                                                        *
******************************************************************************************
*                                                                                        *
*  \\                                                                                    *
*  l '>                                                                                  *
*  | |                                                                                   *
*  | |                                                                                   *
*  | alpaca~                                                                             *
*  ||    ||                                                                              *
*  ''    ''                                                                              *
*                                                                                        *
* ALPACA is a MPI-parallelized C++ code framework to simulate compressible multiphase    *
* flow physics. It allows for advanced high-resolution sharp-interface modeling          *
* empowered with efficient multiresolution compression. The modular code structure       *
* offers a broad flexibility to select among many most-recent numerical methods covering *
* WENO/T-ENO, Riemann solvers (complete/incomplete), strong-stability preserving Runge-  *
* Kutta time integration schemes, level set methods and many more.                       *
*                                                                                        *
* This code is developed by the 'Nanoshock group' at the Chair of Aerodynamics and       *
* Fluid Mechanics, Technical University of Munich.                                       *
*                                                                                        *
******************************************************************************************
*                                                                                        *
* LICENSE                                                                                *
*                                                                                        *
* ALPACA - Adaptive Level-set PArallel Code Alpaca                                       *
* Copyright (C) 2020 Nikolaus A. Adams and contributors (see AUTHORS list)               *
*                                                                                        *
* This program is free software: you can redistribute it and/or modify it under          *
* the terms of the GNU General Public License as published by the Free Software          *
* Foundation version 3.                                                                  *
*                                                                                        *
* This program is distributed in the hope that it will be useful, but WITHOUT ANY        *
* WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A        *
* PARTICULAR PURPOSE. See the GNU General Public License for more details.               *
*                                                                                        *
* You should have received a copy of the GNU General Public License along with           *
* this program (gpl-3.0.txt).  If not, see <https://www.gnu.org/licenses/gpl-3.0.html>   *
*                                                                                        *
******************************************************************************************
*                                                                                        *
* THIRD-PARTY tools                                                                      *
*                                                                                        *
* Please note, several third-party tools are used by ALPACA. These tools are not shipped *
* with ALPACA but available as git submodule (directing to their own repositories).      *
* All used third-party tools are released under open-source licences, see their own      *
* license agreement in 3rdParty/ for further details.                                    *
*                                                                                        *
* 1. tiny_xml           : See LICENSE_TINY_XML.txt for more information.                 *
* 2. expression_toolkit : See LICENSE_EXPRESSION_TOOLKIT.txt for more information.       *
* 3. FakeIt             : See LICENSE_FAKEIT.txt for more information                    *
* 4. Catch2             : See LICENSE_CATCH2.txt for more information                    *
*                                                                                        *
******************************************************************************************
*                                                                                        *
* CONTACT                                                                                *
*                                                                                        *
* nanoshock@aer.mw.tum.de                                                                *
*                                                                                        *
******************************************************************************************
*                                                                                        *
* Munich, July 1st, 2020                                                                 *
*                                                                                        *
*****************************************************************************************/
#include "topology_manager.h"

#include <algorithm>
#include <utility>
#include <mpi.h>
#include <stdexcept>
#include <string>
#include <numeric>
#include <functional>

#include "communication/mpi_utilities.h"
#include "topology/id_information.h"

/**
 * @brief Default constructor. Creates the local and global Id list on level zero. Default arguments allow testability.
 * @param maximum_level The possible maximum level in this simulation.
 * @param level_zero_nodes_x, level_zero_nodes_y, level_zero_nodes_z Number of blocks on level zero in the x/y/z-axis extension.
 * @param active_periodic_locations Side of the domain on which periodic boundaries are activated.
 */
TopologyManager::TopologyManager( unsigned int const maximum_level, unsigned int const level_zero_nodes_x, unsigned int const level_zero_nodes_y,
                                  unsigned int const level_zero_nodes_z, unsigned int const active_periodic_locations ) :
   maximum_level_( maximum_level ),
   active_periodic_locations_( active_periodic_locations ),
   level_zero_nodes_xyz_{ level_zero_nodes_x, level_zero_nodes_y, level_zero_nodes_z },
   forest_{},
   coarsenings_since_load_balance_{0},
   refinements_since_load_balance_{0}
{
   std::uint64_t id = IdSeed();

   std::vector<std::uint64_t> initialization_list;

   /*
    * The Nodes are created in a spatial fashion traversing through X, Y and finally Z.
    * The ids in this traversal are not continuous due to the implicit shadow levels in the tree
    * Therefore some non-straightforward index magic needs to be applied
    * Initialization happens only on Level 0
    */
   for(unsigned int i = 0; i < level_zero_nodes_xyz_[2]; ++i) {
      for(unsigned int j = 0; j < level_zero_nodes_xyz_[1]; ++j) {
         for(unsigned int k = 0; k < level_zero_nodes_xyz_[0]; ++k) {
            forest_.emplace_back(id, 0);
            initialization_list.push_back(id);
            id = EastNeighborOfNodeWithId(initialization_list.back()); //find eastern neighbor of the just created Node
         }
         // Index magic to create the correct node once the (outer) loop counters are resetted
         id = NorthNeighborOfNodeWithId( initialization_list[ level_zero_nodes_xyz_[0] * ( j + level_zero_nodes_xyz_[1] * i )] );
      }
      id = TopNeighborOfNodeWithId( initialization_list[( level_zero_nodes_xyz_[1] * level_zero_nodes_xyz_[0] * i)]);
   }

   //Topology Tree Node creation
   forest_.shrink_to_fit();

   int const number_of_ranks = MpiUtilities::NumberOfRanks();

   // Now we distribute the global nodes accordingly:
   unsigned int number_of_nodes = forest_.size();
   int remainder = number_of_nodes % number_of_ranks;
   int fraction = 0;

   std::vector<int> rank_map;

   for(int i = 0; i < number_of_ranks; ++i) {
      fraction = 0;
      fraction += number_of_nodes / number_of_ranks;
      if(i < remainder) {
         fraction++;
      }
      rank_map.push_back(fraction);
   }

   //Topology Tree Rank setup
   int sum = 0;
   for(int i = 0; i < number_of_ranks; i++) {
      for(int j = 0; j < rank_map[i]; j++) {
         forest_[sum].SetCurrentRankOfLeaf(forest_[sum].Id(), i);
         sum++;
      }
   }
}

/**
 * @brief Updates the topology based on the recorded refinements, coarsening, weights and fluid changes.
 * @return True if Communication_managers cache needs to be invalidated.
 * @note The coarsening list is specially guarded, it needs to be flushed before it is considered here.
 */
bool TopologyManager::UpdateTopology() {

   // Flag that specifies whether the communication manager should update its neighbor relations
   bool invalidate_communication_manager_cache = false;
   int const number_of_ranks = MpiUtilities::NumberOfRanks();

   //Tree update
   //refine
   std::vector<std::uint64_t> global_refine_list;
   MpiUtilities::LocalToGlobalData( local_refine_list_, MPI_LONG_LONG_INT, number_of_ranks, global_refine_list );

   for(auto const& refine_id : global_refine_list) {
      forest_[PositionOfNodeInZeroTopology(refine_id)].Refine(refine_id);
   }
   // Invalididate cache if any node has been refined
   if(global_refine_list.size() > 0) {
      invalidate_communication_manager_cache = true;
   }

   local_refine_list_.clear();

   refinements_since_load_balance_ += global_refine_list.size();

   //UPDATE FLUIDS OF NODES
   std::tuple<std::vector<std::uint64_t>, std::vector<MaterialName>> global_fluids_list;
   MpiUtilities::LocalToGlobalData(std::get<0>( local_added_fluids_list_), MPI_LONG_LONG_INT,  number_of_ranks, std::get<0>(global_fluids_list) );
   MpiUtilities::LocalToGlobalData(std::get<1>( local_added_fluids_list_), MPI_UNSIGNED_SHORT, number_of_ranks, std::get<1>(global_fluids_list) );

#ifndef PERFORMANCE
   if(std::get<0>(global_fluids_list).size() != std::get<1>(global_fluids_list).size()) {
      throw std::logic_error("Thou shall not create material-add-lists of unequal length");
   }
#endif

   for(unsigned int i = 0; i < std::get<0>(global_fluids_list).size(); ++i) {
      int const position = PositionOfNodeInZeroTopology(std::get<0>(global_fluids_list)[i]);

      if(position >= 0 && position < static_cast<int>(forest_.size())) {
         // Forest size cannot exceed 128x128x128 = 10^6, fits in int (10^9 positive values), cast is safe.
         forest_[position].AddFluid(std::get<0>(global_fluids_list)[i], std::get<1>(global_fluids_list)[i]);
      }
#ifndef PERFORMANCE
      else {
         throw std::logic_error("TopologyManager::UpdateIdsList root tree does not exist!");
      }
#endif
   }

   std::get<0>(local_added_fluids_list_).clear();
   std::get<1>(local_added_fluids_list_).clear();

   // We reuse the global list
   std::get<0>(global_fluids_list).clear();
   std::get<1>(global_fluids_list).clear();

   MpiUtilities::LocalToGlobalData( std::get<0>( local_removed_fluids_list_), MPI_LONG_LONG_INT,  number_of_ranks, std::get<0>(global_fluids_list) );
   MpiUtilities::LocalToGlobalData( std::get<1>( local_removed_fluids_list_), MPI_UNSIGNED_SHORT, number_of_ranks, std::get<1>(global_fluids_list) );

#ifndef PERFORMANCE
   if(std::get<0>(global_fluids_list).size() != std::get<1>(global_fluids_list).size()) {
      throw std::logic_error("Created list of unequal length");
   }
#endif

   for(unsigned int i = 0; i < std::get<0>(global_fluids_list).size(); ++i) {
      int const position = PositionOfNodeInZeroTopology(std::get<0>(global_fluids_list)[i]);
      // Forest size cannot exceed 128x128x128 = 10^6, fits in int (10^9 positive values), cast is safe.
      if(position >= 0 && position < static_cast<int>(forest_.size())) {
         forest_[position].RemoveFluid(std::get<0>(global_fluids_list)[i], std::get<1>(global_fluids_list)[i]);
      }
#ifndef PERFORMANCE
      else {
         throw std::logic_error("TopologyManager::UpdateIdsList root tree does not exist!");
      }
#endif
   }

   std::get<0>(local_removed_fluids_list_).clear();
   std::get<1>(local_removed_fluids_list_).clear();

   return invalidate_communication_manager_cache;
}

/**
 * @brief Marks the node with the given id for refinement.
 * @param id The id of the leaf that is to be refined.
 * @note The actual refinement happens in a bundled fashion in another function. $No checks for leaves are performed caller is responsible$
 */
void TopologyManager::RefineNodeWithId(std::uint64_t const id) {
   local_refine_list_.push_back(id);
}

/**
 * @brief Adds the parent whose children may be coarsened to the coarsening list. The actual data deletion is than bundled using this list.
 * @param parent_id The id of the node that is to be made a leaf.
 */
void TopologyManager::CoarseNodeWithId(std::uint64_t const parent_id) {
   forest_[PositionOfNodeInZeroTopology(parent_id)].Coarse(parent_id);
   coarsenings_since_load_balance_++;
}

/**
 * @brief Determines the rank which holds the node of given id.
 * @param id The unique id of the node.
 * @return Rank id for the requested node.
 */
int TopologyManager::GetRankOfNode(std::uint64_t const id) const {
   return forest_[PositionOfNodeInZeroTopology(id)].GetRank(id);
}

/**
 * @brief Gives a list which indicates which node should go from which mpi rank onto which mpi rank.
 * @param number_of_ranks The number of ranks available to distribute the load onto.
 * @return A vector of all nodes and their current as well as their future mpi rank.
 */
std::vector<std::tuple<std::uint64_t const, int const, int const>> TopologyManager::GetLoadBalancedTopology(int const number_of_ranks) {

   AssignTargetRankToLeaves(number_of_ranks);
   AssignBalancedLoad();
   std::vector<std::tuple<std::uint64_t const, int const, int const>> ids_current_future_rank_map;

   ListNodeToBalance(ids_current_future_rank_map);

   return ids_current_future_rank_map;
}

/**
 * @brief Indicates whether a node exists in the global Tree, does not make implications about local tree
 * @param id id of the node one is looking for
 * @return true if node exists, false otherwise
 */
bool TopologyManager::NodeExists(std::uint64_t const id) const {

   int position_in_zero_topology = PositionOfNodeInZeroTopology(id);
   // Forest size cannot exceed 128x128x128 = 10^6, fits in int (10^9 positive values), cast is safe.
   if(position_in_zero_topology >= 0 && position_in_zero_topology < (static_cast<int>(forest_.size())) ) {
      return forest_[position_in_zero_topology].NodeExists(id);
   } else {
      return false;
   }
}

/**
 * @brief Gives the current maximum level of any global node.
 *        Can be less than the user set maximum level (if no interesting physics are present, or at initialization).
 * @return Globally Maximal Present Level.
 */
unsigned int TopologyManager::GetCurrentMaximumLevel() const {

   std::vector<unsigned int> tree_depths;
   tree_depths.reserve(forest_.size());
   for(const TopologyNode& tree : forest_) {
      tree_depths.emplace_back(tree.GetDepth());
   }

   return *std::max_element(tree_depths.begin(),tree_depths.end()) -1; //Tree Depth starts with 1 by definition, vs levels starts at 0.
}

/**
 * @brief Gives the ids of all globally existing nodes which descent from the given id.
 *        I.e. children, grand-children great-grand-children, ...
 * @param id Unique id of node whose descendants are searched for.
 * @return All ids of globally existing descendants.
 */
std::vector<std::uint64_t> TopologyManager::DescendantIdsOfNode(std::uint64_t const id) const {

   std::vector<std::uint64_t> descendants;
   std::vector<std::uint64_t> append_list;

   for(auto const& child_id : IdsOfChildren(id)) {
      if(NodeExists(child_id)) {
         append_list = DescendantIdsOfNode(child_id);
         descendants.insert(descendants.end(),append_list.begin(), append_list.end());
         descendants.push_back(child_id);
      }
   }

   return descendants;
}

/**
 * @brief Indicates whether the invoking MPI rank holds the given node. $Throws exception if node does not exist!$
 * @param id Unique id of the node to be checked for self-ownership.
 * @param rank The rank on which existence is to be checked.
 * @return true if node is on the same rank as invoker, false otherwise.
 */
bool TopologyManager::NodeIsOnRank(std::uint64_t const id, int const rank) const {

#ifndef PERFORMANCE
   if(!NodeExists(id)) {
      throw std::logic_error("Node Ownership cannot be checked - Node does not exist");
   }
#endif

   return GetRankOfNode(id) == rank;
}

/**
 * @brief Indicates whether the given node is a leaf or not. $Throws exception if node does not exist!$
 * @param id Unique id of the node to be checked.
 * @return true if node is a leaf, false otherwise.
 */
bool TopologyManager::NodeIsLeaf(std::uint64_t const id) const {

#ifndef PERFORMANCE
   if(!NodeExists(id)) {
      throw std::logic_error("Node Leaf status cannot be checked - Node does not exist");
   }
#endif

   return forest_[PositionOfNodeInZeroTopology(id)].NodeIsLeaf(id);
}

/**
 * @brief Determines if the specified node is facing a jump at the given location.
 *        I.e. face does not have a global boundary and no neighbor on the same level exists.
 * @param id Unique id of the node under consideration.
 * @param location Location of interest.
 * @return True if the Face is a Jump, false otherwise.
 */
bool TopologyManager::FaceIsJump(std::uint64_t const id, BoundaryLocation const location) const{

   if(IsExternalTopologyBoundary(location, id)) {
      return false;
   }

   // If the neighbor does not exist and it is not an external BC we have a jump
   return !NodeExists(GetTopologyNeighborId(id, location));
}

/**
 * @brief Gives a list of all leaves on this MPI rank
 * @return Local leaf ids.
 */
std::vector<std::uint64_t> TopologyManager::LocalLeafIds() const {
   std::vector<std::uint64_t> local_leaves;
   int const rank_id = MpiUtilities::MyRankId();
   for(TopologyNode const& node : forest_) {
      node.LocalLeaves(local_leaves, rank_id);
   }
   return local_leaves;
}

/**
 * @brief Gives a list of ids of all globally present leaves.
 * @return Leaf Ids.
 */
std::vector<std::uint64_t> TopologyManager::LeafIds() const {
   std::vector<std::uint64_t> leaves;
   for(TopologyNode const& node : forest_) {
      node.GetLeafIds(leaves);
   }
   return leaves;
}

/**
 * @brief Gives the ids of all locally present leaves on the specified level.
 * @param level The level of interest.
 * @return The list of leaf ids.
 */
std::vector<std::uint64_t> TopologyManager::LocalLeafIdsOnLevel(unsigned int const level) const {
   std::vector<std::uint64_t> leaves;
   int const rank_id = MpiUtilities::MyRankId();
   for(TopologyNode const& node : forest_) {
      node.LocalLeavesOnLevel(leaves, rank_id, level);
   }
   return leaves;
}

/**
 * @brief Gives the ids of all globally present leaves on the specified level.
 * @param level The level of interest.
 * @return The list of leaf ids.
 */
std::vector<std::uint64_t> TopologyManager::LeafIdsOnLevel(unsigned int const level) const {
   std::vector<std::uint64_t> leaves;
   for(TopologyNode const& node : forest_) {
      node.GetLeafIdsOnLevel(leaves, level);
   }
   return leaves;
}

/**
 * @brief Assigns the target rank (rank on which the node SHOULD reside) to all leaf nodes.
 *        Uses either a linear or Hilbert Traversal to determine the target rank.
 * @param number_of_ranks The number of ranks available to distribute the load onto.
 */
void TopologyManager::AssignTargetRankToLeaves( int const number_of_ranks) {

   std::vector<std::vector<std::tuple<unsigned int, int>>> ids_current_future_rank_map; //[level][(weight,rank)]
   ids_current_future_rank_map.reserve(number_of_ranks);

   std::vector<unsigned int> weight_list(WeightsOnLevels());

   for(unsigned int level=0; level <= maximum_level_; level++) {
      int const weight_per_rank = weight_list[level] / number_of_ranks; //Here normal ints due to MPI Standard. (for ranks and number of ranks)
      int const remainder       = weight_list[level] % number_of_ranks;
      ids_current_future_rank_map.push_back(std::vector<std::tuple<unsigned int, int>>());
      for(int rank = 0; rank < number_of_ranks; rank++) {
         if(rank < remainder) {
            ids_current_future_rank_map[level].push_back(std::make_tuple(weight_per_rank+1, rank));
         } else {
            ids_current_future_rank_map[level].push_back(std::make_tuple(weight_per_rank , rank));
         }
      }
      std::reverse(ids_current_future_rank_map[level].begin(), ids_current_future_rank_map[level].end());
   }

#ifndef HILBERT
   for(TopologyNode& node : forest_) {
      node.SetTargetRankForLeaf(ids_current_future_rank_map);
   }
#else

   // We traverse the domain in a Hilbert curve traversal, as we have shadow levels, however, some considerations on the forest_ have to be
   // done so the curve is correct.

   bool x_inverse = false; //Inverse: reverse traversal when going back.
   bool y_inverse = false;
   for( unsigned int i = 0; i < level_zero_nodes_xyz_[2]; ++i ) {
      if( !y_inverse ) {
         for( unsigned int j = 0; j < level_zero_nodes_xyz_[1]; ++j ) {
            if(!x_inverse){
               for( unsigned int k = 0; k < level_zero_nodes_xyz_[0]; ++k ) {
                  //Implicit type conversions, but do no harm
                  int z = i * level_zero_nodes_xyz_[1] * level_zero_nodes_xyz_[0] + j * level_zero_nodes_xyz_[0] + k;
                  forest_[z].SetTargetRankForLeaf( ids_current_future_rank_map, HilbertPosition::z_x_y );
               }
            } else {
               for( int k = level_zero_nodes_xyz_[0] - 1; k >=0 ; --k) {
                  //Implicit type conversions, but do no harm
                  int z = i * level_zero_nodes_xyz_[1] * level_zero_nodes_xyz_[0] + j * level_zero_nodes_xyz_[0] + k;
                  forest_[z].SetTargetRankForLeaf( ids_current_future_rank_map, HilbertPosition::_z_xy );
               }
            }
            x_inverse = !x_inverse;
         }
      } else {
         for( int j = level_zero_nodes_xyz_[1] - 1; j >= 0; --j ) {
            if( !x_inverse ) {
               for( unsigned int k = 0; k < level_zero_nodes_xyz_[0]; ++k ) {
                  //Implicit type conversions, but do no harm
                  int z = i * level_zero_nodes_xyz_[1] * level_zero_nodes_xyz_[0] + j * level_zero_nodes_xyz_[0] + k;
                  forest_[z].SetTargetRankForLeaf( ids_current_future_rank_map, HilbertPosition::z_x_y );
                 }
            } else {
               for(int k = level_zero_nodes_xyz_[0] - 1; k >=0 ; --k) {
                  //Implicit type conversions, but do no harm
                  int z = i * level_zero_nodes_xyz_[1] * level_zero_nodes_xyz_[0] + j * level_zero_nodes_xyz_[0] + k;
                  forest_[z].SetTargetRankForLeaf(ids_current_future_rank_map, HilbertPosition::_z_xy);
               }
            }
            x_inverse = !x_inverse;
         }
      }
      y_inverse = !y_inverse;
   }
#endif

#ifndef PERFORMANCE
   // Sanity Check.
   for(unsigned int i = 0; i <= maximum_level_; i++) {
      if(std::get<0>(ids_current_future_rank_map[i].back()) != 0) {
         throw std::logic_error("TopologyManager::AssignTargetRankToLeaves Some miscalculations in the distribution of leaves per rank");
      }
   }
#endif
}

/**
 * @brief Gives the position of the zero level ancestor node identified by the given id in the forest.
 * @param id The id of the node whose ancestor's position is to be determined
 * @return The index of the ancestor node in the zero topology. -1 If no such node could be found.
 */
int TopologyManager::PositionOfNodeInZeroTopology(std::uint64_t const id) const {

   std::uint64_t level_zero_id = id;
   while(LevelOfNode(level_zero_id) != 0) {
      level_zero_id = ParentIdOfNode(level_zero_id);
   }

   auto node_iterator = std::find_if(forest_.begin(),forest_.end(),[&level_zero_id](const TopologyNode& node){return node.Id() == level_zero_id;});

   if(node_iterator == forest_.end()) {
      return -1;
   }  else {
      return std::distance(forest_.begin(),node_iterator);
   }
}

/**
 * @brief Calculates a balanced distribution of nodes among the MPI ranks and assigns the determined rank to the nodes.
 *        Does not directly shift nodes among ranks! "Prepares for sending Load".
 */
void TopologyManager::AssignBalancedLoad() {
   for(TopologyNode& node : forest_) {
      node.BalanceTargetRanks();
   }
}

/**
 * @brief Gives a list of all nodes, that need to be balanced, i.e. shifted to another MPI rank.
 * @param ids_current_future_rank_map Indirect return parameter.
 * @note Lists the ranks to be balanced as tuple of their id, their current rank and the rank they are supposed to be shifted to
 */
void TopologyManager::ListNodeToBalance(std::vector<std::tuple<std::uint64_t const, int const, int const>>& ids_current_future_rank_map) {
   for(TopologyNode& node : forest_) {
      node.ListUnbalancedNodes(ids_current_future_rank_map);
   }
}

/**
 * @brief Gives some statistics about the distribution of leaves on the ranks.
 * @param number_of_ranks The number of ranks for which distribution is to be documented.
 * @return Formatted string of the leaf-rank distribution.
 */
std::string TopologyManager::LeafRankDistribution(int const number_of_ranks) {

   std::string leaf_rank_distribution;

   std::vector<std::vector<int>> leaves_per_level_per_rank( maximum_level_ + 1, std::vector<int>(number_of_ranks,0) );
   for( unsigned int level = 0; level < maximum_level_ + 1; level++ ) {
      std::vector<std::uint64_t> leaves = LeafIdsOnLevel(level);
      for(std::uint64_t id : leaves) {
         int const rank = GetRankOfNode(id);
         leaves_per_level_per_rank[level][rank]++;
      }
   }

   leaf_rank_distribution.append("+++ leave rank distribution +++ ");
   for( unsigned int level = 0; level < maximum_level_ + 1; level++ ) {
      leaf_rank_distribution.append("Level: " + std::to_string(level));
      for(int rank = 0; rank < number_of_ranks; rank++) {
         leaf_rank_distribution.append("Rank: " + std::to_string(rank) + " --> " + std::to_string(leaves_per_level_per_rank[level][rank]) + " | ");
      }
      leaf_rank_distribution.append(" - ");
   }

   return leaf_rank_distribution;
}

/**
 * @brief Gives a list with the combined computation load (weight) on all levels.
 * @return List of summed weights on each level.
 */
std::vector<unsigned int> TopologyManager::WeightsOnLevels() const {
   std::vector<unsigned int> weights_on_level( maximum_level_ + 1 ); //If max level is = 2 we need 3 elements "0, 1, 2".
   for(const TopologyNode& node : forest_){
      node.ChildWeight(weights_on_level);
   }
   return weights_on_level;
}

/**
 * @brief Gives out the ids of all globally existent nodes on the specified level.
 * @param level Level of interest.
 * @return Ids of Nodes on level.
 */
std::vector<std::uint64_t> TopologyManager::GlobalIdsOnLevel(unsigned int const level) const {
   std::vector<std::uint64_t> ids;
   for(const TopologyNode& node : forest_) {
      node.IdsOnLevel(level,ids);
   }
   return ids;
}

/**
 * @brief Gives out the ids of only locally existent nodes on the specifed level for a given rank
 * @param level Level of interest.
 * @param rank_id The rank for which the node ids should be given
 * @return Ids of local Nodes on level.
 */
std::vector<std::uint64_t> TopologyManager::IdsOnLevelOfRank(unsigned int const level, int const rank_id) const {
   std::vector<std::uint64_t> ids;
   for(const TopologyNode& node : forest_) {
      node.LocalIdsOnLevel(level, ids, rank_id);
   }
   return ids;
}

/**
 * @brief Gives whether the node with the given id is a multi-phase node, i.e. contains more than one material.
 * @param id Id of the node in question.
 * @return True if the node is multi-phase, false if it is single-phase.
 * 
 * @note This is different to the call function node.HasLevelset(). Only on the finest level both are equivalent. Multiphase nodes can also exist on coarser 
 *       levels, whereas levelset containing nodes cannot. 
 */
bool TopologyManager::IsNodeMultiPhase(std::uint64_t const id) const {
   return forest_[PositionOfNodeInZeroTopology(id)].GetFluids(id).size() > 1;
}

/**
 * @brief Adds the given material to the node with the given id.
 * @param id Id of the node the material should be added to.
 * @param material The material to be added to the node.
 */
void TopologyManager::AddFluidToNode(std::uint64_t const id, MaterialName const material) {
   std::get<0>(local_added_fluids_list_).push_back(id);
   std::get<1>(local_added_fluids_list_).push_back(material);
}

/**
 * @brief Removes the given material from the node with the given id.
 * @param id Id of the node the material should be removed from.
 * @param material The material to be removed from the node.
 */
void TopologyManager::RemoveFluidFromNode(std::uint64_t const id, MaterialName const material) {
   std::get<0>(local_removed_fluids_list_).push_back(id);
   std::get<1>(local_removed_fluids_list_).push_back(material);
}

/**
 * @brief Gives the sorted materials list of the phases present in the given node.
 * @param id Id of the node in question.
 * @return Vector of the materials in the node.
 */
std::vector<MaterialName> TopologyManager::GetFluidsOfNode(std::uint64_t const id) const {
   return forest_[PositionOfNodeInZeroTopology(id)].GetFluids(id);
}

/**
 * @brief Gives the fluid in a single phase node.
 * @param id Node id.
 * @return The fluid.
 */
MaterialName TopologyManager::SingleFluidOfNode(std::uint64_t const id) const {
   return forest_[PositionOfNodeInZeroTopology(id)].GetSingleFluid(id);
}

/**
 * @brief Gives whether the given material is present in the given node.
 * @param node_id Id of the node in question.
 * @param material Material in question.
 * @return True if the material is present in the node, false otherwise.
 */
bool TopologyManager::NodeContainsFluid(std::uint64_t const node_id, MaterialName const material) const {
   auto materials = GetFluidsOfNode(node_id);
   auto block_iterator = std::find(materials.begin(),materials.end(),material);
   if(block_iterator == materials.end()) {
      return false;
   } else {
      return true;
   }
}

/**
 * @brief Indicates - based on the number of mesh changes since last load balancing - whether or not load balancing should be executed.
 * @return Indicator for load balancing.
 */
bool TopologyManager::IsLoadBalancingNecessary(){
   // Check whether load balancing is required based on CC chosen value
   if( coarsenings_since_load_balance_ >= CC::TCULB() || refinements_since_load_balance_ >= CC::TCULB() ) {
      coarsenings_since_load_balance_= 0;
      refinements_since_load_balance_= 0;
      return true;
   } else {
      return false;
   }
}

/**
 * @brief returns the number of global nodes and leaves in a std::pair
 * @return std::pair<#Nodes, #Leaves> 
 */
std::pair<unsigned int, unsigned int> TopologyManager::NodeAndLeafCount() const {
   std::pair<unsigned int,unsigned int> node_leaf_count = std::make_pair<unsigned int, unsigned int>(0,0);
   for(const TopologyNode& node : forest_) {
      node.NodeLeafCount(node_leaf_count);
   }
   return node_leaf_count;
}

/**
 * @brief Gives a list of pairs. An entry at index i corresponds to the MPI rank_id i. It lists the node and leaf count on this rank
 * @return Vector of std::pair<#Nodes, #Leaves> of size total number of ranks.
 */
std::vector<std::pair<unsigned int, unsigned int>> TopologyManager::NodesAndLeavesPerRank() const {
   std::vector<std::pair<unsigned int, unsigned int>> nodes_and_leaves_per_rank;
   for(const TopologyNode& node : forest_) {
      node.RankWiseNodeLeafCount(nodes_and_leaves_per_rank);
   }
   return nodes_and_leaves_per_rank;
}

/**
 * @brief returns the number of nodes and blocks in a std::pair
 * @return std::pair<#Nodes, #Blocks>
 */
std::pair<unsigned int, unsigned int> TopologyManager::NodeAndBlockCount() const {
   std::pair<unsigned int,unsigned int> node_block_count = std::make_pair<unsigned int, unsigned int>(0,0);
   for(const TopologyNode& node : forest_) {
      node.NodeBlockCount(node_block_count);
   }
   return node_block_count;
}

/**
 * @brief Gives a list of pairs. An entry at index i corresponds to the MPI rank_id i. It lists the node and block count on this rank
 * std::pair<#Nodes, #Blocks>.
 * @return Vector of std::pair<#Nodes, #Blocks> of size total number of ranks.
 */
std::vector<std::pair<unsigned int, unsigned int>> TopologyManager::NodesAndBlocksPerRank() const {
   std::vector<std::pair<unsigned int, unsigned int>> nodes_and_blocks_per_rank;
   for(const TopologyNode& node : forest_) {
      node.RankWiseNodeBlockCount(nodes_and_blocks_per_rank);
   }
   return nodes_and_blocks_per_rank;
}

/**
 * @brief Gives the global count of nodes holding more than one phase in the topology.
 * @return Number of Multiphase nodes
 */
unsigned int TopologyManager::MultiPhaseNodeCount() const {
   unsigned int count = 0;
   for( TopologyNode const& node : forest_ ) {
      count += node.MultiPhaseNodeCount();
   }
   return count;
}

/**
 * @brief Restores the complete topology based on a list of node ids, the number of phases foreach node and the material identifiers of the
 *  respective phases. The topology is also load balanced.
 * @param ids A global list of all nodes that are part of this topology.
 * @param number_of_phases The number of phases present in each node. Has to have the same length as ids.
 * @param materials The material identifiers for all phases. The length equals the accumulation of all entries in number_of_phases.
 * @return A list identifying the nodes that are handled by the current rank by means of their indices in the input list ids.
 */
std::vector<unsigned int> TopologyManager::RestoreTopology( std::vector<std::uint64_t> ids, std::vector<unsigned short> number_of_phases,
                                                            std::vector<unsigned short> materials ) {
   std::array<std::vector<unsigned int>, CC::AMNL()> indices_on_level;
   for(unsigned int index = 0; index < ids.size(); ++index) {
      indices_on_level[LevelOfNode(ids[index])].push_back(index);
   }

   // sanity check on level 0 (no PERFORMANCE macro required here since only used during restart of simulations)
   if(indices_on_level[0].size() != forest_.size()) {
      throw std::runtime_error("Level-zero topology of input and restart file do not match! (size)");
   }
   for(auto const index_node : indices_on_level[0]) {
      if(PositionOfNodeInZeroTopology(ids[index_node]) == -1) {
         throw std::runtime_error("Level-zero topology of input and restart file do not match! (topology)");
      }
   }

   // build up the topology tree
   for(unsigned int level = 0; level < CC::AMNL(); ++level) {
      for(auto const index_node : indices_on_level[level]) {
         std::uint64_t const id = ids[index_node];
         TopologyNode& root = forest_[PositionOfNodeInZeroTopology(id)];
         // check whether the parent has to be refined
         if(!root.NodeExists(id)) {
            root.Refine(ParentIdOfNode(id)); // safe to call on level 0 because the nodes always exist
         }
         // assign the node's materials
         unsigned int offset_material = std::accumulate(number_of_phases.begin(), number_of_phases.begin()+index_node, 0);
         for(unsigned int index_material = offset_material; index_material < offset_material + number_of_phases[index_node]; ++index_material) {
            root.AddFluid(id, static_cast<MaterialName>(materials[index_material]));
         }
      }
   }

   // load balance topology
   GetLoadBalancedTopology( MpiUtilities::NumberOfRanks() );

   // return the indices in the input list of the nodes that ended up on this rank
   std::vector<unsigned int> local_indices;
   int const rank_id = MpiUtilities::MyRankId();
   for(unsigned int index = 0; index < ids.size(); ++index) {
      if(GetRankOfNode(ids[index]) == rank_id) {
         local_indices.push_back(index);
      }
   }
   return local_indices;
}

/**
 * @brief Returns a list of all neighboring leaf ids of a node in a given direction.
 * @param node_id Id of the node in question.
 * @param direction Direction in which to check for neighbors.
 * @return List of tuple of node ids of neighboring leaves and their tree level difference compared to node_id 
 */
std::vector<std::uint64_t> TopologyManager::GetNeighboringLeaves(std::uint64_t const node_id, BoundaryLocation const direction)const {

   std::vector<std::uint64_t> id_list;
   if( !IsExternalBoundary( direction, node_id, level_zero_nodes_xyz_) ) {
      std::uint64_t neighbor_id = GetNeighborId( node_id, direction );
      if( NodeExists( neighbor_id ) ){
         //find set of lowest level neighbors (leaves)
         std::function<bool( std::uint64_t const )> sibling_function;
         switch( direction ) {
            case BoundaryLocation::West: {
               sibling_function = EastInSiblingPack;
            }
            break;
            case BoundaryLocation::East: {
               sibling_function = WestInSiblingPack;
            }
            break;
            case BoundaryLocation::North: {
               sibling_function = SouthInSiblingPack;
            }
            break;
            case BoundaryLocation::South: {
               sibling_function = NorthInSiblingPack;
            }
            break;
            case BoundaryLocation::Bottom: {
               sibling_function = TopInSiblingPack;
            }
            break;
#ifndef PERFORMANCE
            case BoundaryLocation::Top: {
               sibling_function = BottomInSiblingPack;
            }
            break;
            default:
               throw std::invalid_argument("Got invalid direction for TopologyManager::GetNeighborLeavesAndLevelDifference()");
#else 
            default: /* BoundaryLocation::Top */ {
               sibling_function = BottomInSiblingPack;
            }
#endif
         }
         std::vector<std::uint64_t> open_neighbor_ids;
         open_neighbor_ids.push_back( neighbor_id );
         while( open_neighbor_ids.size() > 0 ){
            std::uint64_t open_id = open_neighbor_ids.back();
            open_neighbor_ids.pop_back();
            if( NodeIsLeaf( open_id ) ) {
               //open_id is leaf node -> add to list
               id_list.push_back( open_id );
            }else{
               //open_id has children -> get the relevant ones
               std::vector<std::uint64_t> const open_children_ids = IdsOfChildren( open_id );
               for( std::uint64_t open_children_id : open_children_ids ){
                  if( sibling_function( open_children_id ) ) {
                   open_neighbor_ids.push_back( open_children_id );
                  }
               }
            }
         }
      }else{
          //neighbor has lower resolution
          while(!NodeExists(neighbor_id) && neighbor_id > 2){
              neighbor_id = ParentIdOfNode(neighbor_id);
          }
          //should not need test, since node is not supposed to be boundary
          if(neighbor_id > 2) id_list.push_back(neighbor_id);
      }
   }
   return id_list;
   //Note: This function could also be implemented recursively over topology nodes
}

/**
 * @brief Gives a rank specific offset, i. e. a count of how many leafs are on lower (by rank id) rank.
 *        (e.g., three ranks with three leaves each. Offset rank 0 = 0, Offset rank 1 = 3, Offset rank 2 = 6)
 * @param rank The rank for which the offset is to be obtained.
 * @return The offset.
 */
long long unsigned int TopologyManager::LeafOffsetOfRank( int const rank ) const {
   std::vector<std::pair<unsigned int, unsigned int>>&& rank_node_map = NodesAndLeavesPerRank();
   auto const cend = rank_node_map.size() > std::size_t( rank ) ? rank_node_map.cbegin() + rank : rank_node_map.cend();
   return std::accumulate( rank_node_map.cbegin(), cend, 0ll,
                           []( unsigned int const& a, std::pair<unsigned int, unsigned int> const& b ){ return a + b.second; } );
}