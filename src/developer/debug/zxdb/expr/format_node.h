// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_EXPR_FORMAT_NODE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_EXPR_FORMAT_NODE_H_

#include <map>
#include <string>

#include "lib/fit/defer.h"
#include "lib/fit/function.h"
#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/expr/expr_value.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace zxdb {

// A node in a tree of formatted "stuff" for displaying the user. Currently
// this stuff can be expressions which are evaluated, and ExprValue classes
// which contain already-evaluated values. This tree can represent expansions
// for things like struct members.
//
// TRANSITION
// ----------
// Development of this is currently in-progress.
//
// To print a value in the old model, code in the console frontend converts
// an ExprValue to a string that's printed.
//
// With this class, the formatted "data model" is separate from the code to
// format for the console. This class represents that data model.
//
// During the transition, the console formatting code calls into this code for
// the sub-types that FormatNode supports and immediately stringifies it. This
// allows for an incremental conversion. When FormatNode supports all data
// types, the console formatter will be replaced with some code that takes a
// FormatNode and converts to a string specifically for the console context
// (possibly with wrapping and indenting, etc.).
//
// DESIGN
// ------
// Think of this class as being a tree node in a GUI debugger's "watch" window.
// The "source" is the most fundamental thing that the node represents. They
// can be expressions which are evaluated in the current context or can be
// derived automatically from a parent value (say class members).
//
// The node can be in several states. It can be empty (State::kEmpty), it can
// have an expression that hasn't been evaluated (State::kUnevaluated, say for
// a tree node where the user has typed a watch expression in), that expression
// can be evaluated to get an ExprValue (a value + type = State::kHasValue),
// and that type to get a stringified description + type (State::kDescribed). A
// node can also have an error state. A node might not go through all states,
// to format a known value, the FormatNode can be given a value directly,
// skipping the "expression" state.
//
// Frontend code can take this tree and format it however is most appropriate
// for the environment.
//
// CHILDREN
// --------
// A node can have children. The most obvious example is structure members.
// Children can also be anything else that might be expanded from a parent,
// including base classes or pointer dereferences (again, imagine a watch
// window tree view).
//
// "Describing" a node will fill in the children as well as the single-line
// description. The children might not themselves be evaluated or described
// until explicitly filled. This allows lazy expansion for things like pointer
// dereferencing where computing the fully described value might be slow or
// infintely recursive.
class FormatNode {
 public:
  using ChildVector = std::vector<std::unique_ptr<FormatNode>>;

  // Type of function to use when the value is programatically generated. The
  // callback will issue the callback with the result or error.
  //
  // The callback can be issued immediately (within the callstack of the caller
  // of the getter) or asynchronously in the future. The implementation of
  // GetProgramaticValue does not have to worry about the lifetime of the
  // FormatNode, that will be handled by the implementation of the callback
  // passed to it.
  using GetProgramaticValue = std::function<void(
      fxl::RefPtr<EvalContext> context,
      fit::callback<void(const Err& err, ExprValue value)> cb)>;

  // The original source or the value for this node.
  enum Source {
    kValue,        // Value is given, nothing to do.
    kExpression,   // Evaluate an expression in some context to get the value.
    kProgramatic,  // Evaluate a GetProgramaticValue() callback.
  };

  // The format of the description.
  enum DescriptionKind {
    kNone,
    kBaseType,    // Integers, characters, bools, etc.
    kCollection,  // Structs, classes.
    kRustEnum,    // Rust-style enum (can have values associated with enums).
    kRustTuple,   // Includes TupleStructs (check the type).
    kPointer,
    kReference,
  };

  /* TODO(brettw) in the future I'm think we'll need something like this as
     a hint to the frontend on what kind of node this is.
  enum Kind {
    kAbstract,  // This node doesn't represent anything in the program.
    kExpression,  // Node the result of evaluating an expression.
    kBaseType,  // Integers, characters, etc.
    kCollection,  // Structs and classes.
    kPtrRef,  // Pointers or references to anything.
    kPtrRefExpansion,  // The child of a pointer/reference that derefs it.
  };
  */

