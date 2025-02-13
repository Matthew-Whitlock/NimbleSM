/*
//@HEADER
// ************************************************************************
//
//                                NimbleSM
//                             Copyright 2018
//   National Technology & Engineering Solutions of Sandia, LLC (NTESS)
//
// Under the terms of Contract DE-NA0003525 with NTESS, the U.S. Government
// retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY NTESS "AS IS" AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN
// NO EVENT SHALL NTESS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
// TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions?  Contact David Littlewood (djlittl@sandia.gov)
//
// ************************************************************************
//@HEADER
*/

#include "nimble_contact_manager.h"

#include "nimble_contact_interface.h"
#include "nimble_data_manager.h"
#include "nimble_defs.h"
#include "nimble_exodus_output.h"
#include "nimble_genesis_mesh.h"
#include "nimble_macros.h"
#include "nimble_parser.h"
#include "nimble_utils.h"
#include "nimble_vector_communicator.h"

#ifdef NIMBLE_HAVE_MPI
#include "mpi.h"
#endif

#ifdef NIMBLE_HAVE_KOKKOS
#include "nimble_kokkos_model_data.h"
#endif

#ifdef NIMBLE_HAVE_ARBORX
#include "ArborX_Config.hpp"
//----
#if defined(ARBORX_ENABLE_MPI) && !defined(NIMBLE_HAVE_MPI)
#error "This configuration (ArborX MPI, NimbleSM Serial) is not supported."
#endif
//----
#if !defined(ARBORX_ENABLE_MPI) && defined(NIMBLE_HAVE_MPI)
#error "This configuration (ArborX Serial, NimbleSM MPI) is not supported."
#endif
//----
#if defined(ARBORX_ENABLE_MPI) && defined(NIMBLE_HAVE_MPI)
#include "contact/parallel/arborx_parallel_contact_manager.h"
#else
#include "contact/serial/arborx_serial_contact_manager.h"
#endif
//---
#endif

#ifdef NIMBLE_HAVE_BVH
#include <bvh/kdop.hpp>
#include <bvh/patch.hpp>
#include <bvh/perf/instrument.hpp>
#include <bvh/tree.hpp>

#include "contact/parallel/bvh_contact_manager.h"
//---
#ifdef BVH_ENABLE_VT
#include <bvh/vt/collection.hpp>
#include <bvh/vt/collision_world.hpp>
#include <bvh/vt/helpers.hpp>
#endif
//---
#endif

#include <algorithm>
#include <iostream>
#include <set>
#include <type_traits>
#include <utility>

namespace nimble {

void
ParseContactCommand(
    std::string const&        command,
    std::vector<std::string>& primary_block_names,
    std::vector<std::string>& secondary_block_names,
    double&                   penalty_parameter)
{
  std::string contact_primary_key;
  std::string contact_secondary_key;

  std::stringstream ss(command);

  ss >> contact_primary_key;
  if ((contact_primary_key != "primary_blocks") && (contact_primary_key != "master_blocks")) {
    std::stringstream error_ss;
    error_ss << "\n**** Error processing contact command, unknown key: " << contact_primary_key << std::endl;
    throw std::invalid_argument(error_ss.str());
  }

  bool secondary_key_found = false;
  while (ss.good() && !secondary_key_found) {
    std::string temp;
    ss >> temp;
    if ((temp == "secondary_blocks") || (temp == "slave_blocks")) {
      secondary_key_found = true;
    } else {
      primary_block_names.push_back(temp);
    }
  }

  if (!secondary_key_found) {
    throw std::invalid_argument(
        "\n**** Error processing contact command, expected "
        "\"secondary_blocks\" or \"slave_blocks\" (deprectated).\n");
  }

  bool penalty_parameter_key_found = false;
  while (ss.good() && !penalty_parameter_key_found) {
    std::string temp;
    ss >> temp;
    if (temp == "penalty_parameter") {
      penalty_parameter_key_found = true;
    } else {
      secondary_block_names.push_back(temp);
    }
  }

  if (!penalty_parameter_key_found) {
    throw std::invalid_argument(
        "\n**** Error processing contact command, expected "
        "\"penalty_parameter\".\n");
  }

  ss >> penalty_parameter;
}

std::shared_ptr<nimble::ContactManager>
GetContactManager(std::shared_ptr<ContactInterface> interface, nimble::DataManager& data_manager)
{
  if (!data_manager.GetParser().HasContact()) return nullptr;

#if defined(NIMBLE_HAVE_ARBORX)
  if (data_manager.GetParser().UseKokkos()) {
#if defined(ARBORX_ENABLE_MPI) && defined(NIMBLE_HAVE_MPI)
    return std::make_shared<nimble::ArborXParallelContactManager>(interface, data_manager);
#else
    return std::make_shared<nimble::ArborXSerialContactManager>(interface, data_manager);
#endif
  }
#endif

#ifdef NIMBLE_HAVE_BVH
  if (data_manager.GetParser().UseVT()) {
    return std::make_shared<nimble::BvhContactManager>(
        interface, data_manager, data_manager.GetParser().ContactDicing());
  }
#endif

  return std::make_shared<nimble::ContactManager>(interface, data_manager);
}

//
// Interface functions
//

ContactManager::ContactManager(std::shared_ptr<ContactInterface> interface, nimble::DataManager& data_manager)
    : data_manager_(data_manager), penalty_parameter_(0.0), contact_interface(std::move(interface))
{
}

void
ContactManager::CreateContactEntities(
    GenesisMesh const&          mesh,
    nimble::VectorCommunicator& myVecComm,
    std::vector<int> const&     primary_block_ids,
    std::vector<int> const&     secondary_block_ids)
{
  int mpi_rank  = 0;
  int num_ranks = 1;
#ifdef NIMBLE_HAVE_MPI
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
  MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);
#endif

  contact_enabled_ = true;

  const double* coord_x = mesh.GetCoordinatesX();
  const double* coord_y = mesh.GetCoordinatesY();
  const double* coord_z = mesh.GetCoordinatesZ();

