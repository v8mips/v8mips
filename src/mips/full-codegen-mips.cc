// Copyright 2010 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"

#if defined(V8_TARGET_ARCH_MIPS)

// UNCOMPLETED_MIPS macro.
#ifdef DEBUG
#define UNCOMPLETED_MIPS()                                                  \
  v8::internal::PrintF("%s, \tline %d: \tfunction %s HAS NOT BEEN COMPLETED! \n",    \
                       __FILE__, __LINE__, __func__)
#else
#define UNCOMPLETED_MIPS()
#endif

#include "codegen-inl.h"
#include "compiler.h"
#include "debug.h"
#include "full-codegen.h"
#include "parser.h"
#include "scopes.h"

namespace v8 {
namespace internal {

#define __ ACCESS_MASM(masm_)

// Generate code for a JS function.  On entry to the function the receiver
// and arguments have been pushed on the stack left to right.  The actual
// argument count matches the formal parameter count expected by the
// function.
//
// The live registers are:
//   o a1: the JS function object being called (ie, ourselves)
//   o cp: our context
//   o fp: our caller's frame pointer
//   o sp: stack pointer
//   o ra: return address
//
// The function builds a JS frame.  Please see JavaScriptFrameConstants in
// frames-mips.h for its layout.
void FullCodeGenerator::Generate(CompilationInfo* info, Mode mode) {
  ASSERT(info_ == NULL);
  info_ = info;
  SetFunctionPosition(function());
  Comment cmnt(masm_, "[ function compiled by full code generator");

  if (mode == PRIMARY) {
    int locals_count = scope()->num_stack_slots();

    __ Push(ra, fp, cp, a1);
    if (locals_count > 0) {
      // Load undefined value here, so the value is ready for the loop
      // below.
      __ LoadRoot(at, Heap::kUndefinedValueRootIndex);
    }
    // Adjust fp to point to caller's fp.
    __ Addu(fp, sp, Operand(2 * kPointerSize));

    { Comment cmnt(masm_, "[ Allocate locals");
      for (int i = 0; i < locals_count; i++) {
        __ push(at);
      }
    }

    bool function_in_register = true;

    // Possibly allocate a local context.
    int heap_slots = scope()->num_heap_slots() - Context::MIN_CONTEXT_SLOTS;
    if (heap_slots > 0) {
      Comment cmnt(masm_, "[ Allocate local context");
      // Argument to NewContext is the function, which is in a1.
      __ push(a1);
      if (heap_slots <= FastNewContextStub::kMaximumSlots) {
        FastNewContextStub stub(heap_slots);
        __ CallStub(&stub);
      } else {
        __ CallRuntime(Runtime::kNewContext, 1);
      }
      function_in_register = false;
      // Context is returned in both v0 and cp.  It replaces the context
      // passed to us.  It's saved in the stack and kept live in cp.
      __ sw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
      // Copy any necessary parameters into the context.
      int num_parameters = scope()->num_parameters();
      for (int i = 0; i < num_parameters; i++) {
        Slot* slot = scope()->parameter(i)->slot();
        if (slot != NULL && slot->type() == Slot::CONTEXT) {
          int parameter_offset = StandardFrameConstants::kCallerSPOffset +
                                   (num_parameters - 1 - i) * kPointerSize;
          // Load parameter from stack.
          __ lw(a0, MemOperand(fp, parameter_offset));
          // Store it in the context.
          __ li(a1, Operand(Context::SlotOffset(slot->index())));
          __ addu(a2, cp, a1);
          __ sw(a0, MemOperand(a2, 0));
          // Update the write barrier. This clobbers all involved
          // registers, so we have to use two more registers to avoid
          // clobbering cp.
          __ mov(a2, cp);
          __ RecordWrite(a2, a1, a3);
        }
      }
    }

    Variable* arguments = scope()->arguments()->AsVariable();
    if (arguments != NULL) {
      // Function uses arguments object.
      Comment cmnt(masm_, "[ Allocate arguments object");
      if (!function_in_register) {
        // Load this again, if it's used by the local context below.
        __ lw(a3, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
      } else {
        __ mov(a3, a1);
      }
      // Receiver is just before the parameters on the caller's stack.
      int offset = scope()->num_parameters() * kPointerSize;
      __ Addu(a2, fp,
             Operand(StandardFrameConstants::kCallerSPOffset + offset));
      __ li(a1, Operand(Smi::FromInt(scope()->num_parameters())));
      __ Push(a3, a2, a1);

      // Arguments to ArgumentsAccessStub:
      //   function, receiver address, parameter count.
      // The stub will rewrite receiever and parameter count if the previous
      // stack frame was an arguments adapter frame.
      ArgumentsAccessStub stub(ArgumentsAccessStub::NEW_OBJECT);
      __ CallStub(&stub);
      // Duplicate the value; move-to-slot operation might clobber registers.
      __ mov(a3, v0);
      Move(arguments->slot(), v0, a1, a2);
      Slot* dot_arguments_slot =
          scope()->arguments_shadow()->AsVariable()->slot();
      Move(dot_arguments_slot, a3, a1, a2);
    }
  }

  { Comment cmnt(masm_, "[ Declarations");
    // For named function expressions, declare the function name as a
    // constant.
    if (scope()->is_function_scope() && scope()->function() != NULL) {
      EmitDeclaration(scope()->function(), Variable::CONST, NULL);
    }
    // Visit all the explicit declarations unless there is an illegal
    // redeclaration.
    if (scope()->HasIllegalRedeclaration()) {
      scope()->VisitIllegalRedeclaration(this);
    } else {
      VisitDeclarations(scope()->declarations());
    }
  }

  // Check the stack for overflow or break request.
  { Comment cmnt(masm_, "[ Stack check");
    __ LoadRoot(t0, Heap::kStackLimitRootIndex);
    StackCheckStub stub;
    // Call the stub if lower.
    __ Push(ra);
    __ Call(Operand(reinterpret_cast<intptr_t>(stub.GetCode().location()),
            RelocInfo::CODE_TARGET),
            Uless, sp, Operand(t0));
    __ Pop(ra);
  }

  if (FLAG_trace) {
    __ CallRuntime(Runtime::kTraceEnter, 0);
  }

  { Comment cmnt(masm_, "[ Body");
    ASSERT(loop_depth() == 0);
    VisitStatements(function()->body());
    ASSERT(loop_depth() == 0);
  }

  { Comment cmnt(masm_, "[ return <undefined>;");
    // Emit a 'return undefined' in case control fell off the end of the
    // body.
    __ LoadRoot(v0, Heap::kUndefinedValueRootIndex);
  }
  EmitReturnSequence();
}


void FullCodeGenerator::EmitReturnSequence() {
  Comment cmnt(masm_, "[ Return sequence");
  if (return_label_.is_bound()) {
    __ b(&return_label_);
  } else {
    __ bind(&return_label_);
    if (FLAG_trace) {
      // Push the return value on the stack as the parameter.
      // Runtime::TraceExit returns its parameter in v0.
      __ push(v0);
      __ CallRuntime(Runtime::kTraceExit, 1);
    }

#ifdef DEBUG
    // Add a label for checking the size of the code used for returning.
    Label check_exit_codesize;
    masm_->bind(&check_exit_codesize);
#endif
    // Make sure that the constant pool is not emitted inside of the return
    // sequence.
    { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
      // Here we use masm_-> instead of the __ macro to avoid the code coverage
      // tool from instrumenting as we rely on the code size here.
      int32_t sp_delta = (scope()->num_parameters() + 1) * kPointerSize;
      CodeGenerator::RecordPositions(masm_, function()->end_position() - 1);
      __ RecordJSReturn();
      masm_->mov(sp, fp);
      masm_->MultiPop(static_cast<RegList>(fp.bit() | ra.bit()));
      masm_->Addu(sp, sp, Operand(sp_delta));
      masm_->Jump(ra);
    }

#ifdef DEBUG
    // Check that the size of the code used for returning matches what is
    // expected by the debugger. If the sp_delts above cannot be encoded in the
    // add instruction the add will generate two instructions.
    int return_sequence_length =
        masm_->InstructionsGeneratedSince(&check_exit_codesize);
    CHECK(return_sequence_length ==
          Assembler::kJSReturnSequenceInstructions ||
          return_sequence_length ==
          Assembler::kJSReturnSequenceInstructions + 1);
#endif
  }
}

FullCodeGenerator::ConstantOperand FullCodeGenerator::GetConstantOperand(
    Token::Value op, Expression* left, Expression* right) {
  UNIMPLEMENTED_MIPS();
  return kNoConstants;
}

void FullCodeGenerator::Apply(Expression::Context context, Register reg) {
  switch (context) {
    case Expression::kUninitialized:
      UNREACHABLE();

    case Expression::kEffect:
      // Nothing to do.
      break;

    case Expression::kValue:
      // Move value into place.
      switch (location_) {
        case kAccumulator:
          if (!reg.is(result_register())) __ mov(result_register(), reg);
          break;
        case kStack:
          __ push(reg);
          break;
      }
      break;

    case Expression::kValueTest:
    case Expression::kTestValue:
      // Push an extra copy of the value in case it's needed.
      __ push(reg);
      // Fall through.

    case Expression::kTest:
      // We always call the runtime on ARM, so push the value as argument.
      __ push(reg);
      DoTest(context);
      break;
  }
}


void FullCodeGenerator::Apply(Expression::Context context, Slot* slot) {
  switch (context) {
    case Expression::kUninitialized:
      UNREACHABLE();
    case Expression::kEffect:
      // Nothing to do.
      break;
    case Expression::kValue:
    case Expression::kTest:
    case Expression::kValueTest:
    case Expression::kTestValue:
      // On ARM we have to move the value into a register to do anything
      // with it.
      Move(result_register(), slot);
      Apply(context, result_register());
      break;
  }
}


void FullCodeGenerator::Apply(Expression::Context context, Literal* lit) {
  switch (context) {
    case Expression::kUninitialized:
      UNREACHABLE();
    case Expression::kEffect:
      break;
      // Nothing to do.
    case Expression::kValue:
    case Expression::kTest:
    case Expression::kValueTest:
    case Expression::kTestValue:
      // On ARM we have to move the value into a register to do anything
      // with it.
      __ li(result_register(), Operand(lit->handle()));
      Apply(context, result_register());
      break;
  }
}


void FullCodeGenerator::ApplyTOS(Expression::Context context) {
  switch (context) {
    case Expression::kUninitialized:
      UNREACHABLE();

    case Expression::kEffect:
      __ Drop(1);
      break;

    case Expression::kValue:
      switch (location_) {
        case kAccumulator:
          __ pop(result_register());
          break;
        case kStack:
          break;
      }
      break;

    case Expression::kValueTest:
    case Expression::kTestValue:
      // Duplicate the value on the stack in case it's needed.
      __ lw(t0, MemOperand(sp));
      __ push(t0);
      // Fall through.

    case Expression::kTest:
      DoTest(context);
      break;
  }
}


void FullCodeGenerator::DropAndApply(int count,
                                     Expression::Context context,
                                     Register reg) {
  ASSERT(count > 0);
  ASSERT(!reg.is(sp));
  switch (context) {
    case Expression::kUninitialized:
      UNREACHABLE();

    case Expression::kEffect:
      __ Drop(count);
      break;

    case Expression::kValue:
      switch (location_) {
        case kAccumulator:
          __ Drop(count);
          if (!reg.is(result_register())) __ mov(result_register(), reg);
          break;
        case kStack:
          if (count > 1) __ Drop(count - 1);
          __ sw(reg, MemOperand(sp));
          break;
      }
      break;

    case Expression::kTest:
      if (count > 1) __ Drop(count - 1);
      __ sw(reg, MemOperand(sp));
      DoTest(context);
      break;

    case Expression::kValueTest:
    case Expression::kTestValue:
      if (count == 1) {
        __ sw(reg, MemOperand(sp));
        __ push(reg);
      } else {  // count > 1
        __ Drop(count - 2);
        __ sw(reg, MemOperand(sp, kPointerSize));
        __ sw(reg, MemOperand(sp));
      }
      DoTest(context);
      break;
  }
}

void FullCodeGenerator::PrepareTest(Label* materialize_true,
                                    Label* materialize_false,
                                    Label** if_true,
                                    Label** if_false) {
  switch (context_) {
    case Expression::kUninitialized:
      UNREACHABLE();
      break;
    case Expression::kEffect:
      // In an effect context, the true and the false case branch to the
      // same label.
      *if_true = *if_false = materialize_true;
      break;
    case Expression::kValue:
      *if_true = materialize_true;
      *if_false = materialize_false;
      break;
    case Expression::kTest:
      *if_true = true_label_;
      *if_false = false_label_;
      break;
    case Expression::kValueTest:
      *if_true = materialize_true;
      *if_false = false_label_;
      break;
    case Expression::kTestValue:
      *if_true = true_label_;
      *if_false = materialize_false;
      break;
  }
}


void FullCodeGenerator::Apply(Expression::Context context,
                              Label* materialize_true,
                              Label* materialize_false) {
  switch (context) {
    case Expression::kUninitialized:

    case Expression::kEffect:
      ASSERT_EQ(materialize_true, materialize_false);
      __ bind(materialize_true);
      break;

    case Expression::kValue: {
      Label done;
      switch (location_) {
        case kAccumulator:
          __ bind(materialize_true);
          __ LoadRoot(result_register(), Heap::kTrueValueRootIndex);
          __ jmp(&done);
          __ bind(materialize_false);
          __ LoadRoot(result_register(), Heap::kFalseValueRootIndex);
          break;
        case kStack:
          __ bind(materialize_true);
          __ LoadRoot(t0, Heap::kTrueValueRootIndex);
          __ push(t0);
          __ jmp(&done);
          __ bind(materialize_false);
          __ LoadRoot(t0, Heap::kFalseValueRootIndex);
          __ push(t0);
          break;
      }
      __ bind(&done);
      break;
    }

    case Expression::kTest:
      break;

    case Expression::kValueTest:
      __ bind(materialize_true);
      switch (location_) {
        case kAccumulator:
          __ LoadRoot(result_register(), Heap::kTrueValueRootIndex);
          break;
        case kStack:
          __ LoadRoot(t0, Heap::kTrueValueRootIndex);
          __ push(t0);
          break;
      }
      __ jmp(true_label_);
      break;

    case Expression::kTestValue:
      __ bind(materialize_false);
      switch (location_) {
        case kAccumulator:
          __ LoadRoot(result_register(), Heap::kFalseValueRootIndex);
          break;
        case kStack:
          __ LoadRoot(t0, Heap::kFalseValueRootIndex);
          __ push(t0);
          break;
      }
      __ jmp(false_label_);
      break;
  }
}


// Convert constant control flow (true or false) to the result expected for
// a given expression context.
void FullCodeGenerator::Apply(Expression::Context context, bool flag) {
  switch (context) {
    case Expression::kUninitialized:
      UNREACHABLE();
      break;
    case Expression::kEffect:
      break;
    case Expression::kValue: {
      Heap::RootListIndex value_root_index =
          flag ? Heap::kTrueValueRootIndex : Heap::kFalseValueRootIndex;
      switch (location_) {
        case kAccumulator:
          __ LoadRoot(result_register(), value_root_index);
          break;
        case kStack:
          __ LoadRoot(t0, value_root_index);
          __ push(t0);
          break;
      }
      break;
    }
    case Expression::kTest:
      __ b(flag ? true_label_ : false_label_);
      break;
    case Expression::kTestValue:
      switch (location_) {
        case kAccumulator:
          // If value is false it's needed.
          if (!flag) __ LoadRoot(result_register(), Heap::kFalseValueRootIndex);
          break;
        case kStack:
          // If value is false it's needed.
          if (!flag) {
            __ LoadRoot(t0, Heap::kFalseValueRootIndex);
            __ push(t0);
          }
          break;
      }
      __ b(flag ? true_label_ : false_label_);
      break;
    case Expression::kValueTest:
      switch (location_) {
        case kAccumulator:
          // If value is true it's needed.
          if (flag) __ LoadRoot(result_register(), Heap::kTrueValueRootIndex);
          break;
        case kStack:
          // If value is true it's needed.
          if (flag) {
            __ LoadRoot(t0, Heap::kTrueValueRootIndex);
            __ push(t0);
          }
          break;
      }
      __ b(flag ? true_label_ : false_label_);
      break;
  }
}


void FullCodeGenerator::DoTest(Expression::Context context) {
  UNIMPLEMENTED_MIPS();
}


MemOperand FullCodeGenerator::EmitSlotSearch(Slot* slot, Register scratch) {
  switch (slot->type()) {
    case Slot::PARAMETER:
    case Slot::LOCAL:
      return MemOperand(fp, SlotOffset(slot));
    case Slot::CONTEXT: {
      int context_chain_length =
          scope()->ContextChainLength(slot->var()->scope());
      __ LoadContext(scratch, context_chain_length);
      return CodeGenerator::ContextOperand(scratch, slot->index());
    }
    case Slot::LOOKUP:
      UNREACHABLE();
  }
  UNREACHABLE();
  return MemOperand(v0, 0);
}


void FullCodeGenerator::Move(Register destination, Slot* source) {
  // Use destination as scratch.
  MemOperand slot_operand = EmitSlotSearch(source, destination);
  __ lw(destination, slot_operand);
}


void FullCodeGenerator::Move(Slot* dst,
                             Register src,
                             Register scratch1,
                             Register scratch2) {
  UNIMPLEMENTED_MIPS();
}


void FullCodeGenerator::EmitDeclaration(Variable* variable,
                                        Variable::Mode mode,
                                        FunctionLiteral* function) {
  Comment cmnt(masm_, "[ Declaration");
  ASSERT(variable != NULL);  // Must have been resolved.
  Slot* slot = variable->slot();
  Property* prop = variable->AsProperty();

  if (slot != NULL) {
    switch (slot->type()) {
      case Slot::PARAMETER:
      case Slot::LOCAL:
        if (mode == Variable::CONST) {
          __ LoadRoot(t0, Heap::kTheHoleValueRootIndex);
          __ sw(t0, MemOperand(fp, SlotOffset(slot)));
        } else if (function != NULL) {
          VisitForValue(function, kAccumulator);
          __ sw(result_register(), MemOperand(fp, SlotOffset(slot)));
        }
        break;

      case Slot::CONTEXT:
         UNCOMPLETED_MIPS();
         ASSERT(0);
        break;

      case Slot::LOOKUP: {
         UNCOMPLETED_MIPS();
         ASSERT(0);
      }
    }

  } else if (prop != NULL) {
         UNCOMPLETED_MIPS();
         ASSERT(0);
  }
}

void FullCodeGenerator::VisitDeclaration(Declaration* decl) {
  EmitDeclaration(decl->proxy()->var(), decl->mode(), decl->fun());
}


void FullCodeGenerator::DeclareGlobals(Handle<FixedArray> pairs) {
  // Call the runtime to declare the globals.
  // The context is the first argument.
  __ li(a1, Operand(pairs));
  __ li(a0, Operand(Smi::FromInt(is_eval() ? 1 : 0)));
  __ Push(cp, a1, a0);
  __ CallRuntime(Runtime::kDeclareGlobals, 3);
  // Return value is ignored.
}


void FullCodeGenerator::VisitSwitchStatement(SwitchStatement* stmt) {
  UNIMPLEMENTED_MIPS();
}


void FullCodeGenerator::VisitForInStatement(ForInStatement* stmt) {
  UNIMPLEMENTED_MIPS();
}


void FullCodeGenerator::EmitNewClosure(Handle<SharedFunctionInfo> info) {
  // Use the fast case closure allocation code that allocates in new
  // space for nested functions that don't need literals cloning.
  if (scope()->is_function_scope() && info->num_literals() == 0) {
    FastNewClosureStub stub;
    __ li(a0, Operand(info));
    __ push(a0);
    __ CallStub(&stub);
  } else {
    __ li(a0, Operand(info));
    __ Push(cp, a0);
    __ CallRuntime(Runtime::kNewClosure, 2);
  }
  Apply(context_, v0);
}


void FullCodeGenerator::VisitVariableProxy(VariableProxy* expr) {
  Comment cmnt(masm_, "[ VariableProxy");
  EmitVariableLoad(expr->var(), context_);
}

MemOperand FullCodeGenerator::ContextSlotOperandCheckExtensions(
    Slot* slot,
    Label* slow) {
  UNIMPLEMENTED_MIPS();
  return MemOperand(zero_reg, 0);
}

void FullCodeGenerator::EmitDynamicLoadFromSlotFastCase(
    Slot* slot,
    TypeofState typeof_state,
    Label* slow,
    Label* done) {
  UNIMPLEMENTED_MIPS();
}

void FullCodeGenerator::EmitLoadGlobalSlotCheckExtensions(
    Slot* slot,
    TypeofState typeof_state,
    Label* slow) {
  UNIMPLEMENTED_MIPS();
}


void FullCodeGenerator::EmitVariableLoad(Variable* var,
                                         Expression::Context context) {
  // Four cases: non-this global variables, lookup slots, all other
  // types of slots, and parameters that rewrite to explicit property
  // accesses on the arguments object.
  Slot* slot = var->slot();
  Property* property = var->AsProperty();

  if (var->is_global() && !var->is_this()) {
    Comment cmnt(masm_, "Global variable");
    // Use inline caching. Variable name is passed in a2 and the global
    // object (receiver) in a0.
    __ lw(a0, CodeGenerator::GlobalObject());
    __ li(a2, Operand(var->name()));
    Handle<Code> ic(Builtins::builtin(Builtins::LoadIC_Initialize));
    __ Call(ic, RelocInfo::CODE_TARGET_CONTEXT);
    Apply(context, v0);

  } else if (slot != NULL && slot->type() == Slot::LOOKUP) {
    Comment cmnt(masm_, "Lookup slot");
    __ li(a1, Operand(var->name()));
    __ Push(cp, a1);  // Context and name.
    __ CallRuntime(Runtime::kLoadContextSlot, 2);
    Apply(context, v0);

  } else if (slot != NULL) {
    Comment cmnt(masm_, (slot->type() == Slot::CONTEXT)
                            ? "Context slot"
                            : "Stack slot");
    if (var->mode() == Variable::CONST) {
       // Constants may be the hole value if they have not been initialized.
       // Unhole them.
       Label done;
       MemOperand slot_operand = EmitSlotSearch(slot, a0);
       __ lw(v0, slot_operand);
       __ LoadRoot(t0, Heap::kTheHoleValueRootIndex);
       __ Branch(&done, ne, v0, Operand(t0));
       __ LoadRoot(v0, Heap::kUndefinedValueRootIndex);
       __ bind(&done);
       Apply(context, v0);
     } else {
       Apply(context, slot);
     }
  } else {
    Comment cmnt(masm_, "Rewritten parameter");
    ASSERT_NOT_NULL(property);
    // Rewritten parameter accesses are of the form "slot[literal]".
    // Assert that the object is in a slot.
    Variable* object_var = property->obj()->AsVariableProxy()->AsVariable();
    ASSERT_NOT_NULL(object_var);
    Slot* object_slot = object_var->slot();
    ASSERT_NOT_NULL(object_slot);

    // Load the object.
    Move(a1, object_slot);

    // Assert that the key is a smi.
    Literal* key_literal = property->key()->AsLiteral();
    ASSERT_NOT_NULL(key_literal);
    ASSERT(key_literal->handle()->IsSmi());

    // Load the key.
    __ li(a0, Operand(key_literal->handle()));

//    __ break_(0x5544);
    // Call keyed load IC. It has arguments key and receiver in r0 and r1.
    Handle<Code> ic(Builtins::builtin(Builtins::KeyedLoadIC_Initialize));
    __ Call(ic, RelocInfo::CODE_TARGET);
    Apply(context, v0);
  }
}


void FullCodeGenerator::VisitRegExpLiteral(RegExpLiteral* expr) {
  UNIMPLEMENTED_MIPS();
}


void FullCodeGenerator::VisitObjectLiteral(ObjectLiteral* expr) {
  Comment cmnt(masm_, "[ ObjectLiteral");
  __ lw(a3, MemOperand(fp,  JavaScriptFrameConstants::kFunctionOffset));
  __ lw(a3, FieldMemOperand(a3, JSFunction::kLiteralsOffset));
  __ li(a2, Operand(Smi::FromInt(expr->literal_index())));
  __ li(a1, Operand(expr->constant_properties()));
  __ li(a0, Operand(Smi::FromInt(expr->fast_elements() ? 1 : 0)));
  __ Push(a3, a2, a1, a0);
  if (expr->depth() > 1) {
    __ CallRuntime(Runtime::kCreateObjectLiteral, 4);
  } else {
    __ CallRuntime(Runtime::kCreateObjectLiteralShallow, 4);
  }

  // If result_saved is true the result is on top of the stack.  If
  // result_saved is false the result is in v0.
  bool result_saved = false;

  for (int i = 0; i < expr->properties()->length(); i++) {
    ObjectLiteral::Property* property = expr->properties()->at(i);
    if (property->IsCompileTimeValue()) continue;

    Literal* key = property->key();
    Expression* value = property->value();
    if (!result_saved) {
      __ push(v0);  // Save result on stack
      result_saved = true;
    }
    switch (property->kind()) {
      case ObjectLiteral::Property::CONSTANT:
        UNREACHABLE();
      case ObjectLiteral::Property::MATERIALIZED_LITERAL:
        ASSERT(!CompileTimeValue::IsCompileTimeValue(property->value()));
        // Fall through.
      case ObjectLiteral::Property::COMPUTED:
        if (key->handle()->IsSymbol()) {
          VisitForValue(value, kAccumulator);
          __ mov(a0, result_register());
          __ li(a2, Operand(key->handle()));
          __ lw(a1, MemOperand(sp));
          Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
          __ Call(ic, RelocInfo::CODE_TARGET);
          break;
        }
        // Fall through.
      case ObjectLiteral::Property::PROTOTYPE:
         UNCOMPLETED_MIPS();
         ASSERT(0);
        break;
      case ObjectLiteral::Property::GETTER:
      case ObjectLiteral::Property::SETTER:
         UNCOMPLETED_MIPS();
         ASSERT(0);
        break;
    }
  }

  if (result_saved) {
    ApplyTOS(context_);
  } else {
    Apply(context_, v0);
  }
}


void FullCodeGenerator::VisitArrayLiteral(ArrayLiteral* expr) {
  Comment cmnt(masm_, "[ ArrayLiteral");

  ZoneList<Expression*>* subexprs = expr->values();
  int length = subexprs->length();
  __ mov(a0, result_register());
  __ lw(a3, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
  __ lw(a3, FieldMemOperand(a3, JSFunction::kLiteralsOffset));
  __ li(a2, Operand(Smi::FromInt(expr->literal_index())));
  __ li(a1, Operand(expr->constant_elements()));
  __ Push(a3, a2, a1);
  if (expr->depth() > 1) {
    __ CallRuntime(Runtime::kCreateArrayLiteral, 3);
  } else if (length > FastCloneShallowArrayStub::kMaximumLength) {
    __ CallRuntime(Runtime::kCreateArrayLiteralShallow, 3);
  } else {
    FastCloneShallowArrayStub stub(length);
    __ CallStub(&stub);
  }

  bool result_saved = false;  // Is the result saved to the stack?

  // Emit code to evaluate all the non-constant subexpressions and to store
  // them into the newly cloned array.
  for (int i = 0; i < length; i++) {
    Expression* subexpr = subexprs->at(i);
    // If the subexpression is a literal or a simple materialized literal it
    // is already set in the cloned array.
    if (subexpr->AsLiteral() != NULL ||
        CompileTimeValue::IsCompileTimeValue(subexpr)) {
      continue;
    }

    if (!result_saved) {
      __ push(v0);
      result_saved = true;
    }
    VisitForValue(subexpr, kAccumulator);

    // Store the subexpression value in the array's elements.
    __ lw(a1, MemOperand(sp));  // Copy of array literal.
    __ lw(a1, FieldMemOperand(a1, JSObject::kElementsOffset));
    int offset = FixedArray::kHeaderSize + (i * kPointerSize);
    __ sw(result_register(), FieldMemOperand(a1, offset));

    // Update the write barrier for the array store with v0 as the scratch
    // register.
    __ li(a2, Operand(offset));
    // TODO(PJ): double check this RecordWrite call.
    __ RecordWrite(a1, a2, result_register());
  }

  if (result_saved) {
    ApplyTOS(context_);
  } else {
    Apply(context_, v0);
  }
}


void FullCodeGenerator::VisitAssignment(Assignment* expr) {
  Comment cmnt(masm_, "[ Assignment");
  // Invalid left-hand sides are rewritten to have a 'throw ReferenceError'
  // on the left-hand side.
  if (!expr->target()->IsValidLeftHandSide()) {
    VisitForEffect(expr->target());
    return;
  }

  // Left-hand side can only be a property, a global or a (parameter or local)
  // slot. Variables with rewrite to .arguments are treated as KEYED_PROPERTY.
  enum LhsKind { VARIABLE, NAMED_PROPERTY, KEYED_PROPERTY };
  LhsKind assign_type = VARIABLE;
  Property* prop = expr->target()->AsProperty();
  if (prop != NULL) {
    assign_type =
        (prop->key()->IsPropertyName()) ? NAMED_PROPERTY : KEYED_PROPERTY;
  }

  // Evaluate LHS expression.
  switch (assign_type) {
    case VARIABLE:
      // Nothing to do here.
      break;
    case NAMED_PROPERTY:
      if (expr->is_compound()) {
        // We need the receiver both on the stack and in the accumulator.
        VisitForValue(prop->obj(), kAccumulator);
        __ push(result_register());
      } else {
        VisitForValue(prop->obj(), kStack);
      }
      break;
    case KEYED_PROPERTY:
      // We need the key and receiver on both the stack and in r0 and r1.
      if (expr->is_compound()) {
        VisitForValue(prop->obj(), kStack);
        VisitForValue(prop->key(), kAccumulator);
        __ lw(a1, MemOperand(sp, 0));
        __ push(v0);
      } else {
        VisitForValue(prop->obj(), kStack);
        VisitForValue(prop->key(), kStack);
      }
      break;
  }

  // If we have a compound assignment: Get value of LHS expression and
  // store in on top of the stack.
  if (expr->is_compound()) {
    Location saved_location = location_;
    location_ = kStack;
    switch (assign_type) {
      case VARIABLE:
        EmitVariableLoad(expr->target()->AsVariableProxy()->var(),
                         Expression::kValue);
        break;
      case NAMED_PROPERTY:
        EmitNamedPropertyLoad(prop);
        __ push(result_register());
        break;
      case KEYED_PROPERTY:
        EmitKeyedPropertyLoad(prop);
        __ push(result_register());
        break;
    }
    location_ = saved_location;
  }

  // Evaluate RHS expression.
  Expression* rhs = expr->value();
  VisitForValue(rhs, kAccumulator);

  // If we have a compound assignment: Apply operator.
  if (expr->is_compound()) {
    Location saved_location = location_;
    location_ = kAccumulator;
    EmitBinaryOp(expr->binary_op(), Expression::kValue);
    location_ = saved_location;
  }

  // Record source position before possible IC call.
  SetSourcePosition(expr->position());

  // Store the value.
  switch (assign_type) {
    case VARIABLE:
      EmitVariableAssignment(expr->target()->AsVariableProxy()->var(),
                             expr->op(),
                             context_);
      break;
    case NAMED_PROPERTY:
      EmitNamedPropertyAssignment(expr);
      break;
    case KEYED_PROPERTY:
      EmitKeyedPropertyAssignment(expr);
      break;
  }
}


void FullCodeGenerator::EmitNamedPropertyLoad(Property* prop) {
  SetSourcePosition(prop->position());
  Literal* key = prop->key()->AsLiteral();
  __ mov(a0, result_register());
  __ li(a2, Operand(key->handle()));
  // Call load IC. It has arguments receiver and property name a0 and a2.
  Handle<Code> ic(Builtins::builtin(Builtins::LoadIC_Initialize));
  __ Call(ic, RelocInfo::CODE_TARGET);
}


void FullCodeGenerator::EmitKeyedPropertyLoad(Property* prop) {
  SetSourcePosition(prop->position());
  __ mov(a0, result_register());
  // Call keyed load IC. It has arguments key and receiver in a0 and a1.
  Handle<Code> ic(Builtins::builtin(Builtins::KeyedLoadIC_Initialize));
  __ Call(ic, RelocInfo::CODE_TARGET);
}


void FullCodeGenerator::EmitInlineSmiBinaryOp(Expression* expr,
                                              Token::Value op,
                                              Expression::Context context,
                                              OverwriteMode mode,
                                              Expression* left,
                                              Expression* right,
                                              ConstantOperand constant) {
  UNIMPLEMENTED_MIPS();
}

void FullCodeGenerator::EmitBinaryOp(Token::Value op,
                                     Expression::Context context,
                                     OverwriteMode mode) {
  __ mov(a0, result_register());
  __ pop(a1);
  GenericBinaryOpStub stub(op, NO_OVERWRITE, a1, a0);
  __ CallStub(&stub);
  Apply(context, v0);
}


void FullCodeGenerator::EmitVariableAssignment(Variable* var,
                                               Token::Value op,
                                               Expression::Context context) {
  // Left-hand sides that rewrite to explicit property accesses do not reach
  // here.
  ASSERT(var != NULL);
  ASSERT(var->is_global() || var->slot() != NULL);

  if (var->is_global()) {
    ASSERT(!var->is_this());
    // Assignment to a global variable.  Use inline caching for the
    // assignment.  Right-hand-side value is passed in a0, variable name in
    // a2, and the global object in a1.
    __ mov(a0, result_register());
    __ li(a2, Operand(var->name()));
    __ lw(a1, CodeGenerator::GlobalObject());
    Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
    __ Call(ic, RelocInfo::CODE_TARGET);

  } else if (var->mode() != Variable::CONST || op == Token::INIT_CONST) {
    // Perform the assignment for non-const variables and for initialization
    // of const variables.  Const assignments are simply skipped.
    Label done;
    Slot* slot = var->slot();
    switch (slot->type()) {
      case Slot::PARAMETER:
      case Slot::LOCAL:
        if (op == Token::INIT_CONST) {
          // Detect const reinitialization by checking for the hole value.
          __ lw(a1, MemOperand(fp, SlotOffset(slot)));
          __ LoadRoot(t0, Heap::kTheHoleValueRootIndex);
          __ Branch(&done, ne, a1, Operand(t0));
        }
        // Perform the assignment.
        __ sw(result_register(), MemOperand(fp, SlotOffset(slot)));
        break;

      case Slot::CONTEXT: {
        MemOperand target = EmitSlotSearch(slot, a1);
        if (op == Token::INIT_CONST) {
          // Detect const reinitialization by checking for the hole value.
          __ lw(a2, target);
          __ LoadRoot(t0, Heap::kTheHoleValueRootIndex);
          __ Branch(&done, ne, a2, Operand(t0));
        }
        // Perform the assignment and issue the write barrier.
        __ sw(result_register(), target);
        // RecordWrite may destroy all its register arguments.
        __ mov(a3, result_register());
         UNCOMPLETED_MIPS();
         ASSERT(0);  // TODO(PJ): Get back to this!
        //  int offset = FixedArray::kHeaderSize + slot->index() * kPointerSize;
        // __ RecordWrite(a1, Operand(offset), a2, a3);
        break;
      }

      case Slot::LOOKUP:
        // Call the runtime for the assignment.  The runtime will ignore
        // const reinitialization.
        __ push(a0);  // Value.
        __ li(a0, Operand(slot->var()->name()));
        __ Push(cp, a0);  // Context and name.
        if (op == Token::INIT_CONST) {
          // The runtime will ignore const redeclaration.
          __ CallRuntime(Runtime::kInitializeConstContextSlot, 3);
        } else {
          __ CallRuntime(Runtime::kStoreContextSlot, 3);
        }
        break;
    }
    __ bind(&done);
  }

  Apply(context, result_register());
}


void FullCodeGenerator::EmitNamedPropertyAssignment(Assignment* expr) {
  // Assignment to a property, using a named store IC.
  Property* prop = expr->target()->AsProperty();
  ASSERT(prop != NULL);
  ASSERT(prop->key()->AsLiteral() != NULL);

  // If the assignment starts a block of assignments to the same object,
  // change to slow case to avoid the quadratic behavior of repeatedly
  // adding fast properties.
  if (expr->starts_initialization_block()) {
    __ push(result_register());
    __ lw(t0, MemOperand(sp, kPointerSize));  // Receiver is now under value.
    __ push(t0);
    __ CallRuntime(Runtime::kToSlowProperties, 1);
    __ pop(result_register());
  }

  // Record source code position before IC call.
  SetSourcePosition(expr->position());
  __ mov(a0, result_register());  // Load the value.
  __ li(a2, Operand(prop->key()->AsLiteral()->handle()));
  // Load receiver to a1. Leave a copy in the stack if needed for turning the
  // receiver into fast case.
  if (expr->ends_initialization_block()) {
    __ lw(a1, MemOperand(sp));
  } else {
    __ pop(a1);
  }

  Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
  __ Call(ic, RelocInfo::CODE_TARGET);

  // If the assignment ends an initialization block, revert to fast case.
  if (expr->ends_initialization_block()) {
    __ push(v0);  // Result of assignment, saved even if not needed.
    // Receiver is under the result value.
    __ lw(t0, MemOperand(sp, kPointerSize));
    __ push(t0);
    __ CallRuntime(Runtime::kToFastProperties, 1);
    __ pop(v0);
    DropAndApply(1, context_, v0);
  } else {
    Apply(context_, v0);
  }
}


void FullCodeGenerator::EmitKeyedPropertyAssignment(Assignment* expr) {
  // Assignment to a property, using a keyed store IC.

  // If the assignment starts a block of assignments to the same object,
  // change to slow case to avoid the quadratic behavior of repeatedly
  // adding fast properties.
  if (expr->starts_initialization_block()) {
    __ push(result_register());
    // Receiver is now under the key and value.
    __ lw(t0, MemOperand(sp, 2 * kPointerSize));
    __ push(t0);
    __ CallRuntime(Runtime::kToSlowProperties, 1);
    __ pop(result_register());
  }

  // Record source code position before IC call.
  SetSourcePosition(expr->position());
  // Call keyed store IC.
  // The arguments are:
  // - a0 is the value,
  // - a1 is the key,
  // - a2 is the receiver.
  __ mov(a0, result_register());
  __ pop(a1);  // Key.
  // Load receiver to a2. Leave a copy in the stack if needed for turning the
  // receiver into fast case.
  if (expr->ends_initialization_block()) {
    __ lw(a2, MemOperand(sp));
  } else {
    __ pop(a2);
  }
  Handle<Code> ic(Builtins::builtin(Builtins::KeyedStoreIC_Initialize));
  __ Call(ic, RelocInfo::CODE_TARGET);

  // If the assignment ends an initialization block, revert to fast case.
  if (expr->ends_initialization_block()) {
    __ push(v0);  // Result of assignment, saved even if not needed.
    // Receiver is under the result value.
    __ lw(t0, MemOperand(sp, kPointerSize));
    __ push(t0);
    __ CallRuntime(Runtime::kToFastProperties, 1);
    __ pop(v0);
    DropAndApply(1, context_, v0);
  } else {
    Apply(context_, v0);
  }
}


void FullCodeGenerator::VisitProperty(Property* expr) {
  Comment cmnt(masm_, "[ Property");
  Expression* key = expr->key();

  if (key->IsPropertyName()) {
    VisitForValue(expr->obj(), kAccumulator);
    EmitNamedPropertyLoad(expr);
    Apply(context_, v0);
  } else {
    VisitForValue(expr->obj(), kStack);
    VisitForValue(expr->key(), kAccumulator);
    __ pop(a1);
    EmitKeyedPropertyLoad(expr);
    Apply(context_, v0);
  }
}

void FullCodeGenerator::EmitCallWithIC(Call* expr,
                                       Handle<Object> name,
                                       RelocInfo::Mode mode) {
  // Code common for calls using the IC.
  ZoneList<Expression*>* args = expr->arguments();
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    VisitForValue(args->at(i), kStack);
  }
  __ li(a2, Operand(name));
  // Record source position for debugger.
  SetSourcePosition(expr->position());
  // Call the IC initialization code.
  InLoopFlag in_loop = (loop_depth() > 0) ? IN_LOOP : NOT_IN_LOOP;
  Handle<Code> ic = CodeGenerator::ComputeCallInitialize(arg_count, in_loop);
  __ Call(ic, mode);
  // Restore context register.
  __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
  Apply(context_, v0);
}


void FullCodeGenerator::EmitKeyedCallWithIC(Call* expr,
                                            Expression* key,
                                            RelocInfo::Mode mode) {
  // Code common for calls using the IC.
  ZoneList<Expression*>* args = expr->arguments();
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    VisitForValue(args->at(i), kStack);
  }
  VisitForValue(key, kAccumulator);
  __ mov(a2, result_register());
  // Record source position for debugger.
  SetSourcePosition(expr->position());
  // Call the IC initialization code.
  InLoopFlag in_loop = (loop_depth() > 0) ? IN_LOOP : NOT_IN_LOOP;
  Handle<Code> ic = CodeGenerator::ComputeKeyedCallInitialize(arg_count,
                                                              in_loop);
  __ Call(ic, mode);
  // Restore context register.
  __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
  Apply(context_, v0);
}

void FullCodeGenerator::EmitCallWithStub(Call* expr) {
  // Code common for calls using the call stub.
  ZoneList<Expression*>* args = expr->arguments();
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    VisitForValue(args->at(i), kStack);
  }
  // Record source position for debugger.
  SetSourcePosition(expr->position());
  InLoopFlag in_loop = (loop_depth() > 0) ? IN_LOOP : NOT_IN_LOOP;
  CallFunctionStub stub(arg_count, in_loop, RECEIVER_MIGHT_BE_VALUE);
  __ CallStub(&stub);
  // Restore context register.
  __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
  DropAndApply(1, context_, v0);
}


void FullCodeGenerator::VisitCall(Call* expr) {
  Comment cmnt(masm_, "[ Call");
  Expression* fun = expr->expression();
  Variable* var = fun->AsVariableProxy()->AsVariable();

  if (var != NULL && var->is_possibly_eval()) {
    // In a call to eval, we first call %ResolvePossiblyDirectEval to
    // resolve the function we need to call and the receiver of the
    // call.  Then we call the resolved function using the given
    // arguments.
    VisitForValue(fun, kStack);
    __ LoadRoot(a2, Heap::kUndefinedValueRootIndex);
    __ push(a2);  // Reserved receiver slot.

    // Push the arguments.
    ZoneList<Expression*>* args = expr->arguments();
    int arg_count = args->length();
    for (int i = 0; i < arg_count; i++) {
      VisitForValue(args->at(i), kStack);
    }

    // Push copy of the function - found below the arguments.
    __ lw(a1, MemOperand(sp, (arg_count + 1) * kPointerSize));
    __ push(a1);

    // Push copy of the first argument or undefined if it doesn't exist.
    if (arg_count > 0) {
      __ lw(a1, MemOperand(sp, arg_count * kPointerSize));
      __ push(a1);
    } else {
      __ push(a2);
    }

    // Push the receiver of the enclosing function and do runtime call.
    __ lw(a1, MemOperand(fp, (2 + scope()->num_parameters()) * kPointerSize));
    __ push(a1);
    __ CallRuntime(Runtime::kResolvePossiblyDirectEval, 3);

    // The runtime call returns a pair of values in v0 (function) and
    // v1 (receiver). Touch up the stack with the right values.
    __ sw(v0, MemOperand(sp, (arg_count + 1) * kPointerSize));
    __ sw(v1, MemOperand(sp, arg_count * kPointerSize));

    // Record source position for debugger.
    SetSourcePosition(expr->position());
    InLoopFlag in_loop = (loop_depth() > 0) ? IN_LOOP : NOT_IN_LOOP;
    CallFunctionStub stub(arg_count, in_loop, RECEIVER_MIGHT_BE_VALUE);
    __ CallStub(&stub);
    // Restore context register.
    __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
    DropAndApply(1, context_, v0);
  } else if (var != NULL && !var->is_this() && var->is_global()) {
    // Push global object as receiver for the call IC.
    __ lw(a0, CodeGenerator::GlobalObject());
    __ push(a0);
    EmitCallWithIC(expr, var->name(), RelocInfo::CODE_TARGET_CONTEXT);
  } else if (var != NULL && var->slot() != NULL &&
             var->slot()->type() == Slot::LOOKUP) {
    // Call to a lookup slot (dynamically introduced variable).  Call the
    // runtime to find the function to call (returned in eax) and the object
    // holding it (returned in edx).
    __ push(context_register());
    __ li(a2, Operand(var->name()));
    __ push(a2);
    __ CallRuntime(Runtime::kLoadContextSlot, 2);
    __ push(v0);  // Function.
    __ push(v1);  // Receiver.
    EmitCallWithStub(expr);
  } else if (fun->AsProperty() != NULL) {
    // Call to an object property.
    Property* prop = fun->AsProperty();
    Literal* key = prop->key()->AsLiteral();
    if (key != NULL && key->handle()->IsSymbol()) {
      // Call to a named property, use call IC.
      VisitForValue(prop->obj(), kStack);
      EmitCallWithIC(expr, key->handle(), RelocInfo::CODE_TARGET);
    } else {
      // Call to a keyed property.
      // For a synthetic property use keyed load IC followed by function call,
      // for a regular property use keyed CallIC.
      VisitForValue(prop->obj(), kStack);
      if (prop->is_synthetic()) {
        VisitForValue(prop->key(), kAccumulator);
        // Record source code position for IC call.
        SetSourcePosition(prop->position());
        __ pop(v1);  // We do not need to keep the receiver.

        Handle<Code> ic(Builtins::builtin(Builtins::KeyedLoadIC_Initialize));
        __ Call(ic, RelocInfo::CODE_TARGET);
        // Push result (function).
        __ push(v0);
        // Push Global receiver.
        __ lw(a1, CodeGenerator::GlobalObject());
        __ lw(a1, FieldMemOperand(a1, GlobalObject::kGlobalReceiverOffset));
        __ push(a1);
        EmitCallWithStub(expr);
      } else {
        EmitKeyedCallWithIC(expr, prop->key(), RelocInfo::CODE_TARGET);
      }
    }
  } else {
    // Call to some other expression.  If the expression is an anonymous
    // function literal not called in a loop, mark it as one that should
    // also use the fast code generator.
    FunctionLiteral* lit = fun->AsFunctionLiteral();
    if (lit != NULL &&
        lit->name()->Equals(Heap::empty_string()) &&
        loop_depth() == 0) {
      lit->set_try_full_codegen(true);
    }
    VisitForValue(fun, kStack);
    // Load global receiver object.
    __ lw(a1, CodeGenerator::GlobalObject());
    __ lw(a1, FieldMemOperand(a1, GlobalObject::kGlobalReceiverOffset));
    __ push(a1);
    // Emit function call.
    EmitCallWithStub(expr);
  }
}


void FullCodeGenerator::VisitCallNew(CallNew* expr) {
  Comment cmnt(masm_, "[ CallNew");
  // According to ECMA-262, section 11.2.2, page 44, the function
  // expression in new calls must be evaluated before the
  // arguments.
  // Push function on the stack.
  VisitForValue(expr->expression(), kStack);

  // Push global object (receiver).
  __ lw(a0, CodeGenerator::GlobalObject());
  __ push(a0);
  // Push the arguments ("left-to-right") on the stack.
  ZoneList<Expression*>* args = expr->arguments();
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    VisitForValue(args->at(i), kStack);
  }

