/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ft=javascript ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const {Cu, Ci} = require("chrome");
let EventEmitter = require("devtools/shared/event-emitter");

/**
 * API
 *
 *   new Selection(walker=null, node=null, track={attributes,detached});
 *   destroy()
 *   node (readonly)
 *   setNode(node, origin="unknown")
 *
 * Helpers:
 *
 *   window
 *   document
 *   isRoot()
 *   isNode()
 *   isHTMLNode()
 *
 * Check the nature of the node:
 *
 *   isElementNode()
 *   isAttributeNode()
 *   isTextNode()
 *   isCDATANode()
 *   isEntityRefNode()
 *   isEntityNode()
 *   isProcessingInstructionNode()
 *   isCommentNode()
 *   isDocumentNode()
 *   isDocumentTypeNode()
 *   isDocumentFragmentNode()
 *   isNotationNode()
 *
 * Events:
 *   "new-node" when the inner node changed
 *   "before-new-node" when the inner node is set to change
 *   "attribute-changed" when an attribute is changed (only if tracked)
 *   "detached" when the node (or one of its parents) is removed from the document (only if tracked)
 *   "reparented" when the node (or one of its parents) is moved under a different node (only if tracked)
 */

/**
 * A Selection object. Hold a reference to a node.
 * Includes some helpers, fire some helpful events.
 *
 * @param node Inner node.
 *    Can be null. Can be (un)set in the future via the "node" property;
 * @param trackAttribute Tell if events should be fired when the attributes of
 *    the node change.
 *
 */
function Selection(walker, node=null, track={attributes:true,detached:true}) {
  EventEmitter.decorate(this);

  this._onMutations = this._onMutations.bind(this);
  this.track = track;
  this.setWalker(walker);
  this.setNode(node);
}

exports.Selection = Selection;

Selection.prototype = {
  _walker: null,
  _node: null,

  _onMutations: function(mutations) {
    let attributeChange = false;
    let detached = false;
    let parentNode = null;
    for (let m of mutations) {
      if (!attributeChange && m.type == "attributes") {
        attributeChange = true;
      }
      if (m.type == "childList") {
        if (!detached && !this.isConnected()) {
          parentNode = m.target;
          detached = true;
        }
      }
    }

    if (attributeChange)
      this.emit("attribute-changed");
    if (detached) {
      this.emit("detached", parentNode ? parentNode.rawNode() : null);
      this.emit("detached-front", parentNode);
    }
  },

  destroy: function SN_destroy() {
    this.setNode(null);
    this.setWalker(null);
  },

  setWalker: function(walker) {
    if (this._walker) {
      this._walker.off("mutations", this._onMutations);
    }
    this._walker = walker;
    if (this._walker) {
      this._walker.on("mutations", this._onMutations);
    }
  },

  // Not remote-safe
  setNode: function SN_setNode(value, reason="unknown") {
    if (value) {
      value = this._walker.frontForRawNode(value);
    }
    this.setNodeFront(value, reason);
  },

  // Not remote-safe
  get node() {
    return this._node;
  },

  // Not remote-safe
  get window() {
    if (this.isNode()) {
      return this.node.ownerDocument.defaultView;
    }
    return null;
  },

  // Not remote-safe
  get document() {
    if (this.isNode()) {
      return this.node.ownerDocument;
    }
    return null;
  },

  setNodeFront: function(value, reason="unknown") {
    this.reason = reason;
    if (value !== this._nodeFront) {
      let rawValue = value ? value.rawNode() : value;
      this.emit("before-new-node", rawValue, reason);
      this.emit("before-new-node-front", value, reason);
      let previousNode = this._node;
      let previousFront = this._nodeFront;
      this._node = rawValue;
      this._nodeFront = value;
      this.emit("new-node", previousNode, this.reason);
      this.emit("new-node-front", value, this.reason);
    }
  },

  get documentFront() {
    return this._walker.document(this._nodeFront);
  },

  get nodeFront() {
    return this._nodeFront;
  },

  isRoot: function SN_isRootNode() {
    return this.isNode() &&
           this.isConnected() &&
           this._nodeFront.isDocumentElement;
  },

  isNode: function SN_isNode() {
    if (!this._nodeFront) {
      return false;
    }

    // As long as tools are still accessing node.rawNode(),
    // this needs to stay here.
    if (this._node && Cu.isDeadWrapper(this._node)) {
      return false;
    }

    return true;
  },

  isLocal: function SN_nsLocal() {
    return !!this._node;
  },

  isConnected: function SN_isConnected() {
    let node = this._nodeFront;
    if (!node || !node.actorID) {
      return false;
    }

    // As long as there are still tools going around
    // accessing node.rawNode, this needs to stay.
    let rawNode = node.rawNode();
    if (rawNode) {
      try {
        let doc = this.document;
        return (doc && doc.defaultView && doc.documentElement.contains(rawNode));
      } catch (e) {
        // "can't access dead object" error
        return false;
      }
    }

    while(node) {
      if (node === this._walker.rootNode) {
        return true;
      }
      node = node.parentNode();
    };
    return false;
  },

  isHTMLNode: function SN_isHTMLNode() {
    let xhtml_ns = "http://www.w3.org/1999/xhtml";
    return this.isNode() && this.node.namespaceURI == xhtml_ns;
  },

  // Node type

  isElementNode: function SN_isElementNode() {
    return this.isNode() && this.nodeFront.nodeType == Ci.nsIDOMNode.ELEMENT_NODE;
  },

  isAttributeNode: function SN_isAttributeNode() {
    return this.isNode() && this.nodeFront.nodeType == Ci.nsIDOMNode.ATTRIBUTE_NODE;
  },

  isTextNode: function SN_isTextNode() {
    return this.isNode() && this.nodeFront.nodeType == Ci.nsIDOMNode.TEXT_NODE;
  },

  isCDATANode: function SN_isCDATANode() {
    return this.isNode() && this.nodeFront.nodeType == Ci.nsIDOMNode.CDATA_SECTION_NODE;
  },

  isEntityRefNode: function SN_isEntityRefNode() {
    return this.isNode() && this.nodeFront.nodeType == Ci.nsIDOMNode.ENTITY_REFERENCE_NODE;
  },

  isEntityNode: function SN_isEntityNode() {
    return this.isNode() && this.nodeFront.nodeType == Ci.nsIDOMNode.ENTITY_NODE;
  },

  isProcessingInstructionNode: function SN_isProcessingInstructionNode() {
    return this.isNode() && this.nodeFront.nodeType == Ci.nsIDOMNode.PROCESSING_INSTRUCTION_NODE;
  },

  isCommentNode: function SN_isCommentNode() {
    return this.isNode() && this.nodeFront.nodeType == Ci.nsIDOMNode.PROCESSING_INSTRUCTION_NODE;
  },

  isDocumentNode: function SN_isDocumentNode() {
    return this.isNode() && this.nodeFront.nodeType == Ci.nsIDOMNode.DOCUMENT_NODE;
  },

  isDocumentTypeNode: function SN_isDocumentTypeNode() {
    return this.isNode() && this.nodeFront.nodeType == Ci.nsIDOMNode.DOCUMENT_TYPE_NODE;
  },

  isDocumentFragmentNode: function SN_isDocumentFragmentNode() {
    return this.isNode() && this.nodeFront.nodeType == Ci.nsIDOMNode.DOCUMENT_FRAGMENT_NODE;
  },

  isNotationNode: function SN_isNotationNode() {
    return this.isNode() && this.nodeFront.nodeType == Ci.nsIDOMNode.NOTATION_NODE;
  },
}
