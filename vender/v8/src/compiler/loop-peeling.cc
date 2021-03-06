// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/common-operator.h"
#include "src/compiler/graph.h"
#include "src/compiler/loop-peeling.h"
#include "src/compiler/node.h"
#include "src/compiler/node-marker.h"
#include "src/compiler/node-properties-inl.h"
#include "src/zone.h"

// Loop peeling is an optimization that copies the body of a loop, creating
// a new copy of the body called the "peeled iteration" that represents the
// first iteration. Beginning with a loop as follows:

//             E
//             |                 A
//             |                 |                     (backedges)
//             | +---------------|---------------------------------+
//             | | +-------------|-------------------------------+ |
//             | | |             | +--------+                    | |
//             | | |             | | +----+ |                    | |
//             | | |             | | |    | |                    | |
//           ( Loop )<-------- ( phiA )   | |                    | |
//              |                 |       | |                    | |
//      ((======P=================U=======|=|=====))             | |
//      ((                                | |     ))             | |
//      ((        X <---------------------+ |     ))             | |
//      ((                                  |     ))             | |
//      ((     body                         |     ))             | |
//      ((                                  |     ))             | |
//      ((        Y <-----------------------+     ))             | |
//      ((                                        ))             | |
//      ((===K====L====M==========================))             | |
//           |    |    |                                         | |
//           |    |    +-----------------------------------------+ |
//           |    +------------------------------------------------+
//           |
//          exit

// The body of the loop is duplicated so that all nodes considered "inside"
// the loop (e.g. {P, U, X, Y, K, L, M}) have a corresponding copies in the
// peeled iteration (e.g. {P', U', X', Y', K', L', M'}). What were considered
// backedges of the loop correspond to edges from the peeled iteration to
// the main loop body, with multiple backedges requiring a merge.

// Similarly, any exits from the loop body need to be merged with "exits"
// from the peeled iteration, resulting in the graph as follows:

//             E
//             |                 A
//             |                 |
//      ((=====P'================U'===============))
//      ((                                        ))
//      ((        X'<-------------+               ))
//      ((                        |               ))
//      ((   peeled iteration     |               ))
//      ((                        |               ))
//      ((        Y'<-----------+ |               ))
//      ((                      | |               ))
//      ((===K'===L'====M'======|=|===============))
//           |    |     |       | |
//  +--------+    +-+ +-+       | |
//  |               | |         | |
//  |              Merge <------phi
//  |                |           |
//  |          +-----+           |
//  |          |                 |                     (backedges)
//  |          | +---------------|---------------------------------+
//  |          | | +-------------|-------------------------------+ |
//  |          | | |             | +--------+                    | |
//  |          | | |             | | +----+ |                    | |
//  |          | | |             | | |    | |                    | |
//  |        ( Loop )<-------- ( phiA )   | |                    | |
//  |           |                 |       | |                    | |
//  |   ((======P=================U=======|=|=====))             | |
//  |   ((                                | |     ))             | |
//  |   ((        X <---------------------+ |     ))             | |
//  |   ((                                  |     ))             | |
//  |   ((     body                         |     ))             | |
//  |   ((                                  |     ))             | |
//  |   ((        Y <-----------------------+     ))             | |
//  |   ((                                        ))             | |
//  |   ((===K====L====M==========================))             | |
//  |        |    |    |                                         | |
//  |        |    |    +-----------------------------------------+ |
//  |        |    +------------------------------------------------+
//  |        |
//  |        |
//  +----+ +-+
//       | |
//      Merge
//        |
//      exit

// Note that the boxes ((===)) above are not explicitly represented in the
// graph, but are instead computed by the {LoopFinder}.

namespace v8 {
namespace internal {
namespace compiler {

struct Peeling {
  // Maps a node to its index in the {pairs} vector.
  NodeMarker<size_t> node_map;
  // The vector which contains the mapped nodes.
  NodeVector* pairs;

  Peeling(Graph* graph, Zone* tmp_zone, size_t max, NodeVector* p)
      : node_map(graph, static_cast<uint32_t>(max)), pairs(p) {}