  // Call the construct call builtin that handles allocation and
  // constructor invocation.
  SetSourcePosition(expr->position());

  // Load function, arg_count into a1 and a0.
  __ li(a0, Operand(arg_count));
  // Function is in sp[arg_count + 1].
  __ lw(a1, MemOperand(sp, (arg_count + 1) * kPointerSize));

  Handle<Code> construct_builtin(Builtins::builtin(Builtins::JSConstructCall));
  __ Call(construct_builtin, RelocInfo::CONSTRUCT_CALL);

  // Replace function on TOS with result in v0, or pop it.
  DropAndApply(1, context_, v0);
}

void FullCodeGenerator::EmitIsFunction(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 1);

  VisitForValue(args->at(0), kAccumulator);

  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  PrepareTest(&materialize_true, &materialize_false, &if_true, &if_false);

  __ BranchOnSmi(v0, if_false);
  __ GetObjectType(v0, a1, a2);
  __ Branch(if_true, eq, a2, Operand(JS_FUNCTION_TYPE));
  __ b(if_false);

  Apply(context_, if_true, if_false);
}
void FullCodeGenerator::VisitCallRuntime(CallRuntime* expr) {
  Handle<String> name = expr->name();
  if (name->length() > 0 && name->Get(0) == '_') {
    Comment cmnt(masm_, "[ InlineRuntimeCall");
    EmitInlineRuntimeCall(expr);
    return;
  }

  Comment cmnt(masm_, "[ CallRuntime");
  ZoneList<Expression*>* args = expr->arguments();

  if (expr->is_jsruntime()) {
    // Prepare for calling JS runtime function.
    __ lw(a0, CodeGenerator::GlobalObject());
    __ lw(a0, FieldMemOperand(a0, GlobalObject::kBuiltinsOffset));
    __ push(a0);
  }

  // Push the arguments ("left-to-right").
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    VisitForValue(args->at(i), kStack);
  }

  if (expr->is_jsruntime()) {
    // Call the JS runtime function.
    __ li(a2, Operand(expr->name()));
    Handle<Code> ic = CodeGenerator::ComputeCallInitialize(arg_count,
                                                           NOT_IN_LOOP);
    __ Call(ic, RelocInfo::CODE_TARGET);
    // Restore context register.
    __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
  } else {
    // Call the C runtime function.
    __ CallRuntime(expr->function(), arg_count);
  }
  Apply(context_, v0);
}