  int max_node_global_id = mesh.GetMaxNodeGlobalId();
#ifdef NIMBLE_HAVE_MPI
  int global_max_node_global_id = max_node_global_id;
  MPI_Allreduce(&max_node_global_id, &global_max_node_global_id, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
  max_node_global_id = global_max_node_global_id;
#endif

  // find all the element faces on the primary and secondary contact blocks
  // the entity ids created here will be used downstream for the contact faces
  // the contact nodes will not use these entity ids, instead they will just use
  // the exodus id of the node as the entity id
  std::vector<std::vector<int>> primary_skin_faces, secondary_skin_faces;
  std::vector<int>              primary_skin_entity_ids, secondary_skin_entity_ids;
  int contact_entity_id_offset = max_node_global_id;  // ensure that there are no duplicate entity ids
  // between nodes and faces
  SkinBlocks(mesh, primary_block_ids, contact_entity_id_offset, primary_skin_faces, primary_skin_entity_ids);
  SkinBlocks(mesh, secondary_block_ids, contact_entity_id_offset, secondary_skin_faces, secondary_skin_entity_ids);

  // remove faces that are along partition boundaries
  RemoveInternalSkinFaces(mesh, primary_skin_faces, primary_skin_entity_ids);
  RemoveInternalSkinFaces(mesh, secondary_skin_faces, secondary_skin_entity_ids);

  // create a list of ghosted nodes (e.g., nodes that are owned by a different
  // processor)
  std::vector<int> partition_boundary_node_local_ids;
  std::vector<int> min_rank_containing_partition_boundary_nodes;
  myVecComm.GetPartitionBoundaryNodeLocalIds(
      partition_boundary_node_local_ids, min_rank_containing_partition_boundary_nodes);

  std::vector<int> ghosted_node_local_ids;
  for (int i = 0; i < partition_boundary_node_local_ids.size(); i++) {
    if (min_rank_containing_partition_boundary_nodes[i] != mpi_rank) {
      ghosted_node_local_ids.push_back(partition_boundary_node_local_ids[i]);
    }
  }

  // construct containers for the subset of the model that is involved with
  // contact this constitutes a submodel that is stored in the ContactManager
  std::set<int> node_ids_set;
  for (auto const& face : primary_skin_faces) {
    for (auto const& id : face) { node_ids_set.insert(id); }
  }
  for (auto const& face : secondary_skin_faces) {
    for (auto const& id : face) { node_ids_set.insert(id); }
  }
  node_ids_ = std::vector<int>(node_ids_set.begin(), node_ids_set.end());

  std::map<int, int> genesis_mesh_node_id_to_contact_submodel_id;
  for (unsigned int i_node = 0; i_node < node_ids_.size(); ++i_node) {
    genesis_mesh_node_id_to_contact_submodel_id[node_ids_[i_node]] = i_node;
  }

  // replace the node ids that correspond to the genesis mesh
  // with node ids that correspond to the contact submodel
  for (auto& primary_skin_face : primary_skin_faces) {
    for (int& i_node : primary_skin_face) {
      int genesis_mesh_node_id = i_node;
      i_node                   = genesis_mesh_node_id_to_contact_submodel_id.at(genesis_mesh_node_id);
    }
  }
  for (auto& secondary_skin_face : secondary_skin_faces) {
    for (int& i_node : secondary_skin_face) {
      int genesis_mesh_node_id = i_node;
      i_node                   = genesis_mesh_node_id_to_contact_submodel_id.at(genesis_mesh_node_id);
    }
  }

  // create a list of ghosted nodes in the contact submodel
  std::vector<int> ghosted_contact_node_ids;
  for (auto node_id : ghosted_node_local_ids) {
    auto it = genesis_mesh_node_id_to_contact_submodel_id.find(node_id);
    if (it != genesis_mesh_node_id_to_contact_submodel_id.end()) { ghosted_contact_node_ids.push_back(it->second); }
  }

  // allocate data for the contact submodel
  int array_len = 3 * node_ids_.size();
  model_coord_.resize(array_len);
  coord_.resize(array_len);
  force_.resize(array_len, 0.0);
  for (unsigned int i_node = 0; i_node < node_ids_.size(); i_node++) {
    model_coord_[3 * i_node] = coord_[3 * i_node] = coord_x[node_ids_[i_node]];
    model_coord_[3 * i_node + 1] = coord_[3 * i_node + 1] = coord_y[node_ids_[i_node]];
    model_coord_[3 * i_node + 2] = coord_[3 * i_node + 2] = coord_z[node_ids_[i_node]];
  }

  // Store nodes in secondary faces
  // Create a list of nodes and their characteristic lengths
  const int*            genesis_node_global_ids = mesh.GetNodeGlobalIds();
  std::vector<int>      secondary_node_ids;
  std::vector<int>      secondary_node_entity_ids;
  std::map<int, double> secondary_node_char_lens;
  for (auto& face : secondary_skin_faces) {
    int num_nodes_in_face = static_cast<int>(face.size());
    // determine a characteristic length based on max edge length
    double max_edge_length_square = std::numeric_limits<double>::lowest();
    for (int i = 0; i < num_nodes_in_face; ++i) {
      int node_id_1 = face[i];
      int node_id_2 = face[0];
      if (i + 1 < num_nodes_in_face) { node_id_2 = face[i + 1]; }
      double edge_length_square =
          (coord_[3 * node_id_2] - coord_[3 * node_id_1]) * (coord_[3 * node_id_2] - coord_[3 * node_id_1]) +
          (coord_[3 * node_id_2 + 1] - coord_[3 * node_id_1 + 1]) *
              (coord_[3 * node_id_2 + 1] - coord_[3 * node_id_1 + 1]) +
          (coord_[3 * node_id_2 + 2] - coord_[3 * node_id_1 + 2]) *
              (coord_[3 * node_id_2 + 2] - coord_[3 * node_id_1 + 2]);
      if (edge_length_square > max_edge_length_square) { max_edge_length_square = edge_length_square; }
    }
    double characteristic_length = sqrt(max_edge_length_square);
    for (int i_node = 0; i_node < num_nodes_in_face; i_node++) {
      int node_id = face[i_node];
      // omit ghosted nodes
      if (std::find(ghosted_contact_node_ids.begin(), ghosted_contact_node_ids.end(), node_id) ==
          ghosted_contact_node_ids.end()) {
        if (std::find(secondary_node_ids.begin(), secondary_node_ids.end(), node_id) == secondary_node_ids.end()) {
          secondary_node_ids.push_back(node_id);
          // note the mapping from contact manager local id to real FEM mesh
          // local id to real FEM mesh global id
          int contact_node_entity_id = genesis_node_global_ids[node_ids_[node_id]] + 1;
          secondary_node_entity_ids.push_back(contact_node_entity_id);
          secondary_node_char_lens[node_id] = characteristic_length;
        } else {
          // always use the maximum characteristic length
          // this requires a parallel sync
          if (secondary_node_char_lens[node_id] < characteristic_length) {
            secondary_node_char_lens[node_id] = characteristic_length;
          }
        }
      }
    }
  }

  contact_nodes_.resize(secondary_node_ids.size());
  contact_faces_.resize(4 * primary_skin_faces.size());
  CreateContactNodesAndFaces(
      primary_skin_faces,
      primary_skin_entity_ids,
      secondary_node_ids,
      secondary_node_entity_ids,
      secondary_node_char_lens,
      contact_nodes_,
      contact_faces_);

#ifdef NIMBLE_HAVE_KOKKOS
  if (data_manager_.GetParser().UseKokkos()) {
    nimble_kokkos::HostIntegerArrayView node_ids_h("contact_node_ids_h", node_ids_.size());
    for (unsigned int i_node = 0; i_node < node_ids_.size(); i_node++) { node_ids_h[i_node] = node_ids_[i_node]; }

    nimble_kokkos::HostScalarNodeView model_coord_h("contact_model_coord_h", array_len);
    for (unsigned int i_node = 0; i_node < node_ids_.size(); i_node++) {
      model_coord_h[3 * i_node]     = coord_x[node_ids_[i_node]];
      model_coord_h[3 * i_node + 1] = coord_y[node_ids_[i_node]];
      model_coord_h[3 * i_node + 2] = coord_z[node_ids_[i_node]];
    }

    Kokkos::resize(node_ids_d_, node_ids_.size());
    Kokkos::resize(model_coord_d_, array_len);
    Kokkos::resize(coord_d_, array_len);
    Kokkos::resize(force_d_, array_len);

    Kokkos::deep_copy(node_ids_d_, node_ids_h);
    Kokkos::deep_copy(model_coord_d_, model_coord_h);
    Kokkos::deep_copy(coord_d_, model_coord_h);
    Kokkos::deep_copy(force_d_, 0.0);
  }

  //
  // Create Kokkos::View objects for contact nodes and faces
  //

  Kokkos::resize(contact_nodes_h_, secondary_node_ids.size());
  Kokkos::resize(contact_faces_h_, 4 * primary_skin_faces.size());
  CreateContactNodesAndFaces(
      primary_skin_faces,
      primary_skin_entity_ids,
      secondary_node_ids,
      secondary_node_entity_ids,
      secondary_node_char_lens,
      contact_nodes_h_,
      contact_faces_h_);

  Kokkos::resize(contact_nodes_d_, secondary_node_ids.size());
  Kokkos::resize(contact_faces_d_, 4 * primary_skin_faces.size());
  Kokkos::deep_copy(contact_nodes_d_, contact_nodes_h_);
  Kokkos::deep_copy(contact_faces_d_, contact_faces_h_);

#endif

  int num_contact_faces = contact_faces_.size();
  int num_contact_nodes = contact_nodes_.size();
#ifdef NIMBLE_HAVE_MPI
  std::vector<int> input(2);
  std::vector<int> output(2);
  input[0] = num_contact_faces;
  input[1] = num_contact_nodes;
  MPI_Reduce(input.data(), output.data(), input.size(), MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
  num_contact_faces = output[0];
  num_contact_nodes = output[1];
#endif
  if (mpi_rank == 0) {
    std::cout << "Contact initialization:" << std::endl;
    std::cout << "  number of triangular contact facets (primary blocks): " << num_contact_faces << std::endl;
    std::cout << "  number of contact nodes (secondary blocks): " << num_contact_nodes << "\n" << std::endl;
  }
}

void
ContactManager::ComputeContactForce(int step, bool debug_output, nimble::Viewify<2> contact_force)
{
  if (penalty_parameter_ <= 0.0) {
    throw std::invalid_argument("\nError in ComputeContactForce(), invalid penalty_parameter.\n");
  }

  const auto& parser = data_manager_.GetParser();
  if (parser.UseKokkos()) {
#ifdef NIMBLE_HAVE_KOKKOS
    std::cout << " Enter if section .. ComputeContactForce \n";
    auto model_ptr       = data_manager_.GetModelData();
    auto model_data      = dynamic_cast<nimble_kokkos::ModelData*>(model_ptr.get());
    auto field_ids       = data_manager_.GetFieldIDs();
    auto displacement_d  = model_data->GetDeviceVectorNodeData(field_ids.displacement);
    auto contact_force_d = model_data->GetDeviceVectorNodeData(field_ids.contact_force);
    Kokkos::deep_copy(contact_force_d, (double)(0.0));
    //
    ApplyDisplacements(displacement_d);
    //
    ComputeContactForce(step, debug_output);
    //
    GetForces(contact_force_d);
    //
    auto contact_force_h = model_data->GetHostVectorNodeData(field_ids.contact_force);
    Kokkos::deep_copy(contact_force_h, contact_force_d);
#ifdef NIMBLE_HAVE_MPI
    auto          myVectorCommunicator = data_manager_.GetVectorCommunicator();
    constexpr int vector_dim           = 3;
    myVectorCommunicator->VectorReduction(vector_dim, contact_force_h);
#endif
    return;
#endif
  }

  auto model_data   = data_manager_.GetModelData();
  auto displacement = model_data->GetVectorNodeData("displacement");
  ApplyDisplacements(displacement.data());

  ComputeContactForce(step, debug_output);

  GetForces(contact_force.data());
#ifdef NIMBLE_HAVE_MPI
  auto          myVectorCommunicator = data_manager_.GetVectorCommunicator();
  constexpr int vector_dim           = 3;
  myVectorCommunicator->VectorReduction(vector_dim, contact_force.data());
#endif
}

/*!
 * Initialize Exodus file, and prepare it to contain Contact Visualization data
 *
 * @param contact_visualization_exodus_file_name
 */
void
ContactManager::InitializeContactVisualization(std::string const& contact_visualization_exodus_file_name)
{
  // Exodus id convention for contact visualization:
  //
  // Both node and face contact entities have a unique, parallel-consistent id
  // called contact_entity_global_id_. For faces, the contact_entity_global_id_
  // a bit-wise combination of the global exodus id of the parent element, plus
  // the face ordinal (1-6), plus the triangle ordinal (1-4). For nodes, the
  // contact_entity_global_id_ is the exodus global node id for the node in the
  // original FEM mesh.
  //
  // For visualization output, we need unique, parallel-consistent node ids and
  // element ids.  For the faces, the contact_entity_global_id_ is used as the
  // element id, and the node ids are constructed here.  For nodes, the
  // contact_entity_global_id_ is used for both the node id and the element id
  // (sphere element containing a single node).
  //
  // For the MPI bounding boxes, both the node ids and the element id are
  // constructed here.
  //
  //   contact faces:
  //     node ids are (3 * contact_entity_global_id_ + max_contact_entity_id +
  //     9,
  //                   3 * contact_entity_global_id_ + max_contact_entity_id +
  //                   10, 3 * contact_entity_global_id_ + max_contact_entity_id
  //                   + 11)
  //     element id is contact_entity_global_id_
  //   contact nodes
  //     node id is contact_entity_global_id_
  //     element id contact_entity_global_id_
  //   mpi partition bounding box:
  //     nodes id are (3 * max_contact_entity_id + 1,
  //                   3 * max_contact_entity_id + 2,
  //                   3 * max_contact_entity_id + 3,
  //                   3 * max_contact_entity_id + 4,
  //                   3 * max_contact_entity_id + 5,
  //                   3 * max_contact_entity_id + 6,
  //                   3 * max_contact_entity_id + 7,
  //                   3 * max_contact_entity_id + 8)
  //     element id max_contact_entity_id + 1

  // Local constants for readability
  const size_t          nnodes = numContactNodes();
  const size_t          nfaces = numContactFaces();
  nimble::GenesisMesh&  mesh   = genesis_mesh_for_contact_visualization_;
  nimble::ExodusOutput& out    = exodus_output_for_contact_visualization_;

  // determine the maximum contact entity global id over all MPI partitions
  int max_contact_entity_id = 0;
  for (int i_face = 0; i_face < nfaces; i_face++) {
    ContactEntity const& face = getContactFace(i_face);
    if (face.contact_entity_global_id_ > max_contact_entity_id) {
      max_contact_entity_id = face.contact_entity_global_id_;
    }
  }
  for (int i_node = 0; i_node < nnodes; i_node++) {
    ContactEntity const& node = getContactNode(i_node);
    if (node.contact_entity_global_id_ > max_contact_entity_id) {
      max_contact_entity_id = node.contact_entity_global_id_;
    }
  }
#ifdef NIMBLE_HAVE_MPI
  int mpi_rank, num_ranks;
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
  MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);
  int global_max_contact_entity_id = max_contact_entity_id;
  MPI_Allreduce(&max_contact_entity_id, &global_max_contact_entity_id, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
  max_contact_entity_id = global_max_contact_entity_id;
#endif

  std::vector<int>                node_global_id;
  std::vector<double>             node_x;
  std::vector<double>             node_y;
  std::vector<double>             node_z;
  std::vector<int>                elem_global_id;
  std::vector<int>                block_ids;
  std::map<int, std::string>      block_names;
  std::map<int, std::vector<int>> block_elem_global_ids;
  std::map<int, int>              block_num_nodes_per_elem;
  std::map<int, std::vector<int>> block_elem_connectivity;

  // first block contains the contact faces
  int block_id = 1;
  block_ids.push_back(block_id);
  block_names[block_id]              = "contact_faces";
  block_elem_global_ids[block_id]    = std::vector<int>();
  block_num_nodes_per_elem[block_id] = 3;
  block_elem_connectivity[block_id]  = std::vector<int>();

  int node_index(0);
  for (int i_face = 0; i_face < nfaces; i_face++) {
    ContactEntity const& face                     = getContactFace(i_face);
    int                  contact_entity_global_id = face.contact_entity_global_id_;
    node_global_id.push_back(3 * contact_entity_global_id + max_contact_entity_id + 9);
    node_x.push_back(face.coord_1_x_);
    node_y.push_back(face.coord_1_y_);
    node_z.push_back(face.coord_1_z_);
    block_elem_connectivity[block_id].push_back(node_index++);
    node_global_id.push_back(3 * contact_entity_global_id + max_contact_entity_id + 10);
    node_x.push_back(face.coord_2_x_);
    node_y.push_back(face.coord_2_y_);
    node_z.push_back(face.coord_2_z_);
    block_elem_connectivity[block_id].push_back(node_index++);
    node_global_id.push_back(3 * contact_entity_global_id + max_contact_entity_id + 11);
    node_x.push_back(face.coord_3_x_);
    node_y.push_back(face.coord_3_y_);
    node_z.push_back(face.coord_3_z_);
    block_elem_connectivity[block_id].push_back(node_index++);
    elem_global_id.push_back(contact_entity_global_id);
  }

  // second block contains the contact nodes
  block_id = 2;
  block_ids.push_back(block_id);
  block_names[block_id]              = "contact_nodes";
  block_elem_global_ids[block_id]    = std::vector<int>();
  block_num_nodes_per_elem[block_id] = 1;
  block_elem_connectivity[block_id]  = std::vector<int>();

  for (int i_node = 0; i_node < nnodes; i_node++) {
    ContactEntity const& node                     = getContactNode(i_node);
    int                  contact_entity_global_id = node.contact_entity_global_id_;
    node_global_id.push_back(contact_entity_global_id);
    node_x.push_back(node.coord_1_x_);
    node_y.push_back(node.coord_1_y_);
    node_z.push_back(node.coord_1_z_);
    block_elem_connectivity[block_id].push_back(node_index++);
    elem_global_id.push_back(contact_entity_global_id);
  }

  // third block is the bounding box for this mpi rank
  mesh.Initialize(
      "contact_visualization",
      node_global_id,
      node_x,
      node_y,
      node_z,
      elem_global_id,
      block_ids,
      block_names,
      block_elem_global_ids,
      block_num_nodes_per_elem,
      block_elem_connectivity);

  out.Initialize(contact_visualization_exodus_file_name, mesh);

  std::vector<std::string> global_data_labels;
  global_data_labels.emplace_back("num_contacts");
  std::vector<std::string> node_data_labels_for_output;
  node_data_labels_for_output.emplace_back("displacement_x");
  node_data_labels_for_output.emplace_back("displacement_y");
  node_data_labels_for_output.emplace_back("displacement_z");
  node_data_labels_for_output.emplace_back("contact_status");
  std::map<int, std::vector<std::string>> elem_data_labels_for_output;
  std::map<int, std::vector<std::string>> derived_elem_data_labels;
  for (auto iblock : block_ids) {
    elem_data_labels_for_output[iblock] = std::vector<std::string>();
    derived_elem_data_labels[iblock]    = std::vector<std::string>();
  }
  out.InitializeDatabase(
      mesh, global_data_labels, node_data_labels_for_output, elem_data_labels_for_output, derived_elem_data_labels);
}

void
ContactManager::ContactVisualizationWriteStep(double time_current)
{
#ifdef NIMBLE_HAVE_KOKKOS
  if (data_manager_.GetParser().UseKokkos()) {
    // copy contact entities from host to device
    Kokkos::deep_copy(contact_nodes_h_, contact_nodes_d_);
    Kokkos::deep_copy(contact_faces_h_, contact_faces_d_);
  }
#endif
  WriteVisualizationData(time_current);
}

void
ContactManager::WriteVisualizationData(double t)
{
  nimble::GenesisMesh&  mesh = genesis_mesh_for_contact_visualization_;
  nimble::ExodusOutput& out  = exodus_output_for_contact_visualization_;

  std::vector<double>                             global_data;
  std::vector<std::vector<double>>                node_data_for_output(4);
  std::map<int, std::vector<std::string>>         elem_data_labels_for_output;
  std::map<int, std::vector<std::vector<double>>> elem_data_for_output;
  std::map<int, std::vector<std::string>>         derived_elem_data_labels;
  std::map<int, std::vector<std::vector<double>>> derived_elem_data;

  // Get the number of contacts from one block
  auto num_contacts = numActiveContactFaces();
  global_data.push_back(static_cast<double>(num_contacts));

  std::vector<int> const& block_ids = mesh.GetBlockIds();
  for (auto& block_id : block_ids) {
    elem_data_labels_for_output[block_id] = std::vector<std::string>();
    derived_elem_data_labels[block_id]    = std::vector<std::string>();
  }

  // node_data_for_output contains displacement_x, displacement_y,
  // displacement_z
  auto num_nodes = mesh.GetNumNodes();
  for (auto& ndata : node_data_for_output) ndata.resize(num_nodes);
  const double* model_coord_x = mesh.GetCoordinatesX();
  const double* model_coord_y = mesh.GetCoordinatesY();
  const double* model_coord_z = mesh.GetCoordinatesZ();

  size_t nfaces = numContactFaces();

  int node_index(0);
  for (size_t i_face = 0; i_face < nfaces; i_face++) {
    ContactEntity const& face           = getContactFace(i_face);
    auto                 contact_status = static_cast<double>(face.contact_status());
    node_data_for_output[0][node_index] = face.coord_1_x_ - model_coord_x[node_index];
    node_data_for_output[1][node_index] = face.coord_1_y_ - model_coord_y[node_index];
    node_data_for_output[2][node_index] = face.coord_1_z_ - model_coord_z[node_index];
    node_data_for_output[3][node_index] = contact_status;
    node_index += 1;
    node_data_for_output[0][node_index] = face.coord_2_x_ - model_coord_x[node_index];
    node_data_for_output[1][node_index] = face.coord_2_y_ - model_coord_y[node_index];
    node_data_for_output[2][node_index] = face.coord_2_z_ - model_coord_z[node_index];
    node_data_for_output[3][node_index] = contact_status;
    node_index += 1;
    node_data_for_output[0][node_index] = face.coord_3_x_ - model_coord_x[node_index];
    node_data_for_output[1][node_index] = face.coord_3_y_ - model_coord_y[node_index];
    node_data_for_output[2][node_index] = face.coord_3_z_ - model_coord_z[node_index];
    node_data_for_output[3][node_index] = contact_status;
    node_index += 1;
  }

  const size_t nnodes = numContactNodes();
  for (size_t i_node = 0; i_node < nnodes; i_node++) {
    ContactEntity const& node           = getContactNode(i_node);
    node_data_for_output[0][node_index] = node.coord_1_x_ - model_coord_x[node_index];
    node_data_for_output[1][node_index] = node.coord_1_y_ - model_coord_y[node_index];
    node_data_for_output[2][node_index] = node.coord_1_z_ - model_coord_z[node_index];
    node_data_for_output[3][node_index] = static_cast<double>(node.contact_status());
    node_index += 1;
  }

  out.WriteStep(
      t,
      global_data,
      node_data_for_output,
      elem_data_labels_for_output,
      elem_data_for_output,
      derived_elem_data_labels,
      derived_elem_data);
}

//
// Protected functions
//

void
ContactManager::ApplyDisplacements(const double* displacement)
{
  for (unsigned int i_node = 0; i_node < node_ids_.size(); i_node++) {
    int node_id = node_ids_[i_node];
    for (int i = 0; i < 3; i++) {
      coord_[3 * i_node + i] = model_coord_[3 * i_node + i] + displacement[3 * node_id + i];
    }
  }
  for (auto& contact_face : contact_faces_) { contact_face.SetCoordinates(coord_.data()); }
  for (auto& contact_node : contact_nodes_) { contact_node.SetCoordinates(coord_.data()); }
}

void
ContactManager::GetForces(double* contact_force) const
{
  for (unsigned int i_node = 0; i_node < node_ids_.size(); i_node++) {
    int node_id = node_ids_[i_node];
    for (int i = 0; i < 3; i++) { contact_force[3 * node_id + i] = force_[3 * i_node + i]; }
  }
}

#ifdef NIMBLE_HAVE_KOKKOS
// Kokkos Versions of GetForces and ApplyDisplacements
void
ContactManager::GetForces(nimble_kokkos::DeviceVectorNodeView contact_force_d) const
{
  int num_nodes_in_contact_manager = node_ids_d_.extent(0);

  // circumvent lambda *this glitch
  nimble_kokkos::DeviceIntegerArrayView node_ids = node_ids_d_;
  nimble_kokkos::DeviceScalarNodeView   force    = force_d_;

  Kokkos::parallel_for(
      "ContactManager::GetForces", num_nodes_in_contact_manager, KOKKOS_LAMBDA(const int i) {
        int node_id                 = node_ids(i);
        contact_force_d(node_id, 0) = force(3 * i);
        contact_force_d(node_id, 1) = force(3 * i + 1);
        contact_force_d(node_id, 2) = force(3 * i + 2);
      });
}

void
ContactManager::ApplyDisplacements(nimble_kokkos::DeviceVectorNodeView displacement_d)
{
  int num_nodes_in_contact_manager = node_ids_d_.extent(0);
  int num_contact_node_entities    = contact_nodes_d_.extent(0);
  int num_contact_face_entities    = contact_faces_d_.extent(0);

  // circumvent lambda *this glitch
  nimble_kokkos::DeviceIntegerArrayView       node_ids      = node_ids_d_;
  nimble_kokkos::DeviceScalarNodeView         model_coord   = model_coord_d_;
  nimble_kokkos::DeviceScalarNodeView         coord         = coord_d_;
  nimble_kokkos::DeviceContactEntityArrayView contact_nodes = contact_nodes_d_;
  nimble_kokkos::DeviceContactEntityArrayView contact_faces = contact_faces_d_;

  Kokkos::parallel_for(
      "ContactManager::ApplyDisplacements set coord_d_ vector",
      num_nodes_in_contact_manager,
      KOKKOS_LAMBDA(const int i) {
        int node_id      = node_ids(i);
        coord(3 * i)     = model_coord(3 * i) + displacement_d(node_id, 0);
        coord(3 * i + 1) = model_coord(3 * i + 1) + displacement_d(node_id, 1);
        coord(3 * i + 2) = model_coord(3 * i + 2) + displacement_d(node_id, 2);
      });

  Kokkos::parallel_for(
      "ContactManager::ApplyDisplacements set contact node entity "
      "displacements",
      num_contact_node_entities,
      KOKKOS_LAMBDA(const int i_node) { contact_nodes(i_node).SetCoordinates(coord); });

  Kokkos::parallel_for(
      "ContactManager::ApplyDisplacements set contact face entity "
      "displacements",
      num_contact_face_entities,
      KOKKOS_LAMBDA(const int i_face) { contact_faces(i_face).SetCoordinates(coord); });
}

#endif  // NIMBLE_HAVE_KOKKOS

// static
void
ContactManager::SkinBlocks(
    GenesisMesh const&             mesh,
    std::vector<int> const&        block_ids,
    int                            entity_id_offset,
    std::vector<std::vector<int>>& skin_faces,
    std::vector<int>&              entity_ids)
{
  std::map<std::vector<int>, std::vector<int>>           faces;
  std::map<std::vector<int>, std::vector<int>>::iterator face_it;

  for (auto& block_id : block_ids) {
    int                     num_elem_in_block = mesh.GetNumElementsInBlock(block_id);
    int                     num_node_per_elem = mesh.GetNumNodesPerElement(block_id);
    const int* const        conn              = mesh.GetConnectivity(block_id);
    std::vector<int> const& elem_global_ids   = mesh.GetElementGlobalIdsInBlock(block_id);
    int                     conn_index        = 0;

    // key is sorted node list for a face
    int              num_node_per_face = 4;
    std::vector<int> key(num_node_per_face);
    // value is count, unsorted node list, exodus element id, and face ordinal
    std::vector<int> value(num_node_per_face + 3);

    for (int i_elem = 0; i_elem < num_elem_in_block; i_elem++) {
      // switch from 0-based indexing to 1-based indexing
      // so that the ids will be valid exodus ids in the contact visualization
      // output
      int elem_global_id = elem_global_ids[i_elem] + 1;

      // Examine each face, following the Exodus node-ordering convention

      // face 0: 0 1 5 4
      value[0] = 1;
      key[0] = value[1] = conn[conn_index + 0];
      key[1] = value[2] = conn[conn_index + 1];
      key[2] = value[3] = conn[conn_index + 5];
      key[3] = value[4] = conn[conn_index + 4];
      value[5]          = elem_global_id;
      value[6]          = 0;
      std::sort(key.begin(), key.end());
      face_it = faces.find(key);
      if (face_it == faces.end())
        faces[key] = value;
      else
        face_it->second[0] += 1;

      // face 1: 1 2 6 5
      value[0] = 1;
      key[0] = value[1] = conn[conn_index + 1];
      key[1] = value[2] = conn[conn_index + 2];
      key[2] = value[3] = conn[conn_index + 6];
      key[3] = value[4] = conn[conn_index + 5];
      value[5]          = elem_global_id;
      value[6]          = 1;
      std::sort(key.begin(), key.end());
      face_it = faces.find(key);
      if (face_it == faces.end())
        faces[key] = value;
      else
        face_it->second[0] += 1;

      // face 2: 2 3 7 6
      value[0] = 1;
      key[0] = value[1] = conn[conn_index + 2];
      key[1] = value[2] = conn[conn_index + 3];
      key[2] = value[3] = conn[conn_index + 7];
      key[3] = value[4] = conn[conn_index + 6];
      value[5]          = elem_global_id;
      value[6]          = 2;
      std::sort(key.begin(), key.end());
      face_it = faces.find(key);
      if (face_it == faces.end())
        faces[key] = value;
      else
        face_it->second[0] += 1;

      // face 3: 0 4 7 3
      value[0] = 1;
      key[0] = value[1] = conn[conn_index + 0];
      key[1] = value[2] = conn[conn_index + 4];
      key[2] = value[3] = conn[conn_index + 7];
      key[3] = value[4] = conn[conn_index + 3];
      value[5]          = elem_global_id;
      value[6]          = 3;
      std::sort(key.begin(), key.end());
      face_it = faces.find(key);
      if (face_it == faces.end())
        faces[key] = value;
      else
        face_it->second[0] += 1;

      // face 4: 0 3 2 1
      value[0] = 1;
      key[0] = value[1] = conn[conn_index + 0];
      key[1] = value[2] = conn[conn_index + 3];
      key[2] = value[3] = conn[conn_index + 2];
      key[3] = value[4] = conn[conn_index + 1];
      value[5]          = elem_global_id;
      value[6]          = 4;
      std::sort(key.begin(), key.end());
      face_it = faces.find(key);
      if (face_it == faces.end())
        faces[key] = value;
      else
        face_it->second[0] += 1;

      // face 5: 4 5 6 7
      value[0] = 1;
      key[0] = value[1] = conn[conn_index + 4];
      key[1] = value[2] = conn[conn_index + 5];
      key[2] = value[3] = conn[conn_index + 6];
      key[3] = value[4] = conn[conn_index + 7];
      value[5]          = elem_global_id;
      value[6]          = 5;
      std::sort(key.begin(), key.end());
      face_it = faces.find(key);
      if (face_it == faces.end())
        faces[key] = value;
      else
        face_it->second[0] += 1;

      conn_index += num_node_per_elem;
    }
  }

  skin_faces.clear();
  entity_ids.clear();
  for (auto face : faces) {
    if (face.second[0] == 1) {
      std::vector<int> skin_face;
      for (int i = 0; i < face.second.size() - 3; i++) {
        int id = face.second.at(i + 1);
        skin_face.push_back(id);
      }
      skin_faces.push_back(skin_face);
      int entity_id = (face.second[5] + entity_id_offset)
                      << 5;              // 59 bits for the genesis element id plus an offset value
      entity_id |= face.second[6] << 2;  // 3 bits for the face ordinal
      entity_id |= 0;                    // 2 bits for triangle ordinal (unknown until face is
                                         // subdivided downstream)
      entity_ids.push_back(entity_id);
    } else if (face.second[0] != 2) {
      NIMBLE_ABORT("Error in mesh skinning routine, face found more than two times!\n");
    }
  }
}

// static
void
ContactManager::RemoveInternalSkinFaces(
    GenesisMesh const&             mesh,
    std::vector<std::vector<int>>& faces,
    std::vector<int>&              entity_ids)
{
#ifdef NIMBLE_HAVE_MPI

  int mpi_rank, num_ranks;
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
  MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

  constexpr int num_nodes_in_face = 4;

  const int*       genesis_node_global_ids = mesh.GetNodeGlobalIds();
  std::vector<int> face_global_ids;
  face_global_ids.reserve(num_nodes_in_face * faces.size());
  std::map<std::vector<int>, int> faceList;
  for (int iface = 0; iface < faces.size(); ++iface) {
    const auto&      face = faces[iface];
    std::vector<int> fvec(num_nodes_in_face, std::numeric_limits<int>::max());
    for (int ii = 0; ii < face.size(); ++ii) fvec[ii] = genesis_node_global_ids[face[ii]];
    std::sort(fvec.begin(), fvec.end());
    for (auto inode : fvec) face_global_ids.push_back(inode);
    faceList.emplace(std::make_pair(std::move(fvec), iface));
  }

  std::vector<bool> remove_face_hash(faces.size(), false);
  std::vector<bool> remove_entity_ids_hash(entity_ids.size(), false);
  size_t            iCountRemovals = 0;
  const auto        bcast_size     = static_cast<int>(face_global_ids.size());
  for (int shift = 1; shift < num_ranks; ++shift) {
    int target = (mpi_rank + shift) % num_ranks;
    int source = (mpi_rank + num_ranks - shift) % num_ranks;
    //
    int recv_size = 0;
    MPI_Sendrecv(
        &bcast_size,
        1,
        MPI_INT,
        target,
        shift,
        &recv_size,
        1,
        MPI_INT,
        source,
        MPI_ANY_TAG,
        MPI_COMM_WORLD,
        MPI_STATUS_IGNORE);
    //
    std::vector<int> mpi_buffer(recv_size);
    MPI_Sendrecv(
        face_global_ids.data(),
        face_global_ids.size(),
        MPI_INT,
        target,
        shift,
        mpi_buffer.data(),
        mpi_buffer.size(),
        MPI_INT,
        source,
        MPI_ANY_TAG,
        MPI_COMM_WORLD,
        MPI_STATUS_IGNORE);
    //
    int mpi_buffer_num_faces = recv_size / num_nodes_in_face;
    //
    for (int i_mpi_buff_face = 0; i_mpi_buff_face < mpi_buffer_num_faces; i_mpi_buff_face++) {
      const auto* mpi_buff_ptr = &mpi_buffer.at(i_mpi_buff_face * num_nodes_in_face);
      //
      std::vector<int> tmpList(mpi_buff_ptr, mpi_buff_ptr + num_nodes_in_face);
      auto             tmpIter = faceList.find(tmpList);
      if (tmpIter != faceList.end()) {
        remove_face_hash[tmpIter->second]       = true;
        remove_entity_ids_hash[tmpIter->second] = true;
        iCountRemovals += 1;
      }
    }
  }

  //
  // Update the vector of faces and entity IDs
  //

  if (iCountRemovals == 0) return;

  std::vector<std::vector<int>> new_faces;
  new_faces.reserve(faces.size());
  for (std::size_t i = 0; i < faces.size(); ++i) {
    if (!remove_face_hash[i]) new_faces.emplace_back(std::move(faces[i]));
  }

  std::vector<int> new_entity_ids;
  new_entity_ids.reserve(entity_ids.size());
  for (std::size_t i = 0; i < entity_ids.size(); ++i) {
    if (!remove_entity_ids_hash[i]) new_entity_ids.emplace_back(entity_ids[i]);
  }
  std::cout << " Rank " << mpi_rank << " Removed " << remove_face_hash.size() - new_faces.size() << " faces\n";
  std::cout << " Rank " << mpi_rank << " Removed " << remove_entity_ids_hash.size() - new_entity_ids.size()
            << " faces\n";
  std::swap(new_faces, faces);
  std::swap(new_entity_ids, entity_ids);

#endif
}

template <typename ArgT>
void
ContactManager::CreateContactNodesAndFaces(
    std::vector<std::vector<int>> const& primary_skin_faces,
    std::vector<int> const&              primary_skin_entity_ids,
    std::vector<int> const&              secondary_node_ids,
    std::vector<int> const&              secondary_node_entity_ids,
    std::map<int, double> const&         secondary_node_char_lens,
    ArgT&                                contact_nodes,
    ArgT&                                contact_faces) const
{
  int index = 0;

  // convert primary faces to triangular facets
  for (unsigned int i_face = 0; i_face < primary_skin_faces.size(); i_face++) {
    auto face = primary_skin_faces[i_face];

    int num_nodes_in_face = static_cast<int>(face.size());
    if (num_nodes_in_face != 4) {
      NIMBLE_ABORT(
          "\nError in ContactManager::CreateContactNodesAndFaces(), invalid "
          "number of face nodes.\n");
    }

    // determine a characteristic length based on max edge length
    double max_edge_length = std::numeric_limits<double>::lowest();
    for (int i = 0; i < num_nodes_in_face; ++i) {
      int node_id_1 = face[i];
      int node_id_2 = face[0];
      if (i + 1 < num_nodes_in_face) { node_id_2 = face[i + 1]; }
      double edge_length = sqrt(
          (coord_[3 * node_id_2] - coord_[3 * node_id_1]) * (coord_[3 * node_id_2] - coord_[3 * node_id_1]) +
          (coord_[3 * node_id_2 + 1] - coord_[3 * node_id_1 + 1]) *
              (coord_[3 * node_id_2 + 1] - coord_[3 * node_id_1 + 1]) +
          (coord_[3 * node_id_2 + 2] - coord_[3 * node_id_1 + 2]) *
              (coord_[3 * node_id_2 + 2] - coord_[3 * node_id_1 + 2]));
      if (edge_length > max_edge_length) { max_edge_length = edge_length; }
    }
    double characteristic_length = max_edge_length;

    // create a node at the barycenter of the face
    double fictitious_node[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < num_nodes_in_face; ++i) {
      int node_id = face[i];
      for (int j = 0; j < 3; j++) { fictitious_node[j] += coord_[3 * node_id + j]; }
    }
    for (double& j : fictitious_node) { j /= num_nodes_in_face; }

    // Create a map for transfering displacements and contact forces from the
    // nodes on the triangle patch to the contact manager data structures. There
    // is a 1-to-1 transfer for the two real nodes, and for the fictitious node
    // the mapping applies an equal fraction of the displacement/force at the
    // fictitious node to each for four real nodes in the original mesh face
    int node_ids_for_fictitious_node[4];
    for (int i = 0; i < 4; i++) { node_ids_for_fictitious_node[i] = face[i]; }

    double model_coord[9];
    int    node_id_1, node_id_2, entity_id;

    // triangle node_0, node_1, fictitious_node
    node_id_1 = face[0];
    node_id_2 = face[1];
    for (int i = 0; i < 3; ++i) {
      model_coord[i]     = coord_[3 * node_id_1 + i];
      model_coord[3 + i] = coord_[3 * node_id_2 + i];
    }
    model_coord[6] = fictitious_node[0];
    model_coord[7] = fictitious_node[1];
    model_coord[8] = fictitious_node[2];
    entity_id      = primary_skin_entity_ids[i_face];
    entity_id |= 0;  // triangle ordinal
    contact_faces[index] = ContactEntity(
        ContactEntity::TRIANGLE,
        entity_id,
        index,
        model_coord,
        characteristic_length,
        node_id_1,
        node_id_2,
        node_ids_for_fictitious_node);
    ++index;

    // triangle node_1, node_2, fictitious_node
    node_id_1 = face[1];
    node_id_2 = face[2];
    for (int i = 0; i < 3; ++i) {
      model_coord[i]     = coord_[3 * node_id_1 + i];
      model_coord[3 + i] = coord_[3 * node_id_2 + i];
    }
    model_coord[6] = fictitious_node[0];
    model_coord[7] = fictitious_node[1];
    model_coord[8] = fictitious_node[2];
    entity_id      = primary_skin_entity_ids[i_face];
    entity_id |= 1;  // triangle ordinal
    contact_faces[index] = ContactEntity(
        ContactEntity::TRIANGLE,
        entity_id,
        index,
        model_coord,
        characteristic_length,
        node_id_1,
        node_id_2,
        node_ids_for_fictitious_node);
    ++index;

    // triangle node_2, node_3, fictitious_node
    node_id_1 = face[2];
    node_id_2 = face[3];
    for (int i = 0; i < 3; ++i) {
      model_coord[i]     = coord_[3 * node_id_1 + i];
      model_coord[3 + i] = coord_[3 * node_id_2 + i];
    }
    model_coord[6] = fictitious_node[0];
    model_coord[7] = fictitious_node[1];
    model_coord[8] = fictitious_node[2];
    entity_id      = primary_skin_entity_ids[i_face];
    entity_id |= 2;  // triangle ordinal
    contact_faces[index] = ContactEntity(
        ContactEntity::TRIANGLE,
        entity_id,
        index,
        model_coord,
        characteristic_length,
        node_id_1,
        node_id_2,
        node_ids_for_fictitious_node);
    ++index;

    // triangle node_3, node_0, fictitious_node
    node_id_1 = face[3];
    node_id_2 = face[0];
    for (int i = 0; i < 3; ++i) {
      model_coord[i]     = coord_[3 * node_id_1 + i];
      model_coord[3 + i] = coord_[3 * node_id_2 + i];
    }
    model_coord[6] = fictitious_node[0];
    model_coord[7] = fictitious_node[1];
    model_coord[8] = fictitious_node[2];
    entity_id      = primary_skin_entity_ids[i_face];
    entity_id |= 3;  // triangle ordinal
    contact_faces[index] = ContactEntity(
        ContactEntity::TRIANGLE,
        entity_id,
        index,
        model_coord,
        characteristic_length,
        node_id_1,
        node_id_2,
        node_ids_for_fictitious_node);
    ++index;
  }

  // Secondary node entities
  for (unsigned int i_node = 0; i_node < secondary_node_ids.size(); ++i_node) {
    int    node_id               = secondary_node_ids.at(i_node);
    int    entity_id             = secondary_node_entity_ids.at(i_node);
    double characteristic_length = secondary_node_char_lens.at(node_id);
    double model_coord[3];
    for (int i = 0; i < 3; ++i) { model_coord[i] = coord_[3 * node_id + i]; }
    contact_nodes[i_node] =
        ContactEntity(ContactEntity::NODE, entity_id, i_node, model_coord, characteristic_length, node_id);
  }
}

void
ContactManager::BoundingBox(double& x_min, double& x_max, double& y_min, double& y_max, double& z_min, double& z_max)
    const
{
  double big = std::numeric_limits<double>::max();

  if (data_manager_.GetParser().UseKokkos()) {
#ifdef NIMBLE_HAVE_KOKKOS
    nimble_kokkos::DeviceScalarNodeView contact_bounding_box_d("contact_bounding_box_d", 6);
    nimble_kokkos::HostScalarNodeView   contact_bounding_box_h("contact_bounding_box_h", 6);
    contact_bounding_box_h(0) = big;         // x_min
    contact_bounding_box_h(1) = -1.0 * big;  // x_max
    contact_bounding_box_h(2) = big;         // y_min
    contact_bounding_box_h(3) = -1.0 * big;  // y_max
    contact_bounding_box_h(4) = big;         // z_min
    contact_bounding_box_h(5) = -1.0 * big;  // z_max
    Kokkos::deep_copy(contact_bounding_box_d, contact_bounding_box_h);

    nimble_kokkos::DeviceScalarNodeView coord_d             = coord_d_;
    auto                                contact_vector_size = static_cast<int>(coord_d.extent(0) / 3);

    Kokkos::parallel_for(
        "Contact Bounding Box", contact_vector_size, KOKKOS_LAMBDA(const int i) {
          double x = coord_d(3 * i);
          double y = coord_d(3 * i + 1);
          double z = coord_d(3 * i + 2);
          Kokkos::atomic_min_fetch(&contact_bounding_box_d(0), x);
          Kokkos::atomic_max_fetch(&contact_bounding_box_d(1), x);
          Kokkos::atomic_min_fetch(&contact_bounding_box_d(2), y);
          Kokkos::atomic_max_fetch(&contact_bounding_box_d(3), y);
          Kokkos::atomic_min_fetch(&contact_bounding_box_d(4), z);
          Kokkos::atomic_max_fetch(&contact_bounding_box_d(5), z);
        });

    Kokkos::deep_copy(contact_bounding_box_h, contact_bounding_box_d);
    x_min = contact_bounding_box_h(0);
    x_max = contact_bounding_box_h(1);
    y_min = contact_bounding_box_h(2);
    y_max = contact_bounding_box_h(3);
    z_min = contact_bounding_box_h(4);
    z_max = contact_bounding_box_h(5);

    return;
#endif
  }

  x_min = big;
  x_max = -1.0 * big;
  y_min = big;
  y_max = -1.0 * big;
  z_min = big;
  z_max = -1.0 * big;
  for (unsigned int i = 0; i < coord_.size() / 3; i++) {
    double x = coord_[i * 3];
    double y = coord_[i * 3 + 1];
    double z = coord_[i * 3 + 2];
    if (x < x_min) x_min = x;
    if (x > x_max) x_max = x;
    if (y < y_min) y_min = y;
    if (y > y_max) y_max = y;
    if (z < z_min) z_min = z;
    if (z > z_max) z_max = z;
  }
}

double
ContactManager::BoundingBoxAverageCharacteristicLengthOverAllRanks() const
{
  double x_min, x_max, y_min, y_max, z_min, z_max;
  BoundingBox(x_min, x_max, y_min, y_max, z_min, z_max);
  double longest_edge = x_max - x_min;
  if ((y_max - y_min) > longest_edge) { longest_edge = y_max - y_min; }
  if ((z_max - z_min) > longest_edge) { longest_edge = z_max - z_min; }
  double ave_characteristic_length = longest_edge;
#ifdef NIMBLE_HAVE_MPI
  int num_ranks;
  MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);
  MPI_Allreduce(&longest_edge, &ave_characteristic_length, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  ave_characteristic_length /= num_ranks;
#endif
  return ave_characteristic_length;
}

void
ContactManager::BruteForceBoxIntersectionSearch(
    std::vector<ContactEntity> const& nodes,
    std::vector<ContactEntity> const& triangles)
{
  for (int i_node = 0; i_node < nodes.size(); i_node++) {
    for (int i_tri = 0; i_tri < triangles.size(); i_tri++) {
      // ContactEntity.get_x_min()
    }
  }
}

void
ContactManager::ClosestPointProjection(
    const ContactEntity*   nodes,
    const ContactEntity*   triangles,
    ContactEntity::vertex* closest_points,
    PROJECTION_TYPE*       projection_types,
    std::size_t            num_elements)
{
  // Wolfgang Heidrich, 2005, Computing the Barycentric Coordinates of a
  // Projected Point, Journal of Graphics Tools, pp 9-12, 10(3).

  double tol = 1.0e-16;

  for (unsigned int i_proj = 0; i_proj < num_elements; i_proj++) {
    ContactEntity const& node = nodes[i_proj];
    ContactEntity const& tri  = triangles[i_proj];

    ClosestPointProjectionSingle(node, tri, &closest_points[i_proj], &projection_types[i_proj], tol);
  }
}

void
ContactManager::ClosestPointProjectionSingle(
    const ContactEntity&   node,
    const ContactEntity&   tri,
    ContactEntity::vertex* closest_point,
    PROJECTION_TYPE*       projection_type,
    double                 tol)
{
  double p[3];
  p[0] = node.coord_1_x_;
  p[1] = node.coord_1_y_;
  p[2] = node.coord_1_z_;

  double p1[3];
  p1[0] = tri.coord_1_x_;
  p1[1] = tri.coord_1_y_;
  p1[2] = tri.coord_1_z_;

  double p2[3];
  p2[0] = tri.coord_2_x_;
  p2[1] = tri.coord_2_y_;
  p2[2] = tri.coord_2_z_;

  double p3[3];
  p3[0] = tri.coord_3_x_;
  p3[1] = tri.coord_3_y_;
  p3[2] = tri.coord_3_z_;

  double u[3], v[3], w[3];
  for (int i = 0; i < 3; i++) {
    u[i] = p2[i] - p1[i];
    v[i] = p3[i] - p1[i];
    w[i] = p[i] - p1[i];
  }

  double n[3];
  CrossProduct(u, v, n);

  double n_squared = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];

  double cross[3];
  CrossProduct(u, w, cross);
  double gamma = (cross[0] * n[0] + cross[1] * n[1] + cross[2] * n[2]) / n_squared;

  CrossProduct(w, v, cross);
  double beta = (cross[0] * n[0] + cross[1] * n[1] + cross[2] * n[2]) / n_squared;

  double alpha = 1 - gamma - beta;

  bool alpha_is_zero, alpha_in_range;
  (alpha > -tol && alpha < tol) ? alpha_is_zero = true : alpha_is_zero = false;
  (alpha > -tol && alpha < 1.0 + tol) ? alpha_in_range = true : alpha_in_range = false;

  bool beta_is_zero, beta_in_range;
  (beta > -tol && beta < tol) ? beta_is_zero = true : beta_is_zero = false;
  (beta > -tol && beta < 1.0 + tol) ? beta_in_range = true : beta_in_range = false;

  bool gamma_is_zero, gamma_in_range;
  (gamma > -tol && gamma < tol) ? gamma_is_zero = true : gamma_is_zero = false;
  (gamma > -tol && gamma < 1.0 + tol) ? gamma_in_range = true : gamma_in_range = false;

  if (alpha_in_range && beta_in_range && gamma_in_range) {
    closest_point->coords_[0] = alpha * p1[0] + beta * p2[0] + gamma * p3[0];
    closest_point->coords_[1] = alpha * p1[1] + beta * p2[1] + gamma * p3[1];
    closest_point->coords_[2] = alpha * p1[2] + beta * p2[2] + gamma * p3[2];
    if (alpha_is_zero || beta_is_zero || gamma_is_zero) {
      *projection_type = PROJECTION_TYPE::NODE_OR_EDGE;
    } else {
      *projection_type = PROJECTION_TYPE::FACE;
    }
  } else {
    *projection_type = PROJECTION_TYPE::NODE_OR_EDGE;

    double x                = p1[0] - p[0];
    double y                = p1[1] - p[1];
    double z                = p1[2] - p[2];
    double distance_squared = x * x + y * y + z * z;

    double min_distance_squared = distance_squared;
    double min_distance_squared_t;
    int    min_case = 1;

    x                = p2[0] - p[0];
    y                = p2[1] - p[1];
    z                = p2[2] - p[2];
    distance_squared = x * x + y * y + z * z;
    if (distance_squared < min_distance_squared) {
      min_distance_squared = distance_squared;
      min_case             = 2;
    }

    x                = p3[0] - p[0];
    y                = p3[1] - p[1];
    z                = p3[2] - p[2];
    distance_squared = x * x + y * y + z * z;
    if (distance_squared < min_distance_squared) {
      min_distance_squared = distance_squared;
      min_case             = 3;
    }

    double t = PointEdgeClosestPointFindT(p1, p2, p);
    if (t > 0.0 && t < 1.0) {
      distance_squared = PointEdgeClosestPointFindDistanceSquared(p1, p2, p, t);
      if (distance_squared < min_distance_squared) {
        min_distance_squared   = distance_squared;
        min_distance_squared_t = t;
        min_case               = 4;
      }
    }

    t = PointEdgeClosestPointFindT(p2, p3, p);
    if (t > 0.0 && t < 1.0) {
      distance_squared = PointEdgeClosestPointFindDistanceSquared(p2, p3, p, t);
      if (distance_squared < min_distance_squared) {
        min_distance_squared   = distance_squared;
        min_distance_squared_t = t;
        min_case               = 5;
      }
    }

    t = PointEdgeClosestPointFindT(p3, p1, p);
    if (t > 0.0 && t < 1.0) {
      distance_squared = PointEdgeClosestPointFindDistanceSquared(p3, p1, p, t);
      if (distance_squared < min_distance_squared) {
        min_distance_squared   = distance_squared;
        min_distance_squared_t = t;
        min_case               = 6;
      }
    }

    switch (min_case) {
      default:
      case 1:
        closest_point->coords_[0] = p1[0];
        closest_point->coords_[1] = p1[1];
        closest_point->coords_[2] = p1[2];
        break;
      case 2:
        closest_point->coords_[0] = p2[0];
        closest_point->coords_[1] = p2[1];
        closest_point->coords_[2] = p2[2];
        break;
      case 3:
        closest_point->coords_[0] = p3[0];
        closest_point->coords_[1] = p3[1];
        closest_point->coords_[2] = p3[2];
        break;
      case 4:
        closest_point->coords_[0] = p1[0] + (p2[0] - p1[0]) * min_distance_squared_t;
        closest_point->coords_[1] = p1[1] + (p2[1] - p1[1]) * min_distance_squared_t;
        closest_point->coords_[2] = p1[2] + (p2[2] - p1[2]) * min_distance_squared_t;
        break;
      case 5:
        closest_point->coords_[0] = p2[0] + (p3[0] - p2[0]) * min_distance_squared_t;
        closest_point->coords_[1] = p2[1] + (p3[1] - p2[1]) * min_distance_squared_t;
        closest_point->coords_[2] = p2[2] + (p3[2] - p2[2]) * min_distance_squared_t;
        break;
      case 6:
        closest_point->coords_[0] = p3[0] + (p1[0] - p3[0]) * min_distance_squared_t;
        closest_point->coords_[1] = p3[1] + (p1[1] - p3[1]) * min_distance_squared_t;
        closest_point->coords_[2] = p3[2] + (p1[2] - p3[2]) * min_distance_squared_t;
        break;
    }
  }
}

// static
void
ContactManager::SimpleClosestPointProjectionSingle(
    const ContactEntity&   node,
    const ContactEntity&   tri,
    PROJECTION_TYPE*       projection_type,
    ContactEntity::vertex* closest_point,
    double&                gap,
    double*                normal,
    double                 tol)
{
  // node
  double p[3];
  p[0] = node.coord_1_x_;
  p[1] = node.coord_1_y_;
  p[2] = node.coord_1_z_;
  // facet
  double p1[3];
  p1[0] = tri.coord_1_x_;
  p1[1] = tri.coord_1_y_;
  p1[2] = tri.coord_1_z_;
  double p2[3];
  p2[0] = tri.coord_2_x_;
  p2[1] = tri.coord_2_y_;
  p2[2] = tri.coord_2_z_;
  double p3[3];
  p3[0] = tri.coord_3_x_;
  p3[1] = tri.coord_3_y_;
  p3[2] = tri.coord_3_z_;
  // u: edge, v: edge, w: vertex to edge
  double u[3], v[3], w[3];
  for (int i = 0; i < 3; i++) {
    u[i] = p2[i] - p1[i];
    v[i] = p3[i] - p1[i];
    w[i] = p[i] - p1[i];
  }
  // n: outward non-unit normal
  double n[3];
  CrossProduct(u, v, n);
  double n_squared = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];  // 4 A
  // baricentric coordinates on facet  [ u, w + a n, n ] = [ u, w, n]
  double cross[3];
  CrossProduct(u, w, cross);
  double alpha3 = (cross[0] * n[0] + cross[1] * n[1] + cross[2] * n[2]) / n_squared;
  CrossProduct(w, v, cross);
  double alpha2 = (cross[0] * n[0] + cross[1] * n[1] + cross[2] * n[2]) / n_squared;
  double alpha1 = 1.0 - alpha2 - alpha3;
  // check inside
  double tol2      = 1.0 + tol;
  bool   a1        = (alpha1 > -tol && alpha1 < tol2);
  bool   a2        = (alpha2 > -tol && alpha2 < tol2);
  bool   a3        = (alpha3 > -tol && alpha3 < tol2);
  *projection_type = PROJECTION_TYPE::UNKNOWN;  // indicates outside
  if (a1 && a2 && a3) {
    *projection_type          = PROJECTION_TYPE::FACE;
    double xp                 = alpha1 * p1[0] + alpha2 * p2[0] + alpha3 * p3[0];
    double yp                 = alpha1 * p1[1] + alpha2 * p2[1] + alpha3 * p3[1];
    double zp                 = alpha1 * p1[2] + alpha2 * p2[2] + alpha3 * p3[2];
    closest_point->coords_[0] = xp;
    closest_point->coords_[1] = yp;
    closest_point->coords_[2] = zp;
    double dx                 = node.coord_1_x_ - xp;
    double dy                 = node.coord_1_y_ - yp;
    double dz                 = node.coord_1_z_ - zp;
    double s                  = 1.0 / std::sqrt(n_squared);
    normal[0]                 = n[0] * s;
    normal[1]                 = n[1] * s;
    normal[2]                 = n[2] * s;
    gap                       = dx * normal[0] + dy * normal[1] + dz * normal[2];
  }
}