  Node* map(Node* node) {
    if (node_map.Get(node) == 0) return node;
    return pairs->at(node_map.Get(node));
  }

  void Insert(Node* original, Node* copy) {
    node_map.Set(original, 1 + pairs->size());
    pairs->push_back(original);
    pairs->push_back(copy);
  }

  void CopyNodes(Graph* graph, Zone* tmp_zone, Node* dead, NodeRange nodes) {
    NodeVector inputs(tmp_zone);
    // Copy all the nodes first.
    for (Node* node : nodes) {
      inputs.clear();
      for (Node* input : node->inputs()) inputs.push_back(map(input));
      Insert(node, graph->NewNode(node->op(), node->InputCount(), &inputs[0]));
    }

    // Fix remaining inputs of the copies.
    for (Node* original : nodes) {
      Node* copy = pairs->at(node_map.Get(original));
      for (int i = 0; i < copy->InputCount(); i++) {
        copy->ReplaceInput(i, map(original->InputAt(i)));
      }
    }
  }

  bool Marked(Node* node) { return node_map.Get(node) > 0; }
};


class PeeledIterationImpl : public PeeledIteration {
 public:
  NodeVector node_pairs_;
  explicit PeeledIterationImpl(Zone* zone) : node_pairs_(zone) {}
};


Node* PeeledIteration::map(Node* node) {
  // TODO(turbofan): we use a simple linear search, since the peeled iteration
  // is really only used in testing.
  PeeledIterationImpl* impl = static_cast<PeeledIterationImpl*>(this);
  for (size_t i = 0; i < impl->node_pairs_.size(); i += 2) {
    if (impl->node_pairs_[i] == node) return impl->node_pairs_[i + 1];
  }
  return node;
}


PeeledIteration* LoopPeeler::Peel(Graph* graph, CommonOperatorBuilder* common,
                                  LoopTree* loop_tree, LoopTree::Loop* loop,
                                  Zone* tmp_zone) {
  PeeledIterationImpl* iter = new (tmp_zone) PeeledIterationImpl(tmp_zone);
  Peeling peeling(graph, tmp_zone, loop->TotalSize() * 2 + 2,
                  &iter->node_pairs_);

  //============================================================================
  // Construct the peeled iteration.
  //============================================================================
  Node* dead = graph->NewNode(common->Dead());

  // Map the loop header nodes to their entry values.
  for (Node* node : loop_tree->HeaderNodes(loop)) {
    // TODO(titzer): assuming loop entry at index 0.
    peeling.Insert(node, node->InputAt(0));
  }

  // Copy all the nodes of loop body for the peeled iteration.
  peeling.CopyNodes(graph, tmp_zone, dead, loop_tree->BodyNodes(loop));

  //============================================================================
  // Replace the entry to the loop with the output of the peeled iteration.
  //============================================================================
  Node* loop_node = loop_tree->GetLoopControl(loop);
  Node* new_entry;
  int backedges = loop_node->InputCount() - 1;
  if (backedges > 1) {
    // Multiple backedges from original loop, therefore multiple output edges
    // from the peeled iteration.
    NodeVector inputs(tmp_zone);
    for (int i = 1; i < loop_node->InputCount(); i++) {
      inputs.push_back(peeling.map(loop_node->InputAt(i)));
    }
    Node* merge =
        graph->NewNode(common->Merge(backedges), backedges, &inputs[0]);

    // Merge values from the multiple output edges of the peeled iteration.
    for (Node* node : loop_tree->HeaderNodes(loop)) {
      if (node->opcode() == IrOpcode::kLoop) continue;  // already done.
      inputs.clear();
      for (int i = 0; i < backedges; i++) {
        inputs.push_back(peeling.map(node->InputAt(1 + i)));
      }
      for (Node* input : inputs) {
        if (input != inputs[0]) {  // Non-redundant phi.
          inputs.push_back(merge);
          const Operator* op = common->ResizeMergeOrPhi(node->op(), backedges);
          Node* phi = graph->NewNode(op, backedges + 1, &inputs[0]);
          node->ReplaceInput(0, phi);
          break;
        }
      }
    }
    new_entry = merge;
  } else {
    // Only one backedge, simply replace the input to loop with output of
    // peeling.
    for (Node* node : loop_tree->HeaderNodes(loop)) {
      node->ReplaceInput(0, peeling.map(node->InputAt(0)));
    }
    new_entry = peeling.map(loop_node->InputAt(1));
  }
  loop_node->ReplaceInput(0, new_entry);

  //============================================================================
  // Find the loop exit region.
  //============================================================================
  NodeVector exits(tmp_zone);
  Node* end = NULL;
  for (Node* node : loop_tree->LoopNodes(loop)) {
    for (Node* use : node->uses()) {
      if (!loop_tree->Contains(loop, use)) {
        if (node->opcode() == IrOpcode::kBranch &&
            (use->opcode() == IrOpcode::kIfTrue ||
             use->opcode() == IrOpcode::kIfFalse)) {
          // This is a branch from inside the loop to outside the loop.
          exits.push_back(use);
        }
      }
    }
  }

  if (exits.size() == 0) return iter;  // no exits => NTL

  if (exits.size() == 1) {
    // Only one exit, so {end} is that exit.
    end = exits[0];
  } else {
    // {end} should be the common merge from the exits.
    NodeVector rets(tmp_zone);
    for (Node* exit : exits) {
      Node* found = NULL;
      for (Node* use : exit->uses()) {
        if (use->opcode() == IrOpcode::kMerge) {
          found = use;
          if (end == NULL) {
            end = found;
          } else {
            CHECK_EQ(end, found);  // it should be unique!
          }
        } else if (use->opcode() == IrOpcode::kReturn) {
          found = use;
          rets.push_back(found);
        }
      }
      // There should be a merge or a return for each exit.
      CHECK_NE(NULL, found);
    }
    // Return nodes, the end merge, and the phis associated with the end merge
    // must be duplicated as well.
    for (Node* node : rets) exits.push_back(node);
    if (end != NULL) {
      exits.push_back(end);
      for (Node* use : end->uses()) {
        if (IrOpcode::IsPhiOpcode(use->opcode())) exits.push_back(use);
      }
    }
  }

  //============================================================================
  // Duplicate the loop exit region and add a merge.
  //============================================================================
  NodeRange exit_range(&exits[0], &exits[0] + exits.size());
  peeling.CopyNodes(graph, tmp_zone, dead, exit_range);

  Node* merge = graph->NewNode(common->Merge(2), end, peeling.map(end));
  end->ReplaceUses(merge);
  merge->ReplaceInput(0, end);  // HULK SMASH!!

  // Find and update all the edges into either the loop or exit region.
  for (int i = 0; i < 2; i++) {
    NodeRange range = i == 0 ? loop_tree->LoopNodes(loop) : exit_range;
    ZoneVector<Edge> value_edges(tmp_zone);
    ZoneVector<Edge> effect_edges(tmp_zone);

    for (Node* node : range) {
      // Gather value and effect edges from outside the region.
      for (Edge edge : node->use_edges()) {
        if (!peeling.Marked(edge.from())) {
          // Edge from outside the loop into the region.
          if (NodeProperties::IsValueEdge(edge) ||
              NodeProperties::IsContextEdge(edge)) {
            value_edges.push_back(edge);
          } else if (NodeProperties::IsEffectEdge(edge)) {
            effect_edges.push_back(edge);
          } else {
            // don't do anything for control edges.
            // TODO(titzer): should update control edges to peeled?
          }
        }
      }

      // Update all the value and effect edges at once.
      if (!value_edges.empty()) {
        // TODO(titzer): machine type is wrong here.
        Node* phi = graph->NewNode(common->Phi(kMachAnyTagged, 2), node,
                                   peeling.map(node), merge);
        for (Edge edge : value_edges) edge.UpdateTo(phi);
        value_edges.clear();
      }
      if (!effect_edges.empty()) {
        Node* effect_phi = graph->NewNode(common->EffectPhi(2), node,
                                          peeling.map(node), merge);
        for (Edge edge : effect_edges) edge.UpdateTo(effect_phi);
        effect_edges.clear();
      }
    }
  }

  return iter;
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