void FullCodeGenerator::VisitUnaryOperation(UnaryOperation* expr) {
  switch (expr->op()) {
    case Token::DELETE: {
      Comment cmnt(masm_, "[ UnaryOperation (DELETE)");
      Property* prop = expr->expression()->AsProperty();
      Variable* var = expr->expression()->AsVariableProxy()->AsVariable();
      if (prop == NULL && var == NULL) {
        // Result of deleting non-property, non-variable reference is true.
        // The subexpression may have side effects.
        VisitForEffect(expr->expression());
        Apply(context_, true);
      } else if (var != NULL &&
                 !var->is_global() &&
                 var->slot() != NULL &&
                 var->slot()->type() != Slot::LOOKUP) {
        // Result of deleting non-global, non-dynamic variables is false.
        // The subexpression does not have side effects.
        Apply(context_, false);
      } else {
        // Property or variable reference.  Call the delete builtin with
        // object and property name as arguments.
        if (prop != NULL) {
          VisitForValue(prop->obj(), kStack);
          VisitForValue(prop->key(), kStack);
        } else if (var->is_global()) {
          __ lw(a1, CodeGenerator::GlobalObject());
          __ li(a0, Operand(var->name()));
          __ Push(a1, a0);
        } else {
          // Non-global variable.  Call the runtime to look up the context
          // where the variable was introduced.
          __ push(context_register());
          __ li(a2, Operand(var->name()));
          __ push(a2);
          __ CallRuntime(Runtime::kLookupContext, 2);
          __ push(v0);
          __ li(a2, Operand(var->name()));
          __ push(a2);
        }
        __ InvokeBuiltin(Builtins::DELETE, CALL_JS);
        Apply(context_, v0);
      }
      break;
    }

    case Token::VOID: {
      Comment cmnt(masm_, "[ UnaryOperation (VOID)");
      VisitForEffect(expr->expression());
      switch (context_) {
        case Expression::kUninitialized:
          UNREACHABLE();
          break;
        case Expression::kEffect:
          break;
        case Expression::kValue:
          __ LoadRoot(result_register(), Heap::kUndefinedValueRootIndex);
          switch (location_) {
            case kAccumulator:
              break;
            case kStack:
              __ push(result_register());
              break;
          }
          break;
        case Expression::kTestValue:
          // Value is false so it's needed.
          __ LoadRoot(result_register(), Heap::kUndefinedValueRootIndex);
          switch (location_) {
            case kAccumulator:
              break;
            case kStack:
              __ push(result_register());
              break;
          }
          // Fall through.
        case Expression::kTest:
        case Expression::kValueTest:
          __ jmp(false_label_);
          break;
      }
      break;
    }

    case Token::NOT: {
      Comment cmnt(masm_, "[ UnaryOperation (NOT)");
      Label materialize_true, materialize_false;
      Label* if_true = NULL;
      Label* if_false = NULL;

      // Notice that the labels are swapped.
      PrepareTest(&materialize_true, &materialize_false, &if_false, &if_true);

      VisitForControl(expr->expression(), if_true, if_false);

      Apply(context_, if_false, if_true);  // Labels swapped.
      break;
    }

    case Token::TYPEOF: {
      Comment cmnt(masm_, "[ UnaryOperation (TYPEOF)");
      VariableProxy* proxy = expr->expression()->AsVariableProxy();
      if (proxy != NULL &&
          !proxy->var()->is_this() &&
          proxy->var()->is_global()) {
        Comment cmnt(masm_, "Global variable");
        __ lw(a0, CodeGenerator::GlobalObject());
        __ li(a2, Operand(proxy->name()));
        Handle<Code> ic(Builtins::builtin(Builtins::LoadIC_Initialize));
        // Use a regular load, not a contextual load, to avoid a reference
        // error.
        __ Call(ic, RelocInfo::CODE_TARGET);
        __ push(v0);
      } else if (proxy != NULL &&
                 proxy->var()->slot() != NULL &&
                 proxy->var()->slot()->type() == Slot::LOOKUP) {
        __ li(a0, Operand(proxy->name()));
        __ Push(cp, a0);
        __ CallRuntime(Runtime::kLoadContextSlotNoReferenceError, 2);
        __ push(v0);
      } else {
        // This expression cannot throw a reference error at the top level.
        VisitForValue(expr->expression(), kStack);
      }

      __ CallRuntime(Runtime::kTypeof, 1);
      Apply(context_, v0);
      break;
    }

    case Token::ADD: {
      Comment cmt(masm_, "[ UnaryOperation (ADD)");
      VisitForValue(expr->expression(), kAccumulator);
      Label no_conversion;
      __ BranchOnSmi(result_register(), &no_conversion);
      __ mov(a0, result_register());
      __ push(a0);
      __ InvokeBuiltin(Builtins::TO_NUMBER, CALL_JS);
      __ bind(&no_conversion);
      Apply(context_, result_register());
      break;
    }

    case Token::SUB: {
      Comment cmt(masm_, "[ UnaryOperation (SUB)");
      bool overwrite =
          (expr->expression()->AsBinaryOperation() != NULL &&
           expr->expression()->AsBinaryOperation()->ResultOverwriteAllowed());
      GenericUnaryOpStub stub(Token::SUB, overwrite);
      // GenericUnaryOpStub expects the argument to be in the
      // register a0.
      VisitForValue(expr->expression(), kAccumulator);
      __ mov(a0, result_register());
      __ CallStub(&stub);
      Apply(context_, v0);
      break;
    }

    case Token::BIT_NOT: {
      Comment cmt(masm_, "[ UnaryOperation (BIT_NOT)");
      bool overwrite =
          (expr->expression()->AsBinaryOperation() != NULL &&
           expr->expression()->AsBinaryOperation()->ResultOverwriteAllowed());
      GenericUnaryOpStub stub(Token::BIT_NOT, overwrite);
      // GenericUnaryOpStub expects the argument to be in the
      // register a0.
      VisitForValue(expr->expression(), kAccumulator);
      // Avoid calling the stub for Smis.
      Label smi, done;
      __ BranchOnSmi(result_register(), &smi);
      // Non-smi: call stub leaving result in accumulator register.
      __ mov(a0, result_register());
      __ CallStub(&stub);
      __ b(&done);
      // Perform operation directly on Smis.
      __ bind(&smi);
      __ Xor(result_register(), result_register(), 0xfffffffe);
      __ bind(&done);
      Apply(context_, result_register());
      break;
    }

    default:
      UNREACHABLE();
  }
}