  // See the class comment above about the lifecycle.
  enum State {
    kEmpty,        // Nothing set, default constructed.
    kUnevaluated,  // Unevaluated expression.
    kHasValue,     // Have the value but not converted to a string.
    kDescribed     // Have the full type and value description.
  };

  FormatNode();

  // Constructor for a known value.
  FormatNode(const std::string& name, ExprValue value);

  // Constructor for the error case.
  FormatNode(const std::string& name, Err err);

  // Constructor with an expression.
  explicit FormatNode(const std::string& expression);

  // Constructor for a programatically-filled value.
  FormatNode(const std::string& name, GetProgramaticValue get_value);

  // Not copyable nor moveable since this doesn't work with the weak ptr
  // factory.

  ~FormatNode();

  fxl::WeakPtr<FormatNode> GetWeakPtr();

  Source source() const { return source_; }

  State state() const { return state_; }
  void set_state(State s) { state_ = s; }

  // The name of this node. This is used for things like structure member
  // names when nodes are expanded. For nodes with an expression type, this
  // name is not used.
  const std::string& name() const { return name_; }

  // When source() == kExpression this is the expression to evaluate. Use
  // FillFormatNodeValue() to convert this expression to a value.
  const std::string& expression() const { return expression_; }
  void set_expression(std::string e) { expression_ = std::move(e); }

  // Call when source == kProgramatic to fill the value from the getter. The
  // callback will be issued (possibly from within this call stack) when the
  // value is filled.
  void FillProgramaticValue(fxl::RefPtr<EvalContext> context,
                            fit::deferred_callback cb);

  // The value. This will be valid when the State == kHasValue. The description
  // and type might not be up-to-date, see FillFormatNodeDescription().
  //
  // The setter is out-of-line because we expect this will need to send change
  // notifications in the future.
  void SetValue(ExprValue v);
  const ExprValue& value() const { return value_; }

  // The type() is the stringified version of value_.type(). It is valid when
  // State == kDescribed.
  const std::string& type() const { return type_; }
  void set_type(std::string t) { type_ = std::move(t); }

  // The short descrition of this node's value. It is valid when State ==
  // kDescribed. For composite things like structs, the description might be
  // an abbreviated version of the struct's members.
  const std::string& description() const { return description_; }
  void set_description(std::string d) { description_ = std::move(d); }

  DescriptionKind description_kind() const { return description_kind_; }
  void set_description_kind(DescriptionKind dk) { description_kind_ = dk; }

  const ChildVector& children() const { return children_; }
  ChildVector& children() { return children_; }

  // There could have been an error filling in the node. The error could be
  // from computing the value of the expression, or in formatting the
  // ExprValue.
  //
  // The state of the node will represent the last good state. So if there was
  // an error evaluating the expression, the state will be "unevaluated" and
  // it could be evaluated again in a new context to resolve the error. If
  // there was an error formatting the value (say symbols are incorrect) the
  // state will be "has value" and in this case trying to reevaluate won't
  // recover from the error without the value changing.
  const Err& err() const { return err_; }
  void set_err(const Err& e) { err_ = e; }

 private:
  // See the getters above for documentation.
  Source source_ = kValue;
  State state_ = kEmpty;

  std::string name_;

  // Valid when source == kExpression.
  std::string expression_;

  // Valid when source == kProgramatic.
  GetProgramaticValue get_programatic_value_;

  ExprValue value_;  // Value when source == kValue or when state == kHasValue.

  // Valid when state == kDescribed.
  std::string type_;
  std::string description_;
  DescriptionKind description_kind_ = kNone;
  Err err_;

  ChildVector children_;

  fxl::WeakPtrFactory<FormatNode> weak_factory_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_EXPR_FORMAT_NODE_H_