/// \brief Projection of node on the plane defined by the triangular face
///
/// \param[in] node Node to project
/// \param[in] tri Triangular face defining the plane
/// \param[out] in Boolean flag whether projection is inside the triangular face
/// \param[out] gap
/// \param[out] normal Unit outer normal to the triangular face
/// \param[out] barycentric_coordinates Barycentric coordinates of projection
/// \param[in] tol Tolerance
///
/// \note Only the coordinates of ContactEntity node are accessed.
///
void
ContactManager::Projection(
    const ContactEntity& node,
    const ContactEntity& tri,
    bool&                in,
    double&              gap,
    double*              normal,
    double*              barycentric_coordinates,
    double               tol)
{
  // node
  double p[3];
  p[0] = node.coord_1_x_;
  p[1] = node.coord_1_y_;
  p[2] = node.coord_1_z_;
  // facet
  double p1[3];
  p1[0] = tri.coord_1_x_;
  p1[1] = tri.coord_1_y_;
  p1[2] = tri.coord_1_z_;
  double p2[3];
  p2[0] = tri.coord_2_x_;
  p2[1] = tri.coord_2_y_;
  p2[2] = tri.coord_2_z_;
  double p3[3];
  p3[0] = tri.coord_3_x_;
  p3[1] = tri.coord_3_y_;
  p3[2] = tri.coord_3_z_;
  // u: edge, v: edge, w: vertex to edge
  double u[3], v[3], w[3];
  for (int i = 0; i < 3; i++) {
    u[i] = p2[i] - p1[i];
    v[i] = p3[i] - p1[i];
    w[i] = p[i] - p1[i];
  }
  // n: outward non-unit normal
  double n[3];
  CrossProduct(u, v, n);
  double n_squared = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];  // 4 A
  // baricentric coordinates on facet  [ u, w + a n, n ] = [ u, w, n]
  double cross[3];
  CrossProduct(u, w, cross);
  double alpha3 = (cross[0] * n[0] + cross[1] * n[1] + cross[2] * n[2]) / n_squared;
  CrossProduct(w, v, cross);
  double alpha2 = (cross[0] * n[0] + cross[1] * n[1] + cross[2] * n[2]) / n_squared;
  double alpha1 = 1.0 - alpha2 - alpha3;
  // check inside
  double tol2 = 1.0 + tol;
  bool   a1   = (alpha1 > -tol && alpha1 < tol2);
  bool   a2   = (alpha2 > -tol && alpha2 < tol2);
  bool   a3   = (alpha3 > -tol && alpha3 < tol2);
  in          = false;
  if (a1 && a2 && a3) {
    double xp                  = alpha1 * p1[0] + alpha2 * p2[0] + alpha3 * p3[0];
    double yp                  = alpha1 * p1[1] + alpha2 * p2[1] + alpha3 * p3[1];
    double zp                  = alpha1 * p1[2] + alpha2 * p2[2] + alpha3 * p3[2];
    double dx                  = node.coord_1_x_ - xp;
    double dy                  = node.coord_1_y_ - yp;
    double dz                  = node.coord_1_z_ - zp;
    double s                   = 1.0 / std::sqrt(n_squared);
    normal[0]                  = n[0] * s;
    normal[1]                  = n[1] * s;
    normal[2]                  = n[2] * s;
    gap                        = dx * normal[0] + dy * normal[1] + dz * normal[2];
    barycentric_coordinates[0] = alpha1;
    barycentric_coordinates[1] = alpha2;
    barycentric_coordinates[2] = alpha3;
    if ((gap < 0.0) && (gap > -tri.char_len_)) {  // inside but not through
      in = true;
    }
  }
}