void FullCodeGenerator::VisitCountOperation(CountOperation* expr) {
  Comment cmnt(masm_, "[ CountOperation");
  // Invalid left-hand sides are rewritten to have a 'throw ReferenceError'
  // as the left-hand side.
  if (!expr->expression()->IsValidLeftHandSide()) {
    VisitForEffect(expr->expression());
    return;
  }

  // Expression can only be a property, a global or a (parameter or local)
  // slot. Variables with rewrite to .arguments are treated as KEYED_PROPERTY.
  enum LhsKind { VARIABLE, NAMED_PROPERTY, KEYED_PROPERTY };
  LhsKind assign_type = VARIABLE;
  Property* prop = expr->expression()->AsProperty();
  // In case of a property we use the uninitialized expression context
  // of the key to detect a named property.
  if (prop != NULL) {
    assign_type =
        (prop->key()->IsPropertyName()) ? NAMED_PROPERTY : KEYED_PROPERTY;
  }

  // Evaluate expression and get value.
  if (assign_type == VARIABLE) {
    ASSERT(expr->expression()->AsVariableProxy()->var() != NULL);
    Location saved_location = location_;
    location_ = kAccumulator;
    EmitVariableLoad(expr->expression()->AsVariableProxy()->var(),
                     Expression::kValue);
    location_ = saved_location;
  } else {
    // Reserve space for result of postfix operation.
    if (expr->is_postfix() && context_ != Expression::kEffect) {
      __ li(t0, Operand(Smi::FromInt(0)));
      __ push(t0);
    }
    if (assign_type == NAMED_PROPERTY) {
      // Put the object both on the stack and in the accumulator.
      VisitForValue(prop->obj(), kAccumulator);
      __ push(v0);
      EmitNamedPropertyLoad(prop);
    } else {
      VisitForValue(prop->obj(), kStack);
      VisitForValue(prop->key(), kAccumulator);
      __ lw(a1, MemOperand(sp, 0));
      __ push(v0);
      EmitKeyedPropertyLoad(prop);
    }
  }
  // Call ToNumber only if operand is not a smi.
  Label no_conversion;
  __ BranchOnSmi(v0, &no_conversion);
  __ push(v0);
  __ InvokeBuiltin(Builtins::TO_NUMBER, CALL_JS);
  __ bind(&no_conversion);

  // Save result for postfix expressions.
  if (expr->is_postfix()) {
    switch (context_) {
      case Expression::kUninitialized:
        UNREACHABLE();
      case Expression::kEffect:
        // Do not save result.
        break;
      case Expression::kValue:
      case Expression::kTest:
      case Expression::kValueTest:
      case Expression::kTestValue:
        // Save the result on the stack. If we have a named or keyed property
        // we store the result under the receiver that is currently on top
        // of the stack.
        switch (assign_type) {
          case VARIABLE:
            __ push(v0);
            break;
          case NAMED_PROPERTY:
            __ sw(v0, MemOperand(sp, kPointerSize));
            break;
          case KEYED_PROPERTY:
            __ sw(v0, MemOperand(sp, 2 * kPointerSize));
            break;
        }
        break;
    }
  }


  // Inline smi case if we are in a loop.
  Label stub_call, done;
  int count_value = expr->op() == Token::INC ? 1 : -1;
  if (loop_depth() > 0) {
    UNCOMPLETED_MIPS();
    ASSERT(0);
  }
  __ mov(a0, result_register());
  __ li(a1, Operand(Smi::FromInt(count_value)));
  GenericBinaryOpStub stub(Token::ADD, NO_OVERWRITE, a1, a0);
  __ CallStub(&stub);
  __ bind(&done);

  // Store the value returned in v0.
  switch (assign_type) {
    case VARIABLE:
      if (expr->is_postfix()) {
        EmitVariableAssignment(expr->expression()->AsVariableProxy()->var(),
                               Token::ASSIGN,
                               Expression::kEffect);
        // For all contexts except kEffect: We have the result on
        // top of the stack.
        if (context_ != Expression::kEffect) {
          ApplyTOS(context_);
        }
      } else {
        EmitVariableAssignment(expr->expression()->AsVariableProxy()->var(),
                               Token::ASSIGN,
                               context_);
      }
      break;
    case NAMED_PROPERTY: {
      __ mov(a0, result_register());  // Value.
      __ li(a2, Operand(prop->key()->AsLiteral()->handle()));  // Name.
      __ pop(a1);  // Receiver.
      Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
      __ Call(ic, RelocInfo::CODE_TARGET);
      if (expr->is_postfix()) {
        if (context_ != Expression::kEffect) {
          ApplyTOS(context_);
        }
      } else {
        Apply(context_, v0);
      }
      break;
    }
    case KEYED_PROPERTY: {
      __ mov(a0, result_register());  // Value.
      __ pop(a1);  // Key.
      __ pop(a2);  // Receiver.
      Handle<Code> ic(Builtins::builtin(Builtins::KeyedStoreIC_Initialize));
      __ Call(ic, RelocInfo::CODE_TARGET);
      if (expr->is_postfix()) {
        if (context_ != Expression::kEffect) {
          ApplyTOS(context_);
        }
      } else {
        Apply(context_, v0);
      }
      break;
    }
  }
}


