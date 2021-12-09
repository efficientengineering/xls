/**
 * Copyright 2020 The XLS Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Library for generating IR graphs with the Cytoscape library.
goog.module('xls.selectableGraph');

const irGraph = goog.require('xls.irGraph');

/**
 * Possible selection states of nodes and edges.
 * @enum {number}
 */
const SelectState = {
  NONE: 0,
  SELECTED: 1,
  FRONTIER: 2,
};

/**
 * Describes the state change of a single graph element.
 */
class SelectionChange {
  /**
   * @param {string} id
   * @param {!SelectState} from
   * @param {!SelectState} to
   */
  constructor(id, from, to) {
    /** @const {string} */
    this.id = id;

    /** @const {!SelectState} */
    this.from = from;

    /** @const {!SelectState} */
    this.to = to;
  }
}

class SelectionChangeSet {
  constructor() {
    /** @const {!Array<!SelectionChange>} */
    this.nodes = [];

    /** @const {!Array<!SelectionChange>} */
    this.edges = [];
  }
}

/**
 * Class encapsulating an IR graph which keeps track of selected nodes (and
 * edges) and those nodes and edges on the frontier (adjacent to the nodes or
 * edges.
 */
class SelectableGraph {
  /**
   * @param {!irGraph.IrGraph} irGraph
   */
  constructor(irGraph) {
    /**
     * @private @const {!irGraph.IrGraph}
     */
    this.irGraph_ = irGraph;

    /**
     * The ids set of currently selected nodes.
     * @private {!Set<string>}
     */
    this.selectedNodes_ = new Set();
  }

  /**
   * @return {!irGraph.IrGraph} An IR graph in the form generated by
   * xls::IrToIr.
   */
  irGraph() {
    return this.irGraph_;
  }

  /**
   * @param {string} nodeId The id of the node.
   * @return {!SelectState} Returns selected state of the specified node.
   */
  getNodeState(nodeId) {
    if (this.selectedNodes_.has(nodeId)) {
      return SelectState.SELECTED;
    } else if (this.irGraph_.neighborsOf(nodeId).some(
                   (n) => this.selectedNodes_.has(n))) {
      return SelectState.FRONTIER;
    } else {
      return SelectState.NONE;
    }
  }

  /**
   * @param {string} edgeId The id of the edge.
   * @return {!SelectState} Returns selected state of the specified edge.
   */
  getEdgeState(edgeId) {
    let sourceId = this.irGraph_.edge(edgeId).sourceId;
    let targetId = this.irGraph_.edge(edgeId).targetId;
    if (this.isNodeSelected(sourceId) && this.isNodeSelected(targetId)) {
      return SelectState.SELECTED;
    } else if (
        this.isNodeSelected(sourceId) || this.isNodeSelected(targetId) ||
        this.isNodeOnFrontier(sourceId) && this.isNodeOnFrontier(targetId)) {
      return SelectState.FRONTIER;
    } else {
      return SelectState.NONE;
    }
  }

  /**
   * @param {string} nodeId The id of the node.
   * @return {boolean} Whether the specified node is selected.
   */
  isNodeSelected(nodeId) {
    return this.getNodeState(nodeId) == SelectState.SELECTED;
  }

  /**
   * @param {string} nodeId The id of the node.
   * @return {boolean} Whether the specified node is on the frontier.
   */
  isNodeOnFrontier(nodeId) {
    return this.getNodeState(nodeId) == SelectState.FRONTIER;
  }

  /**
   * @param {string} edgeId The id of the edge.
   * @return {boolean} Whether the specified edge is selected.
   */
  isEdgeSelected(edgeId) {
    return this.getEdgeState(edgeId) == SelectState.SELECTED;
  }

  /**
   * @param {string} edgeId The id of the edge.
   * @return {boolean} Whether the specified edge is on the frontier.
   */
  isEdgeOnFrontier(edgeId) {
    return this.getEdgeState(edgeId) == SelectState.FRONTIER;
  }

  /**
   * Selects or unselects the specified nodes as indicated by 'value'.
   * The selection state of other nodes is unaffected.
   * @param {string} nodeId The id of the node.
   * @param {boolean} value Whether to select or unselect the node.
   * @return {!SelectionChangeSet} An object indicating which nodes and edges
   *     changed state. See computeChanges_.
   */
  selectNode(nodeId, value) {
    return this.computeChanges_(() => {
      if (value) {
        this.selectedNodes_.add(nodeId);
      } else {
        this.selectedNodes_.delete(nodeId);
      }
    });
  }

  /**
   * Selects the given node and  unselects all other nodes.
   * @param {!Array<string>} nodeIds The ids of the nodes to select.
   * @return {!SelectionChangeSet} An object indicating which nodes and edges
   *     changed state. See computeChanges_.
   */
  selectOnlyNodes(nodeIds) {
    return this.computeChanges_(() => {
      this.selectedNodes_ = new Set(nodeIds);
    });
  }

  /**
   * Calls the given function to mutate the node selection state and returns an
   * object indicating which state transitions resulted from the invocation.
   *
   * @param {function()} selectFn The function which mutates the selected state
   *     (this.selectedNodes_).
   * @return {!SelectionChangeSet} An object indicating which nodes and edges
   *     changed state.
   * @private
   */
  computeChanges_(selectFn) {
    // Save the old state of the nodes and edges.
    // TODO(meheff): In general, we don't need to examine every node and edge in
    // the graph. The neighborhood of the nodes being (un)selected is
    // sufficient.
    let oldNodeState = new Map(
        this.irGraph_.nodes().map((n) => [n.id, this.getNodeState(n.id)]));
    let oldEdgeState = new Map(
        this.irGraph_.edges().map((e) => [e.id, this.getEdgeState(e.id)]));

    selectFn();

    let changes = new SelectionChangeSet();
    for (let n of this.irGraph_.nodes()) {
      let newState = this.getNodeState(n.id);
      if (oldNodeState.get(n.id) != newState) {
        changes.nodes.push(
            new SelectionChange(n.id, oldNodeState.get(n.id), newState));
      }
    }

    for (let e of this.irGraph_.edges()) {
      let newState = this.getEdgeState(e.id);
      if (oldEdgeState.get(e.id) != newState) {
        changes.edges.push(
            new SelectionChange(e.id, oldEdgeState.get(e.id), newState));
      }
    }

    return changes;
  }
}

goog.exportSymbol('xls.selectableGraph.SelectState', SelectState);
goog.exportSymbol('xls.selectableGraph.SelectionChange', SelectionChange);
goog.exportSymbol('xls.selectableGraph.SelectionChangeSet', SelectionChangeSet);
goog.exportSymbol('xls.selectableGraph.SelectableGraph', SelectableGraph);

exports = {
  SelectState,
  SelectionChange,
  SelectionChangeSet,
  SelectableGraph
};
