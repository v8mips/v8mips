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

#include "bootstrapper.h"
#include "codegen-inl.h"
#include "compiler.h"
#include "debug.h"
#include "ic-inl.h"
#include "parser.h"
#include "register-allocator-inl.h"
#include "runtime.h"
#include "scopes.h"
#include "virtual-frame-inl.h"


namespace v8 {
namespace internal {

#define __ ACCESS_MASM(masm_)

static void MultiplyByKnownInt(MacroAssembler* masm,
                               Register source,
                               Register destination,
                               int known_int);
static bool IsEasyToMultiplyBy(int x);


// -----------------------------------------------------------------------------
// Platform-specific DeferredCode functions.


void DeferredCode::SaveRegisters() {
  for (int i = 0; i < RegisterAllocator::kNumRegisters; i++) {
    int action = registers_[i];
    if (action == kPush) {
      __ Push(RegisterAllocator::ToRegister(i));
    } else if (action != kIgnore && (action & kSyncedFlag) == 0) {
      __ sw(RegisterAllocator::ToRegister(i), MemOperand(fp, action));
    }
  }
}


void DeferredCode::RestoreRegisters() {
  // Restore registers in reverse order due to the stack.
  for (int i = RegisterAllocator::kNumRegisters - 1; i >= 0; i--) {
    int action = registers_[i];
    if (action == kPush) {
      __ Pop(RegisterAllocator::ToRegister(i));
    } else if (action != kIgnore) {
      action &= ~kSyncedFlag;
      __ lw(RegisterAllocator::ToRegister(i), MemOperand(fp, action));
    }
  }
}


// -----------------------------------------------------------------------------
// CodeGenState implementation.

CodeGenState::CodeGenState(CodeGenerator* owner)
    : owner_(owner),
      true_target_(NULL),
      false_target_(NULL),
      previous_(NULL) {
  owner_->set_state(this);
}


CodeGenState::CodeGenState(CodeGenerator* owner,
                           JumpTarget* true_target,
                           JumpTarget* false_target)
    : owner_(owner),
      true_target_(true_target),
      false_target_(false_target),
      previous_(owner->state()) {
  owner_->set_state(this);
}


CodeGenState::~CodeGenState() {
  ASSERT(owner_->state() == this);
  owner_->set_state(previous_);
}


// -----------------------------------------------------------------------------
// CodeGenerator implementation.

CodeGenerator::CodeGenerator(MacroAssembler* masm)
    : deferred_(8),
      masm_(masm),
      frame_(NULL),
      allocator_(NULL),
      cc_reg_(cc_always),
      state_(NULL),
      function_return_is_shadowed_(false) {
}


// Calling conventions:
// fp: caller's frame pointer
// sp: stack pointer
// a1: called JS function
// cp: callee's context

void CodeGenerator::Generate(CompilationInfo* info) {
  // Record the position for debugging purposes.
  CodeForFunctionPosition(info->function());

  // Initialize state.
  info_ = info;
  ASSERT(allocator_ == NULL);
  RegisterAllocator register_allocator(this);
  allocator_ = &register_allocator;
  ASSERT(frame_ == NULL);
  frame_ = new VirtualFrame();
  cc_reg_ = cc_always;

  {
    CodeGenState state(this);

    // Registers:
    // a1: called JS function
    // ra: return address
    // fp: caller's frame pointer
    // sp: stack pointer
    // cp: callee's context
    //
    // Stack:
    // arguments
    // receiver

    frame_->Enter();

    // Allocate space for locals and initialize them.
    frame_->AllocateStackSlots();

    // Initialize the function return target.
    function_return_.set_direction(JumpTarget::BIDIRECTIONAL);
    function_return_is_shadowed_ = false;

    VirtualFrame::SpilledScope spilled_scope;
    if (scope()->num_heap_slots() > 0) {
      UNIMPLEMENTED_MIPS();
    }

    {
      Comment cmnt2(masm_, "[ copy context parameters into .context");

      // Note that iteration order is relevant here! If we have the same
      // parameter twice (e.g., function (x, y, x)), and that parameter
      // needs to be copied into the context, it must be the last argument
      // passed to the parameter that needs to be copied. This is a rare
      // case so we don't check for it, instead we rely on the copying
      // order: such a parameter is copied repeatedly into the same
      // context location and thus the last value is what is seen inside
      // the function.
      for (int i = 0; i < scope()->num_parameters(); i++) {
        Variable* par = scope()->parameter(i);
        Slot* slot = par->slot();
        if (slot != NULL && slot->type() == Slot::CONTEXT) {
          ASSERT(!scope()->is_global_scope());  // no parameters in global scope
          __ lw(a1, frame_->ParameterAt(i));
          // Loads a2 with context; used below in RecordWrite.
          __ sw(a1, SlotOperand(slot, a2));
          // Load the offset into a3.
          int slot_offset =
              FixedArray::kHeaderSize + slot->index() * kPointerSize;
          __ li(a3, Operand(slot_offset));
          __ RecordWrite(a2, a3, a1);
        }
      }
    }

    // Store the arguments object.  This must happen after context
    // initialization because the arguments object may be stored in the
    // context.
    if (scope()->arguments() != NULL) {
      UNIMPLEMENTED_MIPS();
    }

    // Generate code to 'execute' declarations and initialize functions
    // (source elements). In case of an illegal redeclaration we need to
    // handle that instead of processing the declarations.
    if (scope()->HasIllegalRedeclaration()) {
      Comment cmnt(masm_, "[ illegal redeclarations");
      scope()->VisitIllegalRedeclaration(this);
    } else {
      Comment cmnt(masm_, "[ declarations");
      ProcessDeclarations(scope()->declarations());
      // Bail out if a stack-overflow exception occurred when processing
      // declarations.
      if (HasStackOverflow()) return;
    }

    if (FLAG_trace) {
      UNIMPLEMENTED_MIPS();
    }

    // Compile the body of the function in a vanilla state. Don't
    // bother compiling all the code if the scope has an illegal
    // redeclaration.
    if (!scope()->HasIllegalRedeclaration()) {
      Comment cmnt(masm_, "[ function body");
#ifdef DEBUG
      bool is_builtin = Bootstrapper::IsActive();
      bool should_trace =
          is_builtin ? FLAG_trace_builtin_calls : FLAG_trace_calls;
      if (should_trace) {
        UNIMPLEMENTED_MIPS();
      }
#endif
      VisitStatementsAndSpill(info->function()->body());
    }
  }

  if (has_valid_frame() || function_return_.is_linked()) {
    if (!function_return_.is_linked()) {
      CodeForReturnPosition(info->function());
    }
    // Registers:
    // v0: result
    // sp: stack pointer
    // fp: frame pointer
    // cp: callee's context

    __ LoadRoot(v0, Heap::kUndefinedValueRootIndex);

    function_return_.Bind();
    if (FLAG_trace) {
      UNIMPLEMENTED_MIPS();
    }

    // Add a label for checking the size of the code used for returning.
    Label check_exit_codesize;
    masm_->bind(&check_exit_codesize);

    masm_->mov(sp, fp);
    masm_->lw(fp, MemOperand(sp, 0));
    masm_->lw(ra, MemOperand(sp, 4));
    masm_->addiu(sp, sp, 8);

    // Here we use masm_-> instead of the __ macro to avoid the code coverage
    // tool from instrumenting as we rely on the code size here.
    // TODO(MIPS): Should we be able to use more than 0x1ffe parameters?
    masm_->addiu(sp, sp, (scope()->num_parameters() + 1) * kPointerSize);
    masm_->Jump(ra);
    // The Jump automatically generates a nop in the branch delay slot.

    // Check that the size of the code used for returning matches what is
    // expected by the debugger.
    ASSERT_EQ(kJSReturnSequenceLength,
              masm_->InstructionsGeneratedSince(&check_exit_codesize));
  }

  // Code generation state must be reset.
  ASSERT(!has_cc());
  ASSERT(state_ == NULL);
  ASSERT(!function_return_is_shadowed_);
  function_return_.Unuse();
  DeleteFrame();

  // Process any deferred code using the register allocator.
  if (!HasStackOverflow()) {
    ProcessDeferred();
  }

  allocator_ = NULL;
}


void CodeGenerator::LoadReference(Reference* ref) {
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ LoadReference");
  Expression* e = ref->expression();
  Property* property = e->AsProperty();
  Variable* var = e->AsVariableProxy()->AsVariable();

  if (property != NULL) {
    // The expression is either a property or a variable proxy that rewrites
    // to a property.
    LoadAndSpill(property->obj());
    if (property->key()->IsPropertyName()) {
      ref->set_type(Reference::NAMED);
    } else {
      LoadAndSpill(property->key());
      ref->set_type(Reference::KEYED);
    }
  } else if (var != NULL) {
    // The expression is a variable proxy that does not rewrite to a
    // property.  Global variables are treated as named property references.
    if (var->is_global()) {
      LoadGlobal();
      ref->set_type(Reference::NAMED);
    } else {
      ASSERT(var->slot() != NULL);
      ref->set_type(Reference::SLOT);
    }
  } else {
    UNIMPLEMENTED_MIPS();
  }
}


void CodeGenerator::UnloadReference(Reference* ref) {
  VirtualFrame::SpilledScope spilled_scope;
  // Pop a reference from the stack while preserving TOS.
  Comment cmnt(masm_, "[ UnloadReference");
  int size = ref->size();
  if (size > 0) {
    frame_->EmitPop(a0);
    frame_->Drop(size);
    frame_->EmitPush(a0);
  }
  ref->set_unloaded();
}


MemOperand CodeGenerator::SlotOperand(Slot* slot, Register tmp) {
  // Currently, this assertion will fail if we try to assign to
  // a constant variable that is constant because it is read-only
  // (such as the variable referring to a named function expression).
  // We need to implement assignments to read-only variables.
  // Ideally, we should do this during AST generation (by converting
  // such assignments into expression statements); however, in general
  // we may not be able to make the decision until past AST generation,
  // that is when the entire program is known.
  ASSERT(slot != NULL);
  int index = slot->index();
  switch (slot->type()) {
    case Slot::PARAMETER:
      return frame_->ParameterAt(index);

    case Slot::LOCAL:
      return frame_->LocalAt(index);

    case Slot::CONTEXT: {
      ASSERT(!tmp.is(cp));  // do not overwrite context register
      Register context = cp;
      int chain_length = scope()->ContextChainLength(slot->var()->scope());
      for (int i = 0; i < chain_length; i++) {
        // Load the closure.
        // (All contexts, even 'with' contexts, have a closure,
        // and it is the same for all contexts inside a function.
        // There is no need to go to the function context first.)
//        __ ldr(tmp, ContextOperand(context, Context::CLOSURE_INDEX));
        __ lw(tmp, ContextOperand(context, Context::CLOSURE_INDEX));
        // Load the function context (which is the incoming, outer context).
        __ lw(tmp, FieldMemOperand(tmp, JSFunction::kContextOffset));
        context = tmp;
      }
      // We may have a 'with' context now. Get the function context.
      // (In fact this mov may never be the needed, since the scope analysis
      // may not permit a direct context access in this case and thus we are
      // always at a function context. However it is safe to dereference be-
      // cause the function context of a function context is itself. Before
      // deleting this mov we should try to create a counter-example first,
      // though...)
      __ lw(tmp, ContextOperand(context, Context::FCONTEXT_INDEX));
      return ContextOperand(tmp, index);
    }

    default:
      UNREACHABLE();
      return MemOperand(no_reg, 0);
  }
}


// Loads a value on TOS. If it is a boolean value, the result may have been
// (partially) translated into branches, or it may have set the condition
// code register. If force_cc is set, the value is forced to set the
// condition code register and no value is pushed. If the condition code
// register was set, has_cc() is true and cc_reg_ contains the condition to
// test for 'true'.
void CodeGenerator::LoadCondition(Expression* x,
                                  JumpTarget* true_target,
                                  JumpTarget* false_target,
                                  bool force_cc) {
  ASSERT(!has_cc());
  int original_height = frame_->height();

  { CodeGenState new_state(this, true_target, false_target);
    Visit(x);

    // If we hit a stack overflow, we may not have actually visited
    // the expression. In that case, we ensure that we have a
    // valid-looking frame state because we will continue to generate
    // code as we unwind the C++ stack.
    //
    // It's possible to have both a stack overflow and a valid frame
    // state (eg, a subexpression overflowed, visiting it returned
    // with a dummied frame state, and visiting this expression
    // returned with a normal-looking state).
    if (HasStackOverflow() &&
        has_valid_frame() &&
        !has_cc() &&
        frame_->height() == original_height) {
      true_target->Jump();
    }
  }
  if (force_cc && frame_ != NULL && !has_cc()) {
    // Convert the TOS value to a boolean in the condition code register.
    ToBoolean(true_target, false_target);
  }
  ASSERT(!force_cc || !has_valid_frame() || has_cc());
  ASSERT(!has_valid_frame() ||
         (has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


void CodeGenerator::Load(Expression* x) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  JumpTarget true_target;
  JumpTarget false_target;
  LoadCondition(x, &true_target, &false_target, false);

  if (has_cc()) {
    // Convert cc_reg_ into a boolean value.
    JumpTarget loaded;
    JumpTarget materialize_true;

    materialize_true.Branch(cc_reg_);
    __ LoadRoot(t0, Heap::kFalseValueRootIndex);
    loaded.Jump();
    materialize_true.Bind();
    __ LoadRoot(t0, Heap::kTrueValueRootIndex);
    loaded.Bind();
    frame_->EmitPush(t0);
    cc_reg_ = cc_always;
  }

  if (true_target.is_linked() || false_target.is_linked()) {
    UNIMPLEMENTED_MIPS();
  }
  ASSERT(has_valid_frame());
  ASSERT(!has_cc());
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::LoadGlobal() {
  VirtualFrame::SpilledScope spilled_scope;
  __ lw(a0, GlobalObject());
  frame_->EmitPush(a0);
}


void CodeGenerator::LoadTypeofExpression(Expression* x) {
  // Special handling of identifiers as subxessions of typeof.
  VirtualFrame::SpilledScope spilled_scope;
  Variable* variable = x->AsVariableProxy()->AsVariable();
  if (variable != NULL && !variable->is_this() && variable->is_global()) {
    // For a global variable we build the property reference
    // <global>.<variable> and perform a (regular non-contextual) property
    // load to make sure we do not get reference errors.
    Slot global(variable, Slot::CONTEXT, Context::GLOBAL_INDEX);
    Literal key(variable->name());
    Property property(&global, &key, RelocInfo::kNoPosition);
    Reference ref(this, &property);
    ref.GetValueAndSpill();
  } else if (variable != NULL && variable->slot() != NULL) {
    // For a variable that rewrites to a slot, we signal it is the immediate
    // subxession of a typeof.
    LoadFromSlot(variable->slot(), INSIDE_TYPEOF);
    frame_->SpillAll();
  } else {
    // Anything else can be handled normally.
    LoadAndSpill(x);
  }
}


void CodeGenerator::LoadFromSlot(Slot* slot, TypeofState typeof_state) {
  VirtualFrame::SpilledScope spilled_scope;
  if (slot->type() == Slot::LOOKUP) {
    UNIMPLEMENTED_MIPS();
  } else {
    __ lw(a0, SlotOperand(slot, a2));
    frame_->EmitPush(a0);
    if (slot->var()->mode() == Variable::CONST) {
      UNIMPLEMENTED_MIPS();
    }
  }
}


void CodeGenerator::StoreToSlot(Slot* slot, InitState init_state) {
  ASSERT(slot != NULL);
  if (slot->type() == Slot::LOOKUP) {
      UNIMPLEMENTED_MIPS();
  } else {
    ASSERT(!slot->var()->is_dynamic());

    JumpTarget exit;
    if (init_state == CONST_INIT) {
      UNIMPLEMENTED_MIPS();
    }

    // We must execute the store. Storing a variable must keep the
    // (new) value on the stack. This is necessary for compiling
    // assignment expressions.
    //
    // Note: We will reach here even with slot->var()->mode() ==
    // Variable::CONST because of const declarations which will
    // initialize consts to 'the hole' value and by doing so, end up
    // calling this code. a2 may be loaded with context; used below in
    // RecordWrite.
    frame_->EmitPop(a0);
    __ sw(a0, SlotOperand(slot, a2));
    frame_->EmitPush(a0);
    if (slot->type() == Slot::CONTEXT) {
      UNIMPLEMENTED_MIPS();
    }
    // If we definitely did not jump over the assignment, we do not need
    // to bind the exit label. Doing so can defeat peephole
    // optimization.
    if (init_state == CONST_INIT || slot->type() == Slot::CONTEXT) {
      exit.Bind();
    }
  }
}


// ECMA-262, section 9.2, page 30: ToBoolean(). Convert the given
// register to a boolean in the condition code register. The code
// may jump to 'false_target' in case the register converts to 'false'.
void CodeGenerator::ToBoolean(JumpTarget* true_target,
                              JumpTarget* false_target) {
  VirtualFrame::SpilledScope spilled_scope;
  // Note: The generated code snippet does not change stack variables.
  //       Only the condition code should be set.
  frame_->EmitPop(t0);

  // Fast case checks

  // Check if the value is 'false'.
  __ LoadRoot(t1, Heap::kFalseValueRootIndex);
  false_target->Branch(eq, t0, Operand(t1), no_hint);

  // Check if the value is 'true'.
  __ LoadRoot(t2, Heap::kTrueValueRootIndex);
  true_target->Branch(eq, t0, Operand(t2), no_hint);

  // Check if the value is 'undefined'.
  __ LoadRoot(t3, Heap::kUndefinedValueRootIndex);
  false_target->Branch(eq, t0, Operand(t3), no_hint);

  // Check if the value is a smi.
  false_target->Branch(eq, t0, Operand(Smi::FromInt(0)), no_hint);
  __ And(t4, t0, Operand(kSmiTagMask));
  true_target->Branch(eq, t4, Operand(zero_reg), no_hint);

  // Slow case: call the runtime.
  frame_->EmitPush(t0);
  frame_->CallRuntime(Runtime::kToBool, 1);
  // Convert the result (v0) to a condition code.
  __ LoadRoot(condReg1, Heap::kFalseValueRootIndex);
  __ mov(condReg2, v0);

  cc_reg_ = ne;
}


void CodeGenerator::GenericBinaryOperation(Token::Value op,
                                           OverwriteMode overwrite_mode,
                                           int constant_rhs) {
  VirtualFrame::SpilledScope spilled_scope;
  // sp[0] : y
  // sp[1] : x
  // result : v0

  // Stub is entered with a call: 'return address' is in lr.
  switch (op) {
    case Token::ADD:  // Fall through.
    case Token::SUB:  // Fall through.
    case Token::MUL:
    case Token::DIV:
    case Token::MOD:
    case Token::BIT_OR:
    case Token::BIT_AND:
    case Token::BIT_XOR:
    case Token::SHL:
    case Token::SHR:
    case Token::SAR: {
      frame_->EmitPop(a0);  // a0 : y
      frame_->EmitPop(a1);  // a1 : x
      GenericBinaryOpStub stub(op, overwrite_mode, constant_rhs);
      frame_->CallStub(&stub, 0);
      break;
    }

    case Token::COMMA:
      frame_->EmitPop(v0);
      // Simply discard left value.
      frame_->Drop();
      break;

    default:
      // Other cases should have been handled before this point.
      UNREACHABLE();
      break;
  }
}


class DeferredInlineSmiOperation: public DeferredCode {
 public:
  DeferredInlineSmiOperation(Token::Value op,
                             int value,
                             bool reversed,
                             OverwriteMode overwrite_mode)
      : op_(op),
        value_(value),
        reversed_(reversed),
        overwrite_mode_(overwrite_mode) {
    set_comment("[ DeferredInlinedSmiOperation");
  }

  virtual void Generate();

 private:
  Token::Value op_;
  int value_;
  bool reversed_;
  OverwriteMode overwrite_mode_;
};


void DeferredInlineSmiOperation::Generate() {
  // In CodeGenerator::SmiOperation we used a1 instead of a0, and we left the
  // register untouched.
  // We just need to load value_ and switch if necessary.
  switch (op_) {
    case Token::ADD:
    case Token::SUB:
    case Token::MUL:
    case Token::MOD:
    case Token::BIT_OR:
    case Token::BIT_XOR:
    case Token::BIT_AND: {
      if (reversed_) {
        __ mov(a0, a1);
        __ li(a1, Operand(Smi::FromInt(value_)));
      } else {
        __ li(a0, Operand(Smi::FromInt(value_)));
      }
      break;
    }
    case Token::SHL:
    case Token::SHR:
    case Token::SAR: {
      if (!reversed_) {
        __ li(a0, Operand(Smi::FromInt(value_)));
      } else {
        UNREACHABLE();  // Should have been handled in SmiOperation.
      }
      break;
    }

    default:
      // Other cases should have been handled before this point.
      UNREACHABLE();
      break;
  }

  GenericBinaryOpStub stub(op_, overwrite_mode_, value_);
  __ CallStub(&stub);
}


static bool PopCountLessThanEqual2(unsigned int x) {
  x &= x - 1;
  return (x & (x - 1)) == 0;
}


// Returns the index of the lowest bit set.
static int BitPosition(unsigned x) {
  int bit_posn = 0;
  while ((x & 0xf) == 0) {
    bit_posn += 4;
    x >>= 4;
  }
  while ((x & 1) == 0) {
    bit_posn++;
    x >>= 1;
  }
  return bit_posn;
}


void CodeGenerator::SmiOperation(Token::Value op,
                                 Handle<Object> value,
                                 bool reversed,
                                 OverwriteMode mode) {

  VirtualFrame::SpilledScope spilled_scope;
  // NOTE: This is an attempt to inline (a bit) more of the code for
  // some possible smi operations (like + and -) when (at least) one
  // of the operands is a literal smi. With this optimization, the
  // performance of the system is increased by ~15%, and the generated
  // code size is increased by ~1% (measured on a combination of
  // different benchmarks).

  // We care about keeping a1 unchanged, as it spares the need to reverse the
  // optimistic operation if we need to jump to the deferred code.

  // sp[0] : operand

  // TODO(MIPS.1): Implement overflow check.

//  __ break_(0x04008);
  int int_value = Smi::cast(*value)->value();

  JumpTarget exit;
  // We use a1 instead of a0 because in most cases we will need the value in a1
  // if we jump to the deferred code.
  frame_->EmitPop(a1);

  bool something_to_inline = true;
  switch (op) {
    // TODO(MIPS.1): Implement overflow cases in CodeGenerator::SmiOperation.
    case Token::ADD: {
      DeferredCode* deferred =
          new DeferredInlineSmiOperation(op, int_value, reversed, mode);

      __ Addu(v0, a1, Operand(value));
//      deferred->Branch(vs);           // plind: overflow case.
      __ And(t0, v0, Operand(kSmiTagMask));
      deferred->Branch(ne, t0, Operand(zero_reg));
      deferred->BindExit();
      break;
    }

    case Token::SUB: {
      DeferredCode* deferred =
          new DeferredInlineSmiOperation(op, int_value, reversed, mode);

      if (reversed) {
        __ li(t0, Operand(value));
        __ Subu(v0, t0, Operand(a1));
      } else {
        __ li(t0, Operand(value));
        __ Subu(v0, a1, Operand(t0));
      }
//      deferred->Branch(vs);            // plind: overflow case
      __ And(t0, v0, Operand(kSmiTagMask));
      deferred->Branch(ne, t0, Operand(zero_reg));
      deferred->BindExit();
      break;
    }


    case Token::BIT_OR:
    case Token::BIT_XOR:
    case Token::BIT_AND: {
      DeferredCode* deferred =
        new DeferredInlineSmiOperation(op, int_value, reversed, mode);
      __ And(t0, a1, Operand(kSmiTagMask));
      deferred->Branch(ne, t0, Operand(zero_reg));
      deferred->BindExit();
      switch (op) {
        case Token::BIT_OR:  __ Or(v0, a1, Operand(value)); break;
        case Token::BIT_XOR: __ Xor(v0, a1, Operand(value)); break;
        case Token::BIT_AND: __ And(v0, a1, Operand(value)); break;
        default: UNREACHABLE();
      }
      deferred->BindExit();
      break;
    }

    case Token::SHL:
    case Token::SHR:
    case Token::SAR: {
      if (reversed) {
        something_to_inline = false;
        break;
      }
      int shift_value = int_value & 0x1f;  // Least significant 5 bits.
      DeferredCode* deferred =
        new DeferredInlineSmiOperation(op, shift_value, false, mode);
      __ And(t0, a1, Operand(kSmiTagMask));
      deferred->Branch(ne, t0, Operand(zero_reg));
      __ sra(a2, a1, kSmiTagSize);  // Remove tag.
      switch (op) {
        case Token::SHL: {
          if (shift_value != 0) {
            __ sll(v0, a2, shift_value);
          }
          // Check that the result fits in a Smi.
          __ Addu(t3, v0, Operand(0x40000000));
          __ And(t3, t3, Operand(0x80000000));
          deferred->Branch(ne, t3, Operand(zero_reg));
          break;
        }
        case Token::SHR: {
          // LSR by immediate 0 means shifting 32 bits.
          if (shift_value != 0) {
            __ srl(v0, a2, shift_value);
          }
          // Check that the *unsigned* result fits in a smi.
          // Neither of the two high-order bits can be set:
          // - 0x80000000: high bit would be lost when smi tagging
          // - 0x40000000: this number would convert to negative when
          // Smi tagging these two cases can only happen with shifts
          // by 0 or 1 when handed a valid smi.
          // Check that the result fits in a Smi.
          __ And(t3, v0, Operand(0xc0000000));
          deferred->Branch(ne, t3, Operand(zero_reg));
          break;
        }
        case Token::SAR: {
          if (shift_value != 0) {
            // ASR by immediate 0 means shifting 32 bits.
            __ sra(v0, a2, shift_value);
          }
          break;
        }
        default: UNREACHABLE();
      }
      __ sll(v0, v0, kSmiTagSize);  // Tag result.
      deferred->BindExit();
      break;
    }

    case Token::MOD: {
      if (reversed || int_value < 2 || !IsPowerOf2(int_value)) {
        something_to_inline = false;
        break;
      }
      DeferredCode* deferred =
        new DeferredInlineSmiOperation(op, int_value, reversed, mode);
      unsigned mask = (0x80000000u | kSmiTagMask);
      __ And(t0, a1, Operand(mask));
      // Go to deferred code on non-Smis and negative.
      deferred->Branch(ne, t0, Operand(zero_reg));
      mask = (int_value << kSmiTagSize) - 1;
      __ And(v0, a1, Operand(mask));
      deferred->BindExit();
      break;
    }

    case Token::MUL: {
      if (!IsEasyToMultiplyBy(int_value)) {
        something_to_inline = false;
        break;
      }
      DeferredCode* deferred =
        new DeferredInlineSmiOperation(op, int_value, reversed, mode);
      unsigned max_smi_that_wont_overflow = Smi::kMaxValue / int_value;
      max_smi_that_wont_overflow <<= kSmiTagSize;
      unsigned mask = 0x80000000u;
      while ((mask & max_smi_that_wont_overflow) == 0) {
        mask |= mask >> 1;
      }
      mask |= kSmiTagMask;
      // This does a single mask that checks for a too high value in a
      // conservative way and for a non-Smi.  It also filters out negative
      // numbers, unfortunately, but since this code is inline we prefer
      // brevity to comprehensiveness.
      __ And(t0, a1, Operand(mask));
      deferred->Branch(ne, t0, Operand(zero_reg));
      MultiplyByKnownInt(masm_, a1, v0, int_value);
      deferred->BindExit();
      break;
    }

    default:
      something_to_inline = false;
      break;
  }

  if (!something_to_inline) {
    if (!reversed) {
      __ li(a1, Operand(value));
      frame_->EmitMultiPush(a0.bit() | a1.bit());
      GenericBinaryOperation(op, mode, int_value);
    } else {
      __ li(a1, Operand(value));
      frame_->EmitMultiPushReversed(a1.bit() | a0.bit());
      GenericBinaryOperation(op, mode, kUnknownIntValue);
    }
  }

  exit.Bind();
}


// On MIPS we load registers condReg1 and condReg2 with the values which should
// be compared. With the CodeGenerator::cc_reg_ condition, functions will be
// able to evaluate correctly the condition. (eg CodeGenerator::Branch)
void CodeGenerator::Comparison(Condition cc,
                               Expression* left,
                               Expression* right,
                               bool strict) {
  if (left != NULL) LoadAndSpill(left);
  if (right != NULL) LoadAndSpill(right);

  VirtualFrame::SpilledScope spilled_scope;
  // sp[0] : y  (right)
  // sp[1] : x  (left)

  // Strict only makes sense for equality comparisons.
  ASSERT(!strict || cc == eq);

  JumpTarget exit;
  JumpTarget smi;
  // Implement '>' and '<=' by reversal to obtain ECMA-262 conversion order.
  if (cc == greater || cc == less_equal) {
    cc = ReverseCondition(cc);
    frame_->EmitPop(a0);
    frame_->EmitPop(a1);
  } else {
    frame_->EmitPop(a1);
    frame_->EmitPop(a0);
  }
  __ Or(t0, a0, a1);
  __ And(t1, t0, kSmiTagMask);
  smi.Branch(eq, t1, Operand(zero_reg), no_hint);

  // Perform non-smi comparison by stub.
  // CompareStub takes arguments in a0 and a1, returns <0, >0 or 0 in v0.
  // We call with 0 args because there are 0 on the stack.
  UNIMPLEMENTED_MIPS();
  // This is not implemented on MIPS yet. Break.
  __ break_(0x504);
  exit.Jump();

  // Do smi comparison.
  smi.Bind();
  __ mov(condReg1, a0);
  __ mov(condReg2, a1);

  exit.Bind();
  cc_reg_ = cc;
}


void CodeGenerator::VisitStatements(ZoneList<Statement*>* statements) {
  VirtualFrame::SpilledScope spilled_scope;
  for (int i = 0; frame_ != NULL && i < statements->length(); i++) {
    VisitAndSpill(statements->at(i));
  }
}


void CodeGenerator::Branch(bool if_true, JumpTarget* target) {
  VirtualFrame::SpilledScope spilled_scope;
  ASSERT(has_cc());
  Condition cc = if_true ? cc_reg_ : NegateCondition(cc_reg_);
  target->Branch(cc, condReg1, Operand(condReg2), no_hint);
  cc_reg_ = cc_always;
}


void CodeGenerator::CheckStack() {
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ check stack");

  __ LoadRoot(t0, Heap::kStackLimitRootIndex);
  StackCheckStub stub;
  // Call the stub if lower.
  __ Call(Operand(reinterpret_cast<intptr_t>(stub.GetCode().location()),
          RelocInfo::CODE_TARGET),
          Uless, sp, Operand(t0));
}


void CodeGenerator::VisitBlock(Block* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ Block");
  CodeForStatementPosition(node);
  node->break_target()->set_direction(JumpTarget::FORWARD_ONLY);
  VisitStatementsAndSpill(node->statements());
  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  node->break_target()->Unuse();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::DeclareGlobals(Handle<FixedArray> pairs) {
  VirtualFrame::SpilledScope spilled_scope;
  frame_->EmitPush(cp);
  __ li(t0, Operand(pairs));
  frame_->EmitPush(t0);
  __ li(t0, Operand(Smi::FromInt(is_eval() ? 1 : 0)));
  frame_->EmitPush(t0);
  frame_->CallRuntime(Runtime::kDeclareGlobals, 3);
  // The result is discarded.
}


void CodeGenerator::VisitDeclaration(Declaration* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ Declaration");
  Variable* var = node->proxy()->var();
  ASSERT(var != NULL);  // Must have been resolved.
  Slot* slot = var->slot();

  // If it was not possible to allocate the variable at compile time,
  // we need to "declare" it at runtime to make sure it actually
  // exists in the local context.
  if (slot != NULL && slot->type() == Slot::LOOKUP) {
    UNIMPLEMENTED_MIPS();
    return;
  }

  ASSERT(!var->is_global());

  // If we have a function or a constant, we need to initialize the variable.
  Expression* val = NULL;
  if (node->mode() == Variable::CONST) {
    UNIMPLEMENTED_MIPS();
  } else {
    val = node->fun();  // NULL if we don't have a function.
  }

  if (val != NULL) {
    {
      // Set initial value.
      Reference target(this, node->proxy());
      LoadAndSpill(val);
      target.SetValue(NOT_CONST_INIT);
      // The reference is removed from the stack (preserving TOS) when
      // it goes out of scope.
    }
    // Get rid of the assigned value (declarations are statements).
    frame_->Drop();
  }
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitExpressionStatement(ExpressionStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ ExpressionStatement");
  CodeForStatementPosition(node);
  Expression* expression = node->expression();
  expression->MarkAsStatement();
  LoadAndSpill(expression);
  frame_->Drop();
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitEmptyStatement(EmptyStatement* node) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::VisitIfStatement(IfStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ IfStatement");
  // Generate different code depending on which parts of the if statement
  // are present or not.
  bool has_then_stm = node->HasThenStatement();
  bool has_else_stm = node->HasElseStatement();

  CodeForStatementPosition(node);

  JumpTarget exit;
  if (has_then_stm && has_else_stm) {
    Comment cmnt(masm_, "[ IfThenElse");
    JumpTarget then;
    JumpTarget else_;
    // if (cond)
    LoadConditionAndSpill(node->condition(),
                          &then, &else_, true);
    if (frame_ != NULL) {
      Branch(false, &else_);
    }
    // then
    if (frame_ != NULL || then.is_linked()) {
      then.Bind();
      VisitAndSpill(node->then_statement());
    }
    if (frame_ != NULL) {
      exit.Jump();
    }
    // else
    if (else_.is_linked()) {
      else_.Bind();
      VisitAndSpill(node->else_statement());
    }

  } else if (has_then_stm) {
    Comment cmnt(masm_, "[ IfThen");
    ASSERT(!has_else_stm);
    JumpTarget then;
    // if (cond)
    LoadConditionAndSpill(node->condition(),
                          &then, &exit, true);
    if (frame_ != NULL) {
      Branch(false, &exit);
    }
    // then
    if (frame_ != NULL || then.is_linked()) {
      then.Bind();
      VisitAndSpill(node->then_statement());
    }

  } else if (has_else_stm) {
    Comment cmnt(masm_, "[ IfElse");
    ASSERT(!has_then_stm);
    JumpTarget else_;
    // if (!cond)
    LoadConditionAndSpill(node->condition(),
                          &exit, &else_, true);
    if (frame_ != NULL) {
      Branch(true, &exit);
    }
    // else
    if (frame_ != NULL || else_.is_linked()) {
      else_.Bind();
      VisitAndSpill(node->else_statement());
    }

  } else {
    Comment cmnt(masm_, "[ If");
    ASSERT(!has_then_stm && !has_else_stm);
    // if (cond)
    LoadConditionAndSpill(node->condition(),
                          &exit, &exit, false);
    if (frame_ != NULL) {
      if (has_cc()) {
        cc_reg_ = cc_always;
      } else {
        frame_->Drop();
      }
    }
  }

  // end
  if (exit.is_linked()) {
    exit.Bind();
  }
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitContinueStatement(ContinueStatement* node) {
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ ContinueStatement");
  CodeForStatementPosition(node);
  node->target()->continue_target()->Jump();
}


void CodeGenerator::VisitBreakStatement(BreakStatement* node) {
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ BreakStatement");
  CodeForStatementPosition(node);
  node->target()->break_target()->Jump();
}


void CodeGenerator::VisitReturnStatement(ReturnStatement* node) {
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ ReturnStatement");

  CodeForStatementPosition(node);
  LoadAndSpill(node->expression());
  if (function_return_is_shadowed_) {
    frame_->EmitPop(v0);
    function_return_.Jump();
  } else {
    // Pop the result from the frame and prepare the frame for
    // returning thus making it easier to merge.
    frame_->EmitPop(v0);
    frame_->PrepareForReturn();

    function_return_.Jump();
  }
}


void CodeGenerator::VisitWithEnterStatement(WithEnterStatement* node) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::VisitWithExitStatement(WithExitStatement* node) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::VisitSwitchStatement(SwitchStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ SwitchStatement");
  CodeForStatementPosition(node);
  node->break_target()->set_direction(JumpTarget::FORWARD_ONLY);

  LoadAndSpill(node->tag());

  JumpTarget next_test;
  JumpTarget fall_through;
  JumpTarget default_entry;
  JumpTarget default_exit(JumpTarget::BIDIRECTIONAL);
  ZoneList<CaseClause*>* cases = node->cases();
  int length = cases->length();
  CaseClause* default_clause = NULL;

  for (int i = 0; i < length; i++) {
    CaseClause* clause = cases->at(i);
    if (clause->is_default()) {
      // Remember the default clause and compile it at the end.
      default_clause = clause;
      continue;
    }

    Comment cmnt(masm_, "[ Case clause");
    // Compile the test.
    next_test.Bind();
    next_test.Unuse();
    // Duplicate TOS.
    __ lw(t0, frame_->Top());
    frame_->EmitPush(t0);
    Comparison(eq, NULL, clause->label(), true);
    Branch(false, &next_test);

    // Before entering the body from the test, remove the switch value from
    // the stack.
    frame_->Drop();

    // Label the body so that fall through is enabled.
    if (i > 0 && cases->at(i - 1)->is_default()) {
      default_exit.Bind();
    } else {
      fall_through.Bind();
      fall_through.Unuse();
    }
    VisitStatementsAndSpill(clause->statements());

    // If control flow can fall through from the body, jump to the next body
    // or the end of the statement.
    if (frame_ != NULL) {
      if (i < length - 1 && cases->at(i + 1)->is_default()) {
        default_entry.Jump();
      } else {
        fall_through.Jump();
      }
    }
  }

  // The final "test" removes the switch value.
  next_test.Bind();
  frame_->Drop();

  // If there is a default clause, compile it.
  if (default_clause != NULL) {
    Comment cmnt(masm_, "[ Default clause");
    default_entry.Bind();
    VisitStatementsAndSpill(default_clause->statements());
    // If control flow can fall out of the default and there is a case after
    // it, jup to that case's body.
    if (frame_ != NULL && default_exit.is_bound()) {
      default_exit.Jump();
    }
  }

  if (fall_through.is_linked()) {
    fall_through.Bind();
  }

  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  node->break_target()->Unuse();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitDoWhileStatement(DoWhileStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ DoWhileStatement");
  CodeForStatementPosition(node);
  node->break_target()->set_direction(JumpTarget::FORWARD_ONLY);
  JumpTarget body(JumpTarget::BIDIRECTIONAL);

  // Label the top of the loop for the backward CFG edge.  If the test
  // is always true we can use the continue target, and if the test is
  // always false there is no need.
  ConditionAnalysis info = AnalyzeCondition(node->cond());
  switch (info) {
    case ALWAYS_TRUE:
      node->continue_target()->set_direction(JumpTarget::BIDIRECTIONAL);
      node->continue_target()->Bind();
      break;
    case ALWAYS_FALSE:
      node->continue_target()->set_direction(JumpTarget::FORWARD_ONLY);
      break;
    case DONT_KNOW:
      node->continue_target()->set_direction(JumpTarget::FORWARD_ONLY);
      body.Bind();
      break;
  }

  CheckStack();  // TODO(1222600): ignore if body contains calls.
  VisitAndSpill(node->body());

      // Compile the test.
  switch (info) {
    case ALWAYS_TRUE:
      // If control can fall off the end of the body, jump back to the
      // top.
      if (has_valid_frame()) {
        node->continue_target()->Jump();
      }
      break;
    case ALWAYS_FALSE:
      // If we have a continue in the body, we only have to bind its
      // jump target.
      if (node->continue_target()->is_linked()) {
        node->continue_target()->Bind();
      }
      break;
    case DONT_KNOW:
      // We have to compile the test expression if it can be reached by
      // control flow falling out of the body or via continue.
      if (node->continue_target()->is_linked()) {
        node->continue_target()->Bind();
      }
      if (has_valid_frame()) {
        LoadConditionAndSpill(node->cond(), &body, node->break_target(), true);
        if (has_valid_frame()) {
          // A invalid frame here indicates that control did not
          // fall out of the test expression.
          Branch(true, &body);
        }
      }
      break;
  }

  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitWhileStatement(WhileStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ WhileStatement");
  CodeForStatementPosition(node);

  // If the test is never true and has no side effects there is no need
  // to compile the test or body.
  ConditionAnalysis info = AnalyzeCondition(node->cond());
  if (info == ALWAYS_FALSE) return;

  node->break_target()->set_direction(JumpTarget::FORWARD_ONLY);

  // Label the top of the loop with the continue target for the backward
  // CFG edge.
  node->continue_target()->set_direction(JumpTarget::BIDIRECTIONAL);
  node->continue_target()->Bind();


  if (info == DONT_KNOW) {
    JumpTarget body;
    LoadConditionAndSpill(node->cond(), &body, node->break_target(), true);
    if (has_valid_frame()) {
      // A NULL frame indicates that control did not fall out of the
      // test expression.
      Branch(false, node->break_target());
    }
    if (has_valid_frame() || body.is_linked()) {
      body.Bind();
    }
  }

  if (has_valid_frame()) {
    CheckStack();  // TODO(1222600): ignore if body contains calls.
    VisitAndSpill(node->body());

    // If control flow can fall out of the body, jump back to the top.
    if (has_valid_frame()) {
      node->continue_target()->Jump();
    }
  }
  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitForStatement(ForStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ ForStatement");
  CodeForStatementPosition(node);
  if (node->init() != NULL) {
    VisitAndSpill(node->init());
  }

  // If the test is never true there is no need to compile the test or
  // body.
  ConditionAnalysis info = AnalyzeCondition(node->cond());
  if (info == ALWAYS_FALSE) return;

  node->break_target()->set_direction(JumpTarget::FORWARD_ONLY);

  // If there is no update statement, label the top of the loop with the
  // continue target, otherwise with the loop target.
  JumpTarget loop(JumpTarget::BIDIRECTIONAL);
  if (node->next() == NULL) {
    node->continue_target()->set_direction(JumpTarget::BIDIRECTIONAL);
    node->continue_target()->Bind();
  } else {
    node->continue_target()->set_direction(JumpTarget::FORWARD_ONLY);
    loop.Bind();
  }

  // If the test is always true, there is no need to compile it.
  if (info == DONT_KNOW) {
    JumpTarget body;
    LoadConditionAndSpill(node->cond(), &body, node->break_target(), true);
    if (has_valid_frame()) {
      Branch(false, node->break_target());
    }
    if (has_valid_frame() || body.is_linked()) {
      body.Bind();
    }
  }

  if (has_valid_frame()) {
    CheckStack();  // TODO(1222600): ignore if body contains calls.
    VisitAndSpill(node->body());

    if (node->next() == NULL) {
      // If there is no update statement and control flow can fall out
      // of the loop, jump directly to the continue label.
      if (has_valid_frame()) {
        node->continue_target()->Jump();
      }
    } else {
      // If there is an update statement and control flow can reach it
      // via falling out of the body of the loop or continuing, we
      // compile the update statement.
      if (node->continue_target()->is_linked()) {
        node->continue_target()->Bind();
      }
      if (has_valid_frame()) {
        // Record source position of the statement as this code which is
        // after the code for the body actually belongs to the loop
        // statement and not the body.
        CodeForStatementPosition(node);
        VisitAndSpill(node->next());
        loop.Jump();
      }
    }
  }
  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitForInStatement(ForInStatement* node) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::VisitTryCatchStatement(TryCatchStatement* node) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::VisitTryFinallyStatement(TryFinallyStatement* node) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::VisitDebuggerStatement(DebuggerStatement* node) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::InstantiateFunction(
    Handle<SharedFunctionInfo> function_info) {
  VirtualFrame::SpilledScope spilled_scope;
  __ li(a0, Operand(function_info));
  // Use the fast case closure allocation code that allocates in new
  // space for nested functions that don't need literals cloning.
  if (scope()->is_function_scope() && function_info->num_literals() == 0) {
    FastNewClosureStub stub;
    frame_->EmitPush(a0);
    frame_->CallStub(&stub, 1);
    frame_->EmitPush(v0);
  } else {
    // Create a new closure.
    frame_->EmitPush(cp);
    frame_->EmitPush(a0);
    frame_->CallRuntime(Runtime::kNewClosure, 2);
    frame_->EmitPush(v0);
  }
}


void CodeGenerator::VisitFunctionLiteral(FunctionLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ FunctionLiteral");

  // Build the function info and instantiate it.
  Handle<SharedFunctionInfo> function_info =
      Compiler::BuildFunctionInfo(node, script(), this);
  // Check for stack-overflow exception.
  if (HasStackOverflow()) {
    ASSERT(frame_->height() == original_height);
    return;
  }
  InstantiateFunction(function_info);
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitSharedFunctionInfoLiteral(
    SharedFunctionInfoLiteral* node) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::VisitConditional(Conditional* node) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::VisitSlot(Slot* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ Slot");
  LoadFromSlot(node, typeof_state());
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitVariableProxy(VariableProxy* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ VariableProxy");

  Variable* var = node->var();
  Expression* expr = var->rewrite();
  if (expr != NULL) {
    Visit(expr);
  } else {
    ASSERT(var->is_global());
    Reference ref(this, node);
    ref.GetValueAndSpill();
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitLiteral(Literal* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ Literal");
  __ li(t0, Operand(node->handle()));
  frame_->EmitPush(t0);
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitRegExpLiteral(RegExpLiteral* node) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::VisitObjectLiteral(ObjectLiteral* node) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::VisitArrayLiteral(ArrayLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ ArrayLiteral");

  // Load the function of this activation.
  __ lw(a2, frame_->Function());
  // Load the literals array of the function.
  __ lw(a2, FieldMemOperand(a2, JSFunction::kLiteralsOffset));
  __ li(a1, Operand(Smi::FromInt(node->literal_index())));
  __ li(a0, Operand(node->constant_elements()));
  frame_->EmitMultiPush(a2.bit() | a1.bit() | a0.bit());
  int length = node->values()->length();
  if (node->depth() > 1) {
    UNIMPLEMENTED_MIPS();
  } else if (length > FastCloneShallowArrayStub::kMaximumLength) {
    UNIMPLEMENTED_MIPS();
  } else {
    FastCloneShallowArrayStub stub(length);
    frame_->CallStub(&stub, 3);
  }
  frame_->EmitPush(v0);  // Save the result.
  // v0: created object literal

  // Generate code to set the elements in the array that are not
  // literals.
  for (int i = 0; i < node->values()->length(); i++) {
    Expression* value = node->values()->at(i);

    // If value is a literal the property value is already set in the
    // boilerplate object.
    if (value->AsLiteral() != NULL) continue;
    // If value is a materialized literal the property value is already set
    // in the boilerplate object if it is simple.
    if (CompileTimeValue::IsCompileTimeValue(value)) continue;

    // The property must be set by generated code.
    LoadAndSpill(value);
    frame_->EmitPop(a0);

    // Fetch the object literal.
    __ lw(a1, frame_->Top());
    // Get the elements array.
    __ lw(a1, FieldMemOperand(a1, JSObject::kElementsOffset));

    // Write to the indexed properties array.
    int offset = i * kPointerSize + FixedArray::kHeaderSize;
    __ sw(a0, FieldMemOperand(a1, offset));

    // Update the write barrier for the array address.
    __ li(a3, Operand(offset));
    __ RecordWrite(a1, a3, a2);
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitCatchExtensionObject(CatchExtensionObject* node) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::VisitAssignment(Assignment* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ Assignment");

  { Reference target(this, node->target());
    if (target.is_illegal()) {
      // Fool the virtual frame into thinking that we left the assignment's
      // value on the frame.
      frame_->EmitPush(zero_reg);
      ASSERT(frame_->height() == original_height + 1);
      return;
    }

    if (node->op() == Token::ASSIGN ||
        node->op() == Token::INIT_VAR ||
        node->op() == Token::INIT_CONST) {
      LoadAndSpill(node->value());
    } else {
      PrintF("(plind) -- VisitAssignment\n");
      UNIMPLEMENTED_MIPS();
    }

    Variable* var = node->target()->AsVariableProxy()->AsVariable();
    if (var != NULL &&
        (var->mode() == Variable::CONST) &&
        node->op() != Token::INIT_VAR && node->op() != Token::INIT_CONST) {
      // Assignment ignored - leave the value on the stack.
    } else {
      CodeForSourcePosition(node->position());
      if (node->op() == Token::INIT_CONST) {
        // Dynamic constant initializations must use the function context
        // and initialize the actual constant declared. Dynamic variable
        // initializations are simply assignments and use SetValue.
        target.SetValue(CONST_INIT);
      } else {
        target.SetValue(NOT_CONST_INIT);
      }
    }
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitThrow(Throw* node) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::VisitProperty(Property* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ Property");

  { Reference property(this, node);
    property.GetValueAndSpill();
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitCall(Call* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ Call");

  Expression* function = node->expression();
  ZoneList<Expression*>* args = node->arguments();

  // Standard function call.
  // Check if the function is a variable or a property.
  Variable* var = function->AsVariableProxy()->AsVariable();
  Property* property = function->AsProperty();

  // ------------------------------------------------------------------------
  // Fast-case: Use inline caching.
  // ---
  // According to ECMA-262, section 11.2.3, page 44, the function to call
  // must be resolved after the arguments have been evaluated. The IC code
  // automatically handles this by loading the arguments before the function
  // is resolved in cache misses (this also holds for megamorphic calls).
  // ------------------------------------------------------------------------

  if (var != NULL && var->is_possibly_eval()) {
    UNIMPLEMENTED_MIPS();
  } else if (var != NULL && !var->is_this() && var->is_global()) {
    // ----------------------------------
    // JavaScript example: 'foo(1, 2, 3)'  // foo is global
    // ----------------------------------

    int arg_count = args->length();

    // We need sp to be 8 bytes aligned when calling the stub.
    __ SetupAlignedCall(t0, arg_count + 1);

    // Pass the global object as the receiver and let the IC stub
    // patch the stack to use the global proxy as 'this' in the
    // invoked function.
    LoadGlobal();

    // Load the arguments.
    for (int i = 0; i < arg_count; i++) {
      LoadAndSpill(args->at(i));
    }

    // Setup the receiver register and call the IC initialization code.
    __ li(a2, Operand(var->name()));
    InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
    Handle<Code> stub = ComputeCallInitialize(arg_count, in_loop);
    CodeForSourcePosition(node->position());
    frame_->CallCodeObject(stub, RelocInfo::CODE_TARGET_CONTEXT,
                           arg_count + 1);
    __ ReturnFromAlignedCall();
    __ lw(cp, frame_->Context());
    // Remove the function from the stack.
    frame_->EmitPush(v0);

  } else if (var != NULL && var->slot() != NULL &&
             var->slot()->type() == Slot::LOOKUP) {
    UNIMPLEMENTED_MIPS();
  } else if (property != NULL) {
    UNIMPLEMENTED_MIPS();
  } else {
    UNIMPLEMENTED_MIPS();
  }

  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitCallNew(CallNew* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ CallNew");

  // According to ECMA-262, section 11.2.2, page 44, the function
  // expression in new calls must be evaluated before the
  // arguments. This is different from ordinary calls, where the
  // actual function to call is resolved after the arguments have been
  // evaluated.

  ZoneList<Expression*>* args = node->arguments();
  int arg_count = args->length();

  // Compute function to call and use the global object as the
  // receiver. There is no need to use the global proxy here because
  // it will always be replaced with a newly allocated object.
  LoadAndSpill(node->expression());
  LoadGlobal();

  // Push the arguments ("left-to-right") on the stack.
  for (int i = 0; i < arg_count; i++) {
    LoadAndSpill(args->at(i));
  }

  // a0: the number of arguments.
  Result num_args(a0);
  __ li(a0, Operand(arg_count));

  // Load the function into a1 as per calling convention.
  Result function(a1);
  __ lw(a1, frame_->ElementAt(arg_count + 1));

  // Call the construct call builtin that handles allocation and
  // constructor invocation.
  CodeForSourcePosition(node->position());
  Handle<Code> ic(Builtins::builtin(Builtins::JSConstructCall));
  frame_->CallCodeObject(ic,
                         RelocInfo::CONSTRUCT_CALL,
                         arg_count + 1);
  // Discard old TOS value and push r0 on the stack (same as Pop(), push(r0)).
  __ sw(v0, frame_->Top());
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::GenerateClassOf(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateValueOf(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateSetValueOf(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateIsSmi(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateLog(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateIsNonNegativeSmi(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateMathPow(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateMathCos(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateMathSin(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateMathSqrt(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


// This should generate code that performs a charCodeAt() call or returns
// undefined in order to trigger the slow case, Runtime_StringCharCodeAt.
// It is not yet implemented on ARM, so it always goes to the slow case.
void CodeGenerator::GenerateFastCharCodeAt(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateCharFromCode(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateIsArray(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateIsRegExp(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateIsConstructCall(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateArgumentsLength(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateArguments(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateRandomPositiveSmi(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateObjectEquals(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateIsObject(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateIsFunction(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateIsUndetectableObject(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateStringAdd(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateSubString(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateStringCompare(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateRegExpExec(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::GenerateNumberToString(ZoneList<Expression*>* args) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::VisitCallRuntime(CallRuntime* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  if (CheckForInlineRuntimeCall(node)) {
    ASSERT((has_cc() && frame_->height() == original_height) ||
           (!has_cc() && frame_->height() == original_height + 1));
    return;
  }

  ZoneList<Expression*>* args = node->arguments();
  Comment cmnt(masm_, "[ CallRuntime");
  Runtime::Function* function = node->function();

  int arg_count = args->length();

  if (function == NULL) {
    UNIMPLEMENTED_MIPS();
  }

  // Push the arguments ("left-to-right").
  for (int i = 0; i < arg_count; i++) {
    LoadAndSpill(args->at(i));
  }

  if (function == NULL) {
    UNIMPLEMENTED_MIPS();
  } else {
    // Call the C runtime function.
    frame_->CallRuntime(function, arg_count);
    frame_->EmitPush(v0);
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitUnaryOperation(UnaryOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ UnaryOperation");

  Token::Value op = node->op();

  if (op == Token::NOT) {
    // LoadConditionAndSpill reversing the false and true targets.
    LoadConditionAndSpill(node->expression(),
                          false_target(),
                          true_target(),
                          true);
    // LoadCondition may (and usually does) leave a test and branch to
    // be emitted by the caller.  In that case, negate the condition.
    if (has_cc()) cc_reg_ = NegateCondition(cc_reg_);

  } else if (op == Token::DELETE) {
    UNIMPLEMENTED_MIPS();
  
  } else if (op == Token::TYPEOF) {
    // Special case for loading the typeof expression; see comment on
    // LoadTypeofExpression().
    LoadTypeofExpression(node->expression());
    frame_->CallRuntime(Runtime::kTypeof, 1);
    frame_->EmitPush(v0);  // v0 holds the result.

  } else {
    bool overwrite =
        (node->expression()->AsBinaryOperation() != NULL &&
         node->expression()->AsBinaryOperation()->ResultOverwriteAllowed());
    LoadAndSpill(node->expression());
    frame_->EmitPop(a0);
    switch (op) {
      case Token::NOT:
      case Token::DELETE:
      case Token::TYPEOF:
        UNREACHABLE();  // Handled above.
        break;

      case Token::SUB: {
        UNIMPLEMENTED_MIPS();
        break;
      }

      case Token::BIT_NOT: {
        JumpTarget smi_label;
        JumpTarget continue_label;
        __ And(t0, a0, Operand(kSmiTagMask));
        smi_label.Branch(eq, t0, Operand(zero_reg));

        GenericUnaryOpStub stub(Token::BIT_NOT, overwrite);
        frame_->CallStub(&stub, 0);
        continue_label.Jump();

        smi_label.Bind();
        // We have a smi. Invert all bits except bit 0.
        __ Xor(v0, a0, 0xfffffffe);
        continue_label.Bind();
        break;
      }

      case Token::VOID:
        UNIMPLEMENTED_MIPS();
        break;

      case Token::ADD: {
        UNIMPLEMENTED_MIPS();
        break;
      }
      default:
        UNREACHABLE();
    }
    frame_->EmitPush(v0);  // v0 holds the result.
  }
  ASSERT(!has_valid_frame() ||
         (has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


void CodeGenerator::VisitCountOperation(CountOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ CountOperation");

  bool is_postfix = node->is_postfix();
  bool is_increment = node->op() == Token::INC;

  Variable* var = node->expression()->AsVariableProxy()->AsVariable();
  bool is_const = (var != NULL && var->mode() == Variable::CONST);

  // Postfix: Make room for the result.
  if (is_postfix) {
    __ mov(v0, zero_reg);
    frame_->EmitPush(v0);
  }

  { Reference target(this, node->expression(), !is_const);
    if (target.is_illegal()) {
      // Spoof the virtual frame to have the expected height (one higher
      // than on entry).
      if (!is_postfix) {
        __ mov(v0, zero_reg);
        frame_->EmitPush(v0);
      }
      ASSERT(frame_->height() == original_height + 1);
      return;
    }
    // Get the old value in a0.
    target.GetValueAndSpill();
    frame_->EmitPop(a0);

    JumpTarget slow;
    JumpTarget exit;

    // Check for smi operand.
    __ And(t0, a0, Operand(kSmiTagMask));
    slow.Branch(ne, t0, Operand(zero_reg), no_hint);

    // Postfix: Store the old value as the result.
    if (is_postfix) {
      __ sw(a0, frame_->ElementAt(target.size()));
    }

    // Perform optimistic increment/decrement and check for overflow.
    // If we don't overflow we are done.
    if (is_increment) {
      __ Add(v0, a0, Operand(Smi::FromInt(1)));
      exit.Branch(ne, a0, Operand(Smi::kMaxValue), no_hint);
    } else {
      __ Add(v0, a0, Operand((Smi::FromInt(-1))));
      exit.Branch(ne, a0, Operand(Smi::kMinValue), no_hint);
    }

    // We had an overflow. Revert optimistic increment/decrement.
    __ mov(v0, a0);

    // Slow case: Convert to number.
    slow.Bind();
    UNIMPLEMENTED_MIPS();
    __ break_(0x09001); // We should not come here yet.

    // Store the new value in the target if not const.
    exit.Bind();
    frame_->EmitPush(v0);
    if (!is_const) target.SetValue(NOT_CONST_INIT);
  }

  // Postfix: Discard the new value and use the old.
  if (is_postfix) frame_->EmitPop(v0);
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitBinaryOperation(BinaryOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ BinaryOperation");
  Token::Value op = node->op();

  // According to ECMA-262 section 11.11, page 58, the binary logical
  // operators must yield the result of one of the two expressions
  // before any ToBoolean() conversions. This means that the value
  // produced by a && or || operator is not necessarily a boolean.

  // NOTE: If the left hand side produces a materialized value (not in
  // the CC register), we force the right hand side to do the
  // same. This is necessary because we may have to branch to the exit
  // after evaluating the left hand side (due to the shortcut
  // semantics), but the compiler must (statically) know if the result
  // of compiling the binary operation is materialized or not.

  if (op == Token::AND) {
    JumpTarget is_true;
    LoadConditionAndSpill(node->left(),
                          &is_true,
                          false_target(),
                          false);
    if (has_valid_frame() && !has_cc()) {
      // The left-hand side result is on top of the virtual frame.
      JumpTarget pop_and_continue;
      JumpTarget exit;

      __ lw(a0, frame_->Top());  // Duplicate the stack top.
      frame_->EmitPush(a0);
      // Avoid popping the result if it converts to 'false' using the
      // standard ToBoolean() conversion as described in ECMA-262,
      // section 9.2, page 30.
      ToBoolean(&pop_and_continue, &exit);
      Branch(false, &exit);

      // Pop the result of evaluating the first part.
      pop_and_continue.Bind();
      frame_->EmitPop(t0);

      // Evaluate right side expression.
      is_true.Bind();
      LoadAndSpill(node->right());

      // Exit (always with a materialized value).
      exit.Bind();
    } else if (has_cc() || is_true.is_linked()) {
      // The left-hand side is either (a) partially compiled to
      // control flow with a final branch left to emit or (b) fully
      // compiled to control flow and possibly true.
      if (has_cc()) {
        Branch(false, false_target());
      }
      is_true.Bind();
      LoadConditionAndSpill(node->right(),
                            true_target(),
                            false_target(),
                            false);
    } else {
      // Nothing to do.
      ASSERT(!has_valid_frame() && !has_cc() && !is_true.is_linked());
    }

  } else if (op == Token::OR) {
    JumpTarget is_false;
    LoadConditionAndSpill(node->left(),
                          true_target(),
                          &is_false,
                          false);
    if (has_valid_frame() && !has_cc()) {
      // The left-hand side result is on top of the virtual frame.
      JumpTarget pop_and_continue;
      JumpTarget exit;

      __ lw(a0, frame_->Top());
      frame_->EmitPush(a0);
      // Avoid popping the result if it converts to 'true' using the
      // standard ToBoolean() conversion as described in ECMA-262,
      // section 9.2, page 30.
      ToBoolean(&exit, &pop_and_continue);
      Branch(true, &exit);

      // Pop the result of evaluating the first part.
      pop_and_continue.Bind();
      frame_->EmitPop(a0);

      // Evaluate right side expression.
      is_false.Bind();
      LoadAndSpill(node->right());

      // Exit (always with a materialized value).
      exit.Bind();
    } else if (has_cc() || is_false.is_linked()) {
      // The left-hand side is either (a) partially compiled to
      // control flow with a final branch left to emit or (b) fully
      // compiled to control flow and possibly false.
      if (has_cc()) {
        Branch(true, true_target());
      }
      is_false.Bind();
      LoadConditionAndSpill(node->right(),
                            true_target(),
                            false_target(),
                            false);
    } else {
      // Nothing to do.
      ASSERT(!has_valid_frame() && !has_cc() && !is_false.is_linked());
    }

  } else {
    // Optimize for the case where (at least) one of the expressions
    // is a literal small integer.
   Literal* lliteral = node->left()->AsLiteral();
   Literal* rliteral = node->right()->AsLiteral();
    // NOTE: The code below assumes that the slow cases (calls to runtime)
    // never return a constant/immutable object.
    bool overwrite_left =
        (node->left()->AsBinaryOperation() != NULL &&
         node->left()->AsBinaryOperation()->ResultOverwriteAllowed());
    bool overwrite_right =
        (node->right()->AsBinaryOperation() != NULL &&
         node->right()->AsBinaryOperation()->ResultOverwriteAllowed());

    if (rliteral != NULL && rliteral->handle()->IsSmi()) {
      LoadAndSpill(node->left());
      SmiOperation(node->op(),
                   rliteral->handle(),
                   false,
                   overwrite_right ? OVERWRITE_RIGHT : NO_OVERWRITE);

    } else if (lliteral != NULL && lliteral->handle()->IsSmi()) {
      LoadAndSpill(node->right());
      SmiOperation(node->op(),
                   lliteral->handle(),
                   true,
                   overwrite_left ? OVERWRITE_LEFT : NO_OVERWRITE);
    } else {
      OverwriteMode overwrite_mode = NO_OVERWRITE;
      if (overwrite_left) {
        overwrite_mode = OVERWRITE_LEFT;
      } else if (overwrite_right) {
        overwrite_mode = OVERWRITE_RIGHT;
      }
      LoadAndSpill(node->left());
      LoadAndSpill(node->right());
      GenericBinaryOperation(node->op(), overwrite_mode);
    }
    frame_->EmitPush(v0);
  }
  ASSERT(!has_valid_frame() ||
         (has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


void CodeGenerator::VisitThisFunction(ThisFunction* node) {
  UNIMPLEMENTED_MIPS();
}


void CodeGenerator::VisitCompareOperation(CompareOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope;
  Comment cmnt(masm_, "[ CompareOperation");

  // Get the expressions from the node.
  Expression* left = node->left();
  Expression* right = node->right();
  Token::Value op = node->op();

  // To make null checks efficient, we check if either left or right is the
  // literal 'null'. If so, we optimize the code by inlining a null check
  // instead of calling the (very) general runtime routine for checking
  // equality.
  if (op == Token::EQ || op == Token::EQ_STRICT) {
    bool left_is_null =
        left->AsLiteral() != NULL && left->AsLiteral()->IsNull();
    bool right_is_null =
        right->AsLiteral() != NULL && right->AsLiteral()->IsNull();
    // The 'null' value can only be equal to 'null' or 'undefined'.
    if (left_is_null || right_is_null) {
      LoadAndSpill(left_is_null ? right : left);
      frame_->EmitPop(condReg1);
      __ LoadRoot(condReg2, Heap::kNullValueRootIndex);

      // The 'null' value is only equal to 'undefined' if using non-strict
      // comparisons.
      if (op != Token::EQ_STRICT) {
        true_target()->Branch(eq, condReg1, Operand(condReg2), no_hint);

        __ LoadRoot(condReg2, Heap::kUndefinedValueRootIndex);
        true_target()->Branch(eq, condReg1, Operand(condReg2), no_hint);

        __ And(condReg2, condReg1, kSmiTagMask);
        false_target()->Branch(eq, condReg2, Operand(zero_reg), no_hint);

        // It can be an undetectable object.
        __ lw(condReg1, FieldMemOperand(condReg1, HeapObject::kMapOffset));
        __ lbu(condReg1, FieldMemOperand(condReg1, Map::kBitFieldOffset));
        __ And(condReg1, condReg1, 1 << Map::kIsUndetectable);
        __ li(condReg2, Operand(1 << Map::kIsUndetectable));
      }

      // We don't need to load anyting in condReg1 and condReg2 as they are
      // already correctly loaded.
      cc_reg_ = eq;
      ASSERT(has_cc() && frame_->height() == original_height);
      return;
    }
  }

  // To make typeof testing for natives implemented in JavaScript really
  // efficient, we generate special code for expressions of the form:
  // 'typeof <expression> == <string>'.
  UnaryOperation* operation = left->AsUnaryOperation();
  if ((op == Token::EQ || op == Token::EQ_STRICT) &&
      (operation != NULL && operation->op() == Token::TYPEOF) &&
      (right->AsLiteral() != NULL &&
       right->AsLiteral()->handle()->IsString())) {
    Handle<String> check(String::cast(*right->AsLiteral()->handle()));

    // Load the operand, move it to register condReg1.
    LoadTypeofExpression(operation->expression());
    frame_->EmitPop(condReg1);

    if (check->Equals(Heap::number_symbol())) {
      __ And(condReg2, condReg1, kSmiTagMask);
      true_target()->Branch(eq, condReg2, Operand(zero_reg), no_hint);
      __ lw(condReg1, FieldMemOperand(condReg1, HeapObject::kMapOffset));
      __ LoadRoot(condReg2, Heap::kHeapNumberMapRootIndex);
      cc_reg_ = eq;

    } else if (check->Equals(Heap::string_symbol())) {
      __ And(condReg2, condReg1, kSmiTagMask);
      false_target()->Branch(eq, condReg2, Operand(zero_reg), no_hint);

      __ lw(condReg1, FieldMemOperand(condReg1, HeapObject::kMapOffset));

      // It can be an undetectable string object.
      __ lbu(condReg2, FieldMemOperand(condReg1, Map::kBitFieldOffset));
      __ And(condReg2, condReg2, 1 << Map::kIsUndetectable);
      false_target()->Branch(eq, condReg2, Operand(1 << Map::kIsUndetectable),
          no_hint);

      __ lbu(condReg1, FieldMemOperand(condReg1, Map::kInstanceTypeOffset));
      __ li(condReg2, Operand(FIRST_NONSTRING_TYPE));
      cc_reg_ = less;

    } else if (check->Equals(Heap::boolean_symbol())) {
      __ LoadRoot(condReg2, Heap::kTrueValueRootIndex);
      true_target()->Branch(eq, condReg1, Operand(condReg2), no_hint);
      __ LoadRoot(condReg2, Heap::kFalseValueRootIndex);
      cc_reg_ = eq;

    } else if (check->Equals(Heap::undefined_symbol())) {
      __ LoadRoot(condReg2, Heap::kUndefinedValueRootIndex);
      true_target()->Branch(eq, condReg1, Operand(condReg2), no_hint);

      __ And(condReg2, condReg1, kSmiTagMask);
      false_target()->Branch(eq, condReg2, Operand(zero_reg), no_hint);

      // It can be an undetectable object.
      __ lw(condReg1, FieldMemOperand(condReg1, HeapObject::kMapOffset));
      __ lbu(condReg1, FieldMemOperand(condReg1, Map::kBitFieldOffset));
      __ And(condReg1, condReg1, 1 << Map::kIsUndetectable);
      __ li(condReg2, Operand(1 << Map::kIsUndetectable));

      cc_reg_ = eq;

    } else if (check->Equals(Heap::function_symbol())) {
      UNIMPLEMENTED_MIPS();

    } else if (check->Equals(Heap::object_symbol())) {
      UNIMPLEMENTED_MIPS();

    } else {
      // Uncommon case: typeof testing against a string literal that is
      // never returned from the typeof operator.
      false_target()->Jump();
    }
    ASSERT(!has_valid_frame() ||
           (has_cc() && frame_->height() == original_height));
    return;
  }

  switch (op) {
    case Token::EQ:
      Comparison(eq, left, right, false);
      break;

    case Token::LT:
      Comparison(less, left, right);
      break;

    case Token::GT:
      Comparison(greater, left, right);
      break;

    case Token::LTE:
      Comparison(less_equal, left, right);
      break;

    case Token::GTE:
      Comparison(greater_equal, left, right);
      break;

    case Token::EQ_STRICT:
      Comparison(eq, left, right, true);
      break;

    case Token::IN: {
      UNIMPLEMENTED_MIPS();
      break;
    }

    case Token::INSTANCEOF: {
      LoadAndSpill(left);
      LoadAndSpill(right);
      InstanceofStub stub;
      frame_->CallStub(&stub, 2);
      // At this point if instanceof succeeded then v0 == 0.
      __ mov(condReg1, v0);
      __ mov(condReg2, zero_reg);
      cc_reg_ = eq;
      break;
    }

    default:
      UNREACHABLE();
  }
  ASSERT((has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


#ifdef DEBUG
bool CodeGenerator::HasValidEntryRegisters() { return true; }
#endif


#undef __
#define __ ACCESS_MASM(masm)

// -----------------------------------------------------------------------------
// Reference support.

Reference::Reference(CodeGenerator* cgen,
                     Expression* expression,
                     bool persist_after_get)
    : cgen_(cgen),
      expression_(expression),
      type_(ILLEGAL),
      persist_after_get_(persist_after_get) {
  cgen->LoadReference(this);
}


Reference::~Reference() {
  ASSERT(is_unloaded() || is_illegal());
}


Handle<String> Reference::GetName() {
  ASSERT(type_ == NAMED);
  Property* property = expression_->AsProperty();
  if (property == NULL) {
    // Global variable reference treated as a named property reference.
    VariableProxy* proxy = expression_->AsVariableProxy();
    ASSERT(proxy->AsVariable() != NULL);
    ASSERT(proxy->AsVariable()->is_global());
    return proxy->name();
  } else {
    Literal* raw_name = property->key()->AsLiteral();
    ASSERT(raw_name != NULL);
    return Handle<String>(String::cast(*raw_name->handle()));
  }
}


void Reference::GetValue() {
  ASSERT(cgen_->HasValidEntryRegisters());
  ASSERT(!is_illegal());
  ASSERT(!cgen_->has_cc());
  MacroAssembler* masm = cgen_->masm();
  Property* property = expression_->AsProperty();
  if (property != NULL) {
    cgen_->CodeForSourcePosition(property->position());
  }

  switch (type_) {
    case SLOT: {
      UNIMPLEMENTED_MIPS();
      break;
    }

    case NAMED: {
      VirtualFrame* frame = cgen_->frame();
      Comment cmnt(masm, "[ Load from named Property");
      Handle<String> name(GetName());
      Variable* var = expression_->AsVariableProxy()->AsVariable();
      Handle<Code> ic(Builtins::builtin(Builtins::LoadIC_Initialize));
      // Setup the name register.
      __ li(a2, Operand(name));
      ASSERT(var == NULL || var->is_global());
      RelocInfo::Mode rmode = (var == NULL)
                            ? RelocInfo::CODE_TARGET
                            : RelocInfo::CODE_TARGET_CONTEXT;
      frame->CallCodeObject(ic, rmode, 0);
      frame->EmitPush(v0);
      break;
    }

    case KEYED: {
      VirtualFrame* frame = cgen_->frame();
      Comment cmnt(masm, "[ Load from keyed Property");
      ASSERT(property != NULL);
      Handle<Code> ic(Builtins::builtin(Builtins::KeyedLoadIC_Initialize));
      Variable* var = expression_->AsVariableProxy()->AsVariable();
      ASSERT(var == NULL || var->is_global());
      RelocInfo::Mode rmode = (var == NULL)
                            ? RelocInfo::CODE_TARGET
                            : RelocInfo::CODE_TARGET_CONTEXT;
      frame->CallCodeObject(ic, rmode, 0);
      frame->EmitPush(v0);
      break;
    }

    default:
      UNREACHABLE();
  }

  if (!persist_after_get_) {
    cgen_->UnloadReference(this);
  }
}


void Reference::SetValue(InitState init_state) {
  ASSERT(!is_illegal());
  ASSERT(!cgen_->has_cc());
  MacroAssembler* masm = cgen_->masm();
  VirtualFrame* frame = cgen_->frame();
  Property* property = expression_->AsProperty();
  if (property != NULL) {
    cgen_->CodeForSourcePosition(property->position());
  }

  switch (type_) {
    case SLOT: {
      Comment cmnt(masm, "[ Store to Slot");
      Slot* slot = expression_->AsVariableProxy()->AsVariable()->slot();
      cgen_->StoreToSlot(slot, init_state);
      cgen_->UnloadReference(this);
      break;
    }

    case NAMED: {
      Comment cmnt(masm, "[ Store to named Property");
      // Call the appropriate IC code.
      Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
      Handle<String> name(GetName());

      frame->EmitPop(a0);
      frame->EmitPop(a1);

      // Setup the name register.
      Result property_name(a2);
      __ li(a2, Operand(name));
      frame->CallCodeObject(ic, RelocInfo::CODE_TARGET, 0);
      frame->EmitPush(v0);
      set_unloaded();
      break;
    }

    case KEYED: {
      Comment cmnt(masm, "[ Store to keyed Property");
      Property* property = expression_->AsProperty();
      ASSERT(property != NULL);
      cgen_->CodeForSourcePosition(property->position());

      // Call IC code.
      Handle<Code> ic(Builtins::builtin(Builtins::KeyedStoreIC_Initialize));
      frame->EmitPop(a0);
      frame->CallCodeObject(ic, RelocInfo::CODE_TARGET, 0);
      frame->EmitPush(v0);
      cgen_->UnloadReference(this);
      break;
    }

    default:
      UNREACHABLE();
  }
}


void FastNewClosureStub::Generate(MacroAssembler* masm) {
  // Create a new closure from the given function info in new
  // space. Set the context to the current context in cp.
  Label gc;

  // Pop the function info from the stack.
  __ Pop(a3);

  // Attempt to allocate new JSFunction in new space.
  __ AllocateInNewSpace(JSFunction::kSize / kPointerSize,
                        v0,
                        a1,
                        a2,
                        &gc,
                        TAG_OBJECT);

  // Compute the function map in the current global context and set that
  // as the map of the allocated object.
  __ lw(a2, MemOperand(cp, Context::SlotOffset(Context::GLOBAL_INDEX)));
  __ lw(a2, FieldMemOperand(a2, GlobalObject::kGlobalContextOffset));
  __ lw(a2, MemOperand(a2, Context::SlotOffset(Context::FUNCTION_MAP_INDEX)));
  __ sw(a2, FieldMemOperand(v0, HeapObject::kMapOffset));

  // Initialize the rest of the function. We don't have to update the
  // write barrier because the allocated object is in new space.
  __ LoadRoot(a1, Heap::kEmptyFixedArrayRootIndex);
  __ LoadRoot(a2, Heap::kTheHoleValueRootIndex);
  __ sw(a1, FieldMemOperand(v0, JSObject::kPropertiesOffset));
  __ sw(a1, FieldMemOperand(v0, JSObject::kElementsOffset));
  __ sw(a2, FieldMemOperand(v0, JSFunction::kPrototypeOrInitialMapOffset));
  __ sw(a3, FieldMemOperand(v0, JSFunction::kSharedFunctionInfoOffset));
  __ sw(cp, FieldMemOperand(v0, JSFunction::kContextOffset));
  __ sw(a1, FieldMemOperand(v0, JSFunction::kLiteralsOffset));

  // Return result. The argument function info has been popped already.
  __ Ret();

  // Create a new closure through the slower runtime call.
  __ bind(&gc);
  __ addiu(sp, sp, 2 * kPointerSize);
  __ sw(cp, MemOperand(sp, 1 * kPointerSize));
  __ sw(a3, MemOperand(sp, 0 * kPointerSize));
  __ TailCallRuntime(Runtime::kNewClosure, 2, 1);
}


void FastCloneShallowArrayStub::Generate(MacroAssembler* masm) {
  // Stack layout on entry:
  // [sp]: constant elements.
  // [sp + kPointerSize]: literal index.
  // [sp + (2 * kPointerSize)]: literals array.

  // All sizes here are multiples of kPointerSize.
  int elements_size = (length_ > 0) ? FixedArray::SizeFor(length_) : 0;
  int size = JSArray::kSize + elements_size;

  // Load boilerplate object into r3 and check if we need to create a
  // boilerplate.
  Label slow_case;
  __ lw(a3, MemOperand(sp, 2 * kPointerSize));
  __ lw(a0, MemOperand(sp, 1 * kPointerSize));
  __ Add(a3, a3, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  __ sll(t0, a0, kPointerSizeLog2 - kSmiTagSize);
  __ Add(t0, a3, t0);
  __ lw(a3, MemOperand(t0));
  __ LoadRoot(t1, Heap::kUndefinedValueRootIndex);
  __ Branch(eq, &slow_case, a3, Operand(t1));

  // Allocate both the JS array and the elements array in one big
  // allocation. This avoids multiple limit checks.
  __ AllocateInNewSpace(size / kPointerSize,
                        a0,
                        a1,
                        a2,
                        &slow_case,
                        TAG_OBJECT);

  // Copy the JS array part.
  for (int i = 0; i < JSArray::kSize; i += kPointerSize) {
    if ((i != JSArray::kElementsOffset) || (length_ == 0)) {
      __ lw(a1, FieldMemOperand(a3, i));
      __ sw(a1, FieldMemOperand(a0, i));
    }
  }

  if (length_ > 0) {
    // Get hold of the elements array of the boilerplate and setup the
    // elements pointer in the resulting object.
    __ lw(a3, FieldMemOperand(a3, JSArray::kElementsOffset));
    __ Add(a2, a0, Operand(JSArray::kSize));
    __ sw(a2, FieldMemOperand(a0, JSArray::kElementsOffset));

    // Copy the elements array.
    for (int i = 0; i < elements_size; i += kPointerSize) {
      __ lw(a1, FieldMemOperand(a3, i));
      __ sw(a1, FieldMemOperand(a2, i));
    }
  }

  // Return and remove the on-stack parameters.
  __ Add(sp, sp, Operand(3 * kPointerSize));
  __ Ret();

  __ bind(&slow_case);
  __ TailCallRuntime(Runtime::kCreateArrayLiteralShallow, 3, 1);
}


// On entry a0 and a1 are the things to be compared. On exit v0 is 0,
// positive or negative to indicate the result of the comparison.
void CompareStub::Generate(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
  __ break_(0x765);
}


Handle<Code> GetBinaryOpStub(int key, BinaryOpIC::TypeInfo type_info) {
  UNIMPLEMENTED_MIPS();
  return Handle<Code>::null();
}


void StackCheckStub::Generate(MacroAssembler* masm) {
  // Do tail-call to runtime routine.  Runtime routines expect at least one
  // argument, so give it a Smi.
  __ Push(zero_reg);
  __ TailCallRuntime(Runtime::kStackGuard, 1, 1);
  __ StubReturn(1);
}


void GenericUnaryOpStub::Generate(MacroAssembler* masm) {
  Label slow, done;

  if (op_ == Token::SUB) {
    UNIMPLEMENTED_MIPS();

  } else if (op_ == Token::BIT_NOT) {
    UNIMPLEMENTED_MIPS();

  } else {
    UNIMPLEMENTED();
  }

  __ bind(&done);
  __ StubReturn(1);

  // Handle the slow case by jumping to the JavaScript builtin.
  __ bind(&slow);
  __ Push(a0);
  switch (op_) {
    case Token::SUB:
      __ InvokeBuiltin(Builtins::UNARY_MINUS, JUMP_JS);
      break;
    case Token::BIT_NOT:
      __ InvokeBuiltin(Builtins::BIT_NOT, JUMP_JS);
      break;
    default:
      UNREACHABLE();
  }
}


void CEntryStub::GenerateThrowTOS(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
  __ break_(0x808);
}


void CEntryStub::GenerateThrowUncatchable(MacroAssembler* masm,
                                          UncatchableExceptionType type) {
  UNIMPLEMENTED_MIPS();
  __ break_(0x815);
}

void CEntryStub::GenerateCore(MacroAssembler* masm,
                              Label* throw_normal_exception,
                              Label* throw_termination_exception,
                              Label* throw_out_of_memory_exception,
                              bool do_gc,
                              bool always_allocate) {
  // v0: result parameter for PerformGC, if any
  // s0: number of arguments including receiver (C callee-saved)
  // s1: pointer to the first argument          (C callee-saved)
  // s2: pointer to builtin function            (C callee-saved)

  if (do_gc) {
    UNIMPLEMENTED_MIPS();
  }

  ExternalReference scope_depth =
      ExternalReference::heap_always_allocate_scope_depth();
  if (always_allocate) {
    UNIMPLEMENTED_MIPS();
  }

  // Call C built-in.
  // a0 = argc, a1 = argv
  __ mov(a0, s0);
  __ mov(a1, s1);

  // We are calling compiled C/C++ code. a0 and a1 hold our two arguments. We
  // also need the argument slots.
  __ jalr(s2);
  __ addiu(sp, sp, -StandardFrameConstants::kRArgsSlotsSize);
  __ addiu(sp, sp, StandardFrameConstants::kRArgsSlotsSize);

  if (always_allocate) {
    UNIMPLEMENTED_MIPS();
  }

  // Check for failure result.
  Label failure_returned;
  ASSERT(((kFailureTag + 1) & kFailureTagMask) == 0);
  __ addiu(a2, v0, 1);
  __ andi(t0, a2, kFailureTagMask);
  __ Branch(eq, &failure_returned, t0, Operand(zero_reg));

  // Exit C frame and return.
  // v0:v1: result
  // sp: stack pointer
  // fp: frame pointer
  __ LeaveExitFrame(mode_);

  // Check if we should retry or throw exception.
  Label retry;
  __ bind(&failure_returned);
  ASSERT(Failure::RETRY_AFTER_GC == 0);
  __ andi(t0, v0, ((1 << kFailureTypeTagSize) - 1) << kFailureTagSize);
  __ Branch(eq, &retry, t0, Operand(zero_reg));

  // Special handling of out of memory exceptions.
  Failure* out_of_memory = Failure::OutOfMemoryException();
  __ Branch(eq, throw_out_of_memory_exception,
            v0, Operand(reinterpret_cast<int32_t>(out_of_memory)));

  // Retrieve the pending exception and clear the variable.
  __ LoadExternalReference(t0, ExternalReference::the_hole_value_location());
  __ lw(a3, MemOperand(t0));
  __ LoadExternalReference(t0,
      ExternalReference(Top::k_pending_exception_address));
  __ lw(v0, MemOperand(t0));
  __ sw(a3, MemOperand(t0));

  // Special handling of termination exceptions which are uncatchable
  // by javascript code.
  __ Branch(eq, throw_termination_exception,
            v0, Operand(Factory::termination_exception()));

  // Handle normal exception.
  __ b(throw_normal_exception);
  __ nop();   // Branch delay slot nop.

  __ bind(&retry);  // pass last failure (v0) as parameter (a0) when retrying
  __ mov(a0, v0);
}

void CEntryStub::Generate(MacroAssembler* masm) {
  // Called from JavaScript; parameters are on stack as if calling JS function
  // a0: number of arguments including receiver
  // a1: pointer to builtin function
  // fp: frame pointer    (restored after C call)
  // sp: stack pointer    (restored as callee's sp after C call)
  // cp: current context  (C callee-saved)

  // NOTE: Invocations of builtins may return failure objects
  // instead of a proper result. The builtin entry handles
  // this by performing a garbage collection and retrying the
  // builtin once.

  // Enter the exit frame that transitions from JavaScript to C++.
  __ EnterExitFrame(mode_, s0, s1, s2);

  // s0: number of arguments (C callee-saved)
  // s1: pointer to first argument (C callee-saved)
  // s2: pointer to builtin function (C callee-saved)

  Label throw_normal_exception;
  Label throw_termination_exception;
  Label throw_out_of_memory_exception;

  // Call into the runtime system.
  GenerateCore(masm,
               &throw_normal_exception,
               &throw_termination_exception,
               &throw_out_of_memory_exception,
               false,
               false);

  // Do space-specific GC and retry runtime call.
  GenerateCore(masm,
               &throw_normal_exception,
               &throw_termination_exception,
               &throw_out_of_memory_exception,
               true,
               false);

  // Do full GC and retry runtime call one final time.
  Failure* failure = Failure::InternalError();
  __ li(v0, Operand(reinterpret_cast<int32_t>(failure)));
  GenerateCore(masm,
               &throw_normal_exception,
               &throw_termination_exception,
               &throw_out_of_memory_exception,
               true,
               true);

  __ bind(&throw_out_of_memory_exception);
  GenerateThrowUncatchable(masm, OUT_OF_MEMORY);

  __ bind(&throw_termination_exception);
  GenerateThrowUncatchable(masm, TERMINATION);

  __ bind(&throw_normal_exception);
  GenerateThrowTOS(masm);
}

void JSEntryStub::GenerateBody(MacroAssembler* masm, bool is_construct) {
  Label invoke, exit;

  // Registers:
  // a0: entry address
  // a1: function
  // a2: reveiver
  // a3: argc
  //
  // Stack:
  // 4 args slots
  // args

  // Save callee saved registers on the stack.
  __ MultiPush((kCalleeSaved | ra.bit()) & ~sp.bit());

  // We build an EntryFrame.
  __ li(t3, Operand(-1));  // Push a bad frame pointer to fail if it is used.
  int marker = is_construct ? StackFrame::ENTRY_CONSTRUCT : StackFrame::ENTRY;
  __ li(t2, Operand(Smi::FromInt(marker)));
  __ li(t1, Operand(Smi::FromInt(marker)));
  __ LoadExternalReference(t0, ExternalReference(Top::k_c_entry_fp_address));
  __ lw(t0, MemOperand(t0));
  __ MultiPush(t0.bit() | t1.bit() | t2.bit() | t3.bit());

  // Setup frame pointer for the frame to be pushed.
  __ addiu(fp, sp, -EntryFrameConstants::kCallerFPOffset);

  // Load argv in s0 register.
  __ lw(s0, MemOperand(sp, (kNumCalleeSaved + 1) * kPointerSize +
                           StandardFrameConstants::kCArgsSlotsSize));

  // Registers:
  // a0: entry_address
  // a1: function
  // a2: reveiver_pointer
  // a3: argc
  // s0: argv
  //
  // Stack:
  // caller fp          |
  // function slot      | entry frame
  // context slot       |
  // bad fp (0xff...f)  |
  // callee saved registers + ra
  // 4 args slots
  // args

  // Call a faked try-block that does the invoke.
  __ bal(&invoke);
  __ nop();   // Branch delay slot nop.

  // Caught exception: Store result (exception) in the pending
  // exception field in the JSEnv and return a failure sentinel.
  // Coming in here the fp will be invalid because the PushTryHandler below
  // sets it to 0 to signal the existence of the JSEntry frame.
  __ LoadExternalReference(t0,
      ExternalReference(Top::k_pending_exception_address));
  __ sw(v0, MemOperand(t0));  // We come back from 'invoke'. result is in v0.
  __ li(v0, Operand(reinterpret_cast<int32_t>(Failure::Exception())));
  __ b(&exit);
  __ nop();   // Branch delay slot nop.

  // Invoke: Link this frame into the handler chain.
  __ bind(&invoke);
  __ PushTryHandler(IN_JS_ENTRY, JS_ENTRY_HANDLER);
  // If an exception not caught by another handler occurs, this handler
  // returns control to the code after the bal(&invoke) above, which
  // restores all kCalleeSaved registers (including cp and fp) to their
  // saved values before returning a failure to C.

  // Clear any pending exceptions.
  __ LoadExternalReference(t0, ExternalReference::the_hole_value_location());
  __ lw(t1, MemOperand(t0));
  __ LoadExternalReference(t0,
      ExternalReference(Top::k_pending_exception_address));
  __ sw(t1, MemOperand(t0));

  // Invoke the function by calling through JS entry trampoline builtin.
  // Notice that we cannot store a reference to the trampoline code directly in
  // this stub, because runtime stubs are not traversed when doing GC.

  // Registers:
  // a0: entry_address
  // a1: function
  // a2: reveiver_pointer
  // a3: argc
  // s0: argv
  //
  // Stack:
  // handler frame
  // entry frame
  // callee saved registers + ra
  // 4 args slots
  // args

  if (is_construct) {
    ExternalReference construct_entry(Builtins::JSConstructEntryTrampoline);
    __ LoadExternalReference(t0, construct_entry);
  } else {
    ExternalReference entry(Builtins::JSEntryTrampoline);
    __ LoadExternalReference(t0, entry);
  }
  __ lw(t9, MemOperand(t0));  // Deref address.

  // Call JSEntryTrampoline.
  __ addiu(t9, t9, Code::kHeaderSize - kHeapObjectTag);
  __ CallBuiltin(t9);

  // Unlink this frame from the handler chain. When reading the
  // address of the next handler, there is no need to use the address
  // displacement since the current stack pointer (sp) points directly
  // to the stack handler.
  __ lw(t1, MemOperand(sp, StackHandlerConstants::kNextOffset));
  __ LoadExternalReference(t0, ExternalReference(Top::k_handler_address));
  __ sw(t1, MemOperand(t0));

  // This restores sp to its position before PushTryHandler.
  __ addiu(sp, sp, StackHandlerConstants::kSize);

  __ bind(&exit);  // v0 holds result.
  // Restore the top frame descriptors from the stack.
  __ Pop(t1);
  __ LoadExternalReference(t0, ExternalReference(Top::k_c_entry_fp_address));
  __ sw(t1, MemOperand(t0));

  // Reset the stack to the callee saved registers.
  __ addiu(sp, sp, -EntryFrameConstants::kCallerFPOffset);

  // Restore callee saved registers from the stack.
  __ MultiPop((kCalleeSaved | ra.bit()) & ~sp.bit());
  // Return.
  __ Jump(ra);
}


// This stub performs an instanceof, calling the builtin function if
// necessary. Uses a1 for the object, a0 for the function that it may
// be an instance of (these are fetched from the stack).
void InstanceofStub::Generate(MacroAssembler* masm) {
  // Get the object - slow case for smis (we may need to throw an exception
  // depending on the rhs).
  Label slow, loop, is_instance, is_not_instance;
  __ lw(a0, MemOperand(sp, 1 * kPointerSize));
  __ BranchOnSmi(a0, &slow);

  // Check that the left hand is a JS object and put map in a3.
  __ GetObjectType(a0, a3, a2);
  __ Branch(less, &slow, a2, Operand(FIRST_JS_OBJECT_TYPE));
  __ Branch(greater, &slow, a2, Operand(LAST_JS_OBJECT_TYPE));

  // Get the prototype of the function (t0 is result, a2 is scratch).
  __ lw(a1, MemOperand(sp, 0 * kPointerSize));
  __ TryGetFunctionPrototype(a1, t0, a2, &slow);

  // Check that the function prototype is a JS object.
  __ BranchOnSmi(t0, &slow);
  __ GetObjectType(t0, t1, t1);
  __ Branch(less, &slow, t1, Operand(FIRST_JS_OBJECT_TYPE));
  __ Branch(greater, &slow, t1, Operand(LAST_JS_OBJECT_TYPE));

  // Register mapping: a3 is object map and t0 is function prototype.
  // Get prototype of object into a2.
  __ lw(a2, FieldMemOperand(a3, Map::kPrototypeOffset));

  __ LoadRoot(t1, Heap::kNullValueRootIndex);
  // Loop through the prototype chain looking for the function prototype.
  __ bind(&loop);
  __ Branch(eq, &is_instance, a2, Operand(t0));
  __ Branch(eq, &is_not_instance, a2, Operand(t1));
  __ lw(a2, FieldMemOperand(a2, HeapObject::kMapOffset));
  __ lw(a2, FieldMemOperand(a2, Map::kPrototypeOffset));
  __ jmp(&loop);

  __ bind(&is_instance);
  __ li(v0, Operand(Smi::FromInt(0)));
  __ Pop(2);
  __ Ret();

  __ bind(&is_not_instance);
  __ li(v0, Operand(Smi::FromInt(1)));
  __ Pop(2);
  __ Ret();

  // Slow-case. Tail call builtin.
  __ bind(&slow);
  // TODO(MIPS): instanceof slow case. Need JS builtins.
  __ break_(0x3137);
}


void ArgumentsAccessStub::GenerateReadLength(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
  __ break_(0x851);
}


void ArgumentsAccessStub::GenerateReadElement(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
  __ break_(0x857);
}


void ArgumentsAccessStub::GenerateNewObject(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
  __ break_(0x863);
}


const char* CompareStub::GetName() {
  UNIMPLEMENTED_MIPS();
  return NULL;  // UNIMPLEMENTED RETURN.
}


int CompareStub::MinorKey() {
  // Encode the two parameters in a unique 16 bit value.
  ASSERT(static_cast<unsigned>(cc_) >> 28 < (1 << 15));
  return (static_cast<unsigned>(cc_) >> 27) | (strict_ ? 1 : 0);
}


// Allocates a heap number or jumps to the label if the young space is full and
// a scavenge is needed.
static void AllocateHeapNumber(
    MacroAssembler* masm,
    Label* need_gc,       // Jump here if young space is full.
    Register result,      // The tagged address of the new heap number.
    Register scratch1,    // A scratch register.
    Register scratch2) {  // Another scratch register.

  // Allocate an object in the heap for the heap number and tag it as a heap
  // object.
  // We ask for four more bytes to align it as we need and align the result.
  // (HeapNumber::kSize is modified to be 4-byte bigger)
  __ AllocateInNewSpace((HeapNumber::kSize) / kPointerSize,
                        result,
                        scratch1,
                        scratch2,
                        need_gc,
                        TAG_OBJECT);

  // Align to 8 bytes.
  __ addiu(result, result, 7-1);  // -1 because result is tagged
  __ And(result, result, Operand(~7));
  __ Or(result, result, Operand(1));  // Tag it back.

#ifdef DEBUG
////// TODO(MIPS.6)
//  // Check that the result is 8-byte aligned.
//  __ andi(scratch2, result, Operand(7));
//  __ xori(scratch2, scratch2, Operand(1));  // Fail if the tag is missing.
//  __ Check(eq,
//          "Error in HeapNumber allocation (not 8-byte aligned or tag missing)",
//          scratch2, Operand(zero_reg));
#endif

  // Get heap number map and store it in the allocated object.
  __ LoadRoot(scratch1, Heap::kHeapNumberMapRootIndex);
  __ sw(scratch1, FieldMemOperand(result, HeapObject::kMapOffset));
}


// We fall into this code if the operands were Smis, but the result was
// not (eg. overflow).  We branch into this code (to the not_smi label) if
// the operands were not both Smi.  The operands are in a0 and a1.  In order
// to call the C-implemented binary fp operation routines we need to end up
// with the double precision floating point operands in a0 and a1 (for the
// value in a1) and a2 and a3 (for the value in a0).
static void HandleBinaryOpSlowCases(MacroAssembler* masm,
                                    Label* not_smi,
                                    const Builtins::JavaScript& builtin,
                                    Token::Value operation,
                                    OverwriteMode mode) {
  // TODO(MIPS.1): Implement overflow cases.
  Label slow, do_the_call;
  Label a0_is_smi, a1_is_smi, finished_loading_a0, finished_loading_a1;
  // Smi-smi case (overflow).

  // plind, implement this case....and correct the comments for mips

  // Since both are Smis there is no heap number to overwrite, so allocate.
  // The new heap number is in r5.  r6 and r7 are scratch.
  // We should not meet this case yet, as we do not check for smi-smi overflows
  // in GenericBinaryOpStub::Generate
//  AllocateHeapNumber(masm, &slow, r5, r6, r7);
//  // Write Smi from r0 to r3 and r2 in double format.  r6 is scratch.
//  __ mov(r7, Operand(r0));
//  ConvertToDoubleStub stub1(r3, r2, r7, r6);
//  __ push(lr);
//  __ Call(stub1.GetCode(), RelocInfo::CODE_TARGET);
//  // Write Smi from r1 to r1 and r0 in double format.  r6 is scratch.
//  __ mov(r7, Operand(r1));
//  ConvertToDoubleStub stub2(r1, r0, r7, r6);
//  __ Call(stub2.GetCode(), RelocInfo::CODE_TARGET);
//  __ pop(lr);
//  __ jmp(&do_the_call);  // Tail call.  No return.

  // We jump to here if something goes wrong (one param is not a number of any
  // sort or new-space allocation fails).
  __ bind(&slow);
#ifdef NO_NATIVES
  __ break_(0x00707);   // We should not come here yet.
#else
  __ push(a1);
  __ push(a0);
  __ li(a0, Operand(1));  // Set number of arguments.
//  __ break_(0x5622);
  __ InvokeBuiltin(builtin, JUMP_JS);  // Tail call.  No return.
#endif

  // We branch here if at least one of a0 and a1 is not a Smi.
  // Currently we should always get here. See comment about smi-smi case before.
  __ bind(not_smi);
  if (mode == NO_OVERWRITE) {
    // In the case where there is no chance of an overwritable float we may as
    // well do the allocation immediately while a0 and a1 are untouched.
    AllocateHeapNumber(masm, &slow, t5, t6, t7);
  }

  // Check if a0 is smi or not.
  __ And(t0, a0, Operand(kSmiTagMask));
  __ Branch(eq, &a0_is_smi, t0, Operand(zero_reg));  // It's a Smi so don't check it's a heap number.
  __ GetObjectType(a0, t0, t0);
  __ Branch(ne, &slow, t0, Operand(HEAP_NUMBER_TYPE));
  if (mode == OVERWRITE_RIGHT) {
    __ mov(t5, a0);
  }
  // As we have only 2 arguments which are doubles, so we pass them in f12 (a1)
  // and f14 (a0) coprocessor registers.
  __ ldc1(f14, FieldMemOperand(a0, HeapNumber::kValueOffset));
  __ b(&finished_loading_a0);
  __ nop(); // Branch delay slot nop.
  __ bind(&a0_is_smi);
  if (mode == OVERWRITE_RIGHT) {
    // We can't overwrite a Smi so get address of new heap number into t5.
    AllocateHeapNumber(masm, &slow, t5, t6, t7);
  }
  // We move a0 to coprocessor and convert it to a double.
  __ mtc1(a0, f14);
  __ cvt_d_w(f14, f14);
  __ bind(&finished_loading_a0);


  // Check if a1 is smi or not.
  __ And(t1, a1, Operand(kSmiTagMask));
  __ Branch(eq, &a1_is_smi, t1, Operand(zero_reg));  // It's a Smi so don't check it's a heap number.
  __ GetObjectType(a1, t1, t1);
  __ Branch(ne, &slow, t1, Operand(HEAP_NUMBER_TYPE));
  if (mode == OVERWRITE_LEFT) {
    __ mov(t5, a1);  // Overwrite this heap number.
  }

  __ ldc1(f12, FieldMemOperand(a1, HeapNumber::kValueOffset));
  __ b(&finished_loading_a1);
  __ nop(); // Branch delay slot nop.
  __ bind(&a1_is_smi);
  if (mode == OVERWRITE_LEFT) {
    // We can't overwrite a Smi so get address of new heap number into t5.
    AllocateHeapNumber(masm, &slow, t5, t6, t7);
  }
  // We move a1 to coprocessor and convert it to a double.
  __ mtc1(a1, f12);
  __ cvt_d_w(f12, f12);
  __ bind(&finished_loading_a1);

  __ bind(&do_the_call);
  // f12: left value
  // f14: right value
  // t5: Address of heap number for result.
  __ addiu(sp, sp, -12);
  __ sw(s3, MemOperand(sp, 8));
  __ sw(ra, MemOperand(sp, 4));  // For later
  __ sw(t5, MemOperand(sp, 0));  // Address of heap number that is answer.
  // Call C routine that may not cause GC or other trouble.
  // We need to align sp as we use floating point, so we save it in s3.
  __ li(t9, Operand(ExternalReference::double_fp_operation(operation)));
  __ mov(s3, sp);                   // Save sp
  __ li(t3, Operand(~7));           // Load sp mask
  __ And(sp, sp, Operand(t3));     // Align sp. We use the branch delay slot.
  __ Call(t9);                      // Call the code
  __ Addu(sp, sp, Operand(-StandardFrameConstants::kRArgsSlotsSize));
  __ mov(sp, s3);                   // Restore sp.
  // Store answer in the overwritable heap number.
  __ lw(t5, MemOperand(sp, 0));
  // Store double returned in f0
  __ sdc1(f0, MemOperand(t5, HeapNumber::kValueOffset - kHeapObjectTag));
  // Copy result address to v0
  __ mov(v0, t5);
//  __ break_(0x00109);
  // And we are done.
  __ lw(ra, MemOperand(sp, 4));
  __ lw(s3, MemOperand(sp, 8));
  __ Jump(ra);
  __ addiu(sp, sp, 12); // Restore sp.
}


// For bitwise ops where the inputs are not both Smis we here try to determine
// whether both inputs are either Smis or at least heap numbers that can be
// represented by a 32 bit signed value.  We truncate towards zero as required
// by the ES spec.  If this is the case we do the bitwise op and see if the
// result is a Smi.  If so, great, otherwise we try to find a heap number to
// write the answer into (either by allocating or by overwriting).
// On entry the operands are in r0 and r1.  On exit the answer is in r0.
void GenericBinaryOpStub::HandleNonSmiBitwiseOp(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
  __ break_(0x888); // plind
}


const char* GenericBinaryOpStub::GetName() {
  if (name_ != NULL) return name_;
  const int len = 100;
  name_ = Bootstrapper::AllocateAutoDeletedArray(len);
  if (name_ == NULL) return "OOM";
  const char* op_name = Token::Name(op_);
  const char* overwrite_name;
  switch (mode_) {
    case NO_OVERWRITE: overwrite_name = "Alloc"; break;
    case OVERWRITE_RIGHT: overwrite_name = "OverwriteRight"; break;
    case OVERWRITE_LEFT: overwrite_name = "OverwriteLeft"; break;
    default: overwrite_name = "UnknownOverwrite"; break;
  }

  OS::SNPrintF(Vector<char>(name_, len),
               "GenericBinaryOpStub_%s_%s%s",
               op_name,
               overwrite_name,
               specialized_on_rhs_ ? "_ConstantRhs" : 0);
  return name_;
}


// Can we multiply by x with max two shifts and an add.
// This answers yes to all integers from 2 to 10.
static bool IsEasyToMultiplyBy(int x) {
  if (x < 2) return false;                          // Avoid special cases.
  if (x > (Smi::kMaxValue + 1) >> 2) return false;  // Almost always overflows.
  if (IsPowerOf2(x)) return true;                   // Simple shift.
  if (PopCountLessThanEqual2(x)) return true;       // Shift and add and shift.
  if (IsPowerOf2(x + 1)) return true;               // Patterns like 11111.
  return false;
}


// Can multiply by anything that IsEasyToMultiplyBy returns true for.
// Source and destination may be the same register.  This routine does
// not set carry and overflow the way a mul instruction would.
static void MultiplyByKnownInt(MacroAssembler* masm,
                               Register source,
                               Register destination,
                               int known_int) {
  if (IsPowerOf2(known_int)) {
    __ sll(destination, source, BitPosition(known_int));
  } else if (PopCountLessThanEqual2(known_int)) {
    int first_bit = BitPosition(known_int);
    int second_bit = BitPosition(known_int ^ (1 << first_bit));
    __ sll(t0, source, second_bit - first_bit);
    __ Addu(destination, source, Operand(t0));
    if (first_bit != 0) {
      __ sll(destination, destination, first_bit);
    }
  } else {
    ASSERT(IsPowerOf2(known_int + 1));  // Patterns like 1111.
    int the_bit = BitPosition(known_int + 1);
    __ sll(t0, source, the_bit);
    __ Subu(destination, t0, Operand(source));
  }
}


void GenericBinaryOpStub::Generate(MacroAssembler* masm) {
  // a1 : x
  // a0 : y
  // result : v0

  // All ops need to know whether we are dealing with two Smis.  Set up t2 to
  // tell us that.
  __ Or(t2, a1, Operand(a0));  // t2 = x | y;

  switch (op_) {
    case Token::ADD: {
      Label not_smi;
      // Fast path.
      ASSERT(kSmiTag == 0);  // Adjust code below.
      __ And(t3, t2, Operand(kSmiTagMask));
      __ Branch(ne, &not_smi, t3, Operand(zero_reg));
      __ Addu(v0, a1, Operand(a0));  // Add Y Optimistically.
      __ Ret();

      HandleBinaryOpSlowCases(masm,
                              &not_smi,
                              Builtins::ADD,
                              Token::ADD,
                              mode_);
      break;
    }

    case Token::SUB: {
      PrintF("(plind) dude, we stumbled upon a SUB ....\n");
      Label not_smi;
      // Fast path.
      ASSERT(kSmiTag == 0);  // Adjust code below.
      __ And(t3, t2, Operand(kSmiTagMask));
      __ Branch(ne, &not_smi, t3, Operand(zero_reg));
      __ Subu(v0, a1, Operand(a0));  // Subtract y optimistically.
      __ Ret();
      HandleBinaryOpSlowCases(masm,
                              &not_smi,
                              Builtins::SUB,
                              Token::SUB,
                              mode_);
      break;
    }

    case Token::MUL: {
      Label not_smi;
      ASSERT(kSmiTag == 0);  // Adjust code below.
      __ And(t3, t2, Operand(kSmiTagMask));
      __ Branch(ne, &not_smi, t3, Operand(zero_reg));
      // Remove tag from one operand (but keep sign), so that result is Smi.
      __ sra(t0, a0, kSmiTagSize);
      // Do multiplication.              .................... plind - add overflow det per arm code
      __ Mul(v0, a1, Operand(t0));
      __ Ret();

      HandleBinaryOpSlowCases(masm,
                              &not_smi,
                              Builtins::MUL,
                              Token::MUL,
                              mode_);
      break;
    }

    case Token::DIV: {
      Label not_smi;
      ASSERT(kSmiTag == 0);  // Adjust code below.
      __ And(t3, t2, Operand(kSmiTagMask));
      __ Branch(ne, &not_smi, t3, Operand(zero_reg));
      // Remove tags.
      __ sra(t0, a0, kSmiTagSize);
      __ sra(t1, a1, kSmiTagSize);
      // Divide.                      .................... plind - add overflow det per arm code
      __ Div(t1, Operand(t0));
      __ mflo(v0);
      __ sll(v0, v0, 1);
      __ Ret();

      HandleBinaryOpSlowCases(masm,
                              &not_smi,
                              op_ == Token::MOD ? Builtins::MOD : Builtins::DIV,
                              op_,
                              mode_);
      break;
    }

    case Token::MOD: {
      Label not_smi;
      ASSERT(kSmiTag == 0);  // Adjust code below.
      __ And(t3, t2, Operand(kSmiTagMask));
      __ Branch(ne, &not_smi, t3, Operand(zero_reg));
      // Remove tag from one operand (but keep sign), so that result is Smi.
      __ sra(t0, a0, kSmiTagSize);
      __ Div(a1, Operand(a0));
      __ mfhi(v0);
      __ Ret();

      HandleBinaryOpSlowCases(masm,
                              &not_smi,
                              op_ == Token::MOD ? Builtins::MOD : Builtins::DIV,
                              op_,
                              mode_);
      break;
    }

    case Token::BIT_OR:
    case Token::BIT_AND:
    case Token::BIT_XOR:
    case Token::SAR:
    case Token::SHR:
    case Token::SHL: {
      Label slow;
      ASSERT(kSmiTag == 0);  // Adjust code below.
      __ And(t3, t2, Operand(kSmiTagMask));
      __ Branch(ne, &slow, t3, Operand(zero_reg));
      switch (op_) {
        case Token::BIT_OR:  __ Or(v0, a0, Operand(a1)); break;
        case Token::BIT_AND: __ And(v0, a0, Operand(a1)); break;
        case Token::BIT_XOR: __ Xor(v0, a0, Operand(a1)); break;
        case Token::SAR:
          // Remove tags from operands.
          __ sra(a2, a0, kSmiTagSize);
          __ sra(a3, a1, kSmiTagSize);
          // Shift.
          __ srav(v0, a3, a2);
          // Smi tag result.
          __ sll(v0, v0, kSmiTagMask);
          break;
        case Token::SHR:
          // Remove tags from operands.
          __ sra(a2, a0, kSmiTagSize);
          __ sra(a3, a1, kSmiTagSize);
          // Shift.
          __ srlv(v0, a3, a2);
          // Unsigned shift is not allowed to produce a negative number, so
          // check the sign bit and the sign bit after Smi tagging.
          __ And(t3, v0, Operand(0xc0000000));
          __ Branch(ne, &slow, t3, Operand(zero_reg));
          // Smi tag result.
          __ sll(v0, v0, kSmiTagMask);
          break;
        case Token::SHL:
          // Remove tags from operands.
          __ sra(a2, a0, kSmiTagSize);
          __ sra(a3, a1, kSmiTagSize);
          // Shift
          __ sllv(v0, a3, a2);
          // Check that the signed result fits in a Smi.
          __ Add(t3, v0, Operand(0x40000000));
          __ And(t3, t3, Operand(0x80000000));
          __ Branch(ne, &slow, t3, Operand(zero_reg));
          // Smi tag result.
          __ sll(v0, v0, kSmiTagMask);
          break;
        default: UNREACHABLE();
      }
      __ Ret();
      __ bind(&slow);
      HandleNonSmiBitwiseOp(masm);
      break;
    }

    default: UNREACHABLE();
  }
  // This code should be unreachable.
  __ stop("Unreachable");
}

#undef __

} }  // namespace v8::internal