void FullCodeGenerator::VisitBinaryOperation(BinaryOperation* expr) {
  Comment cmnt(masm_, "[ BinaryOperation");
  switch (expr->op()) {
    case Token::COMMA:
      VisitForEffect(expr->left());
      Visit(expr->right());
      break;

    case Token::OR:
    case Token::AND:
      EmitLogicalOperation(expr);
      break;

    case Token::ADD:
    case Token::SUB:
    case Token::DIV:
    case Token::MOD:
    case Token::MUL:
    case Token::BIT_OR:
    case Token::BIT_AND:
    case Token::BIT_XOR:
    case Token::SHL:
    case Token::SHR:
    case Token::SAR:
      VisitForValue(expr->left(), kStack);
      VisitForValue(expr->right(), kAccumulator);
      EmitBinaryOp(expr->op(), context_);
      break;

    default:
      UNREACHABLE();
  }
}


void FullCodeGenerator::EmitNullCompare(bool strict,
                                        Register obj,
                                        Register null_const,
                                        Label* if_true,
                                        Label* if_false,
                                        Register scratch) {
  if (strict) {
    __ Branch(if_true, eq, obj, Operand(null_const));
  } else {
    __ Branch(if_true, eq, obj, Operand(null_const));
    __ LoadRoot(t0, Heap::kUndefinedValueRootIndex);
    __ Branch(if_true, eq, obj, Operand(t0));
    __ BranchOnSmi(obj, if_false);
    // It can be an undetectable object.
    __ lw(scratch, FieldMemOperand(obj, HeapObject::kMapOffset));
    __ lbu(scratch, FieldMemOperand(scratch, Map::kBitFieldOffset));
    __ And(scratch, scratch, Operand(1 << Map::kIsUndetectable));
    __ Branch(if_true, ne, scratch, Operand(0));
  }
  __ jmp(if_false);
}


