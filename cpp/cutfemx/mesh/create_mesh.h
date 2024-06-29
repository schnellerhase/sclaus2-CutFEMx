// Copyright (c) 2022-2024 ONERA
// Authors: Susanne Claus
// This file is part of CutFEMx
//
// SPDX-License-Identifier:    MIT
#pragma once

#include <dolfinx/mesh/Mesh.h>
#include <dolfinx/mesh/cell_types.h>
#include <dolfinx/fem/CoordinateElement.h>
#include "convert.h"

#include <cutcells/cut_mesh.h>

namespace cutfemx::mesh
{
  // Copy of create_mesh from utils.h in dolfinx without re-ordering and partitioning
  template <std::floating_point T>
  dolfinx::mesh::Mesh<T> create_mesh(
      MPI_Comm comm, std::span<const std::int64_t> cells,
      const dolfinx::fem::CoordinateElement<T>& element,
      std::span<const T> x, std::array<std::size_t, 2> xshape)
  {
    dolfinx::mesh::CellType celltype = element.cell_shape();
    const dolfinx::fem::ElementDofLayout doflayout = element.create_dof_layout();

    const int num_cell_vertices = dolfinx::mesh::num_cell_vertices(element.cell_shape());
    const int num_cell_nodes = doflayout.num_dofs();

    // Note: `extract_topology` extracts topology data, i.e. just the
    // vertices. For P1 geometry this should just be the identity
    // operator. For other elements the filtered lists may have 'gaps',
    // i.e. the indices might not be contiguous.
    //
    // `extract_topology` could be skipped for 'P1 geometry' elements

    // -- Partition topology across ranks of comm
    dolfinx::graph::AdjacencyList<std::int64_t> cells1(0);
    // std::vector<std::int64_t> cells1;
    std::vector<std::int64_t> original_idx1;
    std::vector<int> ghost_owners;
    cells1 = graph::regular_adjacency_list(
        std::vector(cells.begin(), cells.end()), num_cell_nodes);
    std::int64_t offset(0), num_owned(cells1.num_nodes());
    MPI_Exscan(&num_owned, &offset, 1, MPI_INT64_T, MPI_SUM, comm);
    original_idx1.resize(cells1.num_nodes());
    std::iota(original_idx1.begin(), original_idx1.end(), offset);

    // Extract cell 'topology', i.e. extract the vertices for each cell
    // and discard any 'higher-order' nodes
    std::vector<std::int64_t> cells1_v
        = extract_topology(celltype, doflayout, cells1.array());

    // Build local dual graph for owned cells to (i) get list of vertices
    // on the process boundary and (ii) apply re-ordering to cells for
    // locality
    std::vector<std::int64_t> boundary_v;
    {
      std::int32_t num_owned_cells
          = cells1_v.size() / num_cell_vertices - ghost_owners.size();
      std::vector<std::int32_t> cell_offsets(num_owned_cells + 1, 0);
      for (std::size_t i = 1; i < cell_offsets.size(); ++i)
        cell_offsets[i] = cell_offsets[i - 1] + num_cell_vertices;
      auto [graph, unmatched_facets, max_v, facet_attached_cells]
          = build_local_dual_graph(
              celltype,
              std::span(cells1_v.data(), num_owned_cells * num_cell_vertices));
      const std::vector<int> remap = graph::reorder_gps(graph);

      // Boundary vertices are marked as 'unknown'
      boundary_v = unmatched_facets;
      std::sort(boundary_v.begin(), boundary_v.end());
      boundary_v.erase(std::unique(boundary_v.begin(), boundary_v.end()),
                      boundary_v.end());

      // Remove -1 if it occurs in boundary vertices (may occur in mixed
      // topology)
      if (!boundary_v.empty() > 0 and boundary_v[0] == -1)
        boundary_v.erase(boundary_v.begin());
    }

    // Create Topology
    dolfinx::mesh::Topology topology = dolfinx::mesh::create_topology(comm, cells1_v, original_idx1,
                                        ghost_owners, celltype, boundary_v);

    // Create connectivities required higher-order geometries for creating
    // a Geometry object
    for (int e = 1; e < topology.dim(); ++e)
      if (doflayout.num_entity_dofs(e) > 0)
        topology.create_entities(e);
    if (element.needs_dof_permutations())
      topology.create_entity_permutations();

    // Build list of unique (global) node indices from cells1 and
    // distribute coordinate data
    std::vector<std::int64_t> nodes1 = cells1.array();
    dolfinx::radix_sort(std::span(nodes1));
    nodes1.erase(std::unique(nodes1.begin(), nodes1.end()), nodes1.end());
    std::vector coords
        = dolfinx::MPI::distribute_data(comm, nodes1, comm, x, xshape[1]);

    // Create geometry object
    dolfinx::mesh::Geometry geometry = dolfinx::mesh::create_geometry(topology, element, nodes1, cells1.array(),
                                        coords, xshape[1]);

    return dolfinx::mesh::Mesh(comm, std::make_shared<dolfinx::mesh::Topology>(std::move(topology)),
                std::move(geometry));
  }

  template <std::floating_point T>
  dolfinx::mesh::Mesh<T> create_mesh2(MPI_Comm comm, const std::span<T> vertex_coordinates,
                      const std::vector<std::vector<int>>& connectivity,
                      const dolfinx::mesh::CellType& cell_type, const unsigned long &gdim)
  {
    //@todo: support hybrid meshes
    dolfinx::fem::CoordinateElement<T> element(cell_type, 1);

    std::array<std::size_t, 2> xshape = {vertex_coordinates.size() / gdim, gdim};
    //flatten connectvity vector
    std::size_t num_entries = 0;
    for(std::size_t i=0;i<connectivity.size();i++)
    {
      num_entries += connectivity[i].size();
    }
    std::vector<std::int64_t> cells(num_entries);
    int cnt = 0;
    for(std::size_t i=0;i<connectivity.size();i++)
    {
      for(std::size_t j=0;j<connectivity[i].size();j++)
      {
        cells[cnt] = connectivity[i][j];
        cnt++;
      }
    }
    return create_mesh<T>(comm, cells, element, vertex_coordinates, xshape);
  }

  //return Mesh and parent cell map as this can be used for interpolation
  template <std::floating_point T>
  std::tuple<dolfinx::mesh::Mesh<T>, std::vector<std::int32_t>> create_mesh(MPI_Comm comm, cutcells::mesh::CutCells& cut_cells)
  {
    //create cut mesh from cut cells
    //i.e. merging of cut cells
    cutcells::mesh::CutMesh cut_mesh = cutcells::mesh::create_cut_mesh(cut_cells._cut_cells);

    return {create_mesh2<T>(comm, cut_mesh._vertex_coords,cut_mesh._connectivity,
                      cutcells_to_dolfinx_cell_type(cut_mesh._types[0]), cut_mesh._gdim),
                      std::move(cut_mesh._parent_cell_index)};

  }
}