void
ContactManager::ZeroContactForce()
{
  for (auto& fval : force_) fval = 0.0;

#ifdef NIMBLE_HAVE_KOKKOS
  Kokkos::deep_copy(force_d_, 0.0);
  //
  nimble_kokkos::DeviceContactEntityArrayView contact_faces = contact_faces_d_;
  auto                                        numFaces      = contact_faces_d_.extent(0);
  Kokkos::parallel_for(
      "Zero Face Force", numFaces, KOKKOS_LAMBDA(const int i_face) {
        contact_faces(i_face).force_1_x_ = 0.0;
        contact_faces(i_face).force_1_y_ = 0.0;
        contact_faces(i_face).force_1_z_ = 0.0;
        contact_faces(i_face).force_2_x_ = 0.0;
        contact_faces(i_face).force_2_y_ = 0.0;
        contact_faces(i_face).force_2_z_ = 0.0;
        contact_faces(i_face).force_3_x_ = 0.0;
        contact_faces(i_face).force_3_y_ = 0.0;
        contact_faces(i_face).force_3_z_ = 0.0;
      });
  //
  nimble_kokkos::DeviceContactEntityArrayView contact_nodes = contact_nodes_d_;
  auto                                        numNodes      = contact_nodes_d_.extent(0);
  Kokkos::parallel_for(
      "Zero Node Force", numNodes, KOKKOS_LAMBDA(const int i_node) {
        contact_nodes(i_node).force_1_x_ = 0.0;
        contact_nodes(i_node).force_1_y_ = 0.0;
        contact_nodes(i_node).force_1_z_ = 0.0;
      });
#endif
}

const std::unordered_map<std::string, double>&
ContactManager::getTimers()
{
  timers_.clear();
#ifdef NIMBLE_TIME_CONTACT
  for (auto st_pair : watch_.timers_) {
    auto name = st_pair.first;
    timers_.insert(std::make_pair(name, st_pair.second.GetElapsedTime()));
  }
#endif
  return timers_;
}

}  // namespace nimble