void FullCodeGenerator::VisitCompareOperation(CompareOperation* expr) {
Comment cmnt(masm_, "[ CompareOperation");

  // Always perform the comparison for its control flow.  Pack the result
  // into the expression's context after the comparison is performed.

  Label materialize_true, materialize_false;
  Label* if_true = NULL;
  Label* if_false = NULL;
  PrepareTest(&materialize_true, &materialize_false, &if_true, &if_false);

  VisitForValue(expr->left(), kStack);
  switch (expr->op()) {
    case Token::IN:
      VisitForValue(expr->right(), kStack);
      __ InvokeBuiltin(Builtins::IN, CALL_JS);
      __ LoadRoot(t0, Heap::kTrueValueRootIndex);
      __ Branch(if_true, eq, v0, Operand(t0));
      __ jmp(if_false);
      break;

    case Token::INSTANCEOF: {
      VisitForValue(expr->right(), kStack);
      InstanceofStub stub;
      __ CallStub(&stub);
      __ Branch(if_true, eq, v0, Operand(zero_reg));
      __ jmp(if_false);
      break;
    }

    default: {
      VisitForValue(expr->right(), kAccumulator);
      Condition cc = eq;
      bool strict = false;
      switch (expr->op()) {
        case Token::EQ_STRICT:
          strict = true;
          // Fall through
        case Token::EQ: {
          cc = eq;
          __ mov(a0, result_register());
          __ pop(a1);
          // If either operand is constant null we do a fast compare
          // against null.
          Literal* right_literal = expr->right()->AsLiteral();
          Literal* left_literal = expr->left()->AsLiteral();
          if (right_literal != NULL && right_literal->handle()->IsNull()) {
            EmitNullCompare(strict, a1, a0, if_true, if_false, a2);
            Apply(context_, if_true, if_false);
            return;
          } else if (left_literal != NULL && left_literal->handle()->IsNull()) {
            EmitNullCompare(strict, a0, a1, if_true, if_false, a2);
            Apply(context_, if_true, if_false);
            return;
          }
          break;
        }
        case Token::LT:
          cc = lt;
          __ mov(a0, result_register());
          __ pop(a1);
          break;
        case Token::GT:
          // Reverse left and right sides to obtain ECMA-262 conversion order.
          cc = lt;
          __ mov(a1, result_register());
          __ pop(a0);
         break;
        case Token::LTE:
          // Reverse left and right sides to obtain ECMA-262 conversion order.
          cc = ge;
          __ mov(a1, result_register());
          __ pop(a0);
          break;
        case Token::GTE:
          cc = ge;
          __ mov(a0, result_register());
          __ pop(a1);
          break;
        case Token::IN:
        case Token::INSTANCEOF:
        default:
          UNREACHABLE();
      }

      // The comparison stub expects the smi vs. smi case to be handled
      // before it is called.
      Label slow_case;
      __ Or(a2, a0, Operand(a1));
      __ BranchOnNotSmi(a2, &slow_case);
      __ Branch(if_true, cc, a1, Operand(a0));
      __ jmp(if_false);

      __ bind(&slow_case);
      CompareStub stub(cc, strict, kBothCouldBeNaN, true, a1, a0);
      __ CallStub(&stub);
      __ Branch(if_true, cc, v0, Operand(0));
      __ jmp(if_false);
    }
  }

  // Convert the result of the comparison into one expected for this
  // expression's context.
  Apply(context_, if_true, if_false);
}


void FullCodeGenerator::VisitThisFunction(ThisFunction* expr) {
  __ lw(a0, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
  Apply(context_, a0);
}

Register FullCodeGenerator::result_register() { return v0; }


Register FullCodeGenerator::context_register() { return cp; }


void FullCodeGenerator::StoreToFrameField(int frame_offset, Register value) {
  UNIMPLEMENTED_MIPS();
}


void FullCodeGenerator::LoadContextField(Register dst, int context_index) {
  UNIMPLEMENTED_MIPS();
}

void FullCodeGenerator::EmitInlineSmiBinaryOp(Expression*,
                                              Token::Value,
                                              OverwriteMode,
                                              Expression*,
                                              Expression*,
                                              ConstantOperand) {
  UNIMPLEMENTED_MIPS();
}

void FullCodeGenerator::EmitBinaryOp(Token::Value, OverwriteMode) {
  UNIMPLEMENTED_MIPS();
}

// ----------------------------------------------------------------------------
// Non-local control flow support.

void FullCodeGenerator::EnterFinallyBlock() {
  UNIMPLEMENTED_MIPS();
}


void FullCodeGenerator::ExitFinallyBlock() {
  UNIMPLEMENTED_MIPS();
}

// ----------------------------------------------------------------------------
// This is a quick way to define some functions that are
// currently unimplemented.
#define MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(Name) \
  void FullCodeGenerator::Name(ZoneList<v8::internal::Expression*>*) \
  { UNIMPLEMENTED_MIPS(); }

#define MIPS_UNIMPLEMENTED_FULL_CODEGEN_PLUG(Name) \
  void FullCodeGenerator::Name##Context::Plug(Slot* slot) const \
  { UNIMPLEMENTED_MIPS(); } \
  void FullCodeGenerator::Name##Context::Plug(Heap::RootListIndex) const \
  { UNIMPLEMENTED_MIPS(); } \
  void FullCodeGenerator::Name##Context::Plug(Handle<Object>) const \
  { UNIMPLEMENTED_MIPS(); } \
  void FullCodeGenerator::Name##Context::DropAndPlug(int, Register) const \
  { UNIMPLEMENTED_MIPS(); } \
  void FullCodeGenerator::Name##Context::Plug(Label*, Label*) const \
  { UNIMPLEMENTED_MIPS(); } \
  void FullCodeGenerator::Name##Context::Plug(bool bool_) const \
  { UNIMPLEMENTED_MIPS(); }

MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitIsSmi)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitIsNonNegativeSmi)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitIsObject)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitIsSpecObject)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitIsUndetectableObject)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitIsFunction)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitIsArray)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitIsRegExp)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitIsConstructCall)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitObjectEquals)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitArguments)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitArgumentsLength)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitClassOf)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitLog)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitRandomHeapNumber)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitSubString)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitRegExpExec)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitValueOf)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitSetValueOf)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitNumberToString)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitStringCharFromCode)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitStringCharCodeAt)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitStringCharAt)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitStringAdd)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitStringCompare)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitMathPow)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitMathSin)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitMathCos)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitMathSqrt)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitCallFunction)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitRegExpConstructResult)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitSwapElements)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitGetFromCache)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitIsRegExpEquivalent)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitIsStringWrapperSafeForDefaultValueOf)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitHasCachedArrayIndex)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_FUNC(EmitGetCachedArrayIndex)

MIPS_UNIMPLEMENTED_FULL_CODEGEN_PLUG(Effect)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_PLUG(AccumulatorValue)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_PLUG(StackValue)
MIPS_UNIMPLEMENTED_FULL_CODEGEN_PLUG(Test)


#undef __

} }  // namespace v8::internal

#endif  // V8_TARGET_ARCH_MIPS
