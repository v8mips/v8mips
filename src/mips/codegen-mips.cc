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

#include "bootstrapper.h"
#include "codegen-inl.h"
#include "compiler.h"
#include "debug.h"
#include "ic-inl.h"
#include "jsregexp.h"
#include "jump-target-light-inl.h"
#include "parser.h"
#include "regexp-macro-assembler.h"
#include "regexp-stack.h"
#include "register-allocator-inl.h"
#include "runtime.h"
#include "scopes.h"
#include "virtual-frame-inl.h"
#include "virtual-frame-mips-inl.h"

namespace v8 {
namespace internal {


static void EmitIdenticalObjectComparison(MacroAssembler* masm,
                                          Label* slow,
                                          Condition cc,
                                          bool never_nan_nan);
static void EmitSmiNonsmiComparison(MacroAssembler* masm,
                                    Label* rhs_not_nan,
                                    Label* slow,
                                    bool strict);
static void EmitTwoNonNanDoubleComparison(MacroAssembler* masm, Condition cc);
static void EmitStrictTwoHeapObjectCompare(MacroAssembler* masm);
static void MultiplyByKnownInt(MacroAssembler* masm,
                               Register source,
                               Register destination,
                               int known_int);
static bool IsEasyToMultiplyBy(int x);


#define __ ACCESS_MASM(masm_)

// -------------------------------------------------------------------------
// Platform-specific DeferredCode functions.

void DeferredCode::SaveRegisters() {
  // On MIPS you either have a completely spilled frame or you
  // handle it yourself, but at the moment there's no automation
  // of registers and deferred code.
}


void DeferredCode::RestoreRegisters() {
}


// -------------------------------------------------------------------------
// Platform-specific RuntimeCallHelper functions.

void VirtualFrameRuntimeCallHelper::BeforeCall(MacroAssembler* masm) const {
  frame_state_->frame()->AssertIsSpilled();
}


void VirtualFrameRuntimeCallHelper::AfterCall(MacroAssembler* masm) const {
}


void ICRuntimeCallHelper::BeforeCall(MacroAssembler* masm) const {
  masm->EnterInternalFrame();
}


void ICRuntimeCallHelper::AfterCall(MacroAssembler* masm) const {
  masm->LeaveInternalFrame();
}


// -----------------------------------------------------------------------------
// CodeGenState implementation.

CodeGenState::CodeGenState(CodeGenerator* owner)
    : owner_(owner),
      previous_(owner->state()) {
  owner->set_state(this);
}


ConditionCodeGenState::ConditionCodeGenState(CodeGenerator* owner,
                           JumpTarget* true_target,
                           JumpTarget* false_target)
    : CodeGenState(owner),
      true_target_(true_target),
      false_target_(false_target) {
  owner->set_state(this);
}


TypeInfoCodeGenState::TypeInfoCodeGenState(CodeGenerator* owner,
                                           Slot* slot,
                                           TypeInfo type_info)
    : CodeGenState(owner),
      slot_(slot) {
  owner->set_state(this);
  old_type_info_ = owner->set_type_info(slot, type_info);
}


CodeGenState::~CodeGenState() {
  ASSERT(owner_->state() == this);
  owner_->set_state(previous_);
}


TypeInfoCodeGenState::~TypeInfoCodeGenState() {
  owner()->set_type_info(slot_, old_type_info_);
}

// -----------------------------------------------------------------------------
// CodeGenerator implementation.

CodeGenerator::CodeGenerator(MacroAssembler* masm)
    : deferred_(8),
      masm_(masm),
      info_(NULL),
      frame_(NULL),
      allocator_(NULL),
      cc_reg_(cc_always),
      state_(NULL),
      loop_nesting_(0),
      type_info_(NULL),
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
  int slots = scope()->num_parameters() + scope()->num_stack_slots();
  ScopedVector<TypeInfo> type_info_array(slots);
  type_info_ = &type_info_array;
  ASSERT(allocator_ == NULL);
  RegisterAllocator register_allocator(this);
  allocator_ = &register_allocator;
  ASSERT(frame_ == NULL);
  frame_ = new VirtualFrame();
  cc_reg_ = cc_always;

  // Adjust for function-level loop nesting.
  ASSERT_EQ(0, loop_nesting_);
  loop_nesting_ = info->loop_nesting();

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

#ifdef DEBUG
    if (strlen(FLAG_stop_at) > 0 &&
        info->function()->name()->IsEqualTo(CStrVector(FLAG_stop_at))) {
      frame_->SpillAll();
      __ stop("stop-at");
    }
#endif

    // Arm codegen supports secondary mode, which mips doesn't support yet.
    // For now, make sure we're always called as primary.
    ASSERT(info->mode() == CompilationInfo::PRIMARY);
    frame_->Enter();

    // Allocate space for locals and initialize them.
    frame_->AllocateStackSlots();

    VirtualFrame::SpilledScope spilled_scope(frame_);
    int heap_slots = scope()->num_heap_slots() - Context::MIN_CONTEXT_SLOTS;
    if (heap_slots > 0) {
      // Allocate local context.
      // Get outer context and create a new context based on it.
      __ lw(a0, frame_->Function());
      frame_->EmitPush(a0);
      if (heap_slots <= FastNewContextStub::kMaximumSlots) {
        FastNewContextStub stub(heap_slots);
        frame_->CallStub(&stub, 1);
      } else {
        frame_->CallRuntime(Runtime::kNewContext, 1);  // v0 holds the result
      }

#ifdef DEBUG
      JumpTarget verified_true;
      verified_true.Branch(eq, v0, Operand(cp), no_hint);
      __ stop("NewContext: v0 is expected to be the same as cp");
      verified_true.Bind();
#endif
      // Update context local.
      __ sw(cp, frame_->Context());
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
          __ RecordWrite(a2, Operand(slot_offset), a3, a1);
        }
      }
    }

    // Store the arguments object.  This must happen after context
    // initialization because the arguments object may be stored in
    // the context.
    if (ArgumentsMode() != NO_ARGUMENTS_ALLOCATION) {
      StoreArgumentsObject(true);
    }

    // Initialize ThisFunction reference if present.
    if (scope()->is_function_scope() && scope()->function() != NULL) {
      __ li(t0, Operand(Factory::the_hole_value()));
      frame_->EmitPush(t0);
      StoreToSlot(scope()->function()->slot(), NOT_CONST_INIT);
    }

    // Initialize the function return target after the locals are set
    // up, because it needs the expected frame height from the frame.
    function_return_.SetExpectedHeight();
    function_return_is_shadowed_ = false;

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
      frame_->CallRuntime(Runtime::kTraceEnter, 0);
      // Ignore the return value.
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
        frame_->CallRuntime(Runtime::kDebugTrace, 0);
        // Ignore the return value.
      }
#endif
      VisitStatements(info->function()->body());
    }
  }

  // Handle the return from the function.
  if (has_valid_frame()) {
    // If there is a valid frame, control flow can fall off the end of
    // the body.  In that case there is an implicit return statement.
    ASSERT(!function_return_is_shadowed_);
    frame_->PrepareForReturn();
    __ LoadRoot(v0, Heap::kUndefinedValueRootIndex);
    if (function_return_.is_bound()) {
      function_return_.Jump();
    } else {
      function_return_.Bind();
      GenerateReturnSequence();
    }
  } else if (function_return_.is_linked()) {
    // If the return target has dangling jumps to it, then we have not
    // yet generated the return sequence.  This can happen when (a)
    // control does not flow off the end of the body so we did not
    // compile an artificial return statement just above, and (b) there
    // are return statements in the body but (c) they are all shadowed.
    function_return_.Bind();
    GenerateReturnSequence();
  }

  // Adjust for function-level loop nesting.
  ASSERT(loop_nesting_ == info->loop_nesting());
  loop_nesting_ = 0;

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
  Comment cmnt(masm_, "[ LoadReference");
  Expression* e = ref->expression();
  Property* property = e->AsProperty();
  Variable* var = e->AsVariableProxy()->AsVariable();

  if (property != NULL) {
    // The expression is either a property or a variable proxy that rewrites
    // to a property.
    Load(property->obj());
    if (property->key()->IsPropertyName()) {
      ref->set_type(Reference::NAMED);
    } else {
      Load(property->key());
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
    // Anything else is a runtime error.
    Load(e);
    frame_->CallRuntime(Runtime::kThrowReferenceError, 1);
  }
}


void CodeGenerator::UnloadReference(Reference* ref) {
  int size = ref->size();
  ref->set_unloaded();
  if (size == 0) return;

  // Pop a reference from the stack while preserving TOS.
  VirtualFrame::RegisterAllocationScope scope(this);
  Comment cmnt(masm_, "[ UnloadReference");
  if (size > 0) {
    Register tos = frame_->PopToRegister();
    frame_->Drop(size);
    frame_->EmitPush(tos);
  }
}

int CodeGenerator::NumberOfSlot(Slot* slot) {
  if (slot == NULL) return kInvalidSlotNumber;
  switch (slot->type()) {
    case Slot::PARAMETER:
      return slot->index();
    case Slot::LOCAL:
      return slot->index() + scope()->num_parameters();
    default:
      break;
  }
  return kInvalidSlotNumber;
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
      ASSERT(!tmp.is(cp));  // Do not overwrite context register.
      Register context = cp;
      int chain_length = scope()->ContextChainLength(slot->var()->scope());
      for (int i = 0; i < chain_length; i++) {
        // Load the closure.
        // (All contexts, even 'with' contexts, have a closure,
        // and it is the same for all contexts inside a function.
        // There is no need to go to the function context first.)
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


MemOperand CodeGenerator::ContextSlotOperandCheckExtensions(
    Slot* slot,
    Register tmp,
    Register tmp2,
    JumpTarget* slow) {
  ASSERT(slot->type() == Slot::CONTEXT);
  Register context = cp;

  for (Scope* s = scope(); s != slot->var()->scope(); s = s->outer_scope()) {
    if (s->num_heap_slots() > 0) {
      if (s->calls_eval()) {
        // Check that extension is NULL.
        __ lw(tmp2, ContextOperand(context, Context::EXTENSION_INDEX));
        slow->Branch(ne, tmp2, Operand(zero_reg));
      }
      __ lw(tmp, ContextOperand(context, Context::CLOSURE_INDEX));
      __ lw(tmp, FieldMemOperand(tmp, JSFunction::kContextOffset));
      context = tmp;
    }
  }
  // Check that last extension is NULL.
  __ lw(tmp2, ContextOperand(context, Context::EXTENSION_INDEX));
  slow->Branch(ne, tmp2, Operand(zero_reg));
  __ lw(tmp, ContextOperand(context, Context::FCONTEXT_INDEX));
  return ContextOperand(tmp, slot->index());
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

  { ConditionCodeGenState new_state(this, true_target, false_target);
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
      frame_->SpillAll();
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
    VirtualFrame::SpilledScope scope(frame_);
    JumpTarget loaded;
    JumpTarget materialize_true;

    materialize_true.Branch(cc_reg_, condReg1, Operand(condReg2));
    __ LoadRoot(v0, Heap::kFalseValueRootIndex);
    frame_->EmitPush(v0);
    loaded.Jump();
    materialize_true.Bind();
    __ LoadRoot(v0, Heap::kTrueValueRootIndex);
    frame_->EmitPush(v0);
    loaded.Bind();
    cc_reg_ = cc_always;
  }

  if (true_target.is_linked() || false_target.is_linked()) {
    VirtualFrame::SpilledScope scope(frame_);
    // We have at least one condition value that has been "translated"
    // into a branch, thus it needs to be loaded explicitly.
    JumpTarget loaded;
    if (frame_ != NULL) {
      loaded.Jump();  // Don't lose the current TOS.
    }
    bool both = true_target.is_linked() && false_target.is_linked();
    // Load "true" if necessary.
    if (true_target.is_linked()) {
      true_target.Bind();
      __ LoadRoot(v0, Heap::kTrueValueRootIndex);
      frame_->EmitPush(v0);
    }
    // If both "true" and "false" need to be loaded jump across the code for
    // "false".
    if (both) {
      loaded.Jump();
    }
    // Load "false" if necessary.
    if (false_target.is_linked()) {
      false_target.Bind();
      __ LoadRoot(v0, Heap::kFalseValueRootIndex);
      frame_->EmitPush(v0);
    }
    // A value is loaded on all paths reaching this point.
    loaded.Bind();
  }
  ASSERT(has_valid_frame());
  ASSERT(!has_cc());
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::LoadGlobal() {
  Register reg = frame_->GetTOSRegister();
  __ lw(reg, GlobalObject());
  frame_->EmitPush(reg);
}


void CodeGenerator::LoadGlobalReceiver(Register scratch) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  __ lw(scratch, ContextOperand(cp, Context::GLOBAL_INDEX));
  __ lw(scratch,
         FieldMemOperand(scratch, GlobalObject::kGlobalReceiverOffset));
  frame_->EmitPush(scratch);
}


ArgumentsAllocationMode CodeGenerator::ArgumentsMode() {
  if (scope()->arguments() == NULL) return NO_ARGUMENTS_ALLOCATION;
  ASSERT(scope()->arguments_shadow() != NULL);
  // We don't want to do lazy arguments allocation for functions that
  // have heap-allocated contexts, because it interfers with the
  // uninitialized const tracking in the context objects.
  return (scope()->num_heap_slots() > 0)
      ? EAGER_ARGUMENTS_ALLOCATION
      : LAZY_ARGUMENTS_ALLOCATION;
}

void CodeGenerator::StoreArgumentsObject(bool initial) {
  VirtualFrame::SpilledScope spilled_scope(frame_);

  ArgumentsAllocationMode mode = ArgumentsMode();
  ASSERT(mode != NO_ARGUMENTS_ALLOCATION);

  Comment cmnt(masm_, "[ store arguments object");
  if (mode == LAZY_ARGUMENTS_ALLOCATION && initial) {
    // When using lazy arguments allocation, we store the hole value
    // as a sentinel indicating that the arguments object hasn't been
    // allocated yet.
    __ LoadRoot(a1, Heap::kTheHoleValueRootIndex);
    frame_->EmitPush(a1);
  } else {
    ArgumentsAccessStub stub(ArgumentsAccessStub::NEW_OBJECT);
    __ lw(a2, frame_->Function());
    // The receiver is below the arguments, the return address, and the
    // frame pointer on the stack.
    const int kReceiverDisplacement = 2 + scope()->num_parameters();
    __ Addu(a1, fp, Operand(kReceiverDisplacement * kPointerSize));
    __ li(a0, Operand(Smi::FromInt(scope()->num_parameters())));
    frame_->Adjust(3);
    __ Push(a2, a1, a0);
    frame_->CallStub(&stub, 3);
    frame_->EmitPush(v0);
  }

  Variable* arguments = scope()->arguments()->var();
  Variable* shadow = scope()->arguments_shadow()->var();
  ASSERT(arguments != NULL && arguments->slot() != NULL);
  ASSERT(shadow != NULL && shadow->slot() != NULL);
  JumpTarget done;
  if (mode == LAZY_ARGUMENTS_ALLOCATION && !initial) {
    // We have to skip storing into the arguments slot if it has
    // already been written to. This can happen if the a function
    // has a local variable named 'arguments'.
    LoadFromSlot(scope()->arguments()->var()->slot(), NOT_INSIDE_TYPEOF);
    frame_->EmitPop(a0);
    __ LoadRoot(a1, Heap::kTheHoleValueRootIndex);
    done.Branch(ne, a0, Operand(a1));
  }
  StoreToSlot(arguments->slot(), NOT_CONST_INIT);
  if (mode == LAZY_ARGUMENTS_ALLOCATION) done.Bind();
  StoreToSlot(shadow->slot(), NOT_CONST_INIT);
}

void CodeGenerator::LoadTypeofExpression(Expression* x) {
  // Special handling of identifiers as subxessions of typeof.
  Variable* variable = x->AsVariableProxy()->AsVariable();
  if (variable != NULL && !variable->is_this() && variable->is_global()) {
    // For a global variable we build the property reference
    // <global>.<variable> and perform a (regular non-contextual) property
    // load to make sure we do not get reference errors.
    Slot global(variable, Slot::CONTEXT, Context::GLOBAL_INDEX);
    Literal key(variable->name());
    Property property(&global, &key, RelocInfo::kNoPosition);
    Reference ref(this, &property);
    ref.GetValue();
  } else if (variable != NULL && variable->slot() != NULL) {
    // For a variable that rewrites to a slot, we signal it is the immediate
    // subxession of a typeof.
    LoadFromSlotCheckForArguments(variable->slot(), INSIDE_TYPEOF);
  } else {
    // Anything else can be handled normally.
    Load(x);
  }
}


void CodeGenerator::LoadFromSlot(Slot* slot, TypeofState typeof_state) {
  if (slot->type() == Slot::LOOKUP) {
    ASSERT(slot->var()->is_dynamic());

    // JumpTargets do not yet support merging frames so the frame must be
    // spilled when jumping to these targets.
    JumpTarget slow;
    JumpTarget done;

    // Generate fast case for loading from slots that correspond to
    // local/global variables or arguments unless they are shadowed by
    // eval-introduced bindings.
    EmitDynamicLoadFromSlotFastCase(slot,
                                    typeof_state,
                                    &slow,
                                    &done);

    slow.Bind();
    VirtualFrame::SpilledScope spilled_scope(frame_);
    frame_->EmitPush(cp);
    __ li(v0, Operand(slot->var()->name()));
    frame_->EmitPush(v0);

    if (typeof_state == INSIDE_TYPEOF) {
      frame_->CallRuntime(Runtime::kLoadContextSlotNoReferenceError, 2);
    } else {
      frame_->CallRuntime(Runtime::kLoadContextSlot, 2);
    }
    done.Bind();
    frame_->EmitPush(v0);

  } else {
    Register scratch0 = VirtualFrame::scratch0();
    Register scratch1 = VirtualFrame::scratch1();
    Register scratch2 = VirtualFrame::scratch2();
    TypeInfo info = type_info(slot);
    __ lw(v0, SlotOperand(slot, scratch2));
    frame_->EmitPush(v0, info);
    if (slot->var()->mode() == Variable::CONST) {
      // Const slots may contain 'the hole' value (the constant hasn't been
      // initialized yet) which needs to be converted into the 'undefined'
      // value.
      Comment cmnt(masm_, "[ Unhole const");
      frame_->EmitPop(scratch2);
      __ LoadRoot(scratch0, Heap::kTheHoleValueRootIndex);
      __ subu(scratch1, scratch2, scratch0);
      __ LoadRoot(scratch2, Heap::kUndefinedValueRootIndex);
      __ movz(v0, scratch2, scratch1);  // Conditional move if v0 was the hole.
      frame_->EmitPush(v0);
    }
  }
}

void CodeGenerator::LoadFromSlotCheckForArguments(Slot* slot,
                                                  TypeofState state) {
  LoadFromSlot(slot, state);

  // Bail out quickly if we're not using lazy arguments allocation.
  if (ArgumentsMode() != LAZY_ARGUMENTS_ALLOCATION) return;

  // ... or if the slot isn't a non-parameter arguments slot.
  if (slot->type() == Slot::PARAMETER || !slot->is_arguments()) return;

  VirtualFrame::SpilledScope spilled_scope(frame_);

  // Load the loaded value from the stack into v0 but leave it on the
  // stack.
  __ lw(v0, MemOperand(sp, 0));

  // If the loaded value is the sentinel that indicates that we
  // haven't loaded the arguments object yet, we need to do it now.
  JumpTarget exit;
  __ LoadRoot(a1, Heap::kTheHoleValueRootIndex);
  exit.Branch(ne, v0, Operand(a1));
  frame_->Drop();
  StoreArgumentsObject(false);
  exit.Bind();
}

void CodeGenerator::LoadFromGlobalSlotCheckExtensions(Slot* slot,
                                                      TypeofState typeof_state,
                                                      JumpTarget* slow) {
  // Check that no extension objects have been created by calls to
  // eval from the current scope to the global scope.
  Register tmp = frame_->scratch0();
  Register tmp2 = frame_->scratch1();
  Register context = cp;
  Scope* s = scope();
  while (s != NULL) {
    if (s->num_heap_slots() > 0) {
      if (s->calls_eval()) {
        frame_->SpillAll();
        // Check that extension is NULL.
        __ lw(tmp2, ContextOperand(context, Context::EXTENSION_INDEX));
        slow->Branch(ne, tmp2, Operand(zero_reg));
      }
      // Load next context in chain.
      __ lw(tmp, ContextOperand(context, Context::CLOSURE_INDEX));
      __ lw(tmp, FieldMemOperand(tmp, JSFunction::kContextOffset));
      context = tmp;
    }
    // If no outer scope calls eval, we do not need to check more
    // context extensions.
    if (!s->outer_scope_calls_eval() || s->is_eval_scope()) break;
    s = s->outer_scope();
  }

  if (s->is_eval_scope()) {
    frame_->SpillAll();
    Label next, fast;
    __ mov(tmp, context);
    __ bind(&next);
    // Terminate at global context.
    __ lw(tmp2, FieldMemOperand(tmp, HeapObject::kMapOffset));
    __ LoadRoot(t8, Heap::kGlobalContextMapRootIndex);
    __ Branch(&fast, eq, tmp2, Operand(t8));
    // Check that extension is NULL.
    __ lw(tmp2, ContextOperand(tmp, Context::EXTENSION_INDEX));
    slow->Branch(ne, tmp2, Operand(zero_reg));
    // Load next context in chain.
    __ lw(tmp, ContextOperand(tmp, Context::CLOSURE_INDEX));
    __ lw(tmp, FieldMemOperand(tmp, JSFunction::kContextOffset));
    __ jmp(&next);
    __ bind(&fast);
  }

  // Load the global object.
  LoadGlobal();
  // Setup the name register and call load IC.
  frame_->CallLoadIC(slot->var()->name(),
                     typeof_state == INSIDE_TYPEOF
                         ? RelocInfo::CODE_TARGET
                         : RelocInfo::CODE_TARGET_CONTEXT);
}


void CodeGenerator::EmitDynamicLoadFromSlotFastCase(Slot* slot,
                                                    TypeofState typeof_state,
                                                    JumpTarget* slow,
                                                    JumpTarget* done) {
  // Generate fast-case code for variables that might be shadowed by
  // eval-introduced variables.  Eval is used a lot without
  // introducing variables.  In those cases, we do not want to
  // perform a runtime call for all variables in the scope
  // containing the eval.
  if (slot->var()->mode() == Variable::DYNAMIC_GLOBAL) {
    LoadFromGlobalSlotCheckExtensions(slot, typeof_state, slow);
    frame_->SpillAll();
    done->Jump();

  } else if (slot->var()->mode() == Variable::DYNAMIC_LOCAL) {
    frame_->SpillAll();
    Slot* potential_slot = slot->var()->local_if_not_shadowed()->slot();
    Expression* rewrite = slot->var()->local_if_not_shadowed()->rewrite();
    if (potential_slot != NULL) {
      // Generate fast case for locals that rewrite to slots.
      __ lw(v0,
             ContextSlotOperandCheckExtensions(potential_slot,
                                               a1,
                                               a2,
                                               slow));
      if (potential_slot->var()->mode() == Variable::CONST) {
        __ LoadRoot(a1, Heap::kTheHoleValueRootIndex);
        __ subu(a1, v0, a1);  // Leave 0 in a1 on equal.
        __ LoadRoot(a0, Heap::kUndefinedValueRootIndex);
        __ movz(v0, a0, a1);  // Cond move Undef if v0 was 'the hole'.
      }
      done->Jump();
    } else if (rewrite != NULL) {
      // Generate fast case for argument loads.
      Property* property = rewrite->AsProperty();
      if (property != NULL) {
        VariableProxy* obj_proxy = property->obj()->AsVariableProxy();
        Literal* key_literal = property->key()->AsLiteral();
        if (obj_proxy != NULL &&
            key_literal != NULL &&
            obj_proxy->IsArguments() &&
            key_literal->handle()->IsSmi()) {
          // Load arguments object if there are no eval-introduced
          // variables. Then load the argument from the arguments
          // object using keyed load.
          __ lw(a0,
                 ContextSlotOperandCheckExtensions(obj_proxy->var()->slot(),
                                                   a1,
                                                   a2,
                                                   slow));
          frame_->EmitPush(a0);
          __ li(a1, Operand(key_literal->handle()));
          frame_->EmitPush(a1);
          EmitKeyedLoad();
          done->Jump();
        }
      }
    }
  }
}


void CodeGenerator::StoreToSlot(Slot* slot, InitState init_state) {
  ASSERT(slot != NULL);
  if (slot->type() == Slot::LOOKUP) {
    VirtualFrame::SpilledScope spilled_scope(frame_);
    ASSERT(slot->var()->is_dynamic());

    // For now, just do a runtime call.
    frame_->EmitPush(cp);
    __ li(a0, Operand(slot->var()->name()));
    frame_->EmitPush(a0);

    if (init_state == CONST_INIT) {
      // Same as the case for a normal store, but ignores attribute
      // (e.g. READ_ONLY) of context slot so that we can initialize
      // const properties (introduced via eval("const foo = (some
      // expr);")). Also, uses the current function context instead of
      // the top context.
      //
      // Note that we must declare the foo upon entry of eval(), via a
      // context slot declaration, but we cannot initialize it at the
      // same time, because the const declaration may be at the end of
      // the eval code (sigh...) and the const variable may have been
      // used before (where its value is 'undefined'). Thus, we can only
      // do the initialization when we actually encounter the expression
      // and when the expression operands are defined and valid, and
      // thus we need the split into 2 operations: declaration of the
      // context slot followed by initialization.
      frame_->CallRuntime(Runtime::kInitializeConstContextSlot, 3);
    } else {
      frame_->CallRuntime(Runtime::kStoreContextSlot, 3);
    }
    // Storing a variable must keep the (new) value on the expression
    // stack. This is necessary for compiling assignment expressions.
    frame_->EmitPush(v0);

  } else {
    ASSERT(!slot->var()->is_dynamic());
    Register scratch = VirtualFrame::scratch0();
    Register scratch2 = VirtualFrame::scratch1();
    Register scratch3 = VirtualFrame::scratch2();
    VirtualFrame::RegisterAllocationScope scope(this);

    // The frame must be spilled when branching to this target.
    JumpTarget exit;
    if (init_state == CONST_INIT) {
      ASSERT(slot->var()->mode() == Variable::CONST);
      // Only the first const initialization must be executed (the slot
      // still contains 'the hole' value). When the assignment is
      // executed, the code is identical to a normal store (see below).
      Comment cmnt(masm_, "[ Init const");
      __ lw(scratch, SlotOperand(slot, scratch));
      __ LoadRoot(scratch2, Heap::kTheHoleValueRootIndex);
      frame_->SpillAll();
      exit.Branch(ne, scratch, Operand(scratch2));
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
    Register tos = frame_->Peek();
    __ sw(tos, SlotOperand(slot, scratch));
    if (slot->type() == Slot::CONTEXT) {
      // Skip write barrier if the written value is a smi.
      __ And(scratch2, tos, Operand(kSmiTagMask));
      // We don't use tos any more after here.
      VirtualFrame::SpilledScope spilled_scope(frame_);
      exit.Branch(eq, scratch2, Operand(zero_reg));
      // scratch is loaded with context when calling SlotOperand above.
      int offset = FixedArray::kHeaderSize + slot->index() * kPointerSize;
      __ RecordWrite(scratch, Operand(offset), scratch3, scratch2);
    }
    // If we definitely did not jump over the assignment, we do not need
    // to bind the exit label. Doing so can defeat peephole
    // optimization.
    if (init_state == CONST_INIT || slot->type() == Slot::CONTEXT) {
      frame_->SpillAll();
      exit.Bind();
    }
  }
}


// ECMA-262, section 9.2, page 30: ToBoolean(). Convert the given
// register to a boolean in the condition code register. The code
// may jump to 'false_target' in case the register converts to 'false'.
void CodeGenerator::ToBoolean(JumpTarget* true_target,
                              JumpTarget* false_target) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
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
                                           GenerateInlineSmi inline_smi,
                                           int constant_rhs) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  // sp[0] : y
  // sp[1] : x
  // result : v0

  // Stub is entered with a call: 'return address' is in lr.
  switch (op) {
    case Token::ADD:  // Fall through.
    case Token::SUB:  // Fall through.
      if (inline_smi) {
        JumpTarget done;
        JumpTarget not_smi;
        Register rhs = frame_->PopToRegister();
        Register lhs = frame_->PopToRegister(rhs);
        Register scratch = VirtualFrame::scratch0();
        __ Or(scratch, rhs, Operand(lhs));
        // Check they are both small and positive.
        __ And(scratch, scratch, Operand(kSmiTagMask | 0xc0000000));
        not_smi.Branch(ne, scratch, Operand(zero_reg));
        ASSERT(rhs.is(a0) || lhs.is(a0));  // r0 is free now.
        ASSERT_EQ(0, kSmiTag);
        if (op == Token::ADD) {
          __ Addu(v0, lhs, Operand(rhs));
        } else {
          __ Subu(v0, lhs, Operand(rhs));
        }
        done.Branch(eq, scratch, Operand(zero_reg));
        not_smi.Bind();
        GenericBinaryOpStub stub(op, overwrite_mode, lhs, rhs, constant_rhs);
        frame_->SpillAll();
        frame_->CallStub(&stub, 0);
        done.Bind();
        frame_->EmitPush(v0);
        break;
      } else {
        // Fall through!
      }

    case Token::BIT_OR:
    case Token::BIT_AND:
    case Token::BIT_XOR:
      if (inline_smi) {
        JumpTarget not_smi;
        bool rhs_is_smi = frame_->KnownSmiAt(0);
        bool lhs_is_smi = frame_->KnownSmiAt(1);
        Register rhs = frame_->PopToRegister();
        Register lhs = frame_->PopToRegister(rhs);
        Register smi_test_reg;
        Register scratch = VirtualFrame::scratch0();
        Condition cond;
        if (!rhs_is_smi || !lhs_is_smi) {
          if (rhs_is_smi) {
            smi_test_reg = lhs;
          } else if (lhs_is_smi) {
            smi_test_reg = rhs;
          } else {
            smi_test_reg = VirtualFrame::scratch0();
            __ Or(smi_test_reg, rhs, Operand(lhs));
          }
          // Check they are both Smis.
          __ And(scratch, smi_test_reg, Operand(kSmiTagMask));
          cond = eq;
          not_smi.Branch(ne, scratch, Operand(zero_reg));
        } else {
          cond = al;
        }
        ASSERT(rhs.is(a0) || lhs.is(a0));  // r0 is free now.
        if (op == Token::BIT_OR) {
          __ or_(v0, lhs, rhs);
        } else if (op == Token::BIT_AND) {
          __ and_(v0, lhs, rhs);
        } else {
          ASSERT(op == Token::BIT_XOR);
          ASSERT_EQ(0, kSmiTag);
          __ xor_(v0, lhs, rhs);
        }
        not_smi.Bind();
        if (cond != al) {
          JumpTarget done;
          done.Branch(cond, scratch, Operand(zero_reg));
          GenericBinaryOpStub stub(op, overwrite_mode, lhs, rhs, constant_rhs);
          frame_->SpillAll();
          frame_->CallStub(&stub, 0);
          done.Bind();
        }
        frame_->EmitPush(v0);
        break;
      } else {
        // Fall through!
      }
    case Token::MUL:
    case Token::DIV:
    case Token::MOD:
    case Token::SHL:
    case Token::SHR:
    case Token::SAR: {
      Register rhs = frame_->PopToRegister();
      Register lhs = frame_->PopToRegister(rhs);  // Don't pop to rhs register.
      GenericBinaryOpStub stub(op, overwrite_mode, lhs, rhs, constant_rhs);
      frame_->SpillAll();
      frame_->CallStub(&stub, 0);
      frame_->EmitPush(v0);
      break;
    }

    case Token::COMMA: {
      Register scratch = frame_->PopToRegister();
      // Simply discard left value.
      frame_->Drop();
      frame_->EmitPush(scratch);
      break;
    }

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
                             OverwriteMode overwrite_mode,
                             Register tos)
      : op_(op),
        value_(value),
        reversed_(reversed),
        overwrite_mode_(overwrite_mode),
        tos_register_(tos) {
    set_comment("[ DeferredInlinedSmiOperation");
  }

  virtual void Generate();

 private:
  Token::Value op_;
  int value_;
  bool reversed_;
  OverwriteMode overwrite_mode_;
  Register tos_register_;
};


void DeferredInlineSmiOperation::Generate() {
  // In CodeGenerator::SmiOperation we used a1 instead of a0, and we left the
  // register untouched.
  // We just need to load value_ and switch if necessary.
  Register lhs = a1;
  Register rhs = a0;

  switch (op_) {
    case Token::ADD:
    case Token::SUB:  {
      if (reversed_) {
        __ mov(a0, tos_register_);
        __ li(a1, Operand(Smi::FromInt(value_)));
      } else {
        __ mov(a1, tos_register_);
        __ li(a0, Operand(Smi::FromInt(value_)));
      }
      break;
    }
    case Token::MUL:
    case Token::MOD:
    case Token::BIT_OR:
    case Token::BIT_XOR:
    case Token::BIT_AND:
    case Token::SHL:
    case Token::SHR:
    case Token::SAR: {
      if (tos_register_.is(a1)) {
        __ li(a0, Operand(Smi::FromInt(value_)));
      } else {
        // This used to look a little different from the arm version.
        // Now it's a copy of that.
        ASSERT(tos_register_.is(a0));
        __ li(a1, Operand(Smi::FromInt(value_)));
      }
      if (reversed_ == tos_register_.is(a1)) {
        lhs = a0;
        rhs = a1;
      }
      break;
    }

    default:
      // Other cases should have been handled before this point.
      UNREACHABLE();
      break;
  }

  GenericBinaryOpStub stub(op_, overwrite_mode_, lhs, rhs, value_);
  __ CallStub(&stub);
  // The generic stub returns its value in v0, but that's not
  // necessarily what we want.  We want whatever the inlined code
  // expected, which is that the answer is in the same register as
  // the operand was.
  __ mov(tos_register_, v0);
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
  int int_value = Smi::cast(*value)->value();
  bool something_to_inline;
  bool both_sides_are_smi = frame_->KnownSmiAt(0);
  switch (op) {
    case Token::ADD:
    case Token::SUB:
    case Token::BIT_AND:
    case Token::BIT_OR:
    case Token::BIT_XOR: {
      something_to_inline = true;
      break;
    }
    case Token::SHL: {
      something_to_inline = (both_sides_are_smi || !reversed);
      break;
    }
    case Token::SHR:
    case Token::SAR: {
      if (reversed) {
        something_to_inline = false;
      } else {
        something_to_inline = true;
      }
      break;
    }
    case Token::MOD: {
      if (reversed || int_value < 2 || !IsPowerOf2(int_value)) {
        something_to_inline = false;
      } else {
        something_to_inline = true;
      }
      break;
    }
    case Token::MUL: {
      if (!IsEasyToMultiplyBy(int_value)) {
        something_to_inline = false;
      } else {
        something_to_inline = true;
      }
      break;
    }
    default: {
      something_to_inline = false;
      break;
    }
  }

  if (!something_to_inline) {
    if (!reversed) {
      // Push the rhs onto the virtual frame by putting it in a TOS register.
      Register rhs = frame_->GetTOSRegister();
      __ li(rhs, Operand(value));
      frame_->EmitPush(rhs, TypeInfo::Smi());
      GenericBinaryOperation(op, mode, GENERATE_INLINE_SMI, int_value);
    } else {
      // Pop the rhs, then push lhs and rhs in the right order.  Only performs
      // at most one pop, the rest takes place in TOS registers.
      Register lhs = frame_->GetTOSRegister();
      Register rhs = frame_->PopToRegister(lhs);
      __ li(lhs, Operand(value));
      frame_->EmitPush(lhs, TypeInfo::Smi());
      TypeInfo t = both_sides_are_smi ? TypeInfo::Smi() : TypeInfo::Unknown();
      frame_->EmitPush(rhs, t);
      GenericBinaryOperation(op, mode, GENERATE_INLINE_SMI, kUnknownIntValue);
    }
    return;
  }

  // We move the top of stack to a register (normally no move is invoved).
  Register tos = frame_->PopToRegister();
  // All other registers are spilled.  The deferred code expects one argument
  // in a register and all other values are flushed to the stack.  The
  // answer is returned in the same register that the top of stack argument was
  // in.
  frame_->SpillAll();

  switch (op) {
    case Token::ADD: {
      Register scratch0 = VirtualFrame::scratch0();
      Register scratch1 = VirtualFrame::scratch1();
      DeferredCode* deferred =
          new DeferredInlineSmiOperation(op, int_value, reversed, mode, tos);

      __ Addu(v0, tos, Operand(value));
      // Check for overflow.
      __ xor_(scratch0, v0, tos);
      __ Xor(scratch1, v0, Operand(value));
      __ and_(scratch0, scratch0, scratch1);
     // Overflow occurred if result is negative.
      deferred->Branch(lt, scratch0, Operand(zero_reg));
      __ And(scratch0, v0, Operand(kSmiTagMask));
      deferred->Branch(ne, scratch0, Operand(zero_reg));
      deferred->BindExit();
      __ mov(tos, v0);
      frame_->EmitPush(tos);
      break;
    }

    case Token::SUB: {
      Register scratch0 = VirtualFrame::scratch0();
      Register scratch1 = VirtualFrame::scratch1();
      Register scratch2 = VirtualFrame::scratch2();
      DeferredCode* deferred =
          new DeferredInlineSmiOperation(op, int_value, reversed, mode, tos);

      __ li(scratch0, Operand(value));
      if (reversed) {
        __ Subu(v0, scratch0, Operand(tos));
        __ xor_(scratch2, v0, scratch0);  // Check for overflow.
      } else {
        __ Subu(v0, tos, Operand(scratch0));
        __ xor_(scratch2, v0, tos);  // Check for overflow.
      }
      __ xor_(scratch1, scratch0, tos);
      __ and_(scratch2, scratch2, scratch1);
      // Overflow occurred if result is negative.
      deferred->Branch(lt, scratch2, Operand(zero_reg));
      if (!both_sides_are_smi) {
        __ And(scratch0, v0, Operand(kSmiTagMask));
        deferred->Branch(ne, scratch0, Operand(zero_reg));
      }
      deferred->BindExit();
      __ mov(tos, v0);
      frame_->EmitPush(tos);
      break;
    }

    case Token::BIT_OR:
    case Token::BIT_XOR:
    case Token::BIT_AND: {
     if (both_sides_are_smi) {
         switch (op) {
          case Token::BIT_OR:  __ Or(v0, tos, Operand(value)); break;
          case Token::BIT_XOR: __ Xor(v0, tos, Operand(value)); break;
          case Token::BIT_AND: __ And(v0, tos, Operand(value)); break;
          default: UNREACHABLE();
        }
        __ mov(tos, v0);
        frame_->EmitPush(tos, TypeInfo::Smi());
      } else {
        Register scratch = VirtualFrame::scratch0();
        DeferredCode* deferred =
          new DeferredInlineSmiOperation(op, int_value, reversed, mode, tos);
        __ And(scratch, tos, Operand(kSmiTagMask));
        deferred->Branch(ne, scratch, Operand(zero_reg));
        switch (op) {
          case Token::BIT_OR:  __ Or(v0, tos, Operand(value)); break;
          case Token::BIT_XOR: __ Xor(v0, tos, Operand(value)); break;
          case Token::BIT_AND: __ And(v0, tos, Operand(value)); break;
          default: UNREACHABLE();
        }
        deferred->BindExit();
        TypeInfo result_type =
            (op == Token::BIT_AND) ? TypeInfo::Smi() : TypeInfo::Integer32();
        __ mov(tos, v0);
        frame_->EmitPush(tos, result_type);
      }
      break;
    }

    case Token::SHL:
      if (reversed) {
        ASSERT(both_sides_are_smi);
        int max_shift = 0;
        int max_result = int_value == 0 ? 1 : int_value;
        while (Smi::IsValid(max_result << 1)) {
          max_shift++;
          max_result <<= 1;
        }
        DeferredCode* deferred =
          new DeferredInlineSmiOperation(op, int_value, true, mode, tos);
        // Mask off the last 5 bits of the shift operand (rhs).  This is part
        // of the definition of shift in JS and we know we have a Smi so we
        // can safely do this.  The masked version gets passed to the
        // deferred code, but that makes no difference.
        __ And(tos, tos, Operand(Smi::FromInt(0x1f)));
        deferred->Branch(ge, tos, Operand(Smi::FromInt(max_shift)));
        Register scratch = VirtualFrame::scratch0();
        __ sra(scratch, tos, kSmiTagSize);  // Untag.
        __ li(tos, Operand(Smi::FromInt(int_value)));    // Load constant.
        __ sllv(tos, tos, scratch);          // Shift constant.
        deferred->BindExit();
        TypeInfo result = TypeInfo::Integer32();
        frame_->EmitPush(tos, result);
        break;
      }
      // Fall through!
    case Token::SHR:
    case Token::SAR: {
      ASSERT(!reversed);
      TypeInfo result = TypeInfo::Integer32();
      Register scratch = VirtualFrame::scratch0();
      int shift_value = int_value & 0x1f;  // least significant 5 bits
      DeferredCode* deferred =
        new DeferredInlineSmiOperation(op, shift_value, false, mode, tos);
      bool skip_smi_test = both_sides_are_smi;
      if (!skip_smi_test) {
        __ And(v0, tos, Operand(kSmiTagMask));
        deferred->Branch(ne, v0, Operand(zero_reg));
      }
      __ sra(v0, tos, kSmiTagSize);  // Remove tag.
      switch (op) {
        case Token::SHL: {
          if (shift_value != 0) {
            __ sll(v0, v0, shift_value);
          }
          // Check that the *unsigned* result fits in a Smi.
          __ Addu(scratch, v0, Operand(0x40000000));
          deferred->Branch(lt, scratch, Operand(zero_reg));
          break;
        }
        case Token::SHR: {
          if (shift_value != 0) {
            __ srl(v0, v0, shift_value);
          }
              // Check that the *unsigned* result fits in a smi.
              // Neither of the two high-order bits can be set:
              // - 0x80000000: high bit would be lost when smi tagging
              // - 0x40000000: this number would convert to negative when
          // Smi tagging these two cases can only happen with shifts
          // by 0 or 1 when handed a valid smi.
          // Check that the result fits in a Smi.
          __ And(scratch, v0, Operand(0xc0000000));
          deferred->Branch(ne, scratch, Operand(zero_reg));
          if (shift_value >= 2) {
            result = TypeInfo::Smi();
          }
          break;
        }
        case Token::SAR: {
          if (shift_value != 0) {
            // ASR by immediate 0 means shifting 32 bits.
            __ sra(v0, v0, shift_value);
           }
          break;
        }
        default: UNREACHABLE();
      }
      __ sll(v0, v0, kSmiTagSize);  // Tag result.
      deferred->BindExit();
      __ mov(tos, v0);
      frame_->EmitPush(tos);
      break;
    }

    case Token::MOD: {
      ASSERT(!reversed);
      ASSERT(int_value >= 2);
      ASSERT(IsPowerOf2(int_value));
      Register scratch = VirtualFrame::scratch0();
      DeferredCode* deferred =
        new DeferredInlineSmiOperation(op, int_value, reversed, mode, tos);
      unsigned mask = (0x80000000u | kSmiTagMask);
      __ And(scratch, tos, Operand(mask));
      // Go to deferred code on non-Smis and negative.
      deferred->Branch(ne, scratch, Operand(zero_reg));
      mask = (int_value << kSmiTagSize) - 1;
      __ And(v0, tos, Operand(mask));
      deferred->BindExit();
      __ mov(tos, v0);
      // Mod of positive power of 2 Smi gives a Smi if the lhs is an integer.
      frame_->EmitPush(
              tos, both_sides_are_smi ? TypeInfo::Smi() : TypeInfo::Number());
      break;
    }

    case Token::MUL: {
      ASSERT(IsEasyToMultiplyBy(int_value));
      Register scratch = VirtualFrame::scratch0();
      DeferredCode* deferred =
        new DeferredInlineSmiOperation(op, int_value, reversed, mode, tos);
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
      __ And(scratch, tos, Operand(mask));
      deferred->Branch(ne, scratch, Operand(zero_reg));
      MultiplyByKnownInt(masm_, tos, v0, int_value);
      deferred->BindExit();
      __ mov(tos, v0);
        frame_->EmitPush(tos);
      break;
    }

    default:
      UNREACHABLE();
      break;
  }
}


// On MIPS we load registers condReg1 and condReg2 with the values which should
// be compared. With the CodeGenerator::cc_reg_ condition, functions will be
// able to evaluate correctly the condition. (eg CodeGenerator::Branch)
void CodeGenerator::Comparison(Condition cc,
                               Expression* left,
                               Expression* right,
                               bool strict) {
  VirtualFrame::RegisterAllocationScope scope(this);

  if (left != NULL) Load(left);
  if (right != NULL) Load(right);

  // sp[0] : y  (right)
  // sp[1] : x  (left)

  // Strict only makes sense for equality comparisons.
  ASSERT(!strict || cc == eq);

  Register lhs;
  Register rhs;

  bool lhs_is_smi;
  bool rhs_is_smi;
  // We load the top two stack positions into registers chosen by the virtual
  // frame.  This should keep the register shuffling to a minimum.
  // Implement '>' and '<=' by reversal to obtain ECMA-262 conversion order.
  if (cc == gt || cc == le) {
    cc = ReverseCondition(cc);
    lhs_is_smi = frame_->KnownSmiAt(0);
    rhs_is_smi = frame_->KnownSmiAt(1);
    lhs = frame_->PopToRegister();
    rhs = frame_->PopToRegister(lhs);  // Don't pop to the same register again!
  } else {
    rhs_is_smi = frame_->KnownSmiAt(0);
    lhs_is_smi = frame_->KnownSmiAt(1);
    rhs = frame_->PopToRegister();
    lhs = frame_->PopToRegister(rhs);  // Don't pop to the same register again!
  }

  ASSERT(rhs.is(a0) || rhs.is(a1));
  ASSERT(lhs.is(a0) || lhs.is(a1));

  bool both_sides_are_smi = (lhs_is_smi && rhs_is_smi);
  JumpTarget exit;

  if (!both_sides_are_smi) {
  // Now we have the two sides in a0 and a1.  We flush any other registers
  // because the stub doesn't know about register allocation.
    frame_->SpillAll();
    Register scratch = VirtualFrame::scratch0();
    Register smi_test_reg;
    if (lhs_is_smi) {
      smi_test_reg = rhs;
    } else if (rhs_is_smi) {
      smi_test_reg = lhs;
    } else {
      __ Or(scratch, lhs, rhs);
      smi_test_reg = scratch;
    }

  __ And(scratch, smi_test_reg, kSmiTagMask);
  JumpTarget smi;
  smi.Branch(eq, scratch, Operand(zero_reg), no_hint);

  // Perform non-smi comparison by stub.
  // CompareStub takes arguments in a0 and a1, returns <0, >0 or 0 in v0.
  // We call with 0 args because there are 0 on the stack.
  if (!rhs.is(a1)) {
    __ Swap(rhs, lhs, scratch);
  }

  CompareStub stub(cc, strict);
  frame_->CallStub(&stub, 0);
  __ mov(condReg1, v0);
  __ li(condReg2, Operand(0));

  exit.Jump();

  // Do smi comparisons by pointer comparison.
  smi.Bind();
  }
  __ mov(condReg1, lhs);
  __ mov(condReg2, rhs);

  exit.Bind();
  cc_reg_ = cc;
}


void CodeGenerator::VisitStatements(ZoneList<Statement*>* statements) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  for (int i = 0; frame_ != NULL && i < statements->length(); i++) {
    Visit(statements->at(i));
  }
}


void CodeGenerator::CallWithArguments(ZoneList<Expression*>* args,
                                      CallFunctionFlags flags,
                                      int position) {
  frame_->AssertIsSpilled();

  // Push the arguments ("left-to-right") on the stack.
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    Load(args->at(i));
  }

  // Record the position for debugging purposes.
  CodeForSourcePosition(position);

  // Use the shared code stub to call the function.
  InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
  CallFunctionStub call_function(arg_count, in_loop, flags);
  frame_->CallStub(&call_function, arg_count + 1);

  // Restore context and pop function from the stack.
  __ lw(cp, frame_->Context());
  frame_->Drop();  // Discard the TOS.
}

void CodeGenerator::CallApplyLazy(Expression* applicand,
                                  Expression* receiver,
                                  VariableProxy* arguments,
                                  int position) {
  // An optimized implementation of expressions of the form
  // x.apply(y, arguments).
  // If the arguments object of the scope has not been allocated,
  // and x.apply is Function.prototype.apply, this optimization
  // just copies y and the arguments of the current function on the
  // stack, as receiver and arguments, and calls x.
  // In the implementation comments, we call x the applicand
  // and y the receiver.
  VirtualFrame::SpilledScope spilled_scope(frame_);

  ASSERT(ArgumentsMode() == LAZY_ARGUMENTS_ALLOCATION);
  ASSERT(arguments->IsArguments());

  // Load applicand.apply onto the stack. This will usually
  // give us a megamorphic load site. Not super, but it works.
  Load(applicand);
  Handle<String> name = Factory::LookupAsciiSymbol("apply");
  frame_->Dup();
  frame_->CallLoadIC(name, RelocInfo::CODE_TARGET);
  frame_->EmitPush(v0);

  // Load the receiver and the existing arguments object onto the
  // expression stack. Avoid allocating the arguments object here.
  Load(receiver);
  LoadFromSlot(scope()->arguments()->var()->slot(), NOT_INSIDE_TYPEOF);

  // Emit the source position information after having loaded the
  // receiver and the arguments.
  CodeForSourcePosition(position);
  // Contents of the stack at this point:
  //   sp[0]: arguments object of the current function or the hole.
  //   sp[1]: receiver
  //   sp[2]: applicand.apply
  //   sp[3]: applicand.

  // Check if the arguments object has been lazily allocated
  // already. If so, just use that instead of copying the arguments
  // from the stack. This also deals with cases where a local variable
  // named 'arguments' has been introduced.
  __ lw(v0, MemOperand(sp, 0));

  Label slow, done;
  __ LoadRoot(a1, Heap::kTheHoleValueRootIndex);
  __ Branch(&slow, ne, a1, Operand(v0));

  Label build_args;
  // Get rid of the arguments object probe.
  frame_->Drop();
  // Stack now has 3 elements on it.
  // Contents of stack at this point:
  //   sp[0]: receiver
  //   sp[1]: applicand.apply
  //   sp[2]: applicand.

  // Check that the receiver really is a JavaScript object.
  __ lw(a0, MemOperand(sp, 0));
  __ BranchOnSmi(a0, &build_args);
  // We allow all JSObjects including JSFunctions.  As long as
  // JS_FUNCTION_TYPE is the last instance type and it is right
  // after LAST_JS_OBJECT_TYPE, we do not have to check the upper
  // bound.
  ASSERT(LAST_TYPE == JS_FUNCTION_TYPE);
  ASSERT(JS_FUNCTION_TYPE == LAST_JS_OBJECT_TYPE + 1);

  __ GetObjectType(a0, a1, a2);
  __ Branch(&build_args, lt, a2, Operand(FIRST_JS_OBJECT_TYPE));

  // Check that applicand.apply is Function.prototype.apply.
  __ lw(v0, MemOperand(sp, kPointerSize));
  __ BranchOnSmi(v0, &build_args);

  __ GetObjectType(a0, a1, a2);
  __ Branch(&build_args, ne, a2, Operand(JS_FUNCTION_TYPE));

  __ lw(a0, FieldMemOperand(v0, JSFunction::kSharedFunctionInfoOffset));
  Handle<Code> apply_code(Builtins::builtin(Builtins::FunctionApply));
  __ lw(a1, FieldMemOperand(a0, SharedFunctionInfo::kCodeOffset));
  __ Branch(&build_args, ne, a1, Operand(apply_code));

  // Check that applicand is a function.
  __ lw(a1, MemOperand(sp, 2 * kPointerSize));
  __ BranchOnSmi(a1, &build_args);

  __ GetObjectType(a1, a2, a3);
  __ Branch(&build_args, ne, a3, Operand(JS_FUNCTION_TYPE));

  // Copy the arguments to this function possibly from the
  // adaptor frame below it.
  Label invoke, adapted;
  __ lw(a2, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ lw(a3, MemOperand(a2, StandardFrameConstants::kContextOffset));
  __ Branch(&adapted, eq, a3,
            Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));

  // No arguments adaptor frame. Copy fixed number of arguments.
  __ Or(v0, zero_reg, Operand(scope()->num_parameters()));
  for (int i = 0; i < scope()->num_parameters(); i++) {
    __ lw(a2, frame_->ParameterAt(i));
    __ push(a2);
  }
  __ jmp(&invoke);

  // Arguments adaptor frame present. Copy arguments from there, but
  // avoid copying too many arguments to avoid stack overflows.
  __ bind(&adapted);
  static const uint32_t kArgumentsLimit = 1 * KB;
  __ lw(v0, MemOperand(a2, ArgumentsAdaptorFrameConstants::kLengthOffset));
  __ srl(v0, v0, kSmiTagSize);
  __ mov(a3, v0);
  __ Branch(&build_args, gt, v0, Operand(kArgumentsLimit));

  // Loop through the arguments pushing them onto the execution
  // stack. We don't inform the virtual frame of the push, so we don't
  // have to worry about getting rid of the elements from the virtual
  // frame.
  Label loop;
  // a3 is a small non-negative integer, due to the test above.
  __ Branch(&invoke, eq, a3, Operand(zero_reg));

  // Compute the address of the first argument.
  __ sll(t0, a3, kPointerSizeLog2);
  __ Addu(a2, a2, t0);
  __ Addu(a2, a2, Operand(kPointerSize));
  __ bind(&loop);
  // Post-decrement argument address by kPointerSize on each iteration.
  __ lw(t0, MemOperand(a2));
  __ Subu(a2, a2, Operand(kPointerSize));
  __ push(t0);
  __ Subu(a3, a3, Operand(1));
  __ Branch(&loop, gt, a3, Operand(zero_reg));

  // Invoke the function.
  __ bind(&invoke);
  ParameterCount actual(a0);
  __ InvokeFunction(a1, actual, CALL_FUNCTION);
  // Drop applicand.apply and applicand from the stack, and push
  // the result of the function call, but leave the spilled frame
  // unchanged, with 3 elements, so it is correct when we compile the
  // slow-case code.
  __ Addu(sp, sp, Operand(2 * kPointerSize));
  __ push(a0);
  // Stack now has 1 element:
  //   sp[0]: result
  __ jmp(&done);

  // Slow-case: Allocate the arguments object since we know it isn't
  // there, and fall-through to the slow-case where we call
  // applicand.apply.
  __ bind(&build_args);
  // Stack now has 3 elements, because we have jumped from where:
  //   sp[0]: receiver
  //   sp[1]: applicand.apply
  //   sp[2]: applicand.
  StoreArgumentsObject(false);

  // Stack and frame now have 4 elements.
  __ bind(&slow);

  // Generic computation of x.apply(y, args) with no special optimization.
  // Flip applicand.apply and applicand on the stack, so
  // applicand looks like the receiver of the applicand.apply call.
  // Then process it as a normal function call.
  __ lw(v0, MemOperand(sp, 3 * kPointerSize));
  __ lw(a1, MemOperand(sp, 2 * kPointerSize));
  __ sw(v0, MemOperand(sp, 2 * kPointerSize));
  __ sw(a1, MemOperand(sp, 3 * kPointerSize));

  CallFunctionStub call_function(2, NOT_IN_LOOP, NO_CALL_FUNCTION_FLAGS);
  frame_->CallStub(&call_function, 3);
  // The function and its two arguments have been dropped.
  frame_->Drop();  // Drop the receiver as well.
  frame_->EmitPush(v0);
  // Stack now has 1 element:
  //   sp[0]: result
  __ bind(&done);

  // Restore the context register after a call.
  __ lw(cp, frame_->Context());
}

void CodeGenerator::Branch(bool if_true, JumpTarget* target) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  ASSERT(has_cc());
  Condition cc = if_true ? cc_reg_ : NegateCondition(cc_reg_);
  target->Branch(cc, condReg1, Operand(condReg2), no_hint);
  cc_reg_ = cc_always;
}


void CodeGenerator::CheckStack() {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ check stack");

  __ LoadRoot(t0, Heap::kStackLimitRootIndex);
  StackCheckStub stub;
  // Call the stub if lower.
  __ Push(ra);
  __ Call(Operand(reinterpret_cast<intptr_t>(stub.GetCode().location()),
          RelocInfo::CODE_TARGET),
          Uless, sp, Operand(t0));
  __ Pop(ra);
}


void CodeGenerator::VisitBlock(Block* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ Block");
  CodeForStatementPosition(node);
  node->break_target()->SetExpectedHeight();
  VisitStatements(node->statements());
  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  node->break_target()->Unuse();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::DeclareGlobals(Handle<FixedArray> pairs) {
  frame_->EmitPush(cp);
  frame_->EmitPush(Operand(pairs));
  frame_->EmitPush(Operand(Smi::FromInt(is_eval() ? 1 : 0)));

  VirtualFrame::SpilledScope spilled_scope(frame_);
  frame_->CallRuntime(Runtime::kDeclareGlobals, 3);
  // The result is discarded.
}


void CodeGenerator::VisitDeclaration(Declaration* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Declaration");
  Variable* var = node->proxy()->var();
  ASSERT(var != NULL);  // Must have been resolved.
  Slot* slot = var->slot();

  // If it was not possible to allocate the variable at compile time,
  // we need to "declare" it at runtime to make sure it actually
  // exists in the local context.
  if (slot != NULL && slot->type() == Slot::LOOKUP) {
    // Variables with a "LOOKUP" slot were introduced as non-locals
    // during variable resolution and must have mode DYNAMIC.
    ASSERT(var->is_dynamic());
    // For now, just do a runtime call.
    frame_->EmitPush(cp);
    frame_->EmitPush(Operand(var->name()));
    // Declaration nodes are always declared in only two modes.
    ASSERT(node->mode() == Variable::VAR || node->mode() == Variable::CONST);
    PropertyAttributes attr = node->mode() == Variable::VAR ? NONE : READ_ONLY;
    frame_->EmitPush(Operand(Smi::FromInt(attr)));
    // Push initial value, if any.
    // Note: For variables we must not push an initial value (such as
    // 'undefined') because we may have a (legal) redeclaration and we
    // must not destroy the current value.
    if (node->mode() == Variable::CONST) {
      frame_->EmitPushRoot(Heap::kTheHoleValueRootIndex);
    } else if (node->fun() != NULL) {
      Load(node->fun());
    } else {
      frame_->EmitPush(Operand(0));
    }

    VirtualFrame::SpilledScope spilled_scope(frame_);
    frame_->CallRuntime(Runtime::kDeclareContextSlot, 4);
    // Ignore the return value (declarations are statements).

    ASSERT(frame_->height() == original_height);
    return;
  }

  ASSERT(!var->is_global());

  // If we have a function or a constant, we need to initialize the variable.
  Expression* val = NULL;
  if (node->mode() == Variable::CONST) {
    val = new Literal(Factory::the_hole_value());
  } else {
    val = node->fun();  // NULL if we don't have a function.
  }

  if (val != NULL) {
    // Set initial value.
    Reference target(this, node->proxy());
    Load(val);
    target.SetValue(NOT_CONST_INIT);

    // Get rid of the assigned value (declarations are statements).
    frame_->Drop();
  }
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitExpressionStatement(ExpressionStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ ExpressionStatement");
  CodeForStatementPosition(node);
  Expression* expression = node->expression();
  expression->MarkAsStatement();
  Load(expression);
  frame_->Drop();
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitEmptyStatement(EmptyStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "// EmptyStatement");
  CodeForStatementPosition(node);
  // nothing to do
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitIfStatement(IfStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
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
    LoadCondition(node->condition(), &then, &else_, true);
    if (frame_ != NULL) {
      Branch(false, &else_);
    }
    // then
    if (frame_ != NULL || then.is_linked()) {
      then.Bind();
      Visit(node->then_statement());
    }
    if (frame_ != NULL) {
      exit.Jump();
    }
    // else
    if (else_.is_linked()) {
      else_.Bind();
      Visit(node->else_statement());
    }

  } else if (has_then_stm) {
    Comment cmnt(masm_, "[ IfThen");
    ASSERT(!has_else_stm);
    JumpTarget then;
    // if (cond)
    LoadCondition(node->condition(), &then, &exit, true);
    if (frame_ != NULL) {
      Branch(false, &exit);
    }
    // then
    if (frame_ != NULL || then.is_linked()) {
      then.Bind();
      Visit(node->then_statement());
    }

  } else if (has_else_stm) {
    Comment cmnt(masm_, "[ IfElse");
    ASSERT(!has_then_stm);
    JumpTarget else_;
    // if (!cond)
    LoadCondition(node->condition(), &exit, &else_, true);
    if (frame_ != NULL) {
      Branch(true, &exit);
    }
    // else
    if (frame_ != NULL || else_.is_linked()) {
      else_.Bind();
      Visit(node->else_statement());
    }

  } else {
    Comment cmnt(masm_, "[ If");
    ASSERT(!has_then_stm && !has_else_stm);
    // if (cond)
    LoadCondition(node->condition(), &exit, &exit, false);
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
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ ContinueStatement");
  CodeForStatementPosition(node);
  node->target()->continue_target()->Jump();
}


void CodeGenerator::VisitBreakStatement(BreakStatement* node) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ BreakStatement");
  CodeForStatementPosition(node);
  node->target()->break_target()->Jump();
}


void CodeGenerator::VisitReturnStatement(ReturnStatement* node) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ ReturnStatement");

  CodeForStatementPosition(node);
  Load(node->expression());
  if (function_return_is_shadowed_) {
    frame_->EmitPop(v0);
    function_return_.Jump();
  } else {
    // Pop the result from the frame and prepare the frame for
    // returning thus making it easier to merge.
    frame_->EmitPop(v0);
    frame_->PrepareForReturn();
    if (function_return_.is_bound()) {
      // If the function return label is already bound we reuse the
      // code by jumping to the return site.
      function_return_.Jump();
    } else {
      function_return_.Bind();
      GenerateReturnSequence();
    }
  }
}


void CodeGenerator::GenerateReturnSequence() {
  if (FLAG_trace) {
    // Push the return value on the stack as the parameter.
    // Runtime::TraceExit returns the parameter as it is.
    frame_->EmitPush(v0);
    frame_->CallRuntime(Runtime::kTraceExit, 1);
  }

#ifdef DEBUG
  // Add a label for checking the size of the code used for returning.
  Label check_exit_codesize;
  masm_->bind(&check_exit_codesize);
#endif
  // Make sure that the trampoline pool is not emitted inside of the return
  // sequence.
  { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
    // Tear down the frame which will restore the caller's frame pointer and
    // the link register.
    frame_->Exit();

    // Here we use masm_-> instead of the __ macro to avoid the code coverage
    // tool from instrumenting as we rely on the code size here.
    int32_t sp_delta = (scope()->num_parameters() + 1) * kPointerSize;
    masm_-> Addu(sp, sp, Operand(sp_delta));
    masm_-> Ret();
    DeleteFrame();

#ifdef DEBUG
    // Check that the size of the code used for returning matches what is
    // expected by the debugger. If the sp_delta above cannot be encoded in
    // the add instruction the add will generate two instructions.
    int return_sequence_length =
        masm_->InstructionsGeneratedSince(&check_exit_codesize);
    CHECK(return_sequence_length ==
          Assembler::kJSReturnSequenceInstructions ||
          return_sequence_length ==
          Assembler::kJSReturnSequenceInstructions + 1);
#endif
  }
}


void CodeGenerator::VisitWithEnterStatement(WithEnterStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ WithEnterStatement");
  CodeForStatementPosition(node);
  Load(node->expression());
  if (node->is_catch_block()) {
    frame_->CallRuntime(Runtime::kPushCatchContext, 1);
  } else {
    frame_->CallRuntime(Runtime::kPushContext, 1);
  }
#ifdef DEBUG
  JumpTarget verified_true;
  verified_true.Branch(eq, v0, Operand(cp), no_hint);
  __ stop("PushContext: v0 is expected to be the same as cp");
  verified_true.Bind();
#endif
  // Update context local.
  __ sw(cp, frame_->Context());
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitWithExitStatement(WithExitStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ WithExitStatement");
  CodeForStatementPosition(node);
  // Pop context.
  __ lw(cp, ContextOperand(cp, Context::PREVIOUS_INDEX));
  // Update context local.
  __ sw(cp, frame_->Context());
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitSwitchStatement(SwitchStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ SwitchStatement");
  CodeForStatementPosition(node);
  node->break_target()->SetExpectedHeight();

  Load(node->tag());

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
    VisitStatements(clause->statements());

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
    VisitStatements(default_clause->statements());
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
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ DoWhileStatement");
  CodeForStatementPosition(node);
  node->break_target()->SetExpectedHeight();
  JumpTarget body(JumpTarget::BIDIRECTIONAL);
  IncrementLoopNesting();

  // Label the top of the loop for the backward CFG edge.  If the test
  // is always true we can use the continue target, and if the test is
  // always false there is no need.
  ConditionAnalysis info = AnalyzeCondition(node->cond());
  switch (info) {
    case ALWAYS_TRUE:
      node->continue_target()->SetExpectedHeight();
      node->continue_target()->Bind();
      break;
    case ALWAYS_FALSE:
      node->continue_target()->SetExpectedHeight();
      break;
    case DONT_KNOW:
      node->continue_target()->SetExpectedHeight();
      body.Bind();
      break;
  }

  CheckStack();  // TODO(1222600): ignore if body contains calls.
  Visit(node->body());

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
        LoadCondition(node->cond(), &body, node->break_target(), true);
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
  DecrementLoopNesting();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitWhileStatement(WhileStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ WhileStatement");
  CodeForStatementPosition(node);

  // If the test is never true and has no side effects there is no need
  // to compile the test or body.
  ConditionAnalysis info = AnalyzeCondition(node->cond());
  if (info == ALWAYS_FALSE) return;

  node->break_target()->SetExpectedHeight();
  IncrementLoopNesting();

  // Label the top of the loop with the continue target for the backward
  // CFG edge.
  node->continue_target()->SetExpectedHeight();
  node->continue_target()->Bind();


  if (info == DONT_KNOW) {
    JumpTarget body;
    LoadCondition(node->cond(), &body, node->break_target(), true);
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
    CheckStack();  // TODO(1222600): Ignore if body contains calls.
    Visit(node->body());

    // If control flow can fall out of the body, jump back to the top.
    if (has_valid_frame()) {
      node->continue_target()->Jump();
    }
  }
  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  DecrementLoopNesting();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitForStatement(ForStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ ForStatement");
  CodeForStatementPosition(node);
  if (node->init() != NULL) {
    Visit(node->init());
  }

  // If the test is never true there is no need to compile the test or
  // body.
  ConditionAnalysis info = AnalyzeCondition(node->cond());
  if (info == ALWAYS_FALSE) return;

  node->break_target()->SetExpectedHeight();
  IncrementLoopNesting();

  // We know that the loop index is a smi if it is not modified in the
  // loop body and it is checked against a constant limit in the loop
  // condition.  In this case, we reset the static type information of the
  // loop index to smi before compiling the body, the update expression, and
  // the bottom check of the loop condition.
  TypeInfoCodeGenState type_info_scope(this,
                                       node->is_fast_smi_loop() ?
                                           node->loop_variable()->slot() :
                                           NULL,
                                       TypeInfo::Smi());
  // If there is no update statement, label the top of the loop with the
  // continue target, otherwise with the loop target.
  JumpTarget loop(JumpTarget::BIDIRECTIONAL);
  if (node->next() == NULL) {
    node->continue_target()->SetExpectedHeight();
    node->continue_target()->Bind();
  } else {
    node->continue_target()->SetExpectedHeight();
    loop.Bind();
  }

  // If the test is always true, there is no need to compile it.
  if (info == DONT_KNOW) {
    JumpTarget body;
    LoadCondition(node->cond(), &body, node->break_target(), true);
    if (has_valid_frame()) {
      Branch(false, node->break_target());
    }
    if (has_valid_frame() || body.is_linked()) {
      body.Bind();
    }
  }

  if (has_valid_frame()) {
    CheckStack();  // TODO(1222600): ignore if body contains calls.
    Visit(node->body());

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
        Visit(node->next());
        loop.Jump();
      }
    }
  }
  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  DecrementLoopNesting();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitForInStatement(ForInStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ ForInStatement");
  CodeForStatementPosition(node);

  JumpTarget primitive;
  JumpTarget jsobject;
  JumpTarget fixed_array;
  JumpTarget entry(JumpTarget::BIDIRECTIONAL);
  JumpTarget end_del_check;
  JumpTarget exit;

  // Get the object to enumerate over (converted to JSObject).
  Load(node->enumerable());

  // Both SpiderMonkey and kjs ignore null and undefined in contrast
  // to the specification.  12.6.4 mandates a call to ToObject.
  frame_->EmitPop(a0);
  __ LoadRoot(t2, Heap::kUndefinedValueRootIndex);
  exit.Branch(eq, a0, Operand(t2), no_hint);
  __ LoadRoot(t2, Heap::kNullValueRootIndex);
  exit.Branch(eq, a0, Operand(t2), no_hint);

  // Stack layout in body:
  // [iteration counter (Smi)]
  // [length of array]
  // [FixedArray]
  // [Map or 0]
  // [Object]

  // Check if enumerable is already a JSObject
  __ And(t0, a0, Operand(kSmiTagMask));
  primitive.Branch(eq, t0, Operand(zero_reg), no_hint);
  __ GetObjectType(a0, a1, a1);
  jsobject.Branch(hs, a1, Operand(FIRST_JS_OBJECT_TYPE), no_hint);

  primitive.Bind();
  frame_->EmitPush(a0);
  frame_->InvokeBuiltin(Builtins::TO_OBJECT, CALL_JS, 1);
  __ mov(a0, v0);

  jsobject.Bind();
  // Get the set of properties (as a FixedArray or Map).
  // r0: value to be iterated over
  frame_->EmitPush(a0);  // Push the object being iterated over.

  // Check cache validity in generated code. This is a fast case for
  // the JSObject::IsSimpleEnum cache validity checks. If we cannot
  // guarantee cache validity, call the runtime system to check cache
  // validity or get the property names in a fixed array.
  JumpTarget call_runtime;
  JumpTarget loop(JumpTarget::BIDIRECTIONAL);
  JumpTarget check_prototype;
  JumpTarget use_cache;
  __ mov(a1, a0);
  loop.Bind();
  // Check that there are no elements.
  __ lw(a2, FieldMemOperand(a1, JSObject::kElementsOffset));
  __ LoadRoot(t0, Heap::kEmptyFixedArrayRootIndex);
  call_runtime.Branch(ne, a2, Operand(t0), no_hint);
  // Check that instance descriptors are not empty so that we can
  // check for an enum cache.  Leave the map in a3 for the subsequent
  // prototype load.
  __ lw(a3, FieldMemOperand(a1, HeapObject::kMapOffset));
  __ lw(a2, FieldMemOperand(a3, Map::kInstanceDescriptorsOffset));
  __ LoadRoot(t2, Heap::kEmptyDescriptorArrayRootIndex);
  call_runtime.Branch(eq, a2, Operand(t2), no_hint);
  // Check that there in an enum cache in the non-empty instance
  // descriptors.  This is the case if the next enumeration index
  // field does not contain a smi.
  __ lw(a2, FieldMemOperand(a2, DescriptorArray::kEnumerationIndexOffset));
  __ And(t1, a2, Operand(kSmiTagMask));
  call_runtime.Branch(eq, t1, Operand(zero_reg), no_hint);
  // For all objects but the receiver, check that the cache is empty.
  // t0: empty fixed array root.
  check_prototype.Branch(eq, a1, Operand(a0), no_hint);
  __ lw(a2, FieldMemOperand(a2, DescriptorArray::kEnumCacheBridgeCacheOffset));
  call_runtime.Branch(ne, a2, Operand(t0), no_hint);
  check_prototype.Bind();
  // Load the prototype from the map and loop if non-null.
  __ lw(a1, FieldMemOperand(a3, Map::kPrototypeOffset));
  __ LoadRoot(t2, Heap::kNullValueRootIndex);
  loop.Branch(ne, a1, Operand(t2), no_hint);
  // The enum cache is valid.  Load the map of the object being
  // iterated over and use the cache for the iteration.
  __ lw(a0, FieldMemOperand(a0, HeapObject::kMapOffset));
  use_cache.Jump();

  call_runtime.Bind();
  // Call the runtime to get the property names for the object.
  frame_->EmitPush(a0);  // push the object (slot 4) for the runtime call
  frame_->CallRuntime(Runtime::kGetPropertyNamesFast, 1);
  __ mov(a0, v0);

  // If we got a map from the runtime call, we can do a fast
  // modification check. Otherwise, we got a fixed array, and we have
  // to do a slow check.
  // a0: map or fixed array (result from call to
  // Runtime::kGetPropertyNamesFast)
  __ mov(a2, a0);
  __ lw(a1, FieldMemOperand(a2, HeapObject::kMapOffset));
  __ LoadRoot(t2, Heap::kMetaMapRootIndex);
  fixed_array.Branch(ne, a1, Operand(t2), no_hint);

  use_cache.Bind();

  // Get enum cache
  // v0: map (either the result from a call to
  // Runtime::kGetPropertyNamesFast or has been fetched directly from
  // the object)
  __ mov(a1, a0);
  __ lw(a1, FieldMemOperand(a1, Map::kInstanceDescriptorsOffset));
  __ lw(a1, FieldMemOperand(a1, DescriptorArray::kEnumerationIndexOffset));
  __ lw(a2,
         FieldMemOperand(a1, DescriptorArray::kEnumCacheBridgeCacheOffset));

  frame_->EmitPush(a0);  // map
  frame_->EmitPush(a2);  // enum cache bridge cache
  __ lw(a0, FieldMemOperand(a2, FixedArray::kLengthOffset));
  frame_->EmitPush(a0);
  __ li(a0, Operand(Smi::FromInt(0)));
  frame_->EmitPush(a0);
  entry.Jump();

  fixed_array.Bind();
  __ li(a1, Operand(Smi::FromInt(0)));
  frame_->EmitPush(a1);  // insert 0 in place of Map
  frame_->EmitPush(a0);

  // Push the length of the array and the initial index onto the stack.
  __ lw(a0, FieldMemOperand(a0, FixedArray::kLengthOffset));
  frame_->EmitPush(a0);
  __ li(a0, Operand(Smi::FromInt(0)));  // init index
  frame_->EmitPush(a0);

  // Condition.
  entry.Bind();
  // sp[0] : index
  // sp[1] : array/enum cache length
  // sp[2] : array or enum cache
  // sp[3] : 0 or map
  // sp[4] : enumerable
  // Grab the current frame's height for the break and continue
  // targets only after all the state is pushed on the frame.
  node->break_target()->SetExpectedHeight();
  node->continue_target()->SetExpectedHeight();

  __ lw(a0, frame_->ElementAt(0));  // load the current count
  __ lw(a1, frame_->ElementAt(1));  // load the length
  node->break_target()->Branch(hs, a0, Operand(a1));

  // Get the i'th entry of the array.
  __ lw(a2, frame_->ElementAt(2));
  __ Addu(a2, a2, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  __ sll(t2, a0, kPointerSizeLog2 - kSmiTagSize);  // Scale index.
  __ addu(t2, t2, a2);  // Base + index.
  __ lw(a3, MemOperand(t2));

  // Get Map or 0.
  __ lw(a2, frame_->ElementAt(3));
  // Check if this (still) matches the map of the enumerable.
  // If not, we have to filter the key.
  __ lw(a1, frame_->ElementAt(4));
  __ lw(a1, FieldMemOperand(a1, HeapObject::kMapOffset));
  end_del_check.Branch(eq, a1, Operand(a2), no_hint);

  // Convert the entry to a string (or null if it isn't a property anymore).
  __ lw(a0, frame_->ElementAt(4));  // push enumerable
  frame_->EmitPush(a0);
  frame_->EmitPush(a3);  // push entry
  frame_->InvokeBuiltin(Builtins::FILTER_KEY, CALL_JS, 2);
  __ mov(a3, v0);

  // If the property has been removed while iterating, we just skip it.
  __ LoadRoot(t2, Heap::kNullValueRootIndex);
  node->continue_target()->Branch(eq, a3, Operand(t2));

  end_del_check.Bind();
  // Store the entry in the 'each' expression and take another spin in the
  // loop.  a3: i'th entry of the enum cache (or string there of)
  frame_->EmitPush(a3);  // push entry
  { Reference each(this, node->each());
    if (!each.is_illegal()) {
      if (each.size() > 0) {
        __ lw(a0, frame_->ElementAt(each.size()));
        frame_->EmitPush(a0);
        each.SetValue(NOT_CONST_INIT);
        frame_->Drop(2);
      } else {
        // If the reference was to a slot we rely on the convenient property
        // that it doesn't matter whether a value (eg, a3 pushed above) is
        // right on top of or right underneath a zero-sized reference.
        each.SetValue(NOT_CONST_INIT);
        frame_->Drop();
      }
    }
  }
  // Body.
  CheckStack();  // TODO(1222600): ignore if body contains calls.
  Visit(node->body());

  // Next.  Reestablish a spilled frame in case we are coming here via
  // a continue in the body.
  node->continue_target()->Bind();
  frame_->SpillAll();
  frame_->EmitPop(a0);
  __ Addu(a0, a0, Operand(Smi::FromInt(1)));
  frame_->EmitPush(a0);
  entry.Jump();

  // Cleanup.  No need to spill because VirtualFrame::Drop is safe for
  // any frame.
  node->break_target()->Bind();
  frame_->Drop(5);

  // Exit.
  exit.Bind();
  node->continue_target()->Unuse();
  node->break_target()->Unuse();
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitTryCatchStatement(TryCatchStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ TryCatchStatement");
  CodeForStatementPosition(node);

  JumpTarget try_block;
  JumpTarget exit;

  try_block.Call();
  // --- Catch block ---
  frame_->EmitPush(v0);

  // Store the caught exception in the catch variable.
  Variable* catch_var = node->catch_var()->var();
  ASSERT(catch_var != NULL && catch_var->slot() != NULL);
  StoreToSlot(catch_var->slot(), NOT_CONST_INIT);

  // Remove the exception from the stack.
  frame_->Drop();

  VisitStatements(node->catch_block()->statements());
  if (frame_ != NULL) {
    exit.Jump();
  }

  // --- Try block ---
  try_block.Bind();

  frame_->PushTryHandler(TRY_CATCH_HANDLER);
  int handler_height = frame_->height();

  // Shadow the labels for all escapes from the try block, including
  // returns. During shadowing, the original label is hidden as the
  // LabelShadow and operations on the original actually affect the
  // shadowing label.
  //
  // We should probably try to unify the escaping labels and the return
  // label.
  int nof_escapes = node->escaping_targets()->length();
  List<ShadowTarget*> shadows(1 + nof_escapes);

  // Add the shadow target for the function return.
  static const int kReturnShadowIndex = 0;
  shadows.Add(new ShadowTarget(&function_return_));
  bool function_return_was_shadowed = function_return_is_shadowed_;
  function_return_is_shadowed_ = true;
  ASSERT(shadows[kReturnShadowIndex]->other_target() == &function_return_);

  // Add the remaining shadow targets.
  for (int i = 0; i < nof_escapes; i++) {
    shadows.Add(new ShadowTarget(node->escaping_targets()->at(i)));
  }

  // Generate code for the statements in the try block.

  VisitStatements(node->try_block()->statements());

  // Stop the introduced shadowing and count the number of required unlinks.
  // After shadowing stops, the original labels are unshadowed and the
  // LabelShadows represent the formerly shadowing labels.
  bool has_unlinks = false;
  for (int i = 0; i < shadows.length(); i++) {
    shadows[i]->StopShadowing();
    has_unlinks = has_unlinks || shadows[i]->is_linked();
  }
  function_return_is_shadowed_ = function_return_was_shadowed;

  // Get an external reference to the handler address.
  ExternalReference handler_address(Top::k_handler_address);

  // If we can fall off the end of the try block, unlink from try chain.
  if (has_valid_frame()) {
    // The next handler address is on top of the frame. Unlink from
    // the handler list and drop the rest of this handler from the
    // frame.
    ASSERT(StackHandlerConstants::kNextOffset == 0);
    frame_->EmitPop(a1);
    __ li(a3, Operand(handler_address));
    __ sw(a1, MemOperand(a3));
    frame_->Drop(StackHandlerConstants::kSize / kPointerSize - 1);
    if (has_unlinks) {
      exit.Jump();
    }
  }

  // Generate unlink code for the (formerly) shadowing labels that have been
  // jumped to.  Deallocate each shadow target.
  for (int i = 0; i < shadows.length(); i++) {
    if (shadows[i]->is_linked()) {
      // Unlink from try chain;
      shadows[i]->Bind();
      // Because we can be jumping here (to spilled code) from unspilled
      // code, we need to reestablish a spilled frame at this block.
      frame_->SpillAll();

      // Reload sp from the top handler, because some statements that we
      // break from (eg, for...in) may have left stuff on the stack.
      __ li(a3, Operand(handler_address));
      __ lw(sp, MemOperand(a3));
      frame_->Forget(frame_->height() - handler_height);

      ASSERT(StackHandlerConstants::kNextOffset == 0);
      frame_->EmitPop(a1);
      __ sw(a1, MemOperand(a3));
      frame_->Drop(StackHandlerConstants::kSize / kPointerSize - 1);

      if (!function_return_is_shadowed_ && i == kReturnShadowIndex) {
        frame_->PrepareForReturn();
      }
      shadows[i]->other_target()->Jump();
    }
  }

  exit.Bind();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitTryFinallyStatement(TryFinallyStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ TryFinallyStatement");
  CodeForStatementPosition(node);

  // State: Used to keep track of reason for entering the finally
  // block. Should probably be extended to hold information for
  // break/continue from within the try block.
  enum { FALLING, THROWING, JUMPING };

  JumpTarget try_block;
  JumpTarget finally_block;

  try_block.Call();

  frame_->EmitPush(v0);  // Save exception object on the stack.
  // In case of thrown exceptions, this is where we continue.
  __ li(a2, Operand(Smi::FromInt(THROWING)));
  finally_block.Jump();

  // --- Try block ---
  try_block.Bind();

  frame_->PushTryHandler(TRY_FINALLY_HANDLER);
  int handler_height = frame_->height();

  // Shadow the labels for all escapes from the try block, including
  // returns. Shadowing hides the original label as the LabelShadow and
  // operations on the original actually affect the shadowing label.

  // We should probably try to unify the escaping labels and the return
  // label.
  int nof_escapes = node->escaping_targets()->length();
  List<ShadowTarget*> shadows(1 + nof_escapes);

  // Add the shadow target for the function return.
  static const int kReturnShadowIndex = 0;
  shadows.Add(new ShadowTarget(&function_return_));
  bool function_return_was_shadowed = function_return_is_shadowed_;
  function_return_is_shadowed_ = true;
  ASSERT(shadows[kReturnShadowIndex]->other_target() == &function_return_);

  // Add the remaining shadow targets.
  for (int i = 0; i < nof_escapes; i++) {
    shadows.Add(new ShadowTarget(node->escaping_targets()->at(i)));
  }

  // Generate code for the statements in the try block.
  VisitStatements(node->try_block()->statements());

  // Stop the introduced shadowing and count the number of required unlinks.
  // After shadowing stops, the original labels are unshadowed and the
  // LabelShadows represent the formerly shadowing labels.
  int nof_unlinks = 0;
  for (int i = 0; i < shadows.length(); i++) {
    shadows[i]->StopShadowing();
    if (shadows[i]->is_linked()) nof_unlinks++;
  }
  function_return_is_shadowed_ = function_return_was_shadowed;

  // Get an external reference to the handler address.
  ExternalReference handler_address(Top::k_handler_address);

  // If we can fall off the end of the try block, unlink from the try
  // chain and set the state on the frame to FALLING.
  if (has_valid_frame()) {
    // The next handler address is on top of the frame.
    ASSERT(StackHandlerConstants::kNextOffset == 0);
    frame_->EmitPop(a1);
    __ li(a3, Operand(handler_address));
    __ sw(a1, MemOperand(a3));
    frame_->Drop(StackHandlerConstants::kSize / kPointerSize - 1);

    // Fake a top of stack value (unneeded when FALLING) and set the
    // state in a2, then jump around the unlink blocks if any.
    __ LoadRoot(v0, Heap::kUndefinedValueRootIndex);
    frame_->EmitPush(v0);
    __ li(a2, Operand(Smi::FromInt(FALLING)));
    if (nof_unlinks > 0) {
      finally_block.Jump();
    }
  }

  // Generate code to unlink and set the state for the (formerly)
  // shadowing targets that have been jumped to.
  for (int i = 0; i < shadows.length(); i++) {
    if (shadows[i]->is_linked()) {
      // If we have come from the shadowed return, the return value is
      // in (a non-refcounted reference to) r0.  We must preserve it
      // until it is pushed.
      //
      // Because we can be jumping here (to spilled code) from
      // unspilled code, we need to reestablish a spilled frame at
      // this block.
      shadows[i]->Bind();
      frame_->SpillAll();

      // Reload sp from the top handler, because some statements that
      // we break from (eg, for...in) may have left stuff on the
      // stack.
      __ li(a3, Operand(handler_address));
      __ lw(sp, MemOperand(a3));
      frame_->Forget(frame_->height() - handler_height);

      // Unlink this handler and drop it from the frame.  The next
      // handler address is currently on top of the frame.
      ASSERT(StackHandlerConstants::kNextOffset == 0);
      frame_->EmitPop(a1);
      __ sw(a1, MemOperand(a3));
      frame_->Drop(StackHandlerConstants::kSize / kPointerSize - 1);

      if (i == kReturnShadowIndex) {
        // If this label shadowed the function return, materialize the
        // return value on the stack.
        frame_->EmitPush(v0);
      } else {
        // Fake TOS for targets that shadowed breaks and continues.
        __ LoadRoot(v0, Heap::kUndefinedValueRootIndex);
        frame_->EmitPush(v0);
      }
      __ li(a2, Operand(Smi::FromInt(JUMPING + i)));
      if (--nof_unlinks > 0) {
        // If this is not the last unlink block, jump around the next.
        finally_block.Jump();
      }
    }
  }

  // --- Finally block ---
  finally_block.Bind();

  // Push the state on the stack.
  frame_->EmitPush(a2);

  // We keep two elements on the stack - the (possibly faked) result
  // and the state - while evaluating the finally block.
  //
  // Generate code for the statements in the finally block.
  VisitStatements(node->finally_block()->statements());

  if (has_valid_frame()) {
    // Restore state and return value or faked TOS.
    frame_->EmitPop(a2);
    frame_->EmitPop(v0);
  }

  // Generate code to jump to the right destination for all used
  // formerly shadowing targets.  Deallocate each shadow target.
  for (int i = 0; i < shadows.length(); i++) {
    if (has_valid_frame() && shadows[i]->is_bound()) {
      JumpTarget* original = shadows[i]->other_target();
      if (!function_return_is_shadowed_ && i == kReturnShadowIndex) {
        JumpTarget skip;
        skip.Branch(ne, a2, Operand(Smi::FromInt(JUMPING + i)), no_hint);
        frame_->PrepareForReturn();
        original->Jump();
        skip.Bind();
      } else {
        original->Branch(eq, a2, Operand(Smi::FromInt(JUMPING + i)), no_hint);
      }
    }
  }

  if (has_valid_frame()) {
    // Check if we need to rethrow the exception.
    JumpTarget exit;
    exit.Branch(ne, a2, Operand(Smi::FromInt(THROWING)), no_hint);

    // Rethrow exception.
    frame_->EmitPush(v0);
    frame_->CallRuntime(Runtime::kReThrow, 1);

    // Done.
    exit.Bind();
  }
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitDebuggerStatement(DebuggerStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ DebuggerStatament");
  CodeForStatementPosition(node);
#ifdef ENABLE_DEBUGGER_SUPPORT
  frame_->DebugBreak();
#endif
  // Ignore the return value.
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::InstantiateFunction(
    Handle<SharedFunctionInfo> function_info) {

    // Create a new closure.
    frame_->EmitPush(cp);
    frame_->EmitPush(Operand(function_info));
    frame_->CallRuntime(Runtime::kNewClosure, 2);
    frame_->EmitPush(v0);
}


void CodeGenerator::VisitFunctionLiteral(FunctionLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
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
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitSharedFunctionInfoLiteral(
    SharedFunctionInfoLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ SharedFunctionInfoLiteral");
  InstantiateFunction(node->shared_function_info());
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitConditional(Conditional* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ Conditional");
  JumpTarget then;
  JumpTarget else_;
  LoadCondition(node->condition(), &then, &else_, true);
  if (has_valid_frame()) {
    Branch(false, &else_);
  }
  if (has_valid_frame() || then.is_linked()) {
    then.Bind();
    Load(node->then_expression());
  }
  if (else_.is_linked()) {
    JumpTarget exit;
    if (has_valid_frame()) exit.Jump();
    else_.Bind();
    Load(node->else_expression());
    if (exit.is_linked()) exit.Bind();
  }
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitSlot(Slot* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Slot");
  LoadFromSlotCheckForArguments(node, NOT_INSIDE_TYPEOF);
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitVariableProxy(VariableProxy* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
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
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitLiteral(Literal* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Literal");
  Register reg = frame_->GetTOSRegister();
  bool is_smi = node->handle()->IsSmi();
  __ li(reg, Operand(node->handle()));
  frame_->EmitPush(reg, is_smi ? TypeInfo::Smi() : TypeInfo::Unknown());
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitRegExpLiteral(RegExpLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ RexExp Literal");

  // Retrieve the literal array and check the allocated entry.

  // Load the function of this activation.
  __ lw(a1, frame_->Function());

  // Load the literals array of the function.
  __ lw(a1, FieldMemOperand(a1, JSFunction::kLiteralsOffset));

  // Load the literal at the ast saved index.
  int literal_offset =
      FixedArray::kHeaderSize + node->literal_index() * kPointerSize;
  __ lw(a2, FieldMemOperand(a1, literal_offset));

  JumpTarget done;
  __ LoadRoot(a0, Heap::kUndefinedValueRootIndex);
  done.Branch(ne, a2, Operand(a0));

  // If the entry is undefined we call the runtime system to compute
  // the literal.
  __ li(t1, Operand(Smi::FromInt(node->literal_index())));
  __ li(t2, Operand(node->pattern()));
  __ li(t3, Operand(node->flags()));
  // a1 : literal array  (0)
  // t1 : literal index  (1)
  // t2 : RegExp pattern (2)
  // t3 : RegExp flags (3)
  frame_->EmitMultiPushReversed(a1.bit() | t1.bit() | t2.bit() | t3.bit());
  frame_->CallRuntime(Runtime::kMaterializeRegExpLiteral, 4);
  __ mov(a2, v0);

  done.Bind();
  // Push the literal.
  frame_->EmitPush(a2);
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitObjectLiteral(ObjectLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ ObjectLiteral");

  // Load the function of this activation.
  __ lw(a3, frame_->Function());
  // Literal array.
  __ lw(t3, FieldMemOperand(a3, JSFunction::kLiteralsOffset));
  // Literal index.
  __ li(t2, Operand(Smi::FromInt(node->literal_index())));
  // Constant properties.
  __ li(t1, Operand(node->constant_properties()));
  // Should the object literal have fast elements?
  __ li(t0, Operand(Smi::FromInt(node->fast_elements() ? 1 : 0)));
  frame_->EmitMultiPush(t3.bit() | t2.bit() | t1.bit() | t0.bit());

  if (node->depth() > 1) {
    frame_->CallRuntime(Runtime::kCreateObjectLiteral, 4);
  } else {
    frame_->CallRuntime(Runtime::kCreateObjectLiteralShallow, 4);
  }
  frame_->EmitPush(v0);  // Save the result.

  for (int i = 0; i < node->properties()->length(); i++) {
    // At the start of each iteration, the top of stack contains
    // the newly created object literal.
    ObjectLiteral::Property* property = node->properties()->at(i);
    Literal* key = property->key();
    Expression* value = property->value();
    switch (property->kind()) {
      case ObjectLiteral::Property::CONSTANT:
        break;
      case ObjectLiteral::Property::MATERIALIZED_LITERAL:
        if (CompileTimeValue::IsCompileTimeValue(property->value())) break;
        // Else fall through
      case ObjectLiteral::Property::COMPUTED:
        if (key->handle()->IsSymbol()) {
          Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
          Load(value);
          frame_->EmitPop(a0);
          __ li(a2, Operand(key->handle()));
          __ lw(a1, frame_->Top());  // Load the receiver.
          frame_->CallCodeObject(ic, RelocInfo::CODE_TARGET, 0);
          break;
        }
        // Else fall through.
      case ObjectLiteral::Property::PROTOTYPE: {
        __ lw(a0, frame_->Top());
        frame_->EmitPush(a0);  // Dup the result.
        Load(key);
        Load(value);
        frame_->CallRuntime(Runtime::kSetProperty, 3);
        break;
      }
      case ObjectLiteral::Property::SETTER: {
        __ lw(a0, frame_->Top());
        frame_->EmitPush(a0);
        Load(key);
        __ li(a0, Operand(Smi::FromInt(1)));
        frame_->EmitPush(a0);
        Load(value);
        frame_->CallRuntime(Runtime::kDefineAccessor, 4);
        break;
      }
      case ObjectLiteral::Property::GETTER: {
        __ lw(a0, frame_->Top());
        frame_->EmitPush(a0);
        Load(key);
        __ li(a0, Operand(Smi::FromInt(0)));
        frame_->EmitPush(a0);
        Load(value);
        frame_->CallRuntime(Runtime::kDefineAccessor, 4);
        break;
      }
    }
  }
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitArrayLiteral(ArrayLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(frame_);
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
    frame_->CallRuntime(Runtime::kCreateArrayLiteral, 3);
  } else if (length > FastCloneShallowArrayStub::kMaximumLength) {
    frame_->CallRuntime(Runtime::kCreateArrayLiteralShallow, 3);
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
    Load(value);
    frame_->EmitPop(a0);

    // Fetch the object literal.
    __ lw(a1, frame_->Top());
    // Get the elements array.
    __ lw(a1, FieldMemOperand(a1, JSObject::kElementsOffset));

    // Write to the indexed properties array.
    int offset = i * kPointerSize + FixedArray::kHeaderSize;
    __ sw(a0, FieldMemOperand(a1, offset));

    // Update the write barrier for the array address.
    __ RecordWrite(a1, Operand(offset), a3, a2);
  }
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitCatchExtensionObject(CatchExtensionObject* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  // Call runtime routine to allocate the catch extension object and
  // assign the exception value to the catch variable.
  Comment cmnt(masm_, "[ CatchExtensionObject");
  Load(node->key());
  Load(node->value());
  frame_->CallRuntime(Runtime::kCreateCatchExtensionObject, 2);
  frame_->EmitPush(v0);
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::EmitSlotAssignment(Assignment* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm(), "[ Variable Assignment");
  Variable* var = node->target()->AsVariableProxy()->AsVariable();
  ASSERT(var != NULL);
  Slot* slot = var->slot();
  ASSERT(slot != NULL);

  // Evaluate the right-hand side.
  if (node->is_compound()) {
    // For a compound assignment the right-hand side is a binary operation
    // between the current property value and the actual right-hand side.
    LoadFromSlot(slot, NOT_INSIDE_TYPEOF);

    // Perform the binary operation.
    Literal* literal = node->value()->AsLiteral();
    bool overwrite_value =
        (node->value()->AsBinaryOperation() != NULL &&
         node->value()->AsBinaryOperation()->ResultOverwriteAllowed());
    if (literal != NULL && literal->handle()->IsSmi()) {
      SmiOperation(node->binary_op(),
                   literal->handle(),
                   false,
                   overwrite_value ? OVERWRITE_RIGHT : NO_OVERWRITE);
    } else {
      GenerateInlineSmi inline_smi =
          loop_nesting() > 0 ? GENERATE_INLINE_SMI : DONT_GENERATE_INLINE_SMI;
      if (literal != NULL) {
        ASSERT(!literal->handle()->IsSmi());
        inline_smi = DONT_GENERATE_INLINE_SMI;
      }
      Load(node->value());
      GenericBinaryOperation(node->binary_op(),
                             overwrite_value ? OVERWRITE_RIGHT : NO_OVERWRITE,
                             inline_smi);
    }
  } else {
    Load(node->value());
  }

  // Perform the assignment.
  if (var->mode() != Variable::CONST || node->op() == Token::INIT_CONST) {
    CodeForSourcePosition(node->position());
    StoreToSlot(slot,
                node->op() == Token::INIT_CONST ? CONST_INIT : NOT_CONST_INIT);
  }
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::EmitNamedPropertyAssignment(Assignment* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm(), "[ Named Property Assignment");
  Variable* var = node->target()->AsVariableProxy()->AsVariable();
  Property* prop = node->target()->AsProperty();
  ASSERT(var == NULL || (prop == NULL && var->is_global()));

  // Initialize name and evaluate the receiver sub-expression if necessary. If
  // the receiver is trivial it is not placed on the stack at this point, but
  // loaded whenever actually needed.
  Handle<String> name;
  bool is_trivial_receiver = false;
  if (var != NULL) {
    name = var->name();
  } else {
    Literal* lit = prop->key()->AsLiteral();
    ASSERT_NOT_NULL(lit);
    name = Handle<String>::cast(lit->handle());
    // Do not materialize the receiver on the frame if it is trivial.
    is_trivial_receiver = prop->obj()->IsTrivial();
    if (!is_trivial_receiver) Load(prop->obj());
  }

  // Change to slow case in the beginning of an initialization block to
  // avoid the quadratic behavior of repeatedly adding fast properties.
  if (node->starts_initialization_block()) {
    // Initialization block consists of assignments of the form expr.x = ..., so
    // this will never be an assignment to a variable, so there must be a
    // receiver object.
    ASSERT_EQ(NULL, var);
    if (is_trivial_receiver) {
      Load(prop->obj());
    } else {
      frame_->Dup();
    }
    frame_->CallRuntime(Runtime::kToSlowProperties, 1);
  }

  // Change to fast case at the end of an initialization block. To prepare for
  // that add an extra copy of the receiver to the frame, so that it can be
  // converted back to fast case after the assignment.
  if (node->ends_initialization_block() && !is_trivial_receiver) {
    frame_->Dup();
  }

  // Stack layout:
  // [tos]   : receiver (only materialized if non-trivial)
  // [tos+1] : receiver if at the end of an initialization block

  // Evaluate the right-hand side.
  if (node->is_compound()) {
    // For a compound assignment the right-hand side is a binary operation
    // between the current property value and the actual right-hand side.
    if (is_trivial_receiver) {
      Load(prop->obj());
    } else if (var != NULL) {
      LoadGlobal();
    } else {
      frame_->Dup();
    }
    EmitNamedLoad(name, var != NULL);

    // Perform the binary operation.
    Literal* literal = node->value()->AsLiteral();
    bool overwrite_value =
        (node->value()->AsBinaryOperation() != NULL &&
         node->value()->AsBinaryOperation()->ResultOverwriteAllowed());
    if (literal != NULL && literal->handle()->IsSmi()) {
      SmiOperation(node->binary_op(),
                   literal->handle(),
                   false,
                   overwrite_value ? OVERWRITE_RIGHT : NO_OVERWRITE);
    } else {
      GenerateInlineSmi inline_smi =
          loop_nesting() > 0 ? GENERATE_INLINE_SMI : DONT_GENERATE_INLINE_SMI;
      if (literal != NULL) {
        ASSERT(!literal->handle()->IsSmi());
        inline_smi = DONT_GENERATE_INLINE_SMI;
      }
      Load(node->value());
      GenericBinaryOperation(node->binary_op(),
                             overwrite_value ? OVERWRITE_RIGHT : NO_OVERWRITE,
                             inline_smi);
    }
  } else {
    // For non-compound assignment just load the right-hand side.
    Load(node->value());
  }

  // Stack layout:
  // [tos]   : value
  // [tos+1] : receiver (only materialized if non-trivial)
  // [tos+2] : receiver if at the end of an initialization block

  // Perform the assignment.  It is safe to ignore constants here.
  ASSERT(var == NULL || var->mode() != Variable::CONST);
  ASSERT_NE(Token::INIT_CONST, node->op());
  if (is_trivial_receiver) {
    // Load the receiver and swap with the value.
    Load(prop->obj());
    Register reg0 = frame_->PopToRegister();
    Register reg1 = frame_->PopToRegister(reg0);
    frame_->EmitPush(reg0);
    frame_->EmitPush(reg1);
  }
  CodeForSourcePosition(node->position());
  bool is_contextual = (var != NULL);
  EmitNamedStore(name, is_contextual);
  frame_->EmitPush(v0);

  // Change to fast case at the end of an initialization block.
  if (node->ends_initialization_block()) {
    ASSERT_EQ(NULL, var);
    // The argument to the runtime call is the receiver.
    if (is_trivial_receiver) {
      Load(prop->obj());
    } else {
      // A copy of the receiver is below the value of the assignment. Swap
      // the receiver and the value of the assignment expression.
      Register reg0 = frame_->PopToRegister();
      Register reg1 = frame_->PopToRegister(reg0);
      frame_->EmitPush(reg0);
      frame_->EmitPush(reg1);
    }
    frame_->CallRuntime(Runtime::kToFastProperties, 1);
  }

  // Stack layout:
  // [tos]   : result

  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::EmitKeyedPropertyAssignment(Assignment* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Keyed Property Assignment");
  Property* prop = node->target()->AsProperty();
  ASSERT_NOT_NULL(prop);

  // Evaluate the receiver subexpression.
  Load(prop->obj());

  // Change to slow case in the beginning of an initialization block to
  // avoid the quadratic behavior of repeatedly adding fast properties.
  if (node->starts_initialization_block()) {
    frame_->Dup();
    frame_->CallRuntime(Runtime::kToSlowProperties, 1);
  }

  // Change to fast case at the end of an initialization block. To prepare for
  // that add an extra copy of the receiver to the frame, so that it can be
  // converted back to fast case after the assignment.
  if (node->ends_initialization_block()) {
    frame_->Dup();
  }

  // Evaluate the key subexpression.
  Load(prop->key());

  // Stack layout:
  // [tos]   : key
  // [tos+1] : receiver
  // [tos+2] : receiver if at the end of an initialization block

  // Evaluate the right-hand side.
  if (node->is_compound()) {
    // For a compound assignment the right-hand side is a binary operation
    // between the current property value and the actual right-hand side.
    // Duplicate receiver and key for loading the current property value.
    frame_->Dup2();
    EmitKeyedLoad();
    frame_->EmitPush(v0);

    // Perform the binary operation.
    Literal* literal = node->value()->AsLiteral();
    bool overwrite_value =
        (node->value()->AsBinaryOperation() != NULL &&
         node->value()->AsBinaryOperation()->ResultOverwriteAllowed());
    if (literal != NULL && literal->handle()->IsSmi()) {
      SmiOperation(node->binary_op(),
                   literal->handle(),
                   false,
                   overwrite_value ? OVERWRITE_RIGHT : NO_OVERWRITE);
    } else {
      GenerateInlineSmi inline_smi =
          loop_nesting() > 0 ? GENERATE_INLINE_SMI : DONT_GENERATE_INLINE_SMI;
      if (literal != NULL) {
        ASSERT(!literal->handle()->IsSmi());
        inline_smi = DONT_GENERATE_INLINE_SMI;
      }
      Load(node->value());
      GenericBinaryOperation(node->binary_op(),
                             overwrite_value ? OVERWRITE_RIGHT : NO_OVERWRITE,
                             inline_smi);
    }
  } else {
    // For non-compound assignment just load the right-hand side.
    Load(node->value());
  }

  // Stack layout:
  // [tos]   : value
  // [tos+1] : key
  // [tos+2] : receiver
  // [tos+3] : receiver if at the end of an initialization block

  // Perform the assignment.  It is safe to ignore constants here.
  ASSERT(node->op() != Token::INIT_CONST);
  CodeForSourcePosition(node->position());
  EmitKeyedStore(prop->key()->type());
  frame_->EmitPush(v0);


  // Stack layout:
  // [tos]   : result
  // [tos+1] : receiver if at the end of an initialization block

  // Change to fast case at the end of an initialization block.
  if (node->ends_initialization_block()) {
    // The argument to the runtime call is the extra copy of the receiver,
    // which is below the value of the assignment. Swap the receiver and
    // the value of the assignment expression.
    Register reg0 = frame_->PopToRegister();
    Register reg1 = frame_->PopToRegister(reg0);
    frame_->EmitPush(reg1);
    frame_->EmitPush(reg0);
    frame_->CallRuntime(Runtime::kToFastProperties, 1);
  }

  // Stack layout:
  // [tos]   : result

  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitAssignment(Assignment* node) {
  VirtualFrame::RegisterAllocationScope scope(this);
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Assignment");

  Variable* var = node->target()->AsVariableProxy()->AsVariable();
  Property* prop = node->target()->AsProperty();

  if (var != NULL && !var->is_global()) {
    EmitSlotAssignment(node);

  } else if ((prop != NULL && prop->key()->IsPropertyName()) ||
             (var != NULL && var->is_global())) {
    // Properties whose keys are property names and global variables are
    // treated as named property references.  We do not need to consider
    // global 'this' because it is not a valid left-hand side.
    EmitNamedPropertyAssignment(node);

  } else if (prop != NULL) {
    // Other properties (including rewritten parameters for a function that
    // uses arguments) are keyed property assignments.
    EmitKeyedPropertyAssignment(node);

  } else {
    // Invalid left-hand side.
    Load(node->target());
    frame_->CallRuntime(Runtime::kThrowReferenceError, 1);
    // The runtime call doesn't actually return but the code generator will
    // still generate code and expects a certain frame height.
    frame_->EmitPush(v0);
  }
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitThrow(Throw* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Throw");

  Load(node->exception());
  CodeForSourcePosition(node->position());
  frame_->CallRuntime(Runtime::kThrow, 1);
  frame_->EmitPush(v0);
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitProperty(Property* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Property");

  { Reference property(this, node);
    property.GetValueAndSpill();
  }
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitCall(Call* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
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
    VirtualFrame::SpilledScope spilled_scope(frame_);
    // ----------------------------------
    // JavaScript example: 'eval(arg)'  // eval is not known to be shadowed.
    // ----------------------------------

    // In a call to eval, we first call %ResolvePossiblyDirectEval to
    // resolve the function we need to call and the receiver of the
    // call.  Then we call the resolved function using the given
    // arguments.

    // Prepare stack for call to resolved function.
    Load(function);

    // Allocate a frame slot for the receiver.
    __ LoadRoot(a2, Heap::kUndefinedValueRootIndex);
    frame_->EmitPush(a2);

    // Load the arguments.
    int arg_count = args->length();
    for (int i = 0; i < arg_count; i++) {
      Load(args->at(i));
    }

    // If we know that eval can only be shadowed by eval-introduced
    // variables we attempt to load the global eval function directly
    // in generated code. If we succeed, there is no need to perform a
    // context lookup in the runtime system.
    JumpTarget done;
    if (var->slot() != NULL && var->mode() == Variable::DYNAMIC_GLOBAL) {
      ASSERT(var->slot()->type() == Slot::LOOKUP);
      JumpTarget slow;
      // Prepare the stack for the call to
      // ResolvePossiblyDirectEvalNoLookup by pushing the loaded
      // function, the first argument to the eval call and the
      // receiver.
      LoadFromGlobalSlotCheckExtensions(var->slot(),
                                        NOT_INSIDE_TYPEOF,
                                        &slow);
      frame_->EmitPush(v0);
      if (arg_count > 0) {
        __ lw(a1, MemOperand(sp, arg_count * kPointerSize));
        frame_->EmitPush(a1);
      } else {
        frame_->EmitPush(a2);
      }
      __ lw(a1, frame_->Receiver());
      frame_->EmitPush(a1);

      frame_->CallRuntime(Runtime::kResolvePossiblyDirectEvalNoLookup, 3);

      done.Jump();
      slow.Bind();
    }

    // Prepare the stack for the call to ResolvePossiblyDirectEval by
    // pushing the loaded function, the first argument to the eval
    // call and the receiver.

    __ lw(a1, MemOperand(sp, arg_count * kPointerSize + kPointerSize));
    frame_->EmitPush(a1);
    if (arg_count > 0) {
      __ lw(a1, MemOperand(sp, arg_count * kPointerSize));
      frame_->EmitPush(a1);
    } else {
      frame_->EmitPush(a2);
    }
    __ lw(a1, frame_->Receiver());
    frame_->EmitPush(a1);

    // Resolve the call.
    frame_->CallRuntime(Runtime::kResolvePossiblyDirectEval, 3);

    // If we generated fast-case code bind the jump-target where fast
    // and slow case merge.
    if (done.is_linked()) done.Bind();

    // Touch up stack with the right values for the function and the receiver.
    // Runtime::kResolvePossiblyDirectEval returns object pair in v0/v1.
    __ sw(v0, MemOperand(sp, (arg_count + 1) * kPointerSize));
    __ sw(v1, MemOperand(sp, arg_count * kPointerSize));

    // Call the function.
    CodeForSourcePosition(node->position());

    InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
    CallFunctionStub call_function(arg_count, in_loop, RECEIVER_MIGHT_BE_VALUE);
    frame_->CallStub(&call_function, arg_count + 1);

    __ lw(cp, frame_->Context());
    // Remove the function from the stack.
    frame_->Drop();
    frame_->EmitPush(v0);

  } else if (var != NULL && !var->is_this() && var->is_global()) {
    // -----------------------------------------------------
    // JavaScript example: 'foo(1, 2, 3)'  // foo is global.
    // -----------------------------------------------------
    // Pass the global object as the receiver and let the IC stub
    // patch the stack to use the global proxy as 'this' in the
    // invoked function.
    LoadGlobal();

    // Load the arguments.
    int arg_count = args->length();
    for (int i = 0; i < arg_count; i++) {
      Load(args->at(i));
    }

    VirtualFrame::SpilledScope spilled_scope(frame_);
    // Setup the receiver register and call the IC initialization code.
    __ li(a2, Operand(var->name()));
    InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
    Handle<Code> stub = ComputeCallInitialize(arg_count, in_loop);
    CodeForSourcePosition(node->position());
    frame_->CallCodeObject(stub, RelocInfo::CODE_TARGET_CONTEXT,
                           arg_count + 1);
    __ lw(cp, frame_->Context());
    // Remove the function from the stack.
    frame_->EmitPush(v0);

  } else if (var != NULL && var->slot() != NULL &&
             var->slot()->type() == Slot::LOOKUP) {
    VirtualFrame::SpilledScope spilled_scope(frame_);
    // ----------------------------------
    // JavaScript examples:
    //
    //  with (obj) foo(1, 2, 3)  // foo may be in obj.
    //
    //  function f() {};
    //  function g() {
    //    eval(...);
    //    f();  // f could be in extension object.
    //  }
    // ----------------------------------

    // JumpTargets do not yet support merging frames so the frame must be
    // spilled when jumping to these targets.
    JumpTarget slow, done;

    // Generate fast case for loading functions from slots that
    // correspond to local/global variables or arguments unless they
    // are shadowed by eval-introduced bindings.
    EmitDynamicLoadFromSlotFastCase(var->slot(),
                                    NOT_INSIDE_TYPEOF,
                                    &slow,
                                    &done);

    slow.Bind();
    // Load the function
    frame_->EmitPush(cp);
    __ li(a0, Operand(var->name()));
    frame_->EmitPush(a0);
    frame_->CallRuntime(Runtime::kLoadContextSlot, 2);
    // v0: slot value; v1: receiver

    // Load the receiver.
    // Push the function and receiver on the stack.
    frame_->EmitMultiPushReversed(v0.bit() | v1.bit());

    // If fast case code has been generated, emit code to push the
    // function and receiver and have the slow path jump around this
    // code.
    if (done.is_linked()) {
      JumpTarget call;
      call.Jump();
      done.Bind();
      frame_->EmitPush(v0);  // function
      LoadGlobalReceiver(v1);  // receiver
      call.Bind();
    }

    // Call the function. At this point, everything is spilled but the
    // function and receiver are in v0 and v1.
    CallWithArguments(args, NO_CALL_FUNCTION_FLAGS, node->position());
    frame_->EmitPush(v0);

  } else if (property != NULL) {
    // Check if the key is a literal string.
    Literal* literal = property->key()->AsLiteral();

    if (literal != NULL && literal->handle()->IsSymbol()) {
      // ------------------------------------------------------------------
      // JavaScript example: 'object.foo(1, 2, 3)' or 'map["key"](1, 2, 3)'
      // ------------------------------------------------------------------

      Handle<String> name = Handle<String>::cast(literal->handle());

      if (ArgumentsMode() == LAZY_ARGUMENTS_ALLOCATION &&
          name->IsEqualTo(CStrVector("apply")) &&
          args->length() == 2 &&
          args->at(1)->AsVariableProxy() != NULL &&
          args->at(1)->AsVariableProxy()->IsArguments()) {
        // Use the optimized Function.prototype.apply that avoids
        // allocating lazily allocated arguments objects.
        CallApplyLazy(property->obj(),
                      args->at(0),
                      args->at(1)->AsVariableProxy(),
                      node->position());

      } else {
        Load(property->obj());  // Receiver.
        // Load the arguments.
        int arg_count = args->length();
        for (int i = 0; i < arg_count; i++) {
          Load(args->at(i));
        }

        VirtualFrame::SpilledScope spilled_scope(frame_);
        // Set the name register and call the IC initialization code.
        __ li(a2, Operand(name));
        InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
        Handle<Code> stub = ComputeCallInitialize(arg_count, in_loop);
        CodeForSourcePosition(node->position());
        frame_->CallCodeObject(stub, RelocInfo::CODE_TARGET, arg_count + 1);
        __ lw(cp, frame_->Context());
        frame_->EmitPush(v0);
      }

    } else {
      // -------------------------------------------
      // JavaScript example: 'array[index](1, 2, 3)'
      // -------------------------------------------
      VirtualFrame::SpilledScope spilled_scope(frame_);

      Load(property->obj());
      if (property->is_synthetic()) {
        Load(property->key());
        EmitKeyedLoad();
        // Put the function below the receiver.
        // Use the global receiver.
        frame_->EmitPush(v0);  // Function.
        LoadGlobalReceiver(a0);
        // Call the function.
        CallWithArguments(args, RECEIVER_MIGHT_BE_VALUE, node->position());
        frame_->EmitPush(v0);
      } else {
        // Load the arguments.
        int arg_count = args->length();
        for (int i = 0; i < arg_count; i++) {
          Load(args->at(i));
        }

        // Set the name register and call the IC initialization code.
        Load(property->key());
        frame_->EmitPop(a2);  // Function name.

        InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
        Handle<Code> stub = ComputeKeyedCallInitialize(arg_count, in_loop);
        CodeForSourcePosition(node->position());
        frame_->CallCodeObject(stub, RelocInfo::CODE_TARGET, arg_count + 1);
        __ lw(cp, frame_->Context());
        frame_->EmitPush(v0);
      }
    }

  } else {
    // --------------------------------------------------------
    // JavaScript example: 'foo(1, 2, 3)'  // foo is not global
    // --------------------------------------------------------

    // Load the function.
    Load(function);

    VirtualFrame::SpilledScope spilled_scope(frame_);

    // Pass the global proxy as the receiver.
    LoadGlobalReceiver(a0);

    // Call the function (and allocate args slots).
    CallWithArguments(args, NO_CALL_FUNCTION_FLAGS, node->position());
    frame_->EmitPush(v0);
  }

  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitCallNew(CallNew* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ CallNew");

  // According to ECMA-262, section 11.2.2, page 44, the function
  // expression in new calls must be evaluated before the
  // arguments. This is different from ordinary calls, where the
  // actual function to call is resolved after the arguments have been
  // evaluated.

  // Compute function to call and use the global object as the
  // receiver. There is no need to use the global proxy here because
  // it will always be replaced with a newly allocated object.
  Load(node->expression());
  LoadGlobal();

  ZoneList<Expression*>* args = node->arguments();
  int arg_count = args->length();
  // Push the arguments ("left-to-right") on the stack.
  for (int i = 0; i < arg_count; i++) {
    Load(args->at(i));
  }

  VirtualFrame::SpilledScope spilled_scope(frame_);

  // a0: the number of arguments.
  __ li(a0, Operand(arg_count));
  // Load the function into a1 as per calling convention.
  __ lw(a1, frame_->ElementAt(arg_count + 1));

  // Call the construct call builtin that handles allocation and
  // constructor invocation.
  CodeForSourcePosition(node->position());
  Handle<Code> ic(Builtins::builtin(Builtins::JSConstructCall));
  frame_->CallCodeObject(ic, RelocInfo::CONSTRUCT_CALL, arg_count + 1);

  // Discard old TOS value and push v0 on the stack (same as Pop(), push(v0)).
  __ sw(v0, frame_->Top());
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::GenerateClassOf(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  ASSERT(args->length() == 1);
  JumpTarget leave, null, function, non_function_constructor;

  // Load the object into a0.
  Load(args->at(0));
  frame_->EmitPop(a0);

  // If the object is a smi, we return null.
  __ And(t0, a0, Operand(kSmiTagMask));
  null.Branch(eq, t0, Operand(zero_reg), no_hint);

  // Check that the object is a JS object but take special care of JS
  // functions to make sure they have 'Function' as their class.
  __ GetObjectType(a0, a0, a1);
  null.Branch(less, a1, Operand(FIRST_JS_OBJECT_TYPE), no_hint);

  // As long as JS_FUNCTION_TYPE is the last instance type and it is
  // right after LAST_JS_OBJECT_TYPE, we can avoid checking for
  // LAST_JS_OBJECT_TYPE.
  ASSERT(LAST_TYPE == JS_FUNCTION_TYPE);
  ASSERT(JS_FUNCTION_TYPE == LAST_JS_OBJECT_TYPE + 1);
  function.Branch(eq, a1, Operand(JS_FUNCTION_TYPE), no_hint);

  // Check if the constructor in the map is a function.
  __ lw(a0, FieldMemOperand(a0, Map::kConstructorOffset));
  __ GetObjectType(a0, a1, a1);
  non_function_constructor.Branch(ne, a1, Operand(JS_FUNCTION_TYPE), no_hint);

  // The a0 register now contains the constructor function. Grab the
  // instance class name from there.
  __ lw(a0, FieldMemOperand(a0, JSFunction::kSharedFunctionInfoOffset));
  __ lw(v0, FieldMemOperand(a0, SharedFunctionInfo::kInstanceClassNameOffset));
  frame_->EmitPush(v0);
  leave.Jump();

  // Functions have class 'Function'.
  function.Bind();
  __ li(v0, Operand(Factory::function_class_symbol()));
  frame_->EmitPush(v0);
  leave.Jump();

  // Objects with a non-function constructor have class 'Object'.
  non_function_constructor.Bind();
  __ li(v0, Operand(Factory::Object_symbol()));
  frame_->EmitPush(v0);
  leave.Jump();

  // Non-JS objects have class null.
  null.Bind();
  __ LoadRoot(v0, Heap::kNullValueRootIndex);
  frame_->EmitPush(v0);

  // All done.
  leave.Bind();
}


void CodeGenerator::GenerateValueOf(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  ASSERT(args->length() == 1);
  JumpTarget leave;
  Load(args->at(0));
  frame_->EmitPop(v0);  // v0 contains object.
  // if (object->IsSmi()) return the object.
  __ And(t0, v0, Operand(kSmiTagMask));
  leave.Branch(eq, t0, Operand(zero_reg));
  // It is a heap object - get map. If (!object->IsJSValue()) return the object.
  __ GetObjectType(v0, a1, a1);
  leave.Branch(ne, a1, Operand(JS_VALUE_TYPE));
  // Load the value.
  __ lw(v0, FieldMemOperand(v0, JSValue::kValueOffset));
  leave.Bind();
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateSetValueOf(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  ASSERT(args->length() == 2);
  JumpTarget leave;
  Load(args->at(0));  // Load the object.
  Load(args->at(1));  // Load the value.
  frame_->EmitPop(v0);  // v0 contains value.
  frame_->EmitPop(a1);  // a1 contains object.
  // if (object->IsSmi()) return value.
  __ And(t1, a1, Operand(kSmiTagMask));
  leave.Branch(eq, t1, Operand(zero_reg), no_hint);
  // It is a heap object - get map. If (!object->IsJSValue()) return the value.
  __ GetObjectType(a1, a2, a2);
  leave.Branch(ne, a2, Operand(JS_VALUE_TYPE), no_hint);
  // Store the value in object, and return value.
  __ sw(v0, FieldMemOperand(a1, JSValue::kValueOffset));
  // Update the write barrier.
  __ RecordWrite(a1, Operand(JSValue::kValueOffset - kHeapObjectTag), a2, a3);
  // Leave.
  leave.Bind();
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateIsSmi(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 1);
  Load(args->at(0));
  Register reg = frame_->PopToRegister();
  __ And(condReg1, reg, Operand(kSmiTagMask));
  __ mov(condReg2, zero_reg);
  cc_reg_ = eq;
}


void CodeGenerator::GenerateLog(ZoneList<Expression*>* args) {
  // See comment in CodeGenerator::GenerateLog in codegen-ia32.cc.
  ASSERT_EQ(args->length(), 3);
#ifdef ENABLE_LOGGING_AND_PROFILING
  if (ShouldGenerateLog(args->at(0))) {
    Load(args->at(1));
    Load(args->at(2));
    frame_->CallRuntime(Runtime::kLog, 2);
  }
#endif
  frame_->EmitPushRoot(Heap::kUndefinedValueRootIndex);
}


void CodeGenerator::GenerateIsNonNegativeSmi(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  ASSERT(args->length() == 1);
  Load(args->at(0));
  frame_->EmitPop(a0);
  __ And(condReg1, a0, Operand(kSmiTagMask | 0x80000000u));
  __ mov(condReg2, zero_reg);
  cc_reg_ = eq;
}


void CodeGenerator::GenerateMathPow(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 2);
  Load(args->at(0));
  Load(args->at(1));
  frame_->CallRuntime(Runtime::kMath_pow, 2);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateMathSin(ZoneList<Expression*>* args) {
  ASSERT_EQ(args->length(), 1);
  Load(args->at(0));
  if (CpuFeatures::IsSupported(FPU)) {
    TranscendentalCacheStub stub(TranscendentalCache::SIN);
    frame_->SpillAllButCopyTOSToA0();
    frame_->CallStub(&stub, 1);
  } else {
    frame_->CallRuntime(Runtime::kMath_sin, 1);
  }
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateMathCos(ZoneList<Expression*>* args) {
  ASSERT_EQ(args->length(), 1);
  Load(args->at(0));
  if (CpuFeatures::IsSupported(FPU)) {
    TranscendentalCacheStub stub(TranscendentalCache::COS);
    frame_->SpillAllButCopyTOSToA0();
    frame_->CallStub(&stub, 1);
  } else {
    frame_->CallRuntime(Runtime::kMath_cos, 1);
  }
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateMathSqrt(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 1);
  Load(args->at(0));
  frame_->CallRuntime(Runtime::kMath_sqrt, 1);
  frame_->EmitPush(v0);
}

class DeferredStringCharCodeAt : public DeferredCode {
 public:
  DeferredStringCharCodeAt(Register object,
                           Register index,
                           Register scratch,
                           Register result)
      : result_(result),
        char_code_at_generator_(object,
                                index,
                                scratch,
                                result,
                                &need_conversion_,
                                &need_conversion_,
                                &index_out_of_range_,
                                STRING_INDEX_IS_NUMBER) {}

  StringCharCodeAtGenerator* fast_case_generator() {
    return &char_code_at_generator_;
  }

  virtual void Generate() {
    VirtualFrameRuntimeCallHelper call_helper(frame_state());
    char_code_at_generator_.GenerateSlow(masm(), call_helper);

    __ bind(&need_conversion_);
    // Move the undefined value into the result register, which will
    // trigger conversion.
    __ LoadRoot(result_, Heap::kUndefinedValueRootIndex);
    __ Branch(exit_label());

    __ bind(&index_out_of_range_);
    // When the index is out of range, the spec requires us to return
    // NaN.
    __ LoadRoot(result_, Heap::kNanValueRootIndex);
    __ Branch(exit_label());
  }

 private:
  Register result_;

  Label need_conversion_;
  Label index_out_of_range_;

  StringCharCodeAtGenerator char_code_at_generator_;
};

// This generates code that performs a String.prototype.charCodeAt() call
// or returns a smi in order to trigger conversion.
void CodeGenerator::GenerateStringCharCodeAt(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment(masm_, "[ GenerateStringCharCodeAt");
  ASSERT(args->length() == 2);

  Load(args->at(0));
  Load(args->at(1));

  Register index = a1;
  Register object = a2;

  frame_->EmitPop(index);
  frame_->EmitPop(object);

  // We need two extra registers.
  Register scratch = a3;
  Register result = v0;

  DeferredStringCharCodeAt* deferred =
      new DeferredStringCharCodeAt(object,
                                   index,
                                   scratch,
                                   result);
  deferred->fast_case_generator()->GenerateFast(masm_);
  deferred->BindExit();
  frame_->EmitPush(result);
}

class DeferredStringCharFromCode : public DeferredCode {
 public:
  DeferredStringCharFromCode(Register code,
                             Register result)
      : char_from_code_generator_(code, result) {}

  StringCharFromCodeGenerator* fast_case_generator() {
    return &char_from_code_generator_;
  }

  virtual void Generate() {
    VirtualFrameRuntimeCallHelper call_helper(frame_state());
    char_from_code_generator_.GenerateSlow(masm(), call_helper);
  }

 private:
  StringCharFromCodeGenerator char_from_code_generator_;
};

// Generates code for creating a one-char string from a char code.
void CodeGenerator::GenerateStringCharFromCode(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment(masm_, "[ GenerateStringCharFromCode");
  ASSERT(args->length() == 1);

  Load(args->at(0));

  Register code = a1;
  Register result = v0;

  frame_->EmitPop(code);

  DeferredStringCharFromCode* deferred = new DeferredStringCharFromCode(
      code, result);
  deferred->fast_case_generator()->GenerateFast(masm_);
  deferred->BindExit();
  frame_->EmitPush(result);
}

class DeferredStringCharAt : public DeferredCode {
 public:
  DeferredStringCharAt(Register object,
                       Register index,
                       Register scratch1,
                       Register scratch2,
                       Register result)
      : result_(result),
        char_at_generator_(object,
                           index,
                           scratch1,
                           scratch2,
                           result,
                           &need_conversion_,
                           &need_conversion_,
                           &index_out_of_range_,
                           STRING_INDEX_IS_NUMBER) {}

  StringCharAtGenerator* fast_case_generator() {
    return &char_at_generator_;
  }

  virtual void Generate() {
    VirtualFrameRuntimeCallHelper call_helper(frame_state());
    char_at_generator_.GenerateSlow(masm(), call_helper);

    __ bind(&need_conversion_);
    // Move smi zero into the result register, which will trigger
    // conversion.
    __ li(result_, Operand(Smi::FromInt(0)));
    __ Branch(exit_label());

    __ bind(&index_out_of_range_);
    // When the index is out of range, the spec requires us to return
    // the empty string.
    __ LoadRoot(result_, Heap::kEmptyStringRootIndex);
    __ Branch(exit_label());
  }

 private:
  Register result_;

  Label need_conversion_;
  Label index_out_of_range_;

  StringCharAtGenerator char_at_generator_;
};


// This generates code that performs a String.prototype.charAt() call
// or returns a smi in order to trigger conversion.
void CodeGenerator::GenerateStringCharAt(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment(masm_, "[ GenerateStringCharAt");
  ASSERT(args->length() == 2);

  Load(args->at(0));
  Load(args->at(1));

  Register index = a1;
  Register object = a2;

  frame_->EmitPop(a1);
  frame_->EmitPop(a2);

  // We need three extra registers.
  Register scratch1 = a3;
  Register scratch2 = t1;
  Register result = v0;

  DeferredStringCharAt* deferred =
      new DeferredStringCharAt(object,
                               index,
                               scratch1,
                               scratch2,
                               result);
  deferred->fast_case_generator()->GenerateFast(masm_);
  deferred->BindExit();
  frame_->EmitPush(result);
}

void CodeGenerator::GenerateIsArray(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 1);
  Load(args->at(0));
  JumpTarget answer;

  // We need the condition to be not_equal if the object is a smi.
  Register possible_array = frame_->PopToRegister();
  Register scratch = VirtualFrame::scratch0();
  __ And(scratch, possible_array, Operand(kSmiTagMask));
  __ Xor(condReg1, scratch, Operand(kSmiTagMask));
  __ mov(condReg2, zero_reg);
  answer.Branch(eq, scratch, Operand(zero_reg));
  // It is a heap object - get the map. Check if the object is a JS array.
  __ GetObjectType(possible_array, scratch, condReg1);
  __ li(condReg2, Operand(JS_ARRAY_TYPE));
  answer.Bind();
  cc_reg_ = eq;
}


void CodeGenerator::GenerateIsRegExp(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 1);
  Load(args->at(0));
  JumpTarget answer;
  // We need the condition to be not_equal is the object is a smi.
  Register possible_regexp = frame_->PopToRegister();
  Register scratch = VirtualFrame::scratch0();
  __ And(scratch, possible_regexp, Operand(kSmiTagMask));
  __ Xor(condReg1, scratch, Operand(kSmiTagMask));
  __ mov(condReg2, zero_reg);
  answer.Branch(eq, scratch, Operand(zero_reg));
  // It is a heap object - get the map. Check if the object is a regexp.
  __ GetObjectType(possible_regexp, scratch, condReg1);
  __ li(condReg2, Operand(JS_REGEXP_TYPE));
  answer.Bind();
  cc_reg_ = eq;
}


void CodeGenerator::GenerateIsConstructCall(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 0);

  Register scratch0 = VirtualFrame::scratch0();
  Register scratch1 = VirtualFrame::scratch1();
  // Get the frame pointer for the calling frame.
  __ lw(scratch0, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));

  // Skip the arguments adaptor frame if it exists.
  Label check_frame_marker;
  __ lw(scratch1,
        MemOperand(scratch0, StandardFrameConstants::kContextOffset));
  __ Branch(&check_frame_marker, ne,
            scratch1, Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));
  __ lw(scratch0,
        MemOperand(scratch0, StandardFrameConstants::kCallerFPOffset));

  // Check the marker in the calling frame.
  __ bind(&check_frame_marker);
  __ lw(condReg1, MemOperand(scratch0, StandardFrameConstants::kMarkerOffset));
  __ li(condReg2, Operand(Smi::FromInt(StackFrame::CONSTRUCT)));
  cc_reg_ = eq;
}


void CodeGenerator::GenerateArgumentsLength(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 0);

  Label exit;
  Register tos = frame_->GetTOSRegister();
  Register scratch0 = VirtualFrame::scratch0();
  Register scratch1 = VirtualFrame::scratch1();

  // Get the number of formal parameters.
  __ li(tos, Operand(Smi::FromInt(scope()->num_parameters())));

  __ lw(scratch0, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ lw(scratch1, MemOperand(scratch0, StandardFrameConstants::kContextOffset));
  __ Branch(&exit,
            ne,
            scratch1,
            Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));

  // Arguments adaptor case: Read the arguments length from the
  // adaptor frame and return it.
  __ lw(tos, MemOperand(scratch0,
                        ArgumentsAdaptorFrameConstants::kLengthOffset));

  __ bind(&exit);
  frame_->EmitPush(tos);
}


void CodeGenerator::GenerateArguments(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  ASSERT(args->length() == 1);

  // Satisfy contract with ArgumentsAccessStub:
  // Load the key into a1 and the formal parameters count into a0.
  Load(args->at(0));
  frame_->EmitPop(a1);
  __ li(a0, Operand(Smi::FromInt(scope()->num_parameters())));

  // Call the shared stub to get to arguments[key].
  ArgumentsAccessStub stub(ArgumentsAccessStub::READ_ELEMENT);
  frame_->CallStub(&stub, 0);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateRandomHeapNumber(
    ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  ASSERT(args->length() == 0);

  Label slow_allocate_heapnumber;
  Label heapnumber_allocated;

  // Save the new heap number in callee-saved register s0, since
  // we call out to external C code below.
  __ AllocateHeapNumber(s0, a1, a2, &slow_allocate_heapnumber);
  __ jmp(&heapnumber_allocated);

  __ bind(&slow_allocate_heapnumber);
  // To allocate a heap number, and ensure that it is not a smi, we
  // call the runtime function FUnaryMinus on 0, returning the double
  // -0.0. A new, distinct heap number is returned each time.
  __ li(a0, Operand(Smi::FromInt(0)));
  __ Push(a0);
  __ CallRuntime(Runtime::kNumberUnaryMinus, 1);
  __ mov(s0, v0);   // move the new heap-number in v0, to s0 as parameter.

  __ bind(&heapnumber_allocated);

  // Convert 32 random bits in r0 to 0.(32 random bits) in a double
  // by computing:
  // ( 1.(20 0s)(32 random bits) x 2^20 ) - (1.0 x 2^20)).
  if (CpuFeatures::IsSupported(FPU)) {
    __ PrepareCallCFunction(0, a1);
    __ CallCFunction(ExternalReference::random_uint32_function(), 0);

    CpuFeatures::Scope scope(FPU);
    // 0x41300000 is the top half of 1.0 x 2^20 as a double.
    __ li(a1, Operand(0x41300000));
    // Move 0x41300000xxxxxxxx (x = random bits in v0) to FPU.
    __ mtc1(a1, f13);
    __ mtc1(v0, f12);
    // Move 0x4130000000000000 to FPU.
    __ mtc1(a1, f15);
    __ mtc1(zero_reg, f14);
    // Subtract and store the result in the heap number.
    __ sub_d(f0, f12, f14);
    __ sdc1(f0, MemOperand(s0, HeapNumber::kValueOffset - kHeapObjectTag));
    frame_->EmitPush(s0);
  } else {
    __ mov(a0, s0);
    __ PrepareCallCFunction(1, a1);
    __ CallCFunction(
        ExternalReference::fill_heap_number_with_random_function(), 1);
    frame_->EmitPush(v0);
  }
}


void CodeGenerator::GenerateObjectEquals(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 2);

  // Load the two objects into registers and perform the comparison.
  Load(args->at(0));
  Load(args->at(1));
  Register lhs = frame_->PopToRegister();
  Register rhs = frame_->PopToRegister(lhs);
  __ mov(condReg1, lhs);
  __ mov(condReg2, rhs);
  cc_reg_ = eq;
}


void CodeGenerator::GenerateIsObject(ZoneList<Expression*>* args) {
  // This generates a fast version of:
  // (typeof(arg) === 'object' || %_ClassOf(arg) == 'RegExp')
  ASSERT(args->length() == 1);
  Load(args->at(0));
  Register possible_object = frame_->PopToRegister();
  __ And(t1, possible_object, Operand(kSmiTagMask));
  false_target()->Branch(eq, t1, Operand(zero_reg));

  __ LoadRoot(t0, Heap::kNullValueRootIndex);
  true_target()->Branch(eq, possible_object, Operand(t0));

  // scratch0 == t4, so it's safe to use t1 below.
  Register map_reg = VirtualFrame::scratch0();
  __ lw(map_reg, FieldMemOperand(possible_object, HeapObject::kMapOffset));
  // Undetectable objects behave like undefined when tested with typeof.
  __ lbu(possible_object, FieldMemOperand(map_reg, Map::kBitFieldOffset));
  __ And(t1, possible_object, Operand(1 << Map::kIsUndetectable));
  false_target()->Branch(ne, t1, Operand(zero_reg));

  __ lbu(possible_object, FieldMemOperand(map_reg, Map::kInstanceTypeOffset));
  false_target()->Branch(less, possible_object, Operand(FIRST_JS_OBJECT_TYPE));
  __ mov(condReg1, possible_object);
  __ li(condReg2, Operand(LAST_JS_OBJECT_TYPE));
  cc_reg_ = less_equal;
}


void CodeGenerator::GenerateIsFunction(ZoneList<Expression*>* args) {
  // This generates a fast version of:
  // (%_ClassOf(arg) === 'Function')
  ASSERT(args->length() == 1);
  Load(args->at(0));
  Register possible_function = frame_->PopToRegister();
  __ And(t0, possible_function, Operand(kSmiTagMask));
  false_target()->Branch(eq, t0, Operand(zero_reg));
  Register map_reg = VirtualFrame::scratch0();
  __ GetObjectType(possible_function, map_reg, condReg1);
  __ li(condReg2, Operand(JS_FUNCTION_TYPE));
  cc_reg_ = eq;
}


void CodeGenerator::GenerateIsUndetectableObject(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 1);
  Load(args->at(0));
  Register possible_undetectable = frame_->PopToRegister();
  __ And(t0, possible_undetectable, Operand(kSmiTagMask));
  false_target()->Branch(eq, t0, Operand(zero_reg));
  Register scratch = VirtualFrame::scratch0();
  __ lw(scratch,
        FieldMemOperand(possible_undetectable, HeapObject::kMapOffset));
  __ lbu(scratch, FieldMemOperand(scratch, Map::kBitFieldOffset));
  __ And(condReg1, scratch, Operand(1 << Map::kIsUndetectable));
  __ mov(condReg2, zero_reg);
  cc_reg_ = ne;
}


void CodeGenerator::GenerateStringAdd(ZoneList<Expression*>* args) {
  Comment cmnt(masm_, "[ GenerateStringAdd");
  ASSERT_EQ(2, args->length());

  Load(args->at(0));
  Load(args->at(1));

  StringAddStub stub(NO_STRING_ADD_FLAGS);
  frame_->SpillAll();
  frame_->CallStub(&stub, 2);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateSubString(ZoneList<Expression*>* args) {
  ASSERT_EQ(3, args->length());

  Load(args->at(0));
  Load(args->at(1));
  Load(args->at(2));

  SubStringStub stub;
  frame_->SpillAll();
  frame_->CallStub(&stub, 3);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateStringCompare(ZoneList<Expression*>* args) {
  ASSERT_EQ(2, args->length());

  Load(args->at(0));
  Load(args->at(1));

  StringCompareStub stub;
  frame_->SpillAll();
  frame_->CallStub(&stub, 2);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateRegExpExec(ZoneList<Expression*>* args) {
  ASSERT_EQ(4, args->length());

  Load(args->at(0));
  Load(args->at(1));
  Load(args->at(2));
  Load(args->at(3));
  RegExpExecStub stub;
  frame_->SpillAll();
  frame_->CallStub(&stub, 4);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateRegExpConstructResult(ZoneList<Expression*>* args) {
  // No stub. This code only occurs a few times in regexp.js.
  const int kMaxInlineLength = 100;
  ASSERT_EQ(3, args->length());
  Load(args->at(0));  // Size of array, smi.
  Load(args->at(1));  // "index" property value.
  Load(args->at(2));  // "input" property value.
  {
    VirtualFrame::SpilledScope spilled_scope();
    Label slowcase;
    Label done;
    __ lw(a1, MemOperand(sp, kPointerSize * 2));
    STATIC_ASSERT(kSmiTag == 0);
    STATIC_ASSERT(kSmiTagSize == 1);
    __ BranchOnNotSmi(a1, &slowcase);
    __ Branch(&slowcase, hi, a1, Operand(Smi::FromInt(kMaxInlineLength)));
    // Smi-tagging is equivalent to multiplying by 2.
    // Allocate RegExpResult followed by FixedArray with size in ebx.
    // JSArray:   [Map][empty properties][Elements][Length-smi][index][input]
    // Elements:  [Map][Length][..elements..]
    // Size of JSArray with two in-object properties and the header of a
    // FixedArray.
    int objects_size =
        (JSRegExpResult::kSize + FixedArray::kHeaderSize) / kPointerSize;
    __ srl(t1, a1, kSmiTagSize + kSmiShiftSize);
    __ Addu(a2, t1, Operand(objects_size));
    __ AllocateInNewSpace(
        a2,  // In: Size, in words.
        v0,  // Out: Start of allocation (tagged).
        a3,  // Scratch register.
        t0,  // Scratch register.
        &slowcase,
        static_cast<AllocationFlags>(TAG_OBJECT | SIZE_IN_WORDS));
    // v0: Start of allocated area, object-tagged.
    // a1: Number of elements in array, as smi.
    // t1: Number of elements, untagged.

    // Set JSArray map to global.regexp_result_map().
    // Set empty properties FixedArray.
    // Set elements to point to FixedArray allocated right after the JSArray.
    // Interleave operations for better latency.
    __ lw(a2, ContextOperand(cp, Context::GLOBAL_INDEX));
    __ Addu(a3, v0, Operand(JSRegExpResult::kSize));
    __ li(t0, Operand(Factory::empty_fixed_array()));
    __ lw(a2, FieldMemOperand(a2, GlobalObject::kGlobalContextOffset));
    __ sw(a3, FieldMemOperand(v0, JSObject::kElementsOffset));
    __ lw(a2, ContextOperand(a2, Context::REGEXP_RESULT_MAP_INDEX));
    __ sw(t0, FieldMemOperand(v0, JSObject::kPropertiesOffset));
    __ sw(a2, FieldMemOperand(v0, HeapObject::kMapOffset));

    // Set input, index and length fields from arguments.
    __ MultiPop(static_cast<RegList>(a2.bit() | t0.bit()));
    __ sw(a1, FieldMemOperand(v0, JSArray::kLengthOffset));
    __ Addu(sp, sp, Operand(kPointerSize));
    __ sw(t0, FieldMemOperand(v0, JSRegExpResult::kIndexOffset));
    __ sw(a2, FieldMemOperand(v0, JSRegExpResult::kInputOffset));

    // Fill out the elements FixedArray.
    // v0: JSArray, tagged.
    // a3: FixedArray, tagged.
    // t1: Number of elements in array, untagged.

    // Set map.
    __ li(a2, Operand(Factory::fixed_array_map()));
    __ sw(a2, FieldMemOperand(a3, HeapObject::kMapOffset));
    // Set FixedArray length.
    __ sll(t2, t1, kSmiTagSize);
    __ sw(t2, FieldMemOperand(a3, FixedArray::kLengthOffset));
    // Fill contents of fixed-array with the-hole.
    __ li(a2, Operand(Factory::the_hole_value()));
    __ Addu(a3, a3, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
    // Fill fixed array elements with hole.
    // v0: JSArray, tagged.
    // a2: the hole.
    // a3: Start of elements in FixedArray.
    // t1: Number of elements to fill.
    Label loop;
    __ sll(t1, t1, kPointerSizeLog2);  // Concert num elements to num bytes.
    __ addu(t1, t1, a3);  // Point past last element to store.
    __ bind(&loop);
    __ Branch(&done, ge, a3, Operand(t1));  // Break when a3 past end of elem.
    __ sw(a2, MemOperand(a3));
    __ Branch(&loop, false);         // Use branch delay slot.
    __ addiu(a3, a3, kPointerSize);  // In branch delay slot.

    __ bind(&slowcase);
    __ CallRuntime(Runtime::kRegExpConstructResult, 3);
    __ bind(&done);
  }
  frame_->Forget(3);
  frame_->EmitPush(v0);
}


class DeferredSearchCache: public DeferredCode {
 public:
  DeferredSearchCache(Register dst, Register cache, Register key)
      : dst_(dst), cache_(cache), key_(key) {
    set_comment("[ DeferredSearchCache");
  }

  virtual void Generate();

 private:
  Register dst_, cache_, key_;
};


void DeferredSearchCache::Generate() {
  __ Push(cache_, key_);
  __ CallRuntime(Runtime::kGetFromCache, 2);
  if (!dst_.is(v0)) {
    __ mov(dst_, v0);
  }
}


void CodeGenerator::GenerateGetFromCache(ZoneList<Expression*>* args) {
  ASSERT_EQ(2, args->length());

  ASSERT_NE(NULL, args->at(0)->AsLiteral());
  int cache_id = Smi::cast(*(args->at(0)->AsLiteral()->handle()))->value();

  Handle<FixedArray> jsfunction_result_caches(
      Top::global_context()->jsfunction_result_caches());
  if (jsfunction_result_caches->length() <= cache_id) {
    __ Abort("Attempt to use undefined cache.");
    frame_->EmitPushRoot(Heap::kUndefinedValueRootIndex);
    return;
  }

  Load(args->at(1));

  VirtualFrame::SpilledScope spilled_scope(frame_);

  frame_->EmitPop(a2);

  __ lw(a1, ContextOperand(cp, Context::GLOBAL_INDEX));
  __ lw(a1, FieldMemOperand(a1, GlobalObject::kGlobalContextOffset));
  __ lw(a1, ContextOperand(a1, Context::JSFUNCTION_RESULT_CACHES_INDEX));
  __ lw(a1, FieldMemOperand(a1, FixedArray::OffsetOfElementAt(cache_id)));

  DeferredSearchCache* deferred = new DeferredSearchCache(v0, a1, a2);

  const int kFingerOffset =
      FixedArray::OffsetOfElementAt(JSFunctionResultCache::kFingerIndex);
  ASSERT(kSmiTag == 0 && kSmiTagSize == 1);
  __ lw(v0, FieldMemOperand(a1, kFingerOffset));
  // v0 now holds finger offset as a smi.
  __ Addu(a3, a1, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  // a3 now points to the start of fixed array elements.
  __ sll(at, v0, kPointerSizeLog2 - kSmiTagSize);  // Smi to byte index.
  __ addu(a3, a3, at);  // a3 now points to the key of the pair.
  __ lw(v0, MemOperand(a3, 0));
  deferred->Branch(ne, a2, Operand(v0));

  __ lw(v0, MemOperand(a3, kPointerSize));

  deferred->BindExit();
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateNumberToString(ZoneList<Expression*>* args) {
  ASSERT_EQ(args->length(), 1);

  // Load the argument on the stack and jump to the runtime.
  Load(args->at(0));

  NumberToStringStub stub;
  frame_->SpillAll();
  frame_->CallStub(&stub, 1);
  frame_->EmitPush(v0);
}

class DeferredSwapElements: public DeferredCode {
 public:
  DeferredSwapElements(Register object, Register index1, Register index2)
      : object_(object), index1_(index1), index2_(index2) {
    set_comment("[ DeferredSwapElements");
  }

  virtual void Generate();

 private:
  Register object_, index1_, index2_;
};


void DeferredSwapElements::Generate() {
  __ Push(object_);
  __ Push(index1_);
  __ Push(index2_);
  __ CallRuntime(Runtime::kSwapElements, 3);
}


void CodeGenerator::GenerateSwapElements(ZoneList<Expression*>* args) {
  Comment cmnt(masm_, "[ GenerateSwapElements");

  ASSERT_EQ(3, args->length());

  Load(args->at(0));
  Load(args->at(1));
  Load(args->at(2));

  VirtualFrame::SpilledScope spilled_scope(frame_);

  Register index2 = a1;
  Register index1 = a0;
  Register object = v0;
  Register tmp1 = a2;
  Register tmp2 = a3;

  frame_->EmitPop(index2);
  frame_->EmitPop(index1);
  frame_->EmitPop(object);

  DeferredSwapElements* deferred =
      new DeferredSwapElements(object, index1, index2);

  // Fetch the map and check if array is in fast case.
  // Check that object doesn't require security checks and
  // has no indexed interceptor.
  __ GetObjectType(object, tmp1, tmp2);
  deferred->Branch(lt, tmp2, Operand(FIRST_JS_OBJECT_TYPE));

  __ lbu(tmp2, FieldMemOperand(tmp1, Map::kBitFieldOffset));
  __ And(tmp2, tmp2, Operand(KeyedLoadIC::kSlowCaseBitFieldMask));
  deferred->Branch(ne, tmp2, Operand(zero_reg));

  // Check the object's elements are in fast case.
  __ lw(tmp1, FieldMemOperand(object, JSObject::kElementsOffset));
  __ lw(tmp2, FieldMemOperand(tmp1, HeapObject::kMapOffset));
  __ LoadRoot(t8, Heap::kFixedArrayMapRootIndex);
  deferred->Branch(ne, tmp2, Operand(t8));

  // Smi-tagging is equivalent to multiplying by 2.
  STATIC_ASSERT(kSmiTag == 0);
  STATIC_ASSERT(kSmiTagSize == 1);

  // Check that both indices are smis.
  __ mov(tmp2, index1);
  __ Or(tmp2, tmp2, index2);
  __ And(tmp2, tmp2, Operand(kSmiTagMask));
  deferred->Branch(ne, tmp2, Operand(zero_reg));

  // Bring the offsets into the fixed array in tmp1 into index1 and
  // index2.
  __ li(tmp2, Operand(FixedArray::kHeaderSize - kHeapObjectTag));

  __ sll(t8, index1, kPointerSizeLog2 - kSmiTagSize);
  __ Addu(index1, tmp2, t8);

  __ sll(t8, index2, kPointerSizeLog2 - kSmiTagSize);
  __ Addu(index2, tmp2, t8);

  // Swap elements.
  Register tmp3 = object;
  object = no_reg;

  __ Addu(t8, tmp1, index1);
  __ Addu(t9, tmp1, index2);

  __ lw(tmp3, MemOperand(t8, 0));
  __ lw(tmp2, MemOperand(t9, 0));
  __ sw(tmp3, MemOperand(t9, 0));
  __ sw(tmp2, MemOperand(t8, 0));

  Label done;
  __ InNewSpace(tmp1, tmp2, eq, &done);
  // Possible optimization: do a check that both values are Smis
  // (or them and test against Smi mask.)

  __ mov(tmp2, tmp1);
  RecordWriteStub recordWrite1(tmp1, index1, tmp3);
  __ CallStub(&recordWrite1);

  RecordWriteStub recordWrite2(tmp2, index2, tmp3);
  __ CallStub(&recordWrite2);

  __ bind(&done);

  deferred->BindExit();
  __ LoadRoot(tmp1, Heap::kUndefinedValueRootIndex);
  frame_->EmitPush(tmp1);
}


void CodeGenerator::GenerateCallFunction(ZoneList<Expression*>* args) {
  Comment cmnt(masm_, "[ GenerateCallFunction");

  ASSERT(args->length() >= 2);

  int n_args = args->length() - 2;  // for receiver and function.
  Load(args->at(0));  // receiver
  for (int i = 0; i < n_args; i++) {
    Load(args->at(i + 1));
  }
  Load(args->at(n_args + 1));  // function
  frame_->CallJSFunction(n_args);
  frame_->EmitPush(v0);
}


void CodeGenerator::VisitCallRuntime(CallRuntime* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  // For some reason this is needed right here.
  // The arm version only uses the spilled scope after loading the arguments.
  VirtualFrame::SpilledScope spilled_scope(frame_);

  if (CheckForInlineRuntimeCall(node)) {
    ASSERT((has_cc() && frame_->height() == original_height) ||
           (!has_cc() && frame_->height() == original_height + 1));
    return;
  }

  ZoneList<Expression*>* args = node->arguments();
  Comment cmnt(masm_, "[ CallRuntime");
  Runtime::Function* function = node->function();

  if (function == NULL) {
    // Prepare stack for calling JS runtime function.
    // Push the builtins object found in the current global object.
    Register scratch = VirtualFrame::scratch0();
    __ lw(scratch, GlobalObject());
    Register builtins = frame_->GetTOSRegister();
    __ lw(builtins, FieldMemOperand(scratch, GlobalObject::kBuiltinsOffset));
    frame_->EmitPush(builtins);
  }

  // Push the arguments ("left-to-right").
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    Load(args->at(i));
  }

  if (function == NULL) {
    // Call the JS runtime function.
    __ li(a2, Operand(node->name()));
    InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
    Handle<Code> stub = ComputeCallInitialize(arg_count, in_loop);
    frame_->CallCodeObject(stub, RelocInfo::CODE_TARGET, arg_count + 1);
    __ lw(cp, frame_->Context());
    frame_->EmitPush(v0);
  } else {
    // Call the C runtime function.
    frame_->CallRuntime(function, arg_count);
    frame_->EmitPush(v0);
  }
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitUnaryOperation(UnaryOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ UnaryOperation");

  Token::Value op = node->op();

  if (op == Token::NOT) {
    // LoadCondition reversing the false and true targets.
    LoadCondition(node->expression(), false_target(), true_target(), true);
    // LoadCondition may (and usually does) leave a test and branch to
    // be emitted by the caller.  In that case, negate the condition.
    if (has_cc()) cc_reg_ = NegateCondition(cc_reg_);

  } else if (op == Token::DELETE) {
    Property* property = node->expression()->AsProperty();
    Variable* variable = node->expression()->AsVariableProxy()->AsVariable();
    if (property != NULL) {
      Load(property->obj());
      Load(property->key());
      frame_->InvokeBuiltin(Builtins::DELETE, CALL_JS, 2);
      frame_->EmitPush(v0);

    } else if (variable != NULL) {
      Slot* slot = variable->slot();
      if (variable->is_global()) {
        LoadGlobal();
        frame_->EmitPush(Operand(variable->name()));
        frame_->InvokeBuiltin(Builtins::DELETE, CALL_JS, 2);
        frame_->EmitPush(v0);

      } else if (slot != NULL && slot->type() == Slot::LOOKUP) {
        // lookup the context holding the named variable
        frame_->EmitPush(cp);
        frame_->EmitPush(Operand(variable->name()));
        frame_->CallRuntime(Runtime::kLookupContext, 2);
        // v0: context
        frame_->EmitPush(v0);
        frame_->EmitPush(Operand(variable->name()));
        frame_->InvokeBuiltin(Builtins::DELETE, CALL_JS, 2);
        frame_->EmitPush(v0);

      } else {
        // Default: Result of deleting non-global, not dynamically
        // introduced variables is false.
        frame_->EmitPushRoot(Heap::kFalseValueRootIndex);
      }

    } else {
      // Default: Result of deleting expressions is true.
      Load(node->expression());  // may have side-effects
      frame_->Drop();
      frame_->EmitPushRoot(Heap::kTrueValueRootIndex);
    }

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
    Load(node->expression());
    switch (op) {
      case Token::NOT:
      case Token::DELETE:
      case Token::TYPEOF:
        UNREACHABLE();  // Handled above.
        break;

      case Token::SUB: {
        VirtualFrame::SpilledScope spilled(frame_);
        frame_->EmitPop(a0);
        GenericUnaryOpStub stub(Token::SUB, overwrite);
        frame_->CallStub(&stub, 0);
        frame_->EmitPush(v0);  // v0 has result
        break;
      }

      case Token::BIT_NOT: {
        VirtualFrame::SpilledScope spilled(frame_);
        frame_->EmitPop(a0);
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
        frame_->EmitPush(v0);  // v0 has result
        break;
      }

      case Token::VOID:
        frame_->Drop();
        frame_->EmitPushRoot(Heap::kUndefinedValueRootIndex);
        break;

      case Token::ADD: {
        VirtualFrame::SpilledScope spilled(frame_);
        frame_->EmitPop(a0);
        // Smi check.
        JumpTarget continue_label;
        __ mov(v0, a0);   // In case Smi test passes, move param to result.
        __ And(t0, a0, Operand(kSmiTagMask));
        continue_label.Branch(eq, t0, Operand(zero_reg));
        frame_->EmitPush(a0);
        frame_->InvokeBuiltin(Builtins::TO_NUMBER, CALL_JS, 1);
        continue_label.Bind();
        frame_->EmitPush(v0);  // v0 holds the result.
        break;
      }
      default:
        UNREACHABLE();
    }
  }
  ASSERT(!has_valid_frame() ||
         (has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


void CodeGenerator::VisitCountOperation(CountOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ CountOperation");

  // TODO(plind) This line is not needed or present in
  // codegen-arm.cc, but it is needed currently for mips. I
  // suspect a virtual-frame change we have not caught up with.
  VirtualFrame::RegisterAllocationScope scope(this);

  bool is_postfix = node->is_postfix();
  bool is_increment = node->op() == Token::INC;

  Variable* var = node->expression()->AsVariableProxy()->AsVariable();
  bool is_const = (var != NULL && var->mode() == Variable::CONST);
  bool is_slot = (var != NULL && var->mode() == Variable::VAR);

  if (!is_const && is_slot && type_info(var->slot()).IsSmi()) {
    // The type info declares that this variable is always a Smi.  That
    // means it is a Smi both before and after the increment/decrement.
    // Lets make use of that to make a very minimal count.
    Reference target(this, node->expression(), !is_const);
    ASSERT(!target.is_illegal());
    target.GetValue();  // Pushes the value.
    Register value = frame_->PopToRegister();
    if (is_postfix) frame_->EmitPush(value);
    if (is_increment) {
      __ Addu(value, value, Operand(Smi::FromInt(1)));
    } else {
      __ Subu(value, value, Operand(Smi::FromInt(1)));
    }
    frame_->EmitPush(value);
    target.SetValue(NOT_CONST_INIT);
    if (is_postfix) frame_->Pop();
    ASSERT_EQ(original_height + 1, frame_->height());
    return;
  }

  // If it's a postfix expression and its result is not ignored and the
  // reference is non-trivial, then push a placeholder on the stack now
  // to hold the result of the expression.
  bool placeholder_pushed = false;
  if (!is_slot && is_postfix) {
    frame_->EmitPush(Operand(Smi::FromInt(0)));
    placeholder_pushed = true;
  }

  // A constant reference is not saved to, so a constant reference is not a
  // compound assignment reference.
  { Reference target(this, node->expression(), !is_const);
    if (target.is_illegal()) {
      // Spoof the virtual frame to have the expected height (one higher
      // than on entry).
      if (!placeholder_pushed) {
        frame_->EmitPush(Operand(Smi::FromInt(0)));
      }
      ASSERT_EQ(original_height + 1, frame_->height());
      return;
    }
    // This pushes 0, 1 or 2 words on the object to be used later when updating
    // the target.  It also pushes the current value of the target.
    target.GetValue();

    JumpTarget slow;
    JumpTarget exit;

    Register value = frame_->PopToRegister();

    // Postfix: Store the old value as the result.
    if (placeholder_pushed) {
      frame_->SetElementAt(value, target.size());
    } else if (is_postfix) {
      frame_->EmitPush(value);
      __ mov(VirtualFrame::scratch0(), value);
      value = VirtualFrame::scratch0();
    }

    // Check for smi operand.
    __ And(t0, value, Operand(kSmiTagMask));
    slow.Branch(ne, t0, Operand(zero_reg));

    // Perform optimistic increment/decrement and check for overflow.
    // If we don't overflow we are done.
    if (is_increment) {
      __ Addu(v0, value, Operand(Smi::FromInt(1)));
      // Check for overflow of value + Smi::FromInt(1).
      __ Xor(t0, v0, value);
      __ Xor(t1, v0, Operand(Smi::FromInt(1)));
      __ and_(t0, t0, t1);    // Overflow occurred if result is negative.
      exit.Branch(ge, t0, Operand(zero_reg));  // Exit on NO overflow (ge 0).
    } else {
      __ Addu(v0, value, Operand(Smi::FromInt(-1)));
      // Check for overflow of value + Smi::FromInt(-1).
      __ Xor(t0, v0, value);
      __ Xor(t1, v0, Operand(Smi::FromInt(-1)));
      __ and_(t0, t0, t1);    // Overflow occurred if result is negative.
      exit.Branch(ge, t0, Operand(zero_reg));  // Exit on NO overflow (ge 0).
    }
    // Slow case: Convert to number.  At this point the
    // value to be incremented is in the value register.
    slow.Bind();

    // Convert the operand to a number.
    frame_->EmitPush(value);

    {
      VirtualFrame::SpilledScope spilled(frame_);
      frame_->InvokeBuiltin(Builtins::TO_NUMBER, CALL_JS, 1);
      if (is_postfix) {
        // Postfix: store to result (on the stack).
        __ sw(v0, frame_->ElementAt(target.size()));
      }

      // Compute the new value.
      frame_->EmitPush(v0);
      frame_->EmitPush(Operand(Smi::FromInt(1)));
      if (is_increment) {
        frame_->CallRuntime(Runtime::kNumberAdd, 2);
      } else {
        frame_->CallRuntime(Runtime::kNumberSub, 2);
      }
    }

    // Store the new value in the target if not const.
    exit.Bind();
    frame_->EmitPush(v0);
    // Set the target with the result, leaving the result on
    // top of the stack.  Removes the target from the stack if
    // it has a non-zero size.
    if (!is_const) target.SetValue(NOT_CONST_INIT);
  }

  // Postfix: Discard the new value and use the old.
  if (is_postfix) frame_->Pop();
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::GenerateLogicalBooleanOperation(BinaryOperation* node) {
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
  VirtualFrame::SpilledScope spilled_scope(frame_);
  if (node->op() == Token::AND) {
    JumpTarget is_true;
    LoadCondition(node->left(), &is_true, false_target(), false);
    if (has_valid_frame() && !has_cc()) {
      // The left-hand side result is on top of the virtual frame.
      JumpTarget pop_and_continue;
      JumpTarget exit;

      frame_->Dup();
      // Avoid popping the result if it converts to 'false' using the
      // standard ToBoolean() conversion as described in ECMA-262,
      // section 9.2, page 30.
      ToBoolean(&pop_and_continue, &exit);
      Branch(false, &exit);

      // Pop the result of evaluating the first part.
      pop_and_continue.Bind();
      frame_->Pop();

      // Evaluate right side expression.
      is_true.Bind();
      Load(node->right());

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
      LoadCondition(node->right(), true_target(), false_target(), false);
    } else {
      // Nothing to do.
      ASSERT(!has_valid_frame() && !has_cc() && !is_true.is_linked());
    }

  } else {
    ASSERT(node->op() == Token::OR);
    JumpTarget is_false;
    LoadCondition(node->left(), true_target(), &is_false, false);
    if (has_valid_frame() && !has_cc()) {
      // The left-hand side result is on top of the virtual frame.
      JumpTarget pop_and_continue;
      JumpTarget exit;

      frame_->Dup();
      // Avoid popping the result if it converts to 'true' using the
      // standard ToBoolean() conversion as described in ECMA-262,
      // section 9.2, page 30.
      ToBoolean(&exit, &pop_and_continue);
      Branch(true, &exit);

      // Pop the result of evaluating the first part.
      pop_and_continue.Bind();
      frame_->Pop();

      // Evaluate right side expression.
      is_false.Bind();
      Load(node->right());

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
      LoadCondition(node->right(), true_target(), false_target(), false);
    } else {
      // Nothing to do.
      ASSERT(!has_valid_frame() && !has_cc() && !is_false.is_linked());
    }
  }
}


void CodeGenerator::VisitBinaryOperation(BinaryOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ BinaryOperation");

  if (node->op() == Token::AND || node->op() == Token::OR) {
    GenerateLogicalBooleanOperation(node);
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
      VirtualFrame::RegisterAllocationScope scope(this);
      Load(node->left());
      if (frame_->KnownSmiAt(0)) overwrite_left = false;
      SmiOperation(node->op(),
                   rliteral->handle(),
                   false,
                   overwrite_left ? OVERWRITE_LEFT : NO_OVERWRITE);

    } else if (lliteral != NULL && lliteral->handle()->IsSmi()) {
      VirtualFrame::RegisterAllocationScope scope(this);
      Load(node->right());
      if (frame_->KnownSmiAt(0)) overwrite_right = false;
      SmiOperation(node->op(),
                   lliteral->handle(),
                   true,
                   overwrite_right ? OVERWRITE_RIGHT : NO_OVERWRITE);
    } else {
      GenerateInlineSmi inline_smi =
          loop_nesting() > 0 ? GENERATE_INLINE_SMI : DONT_GENERATE_INLINE_SMI;
      if (lliteral != NULL) {
        ASSERT(!lliteral->handle()->IsSmi());
        inline_smi = DONT_GENERATE_INLINE_SMI;
      }
      if (rliteral != NULL) {
        ASSERT(!rliteral->handle()->IsSmi());
        inline_smi = DONT_GENERATE_INLINE_SMI;
      }
      VirtualFrame::RegisterAllocationScope scope(this);
      OverwriteMode overwrite_mode = NO_OVERWRITE;
      if (overwrite_left) {
        overwrite_mode = OVERWRITE_LEFT;
      } else if (overwrite_right) {
        overwrite_mode = OVERWRITE_RIGHT;
      }
      Load(node->left());
      Load(node->right());
      GenericBinaryOperation(node->op(), overwrite_mode, inline_smi);
    }
  }
  ASSERT(!has_valid_frame() ||
         (has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


void CodeGenerator::VisitThisFunction(ThisFunction* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  frame_->EmitPush(MemOperand(frame_->Function()));
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitCompareOperation(CompareOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ CompareOperation");

  VirtualFrame::RegisterAllocationScope nonspilled_scope(this);

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
      Load(left_is_null ? right : left);
      Register tos = frame_->PopToRegister();
      __ mov(condReg1, tos);
      // JumpTargets can't cope with register allocation yet.
      frame_->SpillAll();
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
    Register tos = frame_->PopToRegister();
    __ mov(condReg1, tos);
    // JumpTargets can't cope with register allocation yet.
    frame_->SpillAll();

    Register scratch = VirtualFrame::scratch0();
    Register scratch2 = VirtualFrame::scratch1();

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
      __ And(scratch, condReg1, Operand(kSmiTagMask));
      false_target()->Branch(eq, scratch, Operand(zero_reg));

      Register map_reg = scratch;
      __ GetObjectType(condReg1, map_reg, condReg1);
      true_target()->Branch(eq, condReg1, Operand(JS_FUNCTION_TYPE));
      // Regular expressions are callable so typeof == 'function'.
      __ lbu(condReg1, FieldMemOperand(map_reg, Map::kInstanceTypeOffset));
      __ li(condReg2, Operand(JS_REGEXP_TYPE));
      cc_reg_ = eq;

    } else if (check->Equals(Heap::object_symbol())) {
      __ And(scratch, condReg1, Operand(kSmiTagMask));
      false_target()->Branch(eq, scratch, Operand(zero_reg));

      __ LoadRoot(scratch2, Heap::kNullValueRootIndex);
      true_target()->Branch(eq, condReg1, Operand(scratch2));

      Register map_reg = scratch;
      __ GetObjectType(condReg1, map_reg, condReg1);
      false_target()->Branch(eq, condReg1, Operand(JS_REGEXP_TYPE));

      // It can be an undetectable object.
      __ lbu(condReg1, FieldMemOperand(map_reg, Map::kBitFieldOffset));
      __ And(condReg1, condReg1, Operand(1 << Map::kIsUndetectable));
      false_target()->Branch(eq, condReg1, Operand(1 << Map::kIsUndetectable));

      __ lbu(condReg1, FieldMemOperand(map_reg, Map::kInstanceTypeOffset));
      false_target()->Branch(lt, condReg1, Operand(FIRST_JS_OBJECT_TYPE));
      __ li(condReg2, Operand(LAST_JS_OBJECT_TYPE));
      cc_reg_ = le;

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
      VirtualFrame::SpilledScope scope(frame_);
      Load(left);
      Load(right);
      frame_->InvokeBuiltin(Builtins::IN, CALL_JS, 2);
      frame_->EmitPush(v0);
      break;
    }

    case Token::INSTANCEOF: {
      VirtualFrame::SpilledScope scope(frame_);
      Load(left);
      Load(right);
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


class DeferredReferenceGetNamedValue: public DeferredCode {
  public:
    explicit DeferredReferenceGetNamedValue(Register receiver,
                                            Handle<String> name)
        : receiver_(receiver), name_(name) {
      set_comment("[ DeferredReferenceGetNamedValue");
    }

    virtual void Generate();

  private:
    Register receiver_;
    Handle<String> name_;
};

// Convention for this is that on entry the receiver is in a register that
// is not used by the stack.  On exit the answer is found in that same
// register and the stack has the same height.
void DeferredReferenceGetNamedValue::Generate() {
#ifdef DEBUG
  int expected_height = frame_state()->frame()->height();
#endif
  VirtualFrame copied_frame(*frame_state()->frame());
  copied_frame.SpillAll();

  Register scratch1 = VirtualFrame::scratch0();
  Register scratch2 = VirtualFrame::scratch1();
  ASSERT(!receiver_.is(scratch1) && !receiver_.is(scratch2));
  __ DecrementCounter(&Counters::named_load_inline, 1, scratch1, scratch2);
  __ IncrementCounter(&Counters::named_load_inline_miss, 1, scratch1, scratch2);

  // Ensure receiver in a0 and name in a2 to match load ic calling convention.
  __ Move(a0, receiver_);
  __ li(a2, Operand(name_));

  { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
    Handle<Code> ic(Builtins::builtin(Builtins::LoadIC_Initialize));
    __ Call(ic, RelocInfo::CODE_TARGET);

    // The call must be followed by a nop(1) instruction to indicate that the
    // in-object has been inlined.
    __ nop(PROPERTY_ACCESS_INLINED);

    // At this point the answer is in v0.  We move it to the expected register
    // if necessary.
    __ Move(receiver_, v0);

    // Now go back to the frame that we entered with.  This will not overwrite
    // the receiver register since that register was not in use when we came
    // in.  The instructions emitted by this merge are skipped over by the
    // inline load patching mechanism when looking for the branch instruction
    // that tells it where the code to patch is.
    copied_frame.MergeTo(frame_state()->frame());


    // Block the trampoline pool for one more instruction to
    // include the branch instruction ending the deferred code.
    __ BlockTrampolinePoolFor(1);
  }
  ASSERT_EQ(expected_height, frame_state()->frame()->height());
}


class DeferredReferenceGetKeyedValue: public DeferredCode {
 public:
  DeferredReferenceGetKeyedValue(Register key, Register receiver)
      : key_(key), receiver_(receiver) {
    set_comment("[ DeferredReferenceGetKeyedValue");
  }

  virtual void Generate();

 private:
  Register key_;
  Register receiver_;
};


void DeferredReferenceGetKeyedValue::Generate() {
  ASSERT((key_.is(a0) && receiver_.is(a1)) ||
         (key_.is(a1) && receiver_.is(a0)));

  Register scratch1 = VirtualFrame::scratch0();
  Register scratch2 = VirtualFrame::scratch1();
  __ DecrementCounter(&Counters::keyed_load_inline, 1, scratch1, scratch2);
  __ IncrementCounter(&Counters::keyed_load_inline_miss, 1, scratch1, scratch2);

  // Ensure key in a0 and receiver in a1 to match keyed load ic calling
  // convention.
  if (key_.is(a1)) {
    __ Swap(a0, a1, at);
  }

  // The rest of the instructions in the deferred code must be together.
  { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
    // Call keyed load IC. It has the arguments key and receiver in a0 and a1.
    Handle<Code> ic(Builtins::builtin(Builtins::KeyedLoadIC_Initialize));
    __ Call(ic, RelocInfo::CODE_TARGET);
    // The call must be followed by a nop instruction to indicate that the
    // keyed load has been inlined.
    __ nop(PROPERTY_ACCESS_INLINED);

    // Block the trampoline pool for one more instruction after leaving this
    // constant pool block scope to include the branch instruction ending the
    // deferred code.
    __ BlockTrampolinePoolFor(1);
  }
}


class DeferredReferenceSetKeyedValue: public DeferredCode {
 public:
  DeferredReferenceSetKeyedValue(Register value,
                                 Register key,
                                 Register receiver)
      : value_(value), key_(key), receiver_(receiver) {
    set_comment("[ DeferredReferenceSetKeyedValue");
  }

  virtual void Generate();

 private:
  Register value_;
  Register key_;
  Register receiver_;
};


void DeferredReferenceSetKeyedValue::Generate() {
  Register scratch1 = VirtualFrame::scratch0();
  Register scratch2 = VirtualFrame::scratch1();
  __ DecrementCounter(&Counters::keyed_store_inline, 1, scratch1, scratch2);
  __ IncrementCounter(
      &Counters::keyed_store_inline_miss, 1, scratch1, scratch2);

  // Ensure value in a0, key in a1 and receiver in a2 to match keyed store ic
  // calling convention.
  if (value_.is(a1)) {
    __ Swap(a0, a1, t8);
  }
  ASSERT(receiver_.is(a2));

  // The rest of the instructions in the deferred code must be together.
  { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
    // Call keyed store IC. It has the arguments value, key and receiver in a0,
    // a1 and a2.
    Handle<Code> ic(Builtins::builtin(Builtins::KeyedStoreIC_Initialize));
    __ Call(ic, RelocInfo::CODE_TARGET);
    // The call must be followed by a nop instruction to indicate that the
    // keyed store has been inlined.
    __ nop(PROPERTY_ACCESS_INLINED);

    // Block the trampoline pool for one more instruction after leaving this
    // constant pool block scope to include the branch instruction ending the
    // deferred code.
    __ BlockTrampolinePoolFor(1);
  }
}


// Consumes the top of stack (the receiver) and pushes the result instead.
void CodeGenerator::EmitNamedLoad(Handle<String> name, bool is_contextual) {
  if (is_contextual || scope()->is_global_scope() || loop_nesting() == 0) {
    Comment cmnt(masm(), "[ Load from named Property");
    // Setup the name register and call load IC.
    frame_->CallLoadIC(name,
                       is_contextual
                           ? RelocInfo::CODE_TARGET_CONTEXT
                           : RelocInfo::CODE_TARGET);
    frame_->EmitPush(v0);  // Push answer.
  } else {
    // Inline the inobject property case.
    Comment cmnt(masm(), "[ Inlined named property load");

    // Counter will be decremented in the deferred code. Placed here to avoid
    // having it in the instruction stream below where patching will occur.
    __ IncrementCounter(&Counters::named_load_inline, 1,
                        frame_->scratch0(), frame_->scratch1());

    // The following instructions are the inlined load of an in-object property.
    // Parts of this code is patched, so the exact instructions generated needs
    // to be fixed. Therefore the instruction pool is blocked when generating
    // this code

    // Load the receiver from the stack.
    Register receiver = frame_->PopToRegister();
    VirtualFrame::SpilledScope spilled(frame_);

    DeferredReferenceGetNamedValue* deferred =
        new DeferredReferenceGetNamedValue(receiver, name);

#ifdef DEBUG
    // 9 instructions. and:1, branch:2, lw:1, li:2, Branch:2, lw:1.
    const int kInlinedNamedLoadInstructions = 9;
    Label check_inlined_codesize;
    masm_->bind(&check_inlined_codesize);
#endif

    // Generate patchable inline code. See LoadIC::PatchInlinedLoad.
    { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
      // Check that the receiver is a heap object.
      __ And(at, receiver, Operand(kSmiTagMask));
      deferred->Branch(eq, at, Operand(zero_reg));

      Register scratch = VirtualFrame::scratch0();
      Register scratch2 = VirtualFrame::scratch1();

      // Check the map. The null map used below is patched by the inline cache
      // code.  Therefore we can't use a LoadRoot call.

      __ lw(scratch, FieldMemOperand(receiver, HeapObject::kMapOffset));

      // The null map used below is patched by the inline cache code.
      __ li(scratch2, Operand(Factory::null_value()), true);
      deferred->Branch(ne, scratch, Operand(scratch2));

      // Initially use an invalid index. The index will be patched by the
      // inline cache code.
      __ lw(receiver, MemOperand(receiver, 0));

      // Make sure that the expected number of instructions are generated.
      // If this fails, LoadIC::PatchInlinedLoad() must be fixed as well.
      ASSERT_EQ(kInlinedNamedLoadInstructions,
                masm_->InstructionsGeneratedSince(&check_inlined_codesize));
    }
    deferred->BindExit();
    // At this point the receiver register has the result, either from the
    // deferred code or from the inlined code.
    frame_->EmitPush(receiver);
  }
}


void CodeGenerator::EmitNamedStore(Handle<String> name, bool is_contextual) {
#ifdef DEBUG
  int expected_height = frame_->height() - (is_contextual ? 1 : 2);
#endif
  frame_->CallStoreIC(name, is_contextual);

  ASSERT_EQ(expected_height, frame_->height());
}


void CodeGenerator::EmitKeyedLoad() {
  if (loop_nesting() == 0) {
    Comment cmnt(masm_, "[ Load from keyed property");
    frame_->CallKeyedLoadIC();
  } else {
    // Inline the keyed load.
    Comment cmnt(masm_, "[ Inlined load from keyed property");

    // Counter will be decremented in the deferred code. Placed here to avoid
    // having it in the instruction stream below where patching will occur.
    __ IncrementCounter(&Counters::keyed_load_inline, 1,
                        frame_->scratch0(), frame_->scratch1());

    // Load the key and receiver from the stack.
    bool key_is_known_smi = frame_->KnownSmiAt(0);
    Register key = frame_->PopToRegister();
    Register receiver = frame_->PopToRegister(key);
    VirtualFrame::SpilledScope spilled(frame_);

    // The deferred code expects key and receiver in registers.
    DeferredReferenceGetKeyedValue* deferred =
        new DeferredReferenceGetKeyedValue(key, receiver);

    // Check that the receiver is a heap object.
    __ And(at, receiver, Operand(kSmiTagMask));
    deferred->Branch(eq, at, Operand(zero_reg));

    // The following instructions are the part of the inlined load keyed
    // property code which can be patched. Therefore the exact number of
    // instructions generated need to be fixed, so the trampoline pool is
    // blocked while generating this code.
    { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
      Register scratch1 = VirtualFrame::scratch0();
      Register scratch2 = VirtualFrame::scratch1();
      // Check the map. The null map used below is patched by the inline cache
      // code.
      __ lw(scratch1, FieldMemOperand(receiver, HeapObject::kMapOffset));
       // Check that the key is a smi.
      if (!key_is_known_smi) {
        __ And(scratch2, key, Operand(kSmiTagMask));
        deferred->Branch(ne, scratch2, Operand(zero_reg));
      }
#ifdef DEBUG
      Label check_inlined_codesize;
      masm_->bind(&check_inlined_codesize);
#endif
      __ li(scratch2, Operand(Factory::null_value()), true);
      deferred->Branch(ne, scratch1, Operand(scratch2));

      // Check that the key is a smi.
      __ And(at, key, Operand(kSmiTagMask));
      deferred->Branch(ne, at, Operand(zero_reg));

      // Get the elements array from the receiver and check that it
      // is not a dictionary.
      __ lw(scratch1, FieldMemOperand(receiver, JSObject::kElementsOffset));
      __ lw(scratch2, FieldMemOperand(scratch1, JSObject::kMapOffset));
      __ LoadRoot(at, Heap::kFixedArrayMapRootIndex);
      deferred->Branch(ne, scratch2, Operand(at));

      // Check that key is within bounds. Use unsigned comparison to handle
      // negative keys.
      __ lw(scratch2, FieldMemOperand(scratch1, FixedArray::kLengthOffset));
      deferred->Branch(ls, scratch2, Operand(key));  // Unsigned less equal.

      // Load and check that the result is not the hole (key is a smi).
      __ LoadRoot(scratch2, Heap::kTheHoleValueRootIndex);
      __ Addu(scratch1,
              scratch1,
              Operand(FixedArray::kHeaderSize - kHeapObjectTag));
      __ sll(at, key, kPointerSizeLog2 - (kSmiTagSize + kSmiShiftSize));
      __ addu(at, at, scratch1);
      __ lw(scratch1, MemOperand(at, 0));

      deferred->Branch(eq, scratch1, Operand(scratch2));

      __ mov(v0, scratch1);
      ASSERT_EQ(kInlinedKeyedLoadInstructionsAfterPatch,
                masm_->InstructionsGeneratedSince(&check_inlined_codesize));
    }

    deferred->BindExit();
  }
}


void CodeGenerator::EmitKeyedStore(StaticType* key_type) {
  // Generate inlined version of the keyed store if the code is in a loop
  // and the key is likely to be a smi.
  if (loop_nesting() > 0 && key_type->IsLikelySmi()) {
    // Inline the keyed store.
    Comment cmnt(masm_, "[ Inlined store to keyed property");

    Register scratch1 = VirtualFrame::scratch0();
    Register scratch2 = VirtualFrame::scratch1();
    Register scratch3 = a3;

    // Counter will be decremented in the deferred code. Placed here to avoid
    // having it in the instruction stream below where patching will occur.
    __ IncrementCounter(&Counters::keyed_store_inline, 1,
                        scratch1, scratch2);

    // Load the value, key and receiver from the stack.
    Register value = frame_->PopToRegister();
    Register key = frame_->PopToRegister(value);
    Register receiver = a2;
    frame_->EmitPop(receiver);
    VirtualFrame::SpilledScope spilled(frame_);

    // The deferred code expects value, key and receiver in registers.
    DeferredReferenceSetKeyedValue* deferred =
        new DeferredReferenceSetKeyedValue(value, key, receiver);

    // Check that the value is a smi. As this inlined code does not set the
    // write barrier it is only possible to store smi values.
    __ And(at, value, Operand(kSmiTagMask));
    deferred->Branch(ne, at, Operand(zero_reg));

    // Check that the key is a smi.
    __ And(at, key, Operand(kSmiTagMask));
    deferred->Branch(ne, at, Operand(zero_reg));

    // Check that the receiver is a heap object.
    __ And(at, receiver, Operand(kSmiTagMask));
    deferred->Branch(eq, at, Operand(zero_reg));

    // Check that the receiver is a JSArray.
    __ GetObjectType(receiver, scratch1, scratch1);
    deferred->Branch(ne, scratch1, Operand(JS_ARRAY_TYPE));

    // Check that the key is within bounds. Both the key and the length of
    // the JSArray are smis. Use unsigned comparison to handle negative keys.
    __ lw(scratch1, FieldMemOperand(receiver, JSArray::kLengthOffset));
    deferred->Branch(ls, scratch1, Operand(key));  // Unsigned less equal.

    // The following instructions are the part of the inlined store keyed
    // property code which can be patched. Therefore the exact number of
    // instructions generated need to be fixed, so the constant pool is blocked
    // while generating this code.

    { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
      // Get the elements array from the receiver and check that it
      // is not a dictionary.
      __ lw(scratch1, FieldMemOperand(receiver, JSObject::kElementsOffset));
      __ lw(scratch2, FieldMemOperand(scratch1, JSObject::kMapOffset));
      // Read the fixed array map from the constant pool (not from the root
      // array) so that the value can be patched.  When debugging, we patch this
      // comparison to always fail so that we will hit the IC call in the
      // deferred code which will allow the debugger to break for fast case
      // stores.

#ifdef DEBUG
    Label check_inlined_codesize;
    masm_->bind(&check_inlined_codesize);
#endif

      __ li(scratch3, Operand(Factory::fixed_array_map()), true);
      deferred->Branch(ne, scratch2, Operand(scratch3));

      // Store the value.
      __ Addu(scratch1, scratch1,
              Operand(FixedArray::kHeaderSize - kHeapObjectTag));

      // Use (Smi) key in a1 to index array pointed to by a3.
      __ sll(at, key, kPointerSizeLog2 - (kSmiTagSize + kSmiShiftSize));
      __ addu(at, scratch1, at);
      __ sw(value, MemOperand(at, 0));
      __ mov(v0, value);  // Leave stored value in v0.

      // Make sure that the expected number of instructions are generated.
      // If fail, KeyedStoreIC::PatchInlinedStore() must be fixed as well.
      ASSERT_EQ(kInlinedKeyedStoreInstructionsAfterPatch,
                masm_->InstructionsGeneratedSince(&check_inlined_codesize));
    }
    deferred->BindExit();
  } else {
    frame()->CallKeyedStoreIC();
  }
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

void Reference::DupIfPersist() {
  if (persist_after_get_) {
    switch (type_) {
      case KEYED:
        cgen_->frame()->Dup2();
        break;
      case NAMED:
        cgen_->frame()->Dup();
        // Fall through.
      case UNLOADED:
      case ILLEGAL:
      case SLOT:
        // Do nothing.
        ;
    }
  } else {
    set_unloaded();
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
      Comment cmnt(masm, "[ Load from Slot");
      Slot* slot = expression_->AsVariableProxy()->AsVariable()->slot();
      ASSERT(slot != NULL);
      DupIfPersist();
      cgen_->LoadFromSlotCheckForArguments(slot, NOT_INSIDE_TYPEOF);
      break;
    }

    case NAMED: {
      Variable* var = expression_->AsVariableProxy()->AsVariable();
      bool is_global = var != NULL;
      ASSERT(!is_global || var->is_global());
      Handle<String> name = GetName();
      DupIfPersist();
      cgen_->EmitNamedLoad(name, is_global);
      break;
    }

    case KEYED: {
      ASSERT(property != NULL);
      DupIfPersist();
      cgen_->EmitKeyedLoad();
      cgen_->frame()->EmitPush(v0);
      break;
    }

    default:
      UNREACHABLE();
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
      set_unloaded();
      break;
    }

    case NAMED: {
      VirtualFrame::SpilledScope scope(frame);
      Comment cmnt(masm, "[ Store to named Property");
      cgen_->EmitNamedStore(GetName(), false);
      frame->EmitPush(v0);
      set_unloaded();
      break;
    }

    case KEYED: {
      Comment cmnt(masm, "[ Store to keyed Property");
      Property* property = expression_->AsProperty();
      ASSERT(property != NULL);
      cgen_->CodeForSourcePosition(property->position());

      cgen_->EmitKeyedStore(property->key()->type());
      frame->EmitPush(v0);
      set_unloaded();
      break;
    }

    default:
      UNREACHABLE();
  }
}


// Takes a Smi and converts to an IEEE 64 bit floating point value in two
// registers.  The format is 1 sign bit, 11 exponent bits (biased 1023) and
// 52 fraction bits (20 in the first word, 32 in the second).  Zeros is a
// scratch register.  Destroys the source register.  No GC occurs during this
// stub so you don't have to set up the frame.
class ConvertToDoubleStub : public CodeStub {
 public:
  ConvertToDoubleStub(Register result_reg_1,
                      Register result_reg_2,
                      Register source_reg,
                      Register scratch_reg)
      : result1_(result_reg_1),
        result2_(result_reg_2),
        source_(source_reg),
        zeros_(scratch_reg) { }

 private:
  Register result1_;
  Register result2_;
  Register source_;
  Register zeros_;

  // Minor key encoding in 16 bits.
  class ModeBits: public BitField<OverwriteMode, 0, 2> {};
  class OpBits: public BitField<Token::Value, 2, 14> {};

  Major MajorKey() { return ConvertToDouble; }
  int MinorKey() {
    // Encode the parameters in a unique 16 bit value.
    return  result1_.code() +
           (result2_.code() << 4) +
           (source_.code() << 8) +
           (zeros_.code() << 12);
  }

  void Generate(MacroAssembler* masm);

  const char* GetName() { return "ConvertToDoubleStub"; }

#ifdef DEBUG
  void Print() { PrintF("ConvertToDoubleStub\n"); }
#endif
};


void ConvertToDoubleStub::Generate(MacroAssembler* masm) {
#ifndef BIG_ENDIAN_FLOATING_POINT
  Register exponent = result1_;
  Register mantissa = result2_;
#else
  Register exponent = result2_;
  Register mantissa = result1_;
#endif
  Label not_special;
  // Convert from Smi to integer.
  __ sra(source_, source_, kSmiTagSize);
  // Move sign bit from source to destination.  This works because the sign bit
  // in the exponent word of the double has the same position and polarity as
  // the 2's complement sign bit in a Smi.
  ASSERT(HeapNumber::kSignMask == 0x80000000u);
  __ And(exponent, source_, Operand(HeapNumber::kSignMask));
  // Subtract from 0 if source was negative.
  __ subu(at, zero_reg, source_);
  __ movn(source_, at, exponent);

  // We have -1, 0 or 1, which we treat specially. Register source_ contains
  // absolute value: it is either equal to 1 (special case of -1 and 1),
  // greater than 1 (not a special case) or less than 1 (special case of 0).
  __ Branch(&not_special, gt, source_, Operand(1));

  // For 1 or -1 we need to or in the 0 exponent (biased to 1023).
  static const uint32_t exponent_word_for_1 =
      HeapNumber::kExponentBias << HeapNumber::kExponentShift;
  // Safe to use 'at' as dest reg here.
  __ Or(at, exponent, Operand(exponent_word_for_1));
  __ movn(exponent, at, source_);  // Write exp when source not 0.
  // 1, 0 and -1 all have 0 for the second word.
  __ mov(mantissa, zero_reg);
  __ Ret();

  __ bind(&not_special);
  // Count leading zeros.
  // Gets the wrong answer for 0, but we already checked for that case above.
  __ clz(zeros_, source_);
  // Compute exponent and or it into the exponent register.
  // We use mantissa as a scratch register here.
  __ li(mantissa, Operand(31 + HeapNumber::kExponentBias));
  __ subu(mantissa, mantissa, zeros_);
  __ sll(mantissa, mantissa, HeapNumber::kExponentShift);
  __ Or(exponent, exponent, mantissa);

  // Shift up the source chopping the top bit off.
  __ Addu(zeros_, zeros_, Operand(1));
  // This wouldn't work for 1.0 or -1.0 as the shift would be 32 which means 0.
  __ sllv(source_, source_, zeros_);
  // Compute lower part of fraction (last 12 bits).
  __ sll(mantissa, source_, HeapNumber::kMantissaBitsInTopWord);
  // And the top (top 20 bits).
  __ srl(source_, source_, 32 - HeapNumber::kMantissaBitsInTopWord);
  __ or_(exponent, exponent, source_);

  __ Ret();
}


// See comment for class, this does NOT work for int32's that are in Smi range.
void WriteInt32ToHeapNumberStub::Generate(MacroAssembler* masm) {
  Label max_negative_int;
  // the_int_ has the answer which is a signed int32 but not a Smi.
  // We test for the special value that has a different exponent.
  ASSERT(HeapNumber::kSignMask == 0x80000000u);
  // Test sign, and save for later conditionals.
  __ And(sign_, the_int_, Operand(0x80000000u));
  __ Branch(&max_negative_int, eq, the_int_, Operand(0x80000000u));

  // Set up the correct exponent in scratch_.  All non-Smi int32s have the same.
  // A non-Smi integer is 1.xxx * 2^30 so the exponent is 30 (biased).
  uint32_t non_smi_exponent =
      (HeapNumber::kExponentBias + 30) << HeapNumber::kExponentShift;
  __ li(scratch_, Operand(non_smi_exponent));
  // Set the sign bit in scratch_ if the value was negative.
  __ or_(scratch_, scratch_, sign_);
  // Subtract from 0 if the value was negative.
  __ subu(at, zero_reg, the_int_);
  __ movn(the_int_, at, sign_);
  // We should be masking the implict first digit of the mantissa away here,
  // but it just ends up combining harmlessly with the last digit of the
  // exponent that happens to be 1.  The sign bit is 0 so we shift 10 to get
  // the most significant 1 to hit the last bit of the 12 bit sign and exponent.
  ASSERT(((1 << HeapNumber::kExponentShift) & non_smi_exponent) != 0);
  const int shift_distance = HeapNumber::kNonMantissaBitsInTopWord - 2;
  __ srl(at, the_int_, shift_distance);
  __ or_(scratch_, scratch_, at);
  __ sw(scratch_, FieldMemOperand(the_heap_number_,
                                   HeapNumber::kExponentOffset));
  __ sll(scratch_, the_int_, 32 - shift_distance);
  __ sw(scratch_, FieldMemOperand(the_heap_number_,
                                   HeapNumber::kMantissaOffset));
  __ Ret();

  __ bind(&max_negative_int);
  // The max negative int32 is stored as a positive number in the mantissa of
  // a double because it uses a sign bit instead of using two's complement.
  // The actual mantissa bits stored are all 0 because the implicit most
  // significant 1 bit is not stored.
  non_smi_exponent += 1 << HeapNumber::kExponentShift;
  __ li(scratch_, Operand(HeapNumber::kSignMask | non_smi_exponent));
  __ sw(scratch_,
        FieldMemOperand(the_heap_number_, HeapNumber::kExponentOffset));
  __ li(scratch_, Operand(0));
  __ sw(scratch_,
        FieldMemOperand(the_heap_number_, HeapNumber::kMantissaOffset));
  __ Ret();
}


// Handle the case where the lhs and rhs are the same object.
// Equality is almost reflexive (everything but NaN), so this is a test
// for "identity and not NaN".
static void EmitIdenticalObjectComparison(MacroAssembler* masm,
                                          Label* slow,
                                          Condition cc,
                                          bool never_nan_nan) {
  Label not_identical;
  Label heap_number, return_equal;
  Register exp_mask_reg = t5;

  __ Branch(&not_identical, ne, a0, Operand(a1));

  // The two objects are identical. If we know that one of them isn't NaN then
  // we now know they test equal.
  if (cc != eq || !never_nan_nan) {
    __ li(exp_mask_reg, Operand(HeapNumber::kExponentMask));

    // Test for NaN. Sadly, we can't just compare to Factory::nan_value(),
    // so we do the second best thing - test it ourselves.
    // They are both equal and they are not both Smis so both of them are not
    // Smis. If it's not a heap number, then return equal.
    if (cc == less || cc == greater) {
      __ GetObjectType(a0, t4, t4);
      __ Branch(slow, greater, t4, Operand(FIRST_JS_OBJECT_TYPE));
    } else {
      __ GetObjectType(a0, t4, t4);
      __ Branch(&heap_number, eq, t4, Operand(HEAP_NUMBER_TYPE));
      // Comparing JS objects with <=, >= is complicated.
      if (cc != eq) {
      __ Branch(slow, greater, t4, Operand(FIRST_JS_OBJECT_TYPE));
        // Normally here we fall through to return_equal, but undefined is
        // special: (undefined == undefined) == true, but
        // (undefined <= undefined) == false!  See ECMAScript 11.8.5.
        if (cc == less_equal || cc == greater_equal) {
          __ Branch(&return_equal, ne, t4, Operand(ODDBALL_TYPE));
          __ LoadRoot(t2, Heap::kUndefinedValueRootIndex);
          __ Branch(&return_equal, ne, a0, Operand(t2));
          if (cc == le) {
            // undefined <= undefined should fail.
            __ li(v0, Operand(GREATER));
          } else  {
            // undefined >= undefined should fail.
            __ li(v0, Operand(LESS));
          }
          __ Ret();
        }
      }
    }
  }

  __ bind(&return_equal);
  if (cc == less) {
    __ li(v0, Operand(GREATER));  // Things aren't less than themselves.
  } else if (cc == greater) {
    __ li(v0, Operand(LESS));     // Things aren't greater than themselves.
  } else {
    __ li(v0, Operand(0));        // Things are <=, >=, ==, === themselves.
  }
  __ Ret();

  if (cc != eq || !never_nan_nan) {
    // For less and greater we don't have to check for NaN since the result of
    // x < x is false regardless.  For the others here is some code to check
    // for NaN.
    if (cc != lt && cc != gt) {
      __ bind(&heap_number);
      // It is a heap number, so return non-equal if it's NaN and equal if it's
      // not NaN.

      // The representation of NaN values has all exponent bits (52..62) set,
      // and not all mantissa bits (0..51) clear.
      // Read top bits of double representation (second word of value).
      __ lw(t2, FieldMemOperand(a0, HeapNumber::kExponentOffset));
      // Test that exponent bits are all set.
      __ And(t3, t2, Operand(exp_mask_reg));
      // If all bits not set (ne cond), then not a NaN, objects are equal.
      __ Branch(&return_equal, ne, t3, Operand(exp_mask_reg));

      // Shift out flag and all exponent bits, retaining only mantissa.
      __ sll(t2, t2, HeapNumber::kNonMantissaBitsInTopWord);
      // Or with all low-bits of mantissa.
      __ lw(t3, FieldMemOperand(a0, HeapNumber::kMantissaOffset));
      __ Or(v0, t3, Operand(t2));
      // For equal we already have the right value in v0:  Return zero (equal)
      // if all bits in mantissa are zero (it's an Infinity) and non-zero if
      // not (it's a NaN).  For <= and >= we need to load v0 with the failing
      // value if it's a NaN.
      if (cc != eq) {
        // All-zero means Infinity means equal.
        __ Ret(eq, v0, Operand(zero_reg));
        if (cc == le) {
          __ li(v0, Operand(GREATER));  // NaN <= NaN should fail.
        } else {
          __ li(v0, Operand(LESS));     // NaN >= NaN should fail.
        }
      }
      __ Ret();
    }
    // No fall through here.
  }

  __ bind(&not_identical);
}


void FastNewClosureStub::Generate(MacroAssembler* masm) {
  // Create a new closure from the given function info in new
  // space. Set the context to the current context in cp.
  Label gc;

  // Pop the function info from the stack.
  __ Pop(a3);

  // Attempt to allocate new JSFunction in new space.
  __ AllocateInNewSpace(JSFunction::kSize,
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
  __ Push(cp, a3);
  __ TailCallRuntime(Runtime::kNewClosure, 2, 1);
}


void FastNewContextStub::Generate(MacroAssembler* masm) {
  // Try to allocate the context in new space.
  Label gc;
  int length = slots_ + Context::MIN_CONTEXT_SLOTS;

  // Attempt to allocate the context in new space.
  __ AllocateInNewSpace(FixedArray::SizeFor(length),
                        v0,
                        a1,
                        a2,
                        &gc,
                        TAG_OBJECT);

  // Load the function from the stack.
  __ lw(a3, MemOperand(sp, 0));

  // Setup the object header.
  __ LoadRoot(a2, Heap::kContextMapRootIndex);
  __ sw(a2, FieldMemOperand(v0, HeapObject::kMapOffset));
  __ li(a2, Operand(Smi::FromInt(length)));
  __ sw(a2, FieldMemOperand(v0, FixedArray::kLengthOffset));

  // Setup the fixed slots.
  __ li(a1, Operand(Smi::FromInt(0)));
  __ sw(a3, MemOperand(v0, Context::SlotOffset(Context::CLOSURE_INDEX)));
  __ sw(v0, MemOperand(v0, Context::SlotOffset(Context::FCONTEXT_INDEX)));
  __ sw(a1, MemOperand(v0, Context::SlotOffset(Context::PREVIOUS_INDEX)));
  __ sw(a1, MemOperand(v0, Context::SlotOffset(Context::EXTENSION_INDEX)));

  // Copy the global object from the surrounding context.
  __ lw(a1, MemOperand(cp, Context::SlotOffset(Context::GLOBAL_INDEX)));
  __ sw(a1, MemOperand(v0, Context::SlotOffset(Context::GLOBAL_INDEX)));

  // Initialize the rest of the slots to undefined.
  __ LoadRoot(a1, Heap::kUndefinedValueRootIndex);
  for (int i = Context::MIN_CONTEXT_SLOTS; i < length; i++) {
    __ sw(a1, MemOperand(v0, Context::SlotOffset(i)));
  }

  // Remove the on-stack argument and return.
  __ mov(cp, v0);
  __ Pop();
  __ Ret();

  // Need to collect. Call into runtime system.
  __ bind(&gc);
  __ TailCallRuntime(Runtime::kNewContext, 1, 1);
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
  __ Addu(a3, a3, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  __ sll(t0, a0, kPointerSizeLog2 - kSmiTagSize);
  __ Addu(t0, a3, t0);
  __ lw(a3, MemOperand(t0));
  __ LoadRoot(t1, Heap::kUndefinedValueRootIndex);
  __ Branch(&slow_case, eq, a3, Operand(t1));

  // Allocate both the JS array and the elements array in one big
  // allocation. This avoids multiple limit checks.
  // Return new object in v0.
  __ AllocateInNewSpace(size,
                        v0,
                        a1,
                        a2,
                        &slow_case,
                        TAG_OBJECT);

  // Copy the JS array part.
  for (int i = 0; i < JSArray::kSize; i += kPointerSize) {
    if ((i != JSArray::kElementsOffset) || (length_ == 0)) {
      __ lw(a1, FieldMemOperand(a3, i));
      __ sw(a1, FieldMemOperand(v0, i));
    }
  }

  if (length_ > 0) {
    // Get hold of the elements array of the boilerplate and setup the
    // elements pointer in the resulting object.
    __ lw(a3, FieldMemOperand(a3, JSArray::kElementsOffset));
    __ Addu(a2, v0, Operand(JSArray::kSize));
    __ sw(a2, FieldMemOperand(v0, JSArray::kElementsOffset));

    // Copy the elements array.
    for (int i = 0; i < elements_size; i += kPointerSize) {
      __ lw(a1, FieldMemOperand(a3, i));
      __ sw(a1, FieldMemOperand(a2, i));
    }
  }

  // Return and remove the on-stack parameters.
  __ Addu(sp, sp, Operand(3 * kPointerSize));
  __ Ret();

  __ bind(&slow_case);
  __ TailCallRuntime(Runtime::kCreateArrayLiteralShallow, 3, 1);
}

static void EmitSmiNonsmiComparison(MacroAssembler* masm,
                                    Label* both_loaded_as_doubles,
                                    Label* slow,
                                    bool strict) {
  Label lhs_is_smi;
  __ And(t0, a0, Operand(kSmiTagMask));
  __ Branch(&lhs_is_smi, eq, t0, Operand(zero_reg));
  // Rhs is a Smi.
  // Check whether the non-smi is a heap number.
  __ GetObjectType(a0, t4, t4);
  if (strict) {
    // If lhs was not a number and rhs was a Smi then strict equality cannot
    // succeed. Return non-equal (a0 is already not zero)
    __ mov(v0, a0);
    __ Ret(ne, t4, Operand(HEAP_NUMBER_TYPE));
  } else {
    // Smi compared non-strictly with a non-Smi non-heap-number. Call
    // the runtime.
    __ Branch(slow, ne, t4, Operand(HEAP_NUMBER_TYPE));
  }

  // Rhs is a smi, lhs is a number.
  // Convert smi a1 to double.
  if (CpuFeatures::IsSupported(FPU)) {
    CpuFeatures::Scope scope(FPU);
    __ sra(at, a1, kSmiTagSize);
    __ mtc1(at, f14);
    __ cvt_d_w(f14, f14);
    __ ldc1(f12, FieldMemOperand(a0, HeapNumber::kValueOffset));
  } else {
    // Load lhs to a double in a2, a3.
    __ lw(a3, FieldMemOperand(a0, HeapNumber::kValueOffset + 4));
    __ lw(a2, FieldMemOperand(a0, HeapNumber::kValueOffset));

    // Write Smi from a1 to a1 and a0 in double format. t5 is scratch.
    __ mov(t6, a1);
    ConvertToDoubleStub stub1(a1, a0, t6, t5);
    __ Push(ra);
    __ Call(stub1.GetCode(), RelocInfo::CODE_TARGET);

    __ Pop(ra);
  }

  // We now have both loaded as doubles.
  __ jmp(both_loaded_as_doubles);

  __ bind(&lhs_is_smi);
  // Lhs is a Smi.  Check whether the non-smi is a heap number.
  __ GetObjectType(a1, t4, t4);
  if (strict) {
    // If lhs was not a number and rhs was a Smi then strict equality cannot
    // succeed. Return non-equal.
    __ li(v0, Operand(1));
    __ Ret(ne, t4, Operand(HEAP_NUMBER_TYPE));
  } else {
    // Smi compared non-strictly with a non-Smi non-heap-number. Call
    // the runtime.
    __ Branch(slow, ne, t4, Operand(HEAP_NUMBER_TYPE));
  }

  // Lhs is a smi, rhs is a number.
  // a0 is Smi and a1 is heap number.
  // Convert smi a0 to double.
  if (CpuFeatures::IsSupported(FPU)) {
    CpuFeatures::Scope scope(FPU);
    __ sra(at, a0, kSmiTagSize);
    __ mtc1(at, f12);
    __ cvt_d_w(f12, f12);
    __ ldc1(f14, FieldMemOperand(a1, HeapNumber::kValueOffset));
  } else {
    // Convert lhs to a double format. t5 is scratch.
    __ mov(t6, a0);
    ConvertToDoubleStub stub2(a3, a2, t6, t5);
    __ Push(ra);
    __ Call(stub2.GetCode(), RelocInfo::CODE_TARGET);
    __ Pop(ra);
    // Load rhs to a double in a1, a0.
    __ lw(a0, FieldMemOperand(a1, HeapNumber::kValueOffset));
    __ lw(a1, FieldMemOperand(a1, HeapNumber::kValueOffset + 4));
  }
  // Fall through to both_loaded_as_doubles.
}


void EmitNanCheck(MacroAssembler* masm, Condition cc) {
  bool exp_first = (HeapNumber::kExponentOffset == HeapNumber::kValueOffset);
  if (CpuFeatures::IsSupported(FPU)) {
    CpuFeatures::Scope scope(FPU);
    // Lhs and rhs are already loaded to f12 and f14 register pairs
    __ mfc1(t0, f14);  // f14 has LS 32 bits of rhs.
    __ mfc1(t1, f15);  // f15 has MS 32 bits of rhs.
    __ mfc1(t2, f12);  // f12 has LS 32 bits of lhs.
    __ mfc1(t3, f13);  // f13 has MS 32 bits of lhs.
  } else {
    // Lhs and rhs are already loaded to GP registers
    __ mov(t0, a0);  // a0 has LS 32 bits of rhs.
    __ mov(t1, a1);  // a1 has MS 32 bits of rhs.
    __ mov(t2, a2);  // a2 has LS 32 bits of lhs.
    __ mov(t3, a3);  // a3 has MS 32 bits of lhs.
  }
  Register rhs_exponent = exp_first ? t0 : t1;
  Register lhs_exponent = exp_first ? t2 : t3;
  Register rhs_mantissa = exp_first ? t1 : t0;
  Register lhs_mantissa = exp_first ? t3 : t2;
  Label one_is_nan, neither_is_nan;
  Label lhs_not_nan_exp_mask_is_loaded;

  Register exp_mask_reg = t4;
  __ li(exp_mask_reg, HeapNumber::kExponentMask);
  __ and_(t5, lhs_exponent, exp_mask_reg);
  __ Branch(&lhs_not_nan_exp_mask_is_loaded, ne, t5, Operand(exp_mask_reg));

  __ sll(t5, lhs_exponent, HeapNumber::kNonMantissaBitsInTopWord);
  __ Branch(&one_is_nan, ne, t5, Operand(zero_reg));

  __ Branch(&one_is_nan, ne, lhs_mantissa, Operand(zero_reg));

  __ li(exp_mask_reg, HeapNumber::kExponentMask);
  __ bind(&lhs_not_nan_exp_mask_is_loaded);
  __ and_(t5, rhs_exponent, exp_mask_reg);

  __ Branch(&neither_is_nan, ne, t5, Operand(exp_mask_reg));

  __ sll(t5, rhs_exponent, HeapNumber::kNonMantissaBitsInTopWord);
  __ Branch(&one_is_nan, ne, t5, Operand(zero_reg));

  __ Branch(&neither_is_nan, eq, rhs_mantissa, Operand(zero_reg));

  __ bind(&one_is_nan);
  // NaN comparisons always fail.
  // Load whatever we need in v0 to make the comparison fail.
  if (cc == lt || cc == le) {
    __ li(v0, Operand(GREATER));
  } else {
    __ li(v0, Operand(LESS));
  }
  __ Ret();  // Return.

  __ bind(&neither_is_nan);
}


static void EmitTwoNonNanDoubleComparison(MacroAssembler* masm, Condition cc) {
  // f12 and f14 have the two doubles.  Neither is a NaN.
  // Call a native function to do a comparison between two non-NaNs.
  // Call C routine that may not cause GC or other trouble.
  // We use a call_was and return manually because we need arguments slots to
  // be freed.

  Label return_result_not_equal, return_result_equal;
  if (cc == eq) {
    // Doubles are not equal unless they have the same bit pattern.
    // Exception: 0 and -0.
    bool exp_first = (HeapNumber::kExponentOffset == HeapNumber::kValueOffset);
    if (CpuFeatures::IsSupported(FPU)) {
    CpuFeatures::Scope scope(FPU);
      // Lhs and rhs are already loaded to f12 and f14 register pairs
      __ mfc1(t0, f14);  // f14 has LS 32 bits of rhs.
      __ mfc1(t1, f15);  // f15 has MS 32 bits of rhs.
      __ mfc1(t2, f12);  // f12 has LS 32 bits of lhs.
      __ mfc1(t3, f13);  // f13 has MS 32 bits of lhs.
    } else {
      // Lhs and rhs are already loaded to GP registers
      __ mov(t0, a0);  // a0 has LS 32 bits of rhs.
      __ mov(t1, a1);  // a1 has MS 32 bits of rhs.
      __ mov(t2, a2);  // a2 has LS 32 bits of lhs.
      __ mov(t3, a3);  // a3 has MS 32 bits of lhs.
    }
    Register rhs_exponent = exp_first ? t0 : t1;
    Register lhs_exponent = exp_first ? t2 : t3;
    Register rhs_mantissa = exp_first ? t1 : t0;
    Register lhs_mantissa = exp_first ? t3 : t2;

    __ xor_(v0, rhs_mantissa, lhs_mantissa);
    __ Branch(&return_result_not_equal, ne, v0, Operand(zero_reg));

    __ subu(v0, rhs_exponent, lhs_exponent);
    __ Branch(&return_result_equal, eq, v0, Operand(zero_reg));
    // 0, -0 case
    __ sll(rhs_exponent, rhs_exponent, kSmiTagSize);
    __ sll(lhs_exponent, lhs_exponent, kSmiTagSize);
    __ or_(t4, rhs_exponent, lhs_exponent);
    __ or_(t4, t4, rhs_mantissa);

    __ Branch(&return_result_not_equal, ne, t4, Operand(zero_reg));

    __ bind(&return_result_equal);
    __ li(v0, Operand(EQUAL));
    __ Ret();
  }

  __ bind(&return_result_not_equal);

  if (!CpuFeatures::IsSupported(FPU)) {
    __ Push(ra);
    __ PrepareCallCFunction(4, t4);  // Two doubles count as 4 arguments.
    if (!IsMipsSoftFloatABI) {
      // We are not using MIPS FPU instructions, and parameters for the runtime
      // function call are prepaired in a0-a3 registers, but function we are
      // calling is compiled with hard-float flag and expecting hard float ABI
      // (parameters in f12/f14 registers). We need to copy parameters from
      // a0-a3 registers to f12/f14 register pairs.
      __ mtc1(a0, f12);
      __ mtc1(a1, f13);
      __ mtc1(a2, f14);
      __ mtc1(a3, f15);
    }
    __ CallCFunction(ExternalReference::compare_doubles(), 4);
    __ Pop(ra);  // Because this function returns int, result is in v0.
    __ Ret();
  } else {
    CpuFeatures::Scope scope(FPU);
    Label equal, less_than;
    __ c(EQ, D, f12, f14);
    __ bc1t(&equal);
    __ nop();

    __ c(OLT, D, f12, f14);
    __ bc1t(&less_than);
    __ nop();

    // Not equal, not less, not NaN, must be greater.
    __ li(v0, Operand(GREATER));
    __ Ret();

    __ bind(&equal);
    __ li(v0, Operand(EQUAL));
    __ Ret();

    __ bind(&less_than);
    __ li(v0, Operand(LESS));
    __ Ret();
  }
}


static void EmitStrictTwoHeapObjectCompare(MacroAssembler* masm) {
    // If either operand is a JSObject or an oddball value, then they are
    // not equal since their pointers are different.
    // There is no test for undetectability in strict equality.
    ASSERT(LAST_TYPE == JS_FUNCTION_TYPE);
    Label first_non_object;
    // Get the type of the first operand into a2 and compare it with
    // FIRST_JS_OBJECT_TYPE.
    __ GetObjectType(a0, a2, a2);
    __ Branch(&first_non_object, less, a2, Operand(FIRST_JS_OBJECT_TYPE));

    // Return non-zero.
    Label return_not_equal;
    __ bind(&return_not_equal);
    __ li(v0, Operand(1));
    __ Ret();

    __ bind(&first_non_object);
    // Check for oddballs: true, false, null, undefined.
    __ Branch(&return_not_equal, eq, a2, Operand(ODDBALL_TYPE));

    __ GetObjectType(a1, a3, a3);
    __ Branch(&return_not_equal, greater, a3, Operand(FIRST_JS_OBJECT_TYPE));

    // Check for oddballs: true, false, null, undefined.
    __ Branch(&return_not_equal, eq, a3, Operand(ODDBALL_TYPE));

    // Now that we have the types we might as well check for symbol-symbol.
    // Ensure that no non-strings have the symbol bit set.
    ASSERT(kNotStringTag + kIsSymbolMask > LAST_TYPE);
    ASSERT(kSymbolTag != 0);
    __ And(t2, a2, Operand(a3));
    __ And(t0, t2, Operand(kIsSymbolMask));
    __ Branch(&return_not_equal, ne, t0, Operand(zero_reg));
}


static void EmitCheckForTwoHeapNumbers(MacroAssembler* masm,
                                       Label* both_loaded_as_doubles,
                                       Label* not_heap_numbers,
                                       Label* slow) {
  __ GetObjectType(a0, a2, a2);
  __ Branch(not_heap_numbers, ne, a2, Operand(HEAP_NUMBER_TYPE));
  __ GetObjectType(a1, a3, a3);
  // First was a heap number, second wasn't. Go slow case.
  __ Branch(not_heap_numbers, ne, a3, Operand(HEAP_NUMBER_TYPE));

  // Both are heap numbers. Load them up then jump to the code we have
  // for that.
  if (CpuFeatures::IsSupported(FPU)) {
    CpuFeatures::Scope scope(FPU);
    __ ldc1(f12, FieldMemOperand(a0, HeapNumber::kValueOffset));
    __ ldc1(f14, FieldMemOperand(a1, HeapNumber::kValueOffset));
  } else {
    __ lw(a2, FieldMemOperand(a0, HeapNumber::kValueOffset));
    __ lw(a3, FieldMemOperand(a0, HeapNumber::kValueOffset + 4));
    __ lw(a0, FieldMemOperand(a1, HeapNumber::kValueOffset));
    __ lw(a1, FieldMemOperand(a1, HeapNumber::kValueOffset + 4));
  }
  __ jmp(both_loaded_as_doubles);
}


static void EmitCheckForSymbols(MacroAssembler* masm, Label* slow) {
  // a2 is object type of a0.
  // Ensure that no non-strings have the symbol bit set.
  ASSERT(kNotStringTag + kIsSymbolMask > LAST_TYPE);
  ASSERT(kSymbolTag != 0);
  __ And(t2, a2, Operand(kIsSymbolMask));
  __ Branch(slow, eq, t2, Operand(zero_reg));
  __ lw(a3, FieldMemOperand(a1, HeapObject::kMapOffset));
  __ lbu(a3, FieldMemOperand(a3, Map::kInstanceTypeOffset));
  __ And(t3, a3, Operand(kIsSymbolMask));
  __ Branch(slow, eq, t3, Operand(zero_reg));

  // Both are symbols. We already checked they weren't the same pointer
  // so they are not equal.
  __ li(v0, Operand(1));   // Non-zero indicates not equal.
  __ Ret();
}


void NumberToStringStub::GenerateLookupNumberStringCache(MacroAssembler* masm,
                                                         Register object,
                                                         Register result,
                                                         Register scratch1,
                                                         Register scratch2,
                                                         Register scratch3,
                                                         bool object_is_smi,
                                                         Label* not_found) {
  // Use of registers. Register result is used as a temporary.
  Register number_string_cache = result;
  Register mask = scratch3;

  // Load the number string cache.
  __ LoadRoot(number_string_cache, Heap::kNumberStringCacheRootIndex);

  // Make the hash mask from the length of the number string cache. It
  // contains two elements (number and string) for each cache entry.
  __ lw(mask, FieldMemOperand(number_string_cache, FixedArray::kLengthOffset));
  // Divide length by two (length is a smi).
  __ sra(mask, mask, kSmiTagSize + 1);
  __ Addu(mask, mask, -1);  // Make mask.

  // Calculate the entry in the number string cache. The hash value in the
  // number string cache for smis is just the smi value, and the hash for
  // doubles is the xor of the upper and lower words. See
  // Heap::GetNumberStringCache.
  Label is_smi;
  Label load_result_from_cache;
  if (!object_is_smi) {
    __ BranchOnSmi(object, &is_smi);
    if (CpuFeatures::IsSupported(FPU)) {
      CpuFeatures::Scope scope(FPU);
      __ CheckMap(object,
                  scratch1,
                  Heap::kHeapNumberMapRootIndex,
                  not_found,
                  true);

      ASSERT_EQ(8, kDoubleSize);
      __ Addu(scratch1,
              object,
              Operand(HeapNumber::kValueOffset - kHeapObjectTag));
      __ lw(scratch2, MemOperand(scratch1, kPointerSize));
      __ lw(scratch1, MemOperand(scratch1, 0));
      __ Xor(scratch1, scratch1, Operand(scratch2));
      __ And(scratch1, scratch1, Operand(mask));

      // Calculate address of entry in string cache: each entry consists
      // of two pointer sized fields.
      __ sll(scratch1, scratch1, kPointerSizeLog2 + 1);
      __ Addu(scratch1, number_string_cache, scratch1);

      Register probe = mask;
      __ lw(probe,
             FieldMemOperand(scratch1, FixedArray::kHeaderSize));
      __ BranchOnSmi(probe, not_found);
      __ ldc1(f12, FieldMemOperand(object, HeapNumber::kValueOffset));
      __ ldc1(f14, FieldMemOperand(probe, HeapNumber::kValueOffset));
      __ c(EQ, D, f12, f14);
      __ bc1t(&load_result_from_cache);
      __ nop();   // bc1t() requires explicit fill of branch delay slot.
      __ Branch(not_found);
    } else {
      // Note that there is no cache check for non-FPU case, even though
      // it seems there could be. May be a tiny opimization for non-FPU
      // cores.
      __ Branch(not_found);
    }
  }

  __ bind(&is_smi);
  Register scratch = scratch1;
  __ sra(scratch, object, 1);   // Shift away the tag.
  __ And(scratch, mask, Operand(scratch));

  // Calculate address of entry in string cache: each entry consists
  // of two pointer sized fields.
  __ sll(scratch, scratch, kPointerSizeLog2 + 1);
  __ Addu(scratch, number_string_cache, scratch);

  // Check if the entry is the smi we are looking for.
  Register probe = mask;
  __ lw(probe, FieldMemOperand(scratch, FixedArray::kHeaderSize));
  __ Branch(not_found, ne, object, Operand(probe));

  // Get the result from the cache.
  __ bind(&load_result_from_cache);
  __ lw(result,
         FieldMemOperand(scratch, FixedArray::kHeaderSize + kPointerSize));

  __ IncrementCounter(&Counters::number_to_string_native,
                      1,
                      scratch1,
                      scratch2);
}


void NumberToStringStub::Generate(MacroAssembler* masm) {
  Label runtime;

  __ lw(a1, MemOperand(sp, 0));

  // Generate code to lookup number in the number string cache.
  GenerateLookupNumberStringCache(masm, a1, v0, a2, a3, t0, false, &runtime);
  __ Addu(sp, sp, Operand(1 * kPointerSize));
  __ Ret();

  __ bind(&runtime);
  // Handle number to string in the runtime system if not found in the cache.
  __ TailCallRuntime(Runtime::kNumberToString, 1, 1);
}


void RecordWriteStub::Generate(MacroAssembler* masm) {
  __ addu(offset_, object_, offset_);
  __ RecordWriteHelper(object_, offset_, scratch_);
  __ Ret();
}


// On entry a0 (lhs) and a1 (rhs) are the things to be compared. On exit, v0
// is 0, positive, or negative (smi) to indicate the result of the comparison.
void CompareStub::Generate(MacroAssembler* masm) {
  Label slow;  // Call builtin.
  Label not_smis, both_loaded_as_doubles;
  // NOTICE! This code is only reached after a smi-fast-case check, so
  // it is certain that at least one operand isn't a smi.

  // Handle the case where the objects are identical.  Either returns the answer
  // or goes to slow.  Only falls through if the objects were not identical.
  EmitIdenticalObjectComparison(masm, &slow, cc_, never_nan_nan_);

  // If either is a Smi (we know that not both are), then they can only
  // be strictly equal if the other is a HeapNumber.
  ASSERT_EQ(0, kSmiTag);
  ASSERT_EQ(0, Smi::FromInt(0));
  __ And(t2, a0, Operand(a1));
  __ BranchOnNotSmi(t2, &not_smis, t0);
  // One operand is a smi. EmitSmiNonsmiComparison generates code that can:
  // 1) Return the answer.
  // 2) Go to slow.
  // 3) Fall through to both_loaded_as_doubles.
  // 4) Jump to rhs_not_nan.
  // In cases 3 and 4 we have found out we were dealing with a number-number
  // comparison and the numbers have been loaded into f12 and f14 as doubles,
  // or in GP registers (a0, a1, a2, a3) depending on the presence of the FPU.
  EmitSmiNonsmiComparison(masm, &both_loaded_as_doubles, &slow, strict_);

  __ bind(&both_loaded_as_doubles);
  // f12, f14 are the double representations of the left hand side
  // and the right hand side if we have FPU. Otherwise a2, a3 are representing
  // left hand side and a0, a1 represent right hand side.

  // Checks for NaN in the doubles we have loaded.  Can return the answer or
  // fall through if neither is a NaN.  Also binds rhs_not_nan.
  EmitNanCheck(masm, cc_);

  // Compares two doubles that are not NaNs. Returns the answer.
  // Never falls through.
  EmitTwoNonNanDoubleComparison(masm, cc_);

  __ bind(&not_smis);
  // At this point we know we are dealing with two different objects,
  // and neither of them is a Smi. The objects are in a0 and a1.
  if (strict_) {
    // This returns non-equal for some object types, or falls through if it
    // was not lucky.
    EmitStrictTwoHeapObjectCompare(masm);
  }

  Label check_for_symbols;
  Label flat_string_check;
  // Check for heap-number-heap-number comparison. Can jump to slow case,
  // or load both doubles and jump to the code that handles
  // that case. If the inputs are not doubles then jumps to check_for_symbols.
  // In this case a2 will contain the type of a0.
  EmitCheckForTwoHeapNumbers(masm,
                             &both_loaded_as_doubles,
                             &check_for_symbols,
                             &flat_string_check);

  __ bind(&check_for_symbols);
  if (cc_ == eq && !strict_) {
    // Either jumps to slow or returns the answer. Assumes that a2 is the type
    // of a0 on entry.
    EmitCheckForSymbols(masm, &flat_string_check);
  }

  // Check for both being sequential ASCII strings, and inline if that is the
  // case.
  __ bind(&flat_string_check);

  __ JumpIfNonSmisNotBothSequentialAsciiStrings(a0, a1, a2, a3, &slow);

  __ IncrementCounter(&Counters::string_compare_native, 1, a2, a3);
  StringCompareStub::GenerateCompareFlatAsciiStrings(masm,
                                                     a1,
                                                     a0,
                                                     a2,
                                                     a3,
                                                     t0,
                                                     t1);
  // Never falls through to here.

  __ bind(&slow);
  // Prepare for call to builtin. Push object pointers, a0 (lhs) first,
  // a1 (rhs) second.
  __ Push(a0, a1);
  // Figure out which native to call and setup the arguments.
  Builtins::JavaScript native;
  if (cc_ == eq) {
    native = strict_ ? Builtins::STRICT_EQUALS : Builtins::EQUALS;
  } else {
    native = Builtins::COMPARE;
    int ncr;  // NaN compare result
    if (cc_ == lt || cc_ == le) {
      ncr = GREATER;
    } else {
      ASSERT(cc_ == gt || cc_ == ge);  // remaining cases
      ncr = LESS;
    }
    __ li(a0, Operand(Smi::FromInt(ncr)));
    __ Push(a0);
  }

  // Call the native; it returns -1 (less), 0 (equal), or 1 (greater)
  // tagged as a small integer.
  __ InvokeBuiltin(native, JUMP_JS);
}


void CEntryStub::GenerateThrowTOS(MacroAssembler* masm) {
  // v0 holds the exception.

  // Adjust this code if not the case.
  ASSERT(StackHandlerConstants::kSize == 4 * kPointerSize);

  // Drop the sp to the top of the handler.
  __ li(a3, Operand(ExternalReference(Top::k_handler_address)));
  __ lw(sp, MemOperand(a3));

  // Restore the next handler and frame pointer, discard handler state.
  ASSERT(StackHandlerConstants::kNextOffset == 0);
  __ Pop(a2);
  __ sw(a2, MemOperand(a3));
  ASSERT(StackHandlerConstants::kFPOffset == 2 * kPointerSize);
  __ MultiPop(a3.bit() | fp.bit());

  // Before returning we restore the context from the frame pointer if
  // not NULL. The frame pointer is NULL in the exception handler of a
  // JS entry frame.
  // Set cp to NULL if fp is NULL.
  Label done;
  __ Branch(false, &done, eq, fp, Operand(zero_reg));
  __ mov(cp, zero_reg);   // Use the branch delay slot.
  __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
  __ bind(&done);

#ifdef DEBUG
  // TODO(MIPS): Implement debug code.
#endif

  ASSERT(StackHandlerConstants::kPCOffset == 3 * kPointerSize);
  __ Pop(t9);
  __ Jump(t9);
}


void CEntryStub::GenerateThrowUncatchable(MacroAssembler* masm,
                                          UncatchableExceptionType type) {
  // Adjust this code if not the case.
  ASSERT(StackHandlerConstants::kSize == 4 * kPointerSize);

  // Drop sp to the top stack handler.
  __ li(a3, Operand(ExternalReference(Top::k_handler_address)));
  __ lw(sp, MemOperand(a3));

  // Unwind the handlers until the ENTRY handler is found.
  Label loop, done;
  __ bind(&loop);
  // Load the type of the current stack handler.
  const int kStateOffset = StackHandlerConstants::kStateOffset;
  __ lw(a2, MemOperand(sp, kStateOffset));
  __ Branch(&done, eq, a2, Operand(StackHandler::ENTRY));
  // Fetch the next handler in the list.
  const int kNextOffset = StackHandlerConstants::kNextOffset;
  __ lw(sp, MemOperand(sp, kNextOffset));
  __ jmp(&loop);
  __ bind(&done);

  // Set the top handler address to next handler past the current ENTRY handler.
  ASSERT(StackHandlerConstants::kNextOffset == 0);
  __ Pop(a2);
  __ sw(a2, MemOperand(a3));

  if (type == OUT_OF_MEMORY) {
    // Set external caught exception to false.
    ExternalReference external_caught(Top::k_external_caught_exception_address);
    __ li(a0, Operand(false));
    __ li(a2, Operand(external_caught));
    __ sw(a0, MemOperand(a2));

    // Set pending exception and v0 to out of memory exception.
    Failure* out_of_memory = Failure::OutOfMemoryException();
    __ li(v0, Operand(reinterpret_cast<int32_t>(out_of_memory)));
    __ li(a2, Operand(ExternalReference(Top::k_pending_exception_address)));
    __ sw(v0, MemOperand(a2));
  }

  // Stack layout at this point. See also StackHandlerConstants.
  // sp ->   state (ENTRY)
  //         fp
  //         lr

  // Discard handler state (r2 is not used) and restore frame pointer.
  ASSERT(StackHandlerConstants::kFPOffset == 2 * kPointerSize);
  __ MultiPop(a2.bit() | fp.bit());  // a2: discarded state.
  // Before returning we restore the context from the frame pointer if
  // not NULL.  The frame pointer is NULL in the exception handler of a
  // JS entry frame.
  // Set cp to NULL if fp is NULL, else restore cp.
  Label cp_null;
  __ Branch(false, &cp_null, eq, fp, Operand(zero_reg));
  __ mov(cp, zero_reg);   // Use the branch delay slot.
  __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
  __ bind(&cp_null);

#ifdef DEBUG
  // TODO(MIPS): Implement debug code.
  // if (FLAG_debug_code) {
  //   __ mov(lr, Operand(pc));
  // }
#endif
  ASSERT(StackHandlerConstants::kPCOffset == 3 * kPointerSize);
  __ Pop(t9);
  __ Jump(t9);
}

void CEntryStub::GenerateCore(MacroAssembler* masm,
                              Label* throw_normal_exception,
                              Label* throw_termination_exception,
                              Label* throw_out_of_memory_exception,
                              bool do_gc,
                              bool always_allocate,
                              int frame_alignment_skew) {
  // v0: result parameter for PerformGC, if any
  // s0: number of arguments including receiver (C callee-saved)
  // s1: pointer to the first argument          (C callee-saved)
  // s2: pointer to builtin function            (C callee-saved)

  if (do_gc) {
    // Move result passed in v0 into a0 to call PerformGC.
    __ mov(a0, v0);
    __ PrepareCallCFunction(1, a1);
    __ CallCFunction(ExternalReference::perform_gc_function(), 1);
  }

  ExternalReference scope_depth =
      ExternalReference::heap_always_allocate_scope_depth();
  if (always_allocate) {
    __ li(a0, Operand(scope_depth));
    __ lw(a1, MemOperand(a0));
    __ Addu(a1, a1, Operand(1));
    __ sw(a1, MemOperand(a0));
  }

  // Prepare arguments for C routine: a0 = argc, a1 = argv
  __ mov(a0, s0);
  __ mov(a1, s1);

  // We are calling compiled C/C++ code. a0 and a1 hold our two arguments. We
  // also need to reserve the 4 argument slots on the stack.

  // TODO(MIPS): As of 26May10, Arm code has frame-alignment checks
  // and modification code here.

  // The mips __ EnterExitFrame(), which is called in CEntryStub::Generate,
  // does stack alignment to activation_frame_alignment. In this routine,
  // that alignment must be preserved. We do need to push one kPointerSize
  // value (below), plus the argument slots. See comments, caveats in
  // MacroAssembler::AlignStack() function.
#if defined(V8_HOST_ARCH_MIPS)
  int activation_frame_alignment = OS::ActivationFrameAlignment();
#else  // !defined(V8_HOST_ARCH_MIPS)
  int activation_frame_alignment = 2 * kPointerSize;
#endif  // defined(V8_HOST_ARCH_MIPS)

  int stack_adjustment = (StandardFrameConstants::kCArgsSlotsSize
                       + kPointerSize
                       + (activation_frame_alignment - 1))
                       & ~(activation_frame_alignment - 1);

  // From arm version of this function:
  // TODO(1242173): To let the GC traverse the return address of the exit
  // frames, we need to know where the return address is. Right now,
  // we push it on the stack to be able to find it again, but we never
  // restore from it in case of changes, which makes it impossible to
  // support moving the C entry code stub. This should be fixed, but currently
  // this is OK because the CEntryStub gets generated so early in the V8 boot
  // sequence that it is not moving ever.

  // This branch-and-link sequence is needed to find the current PC on mips,
  // saved to the ra register.
  // Use masm-> here instead of the double-underscore macro since extra
  // coverage code can interfere with the proper calculation of ra.
  Label find_ra;
  masm->bal(&find_ra);
  masm->nop();  // Branch delay slot nop.
  masm->bind(&find_ra);

  // Adjust the value in ra to point to the correct return location, 2nd
  // instruction past the real call into C code (the jalr(t9)), and push it.
  // This is the return address of the exit frame.
  masm->Addu(ra, ra, 20);  // 5 instructions is 20 bytes.
  masm->addiu(sp, sp, -(stack_adjustment));
  masm->sw(ra, MemOperand(sp, stack_adjustment - kPointerSize));

  // Call the C routine.
  { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm);
    masm->mov(t9, s2);  // Function pointer to t9 to conform to ABI for PIC.
    masm->jalr(t9);
    masm->nop();    // Branch delay slot nop.
  }

  // Restore stack (remove arg slots and extra parameter).
  masm->addiu(sp, sp, stack_adjustment);


  if (always_allocate) {
    // It's okay to clobber a2 and a3 here. v0 & v1 contain result.
    __ li(a2, Operand(scope_depth));
    __ lw(a3, MemOperand(a2));
    __ Subu(a3, a3, Operand(1));
    __ sw(a3, MemOperand(a2));
  }

  // Check for failure result.
  Label failure_returned;
  ASSERT(((kFailureTag + 1) & kFailureTagMask) == 0);
  __ addiu(a2, v0, 1);
  __ andi(t0, a2, kFailureTagMask);
  __ Branch(&failure_returned, eq, t0, Operand(zero_reg));

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
  __ Branch(&retry, eq, t0, Operand(zero_reg));

  // Special handling of out of memory exceptions.
  Failure* out_of_memory = Failure::OutOfMemoryException();
  __ Branch(throw_out_of_memory_exception, eq,
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
  __ Branch(throw_termination_exception, eq,
            v0, Operand(Factory::termination_exception()));

  // Handle normal exception.
  __ jmp(throw_normal_exception);

  __ bind(&retry);
  // Last failure (v0) will be moved to (a0) for parameter when retrying.
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

  // Load argv in s0 register.
  __ lw(s0, MemOperand(sp, kNumCalleeSaved * kPointerSize +
                           StandardFrameConstants::kCArgsSlotsSize));

  // We build an EntryFrame.
  __ li(t3, Operand(-1));  // Push a bad frame pointer to fail if it is used.
  int marker = is_construct ? StackFrame::ENTRY_CONSTRUCT : StackFrame::ENTRY;
  __ li(t2, Operand(Smi::FromInt(marker)));
  __ li(t1, Operand(Smi::FromInt(marker)));
  __ LoadExternalReference(t0, ExternalReference(Top::k_c_entry_fp_address));
  __ lw(t0, MemOperand(t0));
  __ Push(t3, t2, t1, t0);
  // Setup frame pointer for the frame to be pushed.
  __ addiu(fp, sp, -EntryFrameConstants::kCallerFPOffset);

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
  __ Branch(&slow, less, a2, Operand(FIRST_JS_OBJECT_TYPE));
  __ Branch(&slow, greater, a2, Operand(LAST_JS_OBJECT_TYPE));

  // Get the prototype of the function (t0 is result, a2 is scratch).
  __ lw(a1, MemOperand(sp, 0 * kPointerSize));
  // a1 is function, a3 is map.

  // Look up the function and the map in the instanceof cache.
  Label miss;
  __ LoadRoot(at, Heap::kInstanceofCacheFunctionRootIndex);
  __ Branch(&miss, ne, a1, Operand(at));
  __ LoadRoot(at, Heap::kInstanceofCacheMapRootIndex);
  __ Branch(&miss, ne, a3, Operand(at));
  __ LoadRoot(v0, Heap::kInstanceofCacheAnswerRootIndex);
  __ Ret(false);  // Use branch delay slot.
  __ Pop(2);  // In branch delay slot, remove 2 arguments.

  __ bind(&miss);

  __ TryGetFunctionPrototype(a1, t0, a2, &slow);

  // Check that the function prototype is a JS object.
  __ BranchOnSmi(t0, &slow);
  __ GetObjectType(t0, t1, t1);
  __ Branch(&slow, less, t1, Operand(FIRST_JS_OBJECT_TYPE));
  __ Branch(&slow, greater, t1, Operand(LAST_JS_OBJECT_TYPE));

  __ StoreRoot(a1, Heap::kInstanceofCacheFunctionRootIndex);
  __ StoreRoot(a3, Heap::kInstanceofCacheMapRootIndex);

  // Register mapping: a3 is object map and t0 is function prototype.
  // Get prototype of object into a2.
  __ lw(a2, FieldMemOperand(a3, Map::kPrototypeOffset));

  __ LoadRoot(t1, Heap::kNullValueRootIndex);
  // Loop through the prototype chain looking for the function prototype.
  __ bind(&loop);
  __ Branch(&is_instance, eq, a2, Operand(t0));
  __ Branch(&is_not_instance, eq, a2, Operand(t1));
  __ lw(a2, FieldMemOperand(a2, HeapObject::kMapOffset));
  __ lw(a2, FieldMemOperand(a2, Map::kPrototypeOffset));
  __ jmp(&loop);

  __ bind(&is_instance);
  __ li(v0, Operand(Smi::FromInt(0)));
  __ StoreRoot(v0, Heap::kInstanceofCacheAnswerRootIndex);
  __ Ret(false);  // Use branch delay slot.
  __ Pop(2);  // In branch delay slot.

  __ bind(&is_not_instance);
  __ li(v0, Operand(Smi::FromInt(1)));
  __ StoreRoot(v0, Heap::kInstanceofCacheAnswerRootIndex);
  __ Ret(false);  // Use branch delay slot.
  __ Pop(2);  // In branch delay slot.

  // Slow-case. Tail call builtin.
  __ bind(&slow);
  __ InvokeBuiltin(Builtins::INSTANCE_OF, JUMP_JS);
}


void ArgumentsAccessStub::GenerateReadElement(MacroAssembler* masm) {
  // The displacement is the offset of the last parameter (if any)
  // relative to the frame pointer.
  static const int kDisplacement =
      StandardFrameConstants::kCallerSPOffset - kPointerSize;

  // Check that the key is a smiGenerateReadElement.
  Label slow;
  __ BranchOnNotSmi(a1, &slow);

  // Check if the calling frame is an arguments adaptor frame.
  Label adaptor;
  __ lw(a2, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ lw(a3, MemOperand(a2, StandardFrameConstants::kContextOffset));
  __ Branch(&adaptor,
            eq,
            a3,
            Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));

  // Check index against formal parameters count limit passed in
  // through register a0. Use unsigned comparison to get negative
  // check for free.
  __ Branch(&slow, Ugreater_equal, a0, Operand(a1));

  // Read the argument from the stack and return it.
  __ subu(a3, a0, a1);
  __ sll(t3, a3, kPointerSizeLog2 - kSmiTagSize);
  __ Addu(a3, fp, Operand(t3));
  __ lw(v0, MemOperand(a3, kDisplacement));
  __ Ret();

  // Arguments adaptor case: Check index against actual arguments
  // limit found in the arguments adaptor frame. Use unsigned
  // comparison to get negative check for free.
  __ bind(&adaptor);
  __ lw(a0, MemOperand(a2, ArgumentsAdaptorFrameConstants::kLengthOffset));
  __ Branch(&slow, greater_equal, a1, Operand(a0));

  // Read the argument from the adaptor frame and return it.
  __ subu(a3, a0, a1);
  __ sll(t3, a3, kPointerSizeLog2 - kSmiTagSize);
  __ Addu(a3, a2, Operand(t3));
  __ lw(v0, MemOperand(a3, kDisplacement));
  __ Ret();

  // Slow-case: Handle non-smi or out-of-bounds access to arguments
  // by calling the runtime system.
  __ bind(&slow);
  __ Push(a1);
  __ TailCallRuntime(Runtime::kGetArgumentsProperty, 1, 1);
}


void RegExpExecStub::Generate(MacroAssembler* masm) {
  // Just jump directly to runtime if native RegExp is not selected at compile
  // time or if regexp entry in generated code is turned off runtime switch or
  // at compilation.
#ifdef V8_INTERPRETED_REGEXP
  __ TailCallRuntime(Runtime::kRegExpExec, 4, 1);
#else  // V8_INTERPRETED_REGEXP
  if (!FLAG_regexp_entry_native) {
    __ TailCallRuntime(Runtime::kRegExpExec, 4, 1);
    return;
  }

  // Stack frame on entry.
  //  sp[0]: last_match_info (expected JSArray)
  //  sp[4]: previous index
  //  sp[8]: subject string
  //  sp[12]: JSRegExp object

  static const int kLastMatchInfoOffset = 0 * kPointerSize;
  static const int kPreviousIndexOffset = 1 * kPointerSize;
  static const int kSubjectOffset = 2 * kPointerSize;
  static const int kJSRegExpOffset = 3 * kPointerSize;

  Label runtime, invoke_regexp;

  // Allocation of registers for this function. These are in callee save
  // registers and will be preserved by the call to the native RegExp code, as
  // this code is called using the normal C calling convention. When calling
  // directly from generated code the native RegExp code will not do a GC and
  // therefore the content of these registers are safe to use after the call.
  // MIPS - using s0..s2, since we are not using CEntry Stub.
  Register subject = s0;
  Register regexp_data = s1;
  Register last_match_info_elements = s2;

  // Ensure that a RegExp stack is allocated.
  ExternalReference address_of_regexp_stack_memory_address =
      ExternalReference::address_of_regexp_stack_memory_address();
  ExternalReference address_of_regexp_stack_memory_size =
      ExternalReference::address_of_regexp_stack_memory_size();
  __ li(a0, Operand(address_of_regexp_stack_memory_size));
  __ lw(a0, MemOperand(a0, 0));
  __ Branch(&runtime, eq, a0, Operand(zero_reg));

  // Check that the first argument is a JSRegExp object.
  __ lw(a0, MemOperand(sp, kJSRegExpOffset));
  ASSERT_EQ(0, kSmiTag);
  __ BranchOnSmi(a0, &runtime);
  __ GetObjectType(a0, a1, a1);
  __ Branch(&runtime, ne, a1, Operand(JS_REGEXP_TYPE));

  // Check that the RegExp has been compiled (data contains a fixed array).
  __ lw(regexp_data, FieldMemOperand(a0, JSRegExp::kDataOffset));
  if (FLAG_debug_code) {
    __ And(t0, regexp_data, Operand(kSmiTagMask));
    __ Check(nz,
             "Unexpected type for RegExp data, FixedArray expected",
             t0,
             Operand(zero_reg));
    __ GetObjectType(regexp_data, a0, a0);
    __ Check(eq,
             "Unexpected type for RegExp data, FixedArray expected",
             a0,
             Operand(FIXED_ARRAY_TYPE));
  }

  // regexp_data: RegExp data (FixedArray)
  // Check the type of the RegExp. Only continue if type is JSRegExp::IRREGEXP.
  __ lw(a0, FieldMemOperand(regexp_data, JSRegExp::kDataTagOffset));
  __ Branch(&runtime, ne, a0, Operand(Smi::FromInt(JSRegExp::IRREGEXP)));

  // regexp_data: RegExp data (FixedArray)
  // Check that the number of captures fit in the static offsets vector buffer.
  __ lw(a2,
         FieldMemOperand(regexp_data, JSRegExp::kIrregexpCaptureCountOffset));
  // Calculate number of capture registers (number_of_captures + 1) * 2. This
  // uses the asumption that smis are 2 * their untagged value.
  ASSERT_EQ(0, kSmiTag);
  ASSERT_EQ(1, kSmiTagSize + kSmiShiftSize);
  __ Addu(a2, a2, Operand(2));  // a2 was a smi.
  // Check that the static offsets vector buffer is large enough.
  __ Branch(&runtime, hi, a2, Operand(OffsetsVector::kStaticOffsetsVectorSize));

  // a2: Number of capture registers
  // regexp_data: RegExp data (FixedArray)
  // Check that the second argument is a string.
  __ lw(subject, MemOperand(sp, kSubjectOffset));
  __ BranchOnSmi(subject, &runtime);
  __ GetObjectType(subject, a0, a0);
  __ And(a0, a0, Operand(kIsNotStringMask));
  ASSERT_EQ(0, kStringTag);
  __ Branch(&runtime, ne, a0, Operand(zero_reg));

  // Get the length of the string to r3.
  __ lw(a3, FieldMemOperand(subject, String::kLengthOffset));

  // a2: Number of capture registers
  // a3: Length of subject string as a smi
  // subject: Subject string
  // regexp_data: RegExp data (FixedArray)
  // Check that the third argument is a positive smi less than the subject
  // string length. A negative value will be greater (unsigned comparison).
  __ lw(a0, MemOperand(sp, kPreviousIndexOffset));
  __ And(at, a0, Operand(kSmiTagMask));
  __ Branch(&runtime, ne, at, Operand(zero_reg));
  __ Branch(&runtime, ls, a3, Operand(a0));

  // a2: Number of capture registers
  // subject: Subject string
  // regexp_data: RegExp data (FixedArray)
  // Check that the fourth object is a JSArray object.
  __ lw(a0, MemOperand(sp, kLastMatchInfoOffset));
  __ BranchOnSmi(a0, &runtime);
  __ GetObjectType(a0, a1, a1);
  __ Branch(&runtime, ne, a1, Operand(JS_ARRAY_TYPE));
  // Check that the JSArray is in fast case.
  __ lw(last_match_info_elements,
         FieldMemOperand(a0, JSArray::kElementsOffset));
  __ lw(a0, FieldMemOperand(last_match_info_elements, HeapObject::kMapOffset));
  __ Branch(&runtime, ne, a0, Operand(Factory::fixed_array_map()));
  // Check that the last match info has space for the capture registers and the
  // additional information.
  __ lw(a0,
         FieldMemOperand(last_match_info_elements, FixedArray::kLengthOffset));
  __ Addu(a2, a2, Operand(RegExpImpl::kLastMatchOverhead));
  __ sra(at, a0, kSmiTagSize);  // Untag length for comparison.
  __ Branch(&runtime, gt, a2, Operand(at));
  // subject: Subject string
  // regexp_data: RegExp data (FixedArray)
  // Check the representation and encoding of the subject string.
  Label seq_string;
  __ lw(a0, FieldMemOperand(subject, HeapObject::kMapOffset));
  __ lbu(a0, FieldMemOperand(a0, Map::kInstanceTypeOffset));
  // First check for flat string.
  __ And(at, a0, Operand(kIsNotStringMask | kStringRepresentationMask));
  ASSERT_EQ(0, kStringTag | kSeqStringTag);
  __ Branch(&seq_string, eq, at, Operand(zero_reg));

  // subject: Subject string
  // a0: instance type if Subject string
  // regexp_data: RegExp data (FixedArray)
  // Check for flat cons string.
  // A flat cons string is a cons string where the second part is the empty
  // string. In that case the subject string is just the first part of the cons
  // string. Also in this case the first part of the cons string is known to be
  // a sequential string or an external string.
  ASSERT(kExternalStringTag != 0);
  ASSERT_EQ(0, kConsStringTag & kExternalStringTag);
  __ And(at, a0, Operand(kIsNotStringMask | kExternalStringTag));
  __ Branch(&runtime, ne, at, Operand(zero_reg));
  __ lw(a0, FieldMemOperand(subject, ConsString::kSecondOffset));
  __ LoadRoot(a1, Heap::kEmptyStringRootIndex);
  __ Branch(&runtime, ne, a0, Operand(a1));
  __ lw(subject, FieldMemOperand(subject, ConsString::kFirstOffset));
  __ lw(a0, FieldMemOperand(subject, HeapObject::kMapOffset));
  __ lbu(a0, FieldMemOperand(a0, Map::kInstanceTypeOffset));
  // Is first part a flat string?
  ASSERT_EQ(0, kSeqStringTag);
  __ And(at, a0, Operand(kStringRepresentationMask));
  __ Branch(&runtime, ne, at, Operand(zero_reg));

  __ bind(&seq_string);
  // subject: Subject string
  // regexp_data: RegExp data (FixedArray)
  // a0: Instance type of subject string
  ASSERT_EQ(4, kStringEncodingMask);
  ASSERT_EQ(4, kAsciiStringTag);
  ASSERT_EQ(0, kTwoByteStringTag);
  // Find the code object based on the assumptions above.
  __ And(a0, a0, Operand(kStringEncodingMask));  // Non-zero for ascii.
  __ lw(t9, FieldMemOperand(regexp_data, JSRegExp::kDataAsciiCodeOffset));
  __ sra(a3, a0, 2);  // a3 is 1 for ascii, 0 for UC16 (used below).
  __ lw(t0, FieldMemOperand(regexp_data, JSRegExp::kDataUC16CodeOffset));
  __ movz(t9, t0, a0);  // If UC16 (a0 is 0), replace t9 w/kDataUC16CodeOffset.

  // Check that the irregexp code has been generated for the actual string
  // encoding. If it has, the field contains a code object otherwise it
  // contains the hole.
  __ GetObjectType(t9, a0, a0);
  __ Branch(&runtime, ne, a0, Operand(CODE_TYPE));

  // a3: encoding of subject string (1 if ascii, 0 if two_byte);
  // t9: code
  // subject: Subject string
  // regexp_data: RegExp data (FixedArray)
  // Load used arguments before starting to push arguments for call to native
  // RegExp code to avoid handling changing stack height.
  __ lw(a1, MemOperand(sp, kPreviousIndexOffset));
  __ sra(a1, a1, kSmiTagSize);  // Untag the Smi.

  // a1: previous index
  // a3: encoding of subject string (1 if ascii, 0 if two_byte);
  // t9: code
  // subject: Subject string
  // regexp_data: RegExp data (FixedArray)
  // All checks done. Now push arguments for native regexp code.
  __ IncrementCounter(&Counters::regexp_entry_native, 1, a0, a2);

  static const int kRegExpExecuteArguments = 7;
  __ Push(ra);
  __ PrepareCallCFunction(kRegExpExecuteArguments, a0);

  // Argument 7: Indicate that this is a direct call from JavaScript.
  // kCFuncArg_N take MIPS stack argument slots into account.
  __ li(a0, Operand(1));
  __ sw(a0, MemOperand(sp, MacroAssembler::kCFuncArg_7));

  // Argument 6: Start (high end) of backtracking stack memory area.
  __ li(a0, Operand(address_of_regexp_stack_memory_address));
  __ lw(a0, MemOperand(a0, 0));
  __ li(a2, Operand(address_of_regexp_stack_memory_size));
  __ lw(a2, MemOperand(a2, 0));
  __ addu(a0, a0, a2);
  __ sw(a0, MemOperand(sp, MacroAssembler::kCFuncArg_6));

  // Argument 5: static offsets vector buffer.
  __ li(a0, Operand(ExternalReference::address_of_static_offsets_vector()));
  __ sw(a0, MemOperand(sp, MacroAssembler::kCFuncArg_5));

  // For arguments 4 and 3 get string length, calculate start of string data and
  // calculate the shift of the index (0 for ASCII and 1 for two byte).
  __ lw(a0, FieldMemOperand(subject, String::kLengthOffset));
  __ sra(a0, a0, kSmiTagSize);
  ASSERT_EQ(SeqAsciiString::kHeaderSize, SeqTwoByteString::kHeaderSize);
  __ Addu(t0, subject, Operand(SeqAsciiString::kHeaderSize - kHeapObjectTag));
  __ Xor(a3, a3, Operand(1));  // 1 for 2-byte str, 0 for 1-byte.
  // Argument 4 (a3): End of string data
  // Argument 3 (a2): Start of string data
  __ sllv(t1, a1, a3);
  __ addu(a2, t0, t1);
  __ sllv(t1, a0, a3);
  __ addu(a3, t0, t1);

  // Argument 2 (a1): Previous index.
  // Already there

  // Argument 1 (a0): Subject string.
  __ mov(a0, subject);

  // Locate the code entry and call it.
  __ Addu(t9, t9, Operand(Code::kHeaderSize - kHeapObjectTag));
  __ CallCFunction(t9, kRegExpExecuteArguments);
  __ Pop(ra);

  // v0: result
  // subject: subject string (callee saved)
  // regexp_data: RegExp data (callee saved)
  // last_match_info_elements: Last match info elements (callee saved)

  // Check the result.
  Label success;
  __ Branch(&success, eq, v0, Operand(NativeRegExpMacroAssembler::SUCCESS));
  Label failure;
  __ Branch(&failure, eq, v0, Operand(NativeRegExpMacroAssembler::FAILURE));
  // If not exception it can only be retry. Handle that in the runtime system.
  __ Branch(&runtime, ne, v0, Operand(NativeRegExpMacroAssembler::EXCEPTION));
  // Result must now be exception. If there is no pending exception already a
  // stack overflow (on the backtrack stack) was detected in RegExp code but
  // haven't created the exception yet. Handle that in the runtime system.
  // TODO(592): Rerunning the RegExp to get the stack overflow exception.
  __ li(a0, Operand(ExternalReference::the_hole_value_location()));
  __ lw(a0, MemOperand(a0, 0));
  __ li(a1, Operand(ExternalReference(Top::k_pending_exception_address)));
  __ lw(a1, MemOperand(a1, 0));
  __ Branch(&runtime, eq, a0, Operand(a1));
  __ bind(&failure);
  // For failure and exception return null.
  __ li(v0, Operand(Factory::null_value()));
  __ Addu(sp, sp, Operand(4 * kPointerSize));
  __ Ret();

  // Process the result from the native regexp code.
  __ bind(&success);
  __ lw(a1,
         FieldMemOperand(regexp_data, JSRegExp::kIrregexpCaptureCountOffset));
  // Calculate number of capture registers (number_of_captures + 1) * 2.
  ASSERT_EQ(0, kSmiTag);
  ASSERT_EQ(1, kSmiTagSize + kSmiShiftSize);
  __ Addu(a1, a1, Operand(2));  // a1 was a smi.

  // a1: number of capture registers
  // subject: subject string
  // Store the capture count.
  __ sll(a2, a1, kSmiTagSize + kSmiShiftSize);  // To smi.
  __ sw(a2, FieldMemOperand(last_match_info_elements,
                             RegExpImpl::kLastCaptureCountOffset));
  // Store last subject and last input.
  __ mov(a3, last_match_info_elements);  // Moved up to reduce latency.
  __ sw(subject,
         FieldMemOperand(last_match_info_elements,
                         RegExpImpl::kLastSubjectOffset));
  __ RecordWrite(a3, Operand(RegExpImpl::kLastSubjectOffset), a2, t0);
  __ sw(subject,
         FieldMemOperand(last_match_info_elements,
                         RegExpImpl::kLastInputOffset));
  __ mov(a3, last_match_info_elements);
  __ RecordWrite(a3, Operand(RegExpImpl::kLastInputOffset), a2, t0);

  // Get the static offsets vector filled by the native regexp code.
  ExternalReference address_of_static_offsets_vector =
      ExternalReference::address_of_static_offsets_vector();
  __ li(a2, Operand(address_of_static_offsets_vector));

  // a1: number of capture registers
  // a2: offsets vector
  Label next_capture, done;
  // Capture register counter starts from number of capture registers and
  // counts down until wrapping after zero.
  __ Addu(a0,
         last_match_info_elements,
         Operand(RegExpImpl::kFirstCaptureOffset - kHeapObjectTag));
  __ bind(&next_capture);
  __ Subu(a1, a1, Operand(1));
  __ Branch(&done, lt, a1, Operand(zero_reg));
  // Read the value from the static offsets vector buffer.
  __ lw(a3, MemOperand(a2, 0));
  __ addiu(a2, a2, kPointerSize);
  // Store the smi value in the last match info.
  __ sll(a3, a3, kSmiTagSize);  // Convert to Smi.
  __ sw(a3, MemOperand(a0, 0));
  __ Branch(&next_capture, false);  // Use branch delay slot.
  __ addiu(a0, a0, kPointerSize);   // In branch delay slot.

  __ bind(&done);

  // Return last match info.
  __ lw(v0, MemOperand(sp, kLastMatchInfoOffset));
  __ Addu(sp, sp, Operand(4 * kPointerSize));
  __ Ret();

  // Do the runtime call to execute the regexp.
  __ bind(&runtime);
  __ TailCallRuntime(Runtime::kRegExpExec, 4, 1);
#endif  // V8_INTERPRETED_REGEXP
}


void ArgumentsAccessStub::GenerateNewObject(MacroAssembler* masm) {
  // sp[0] : number of parameters
  // sp[4] : receiver displacement
  // sp[8] : function

  // Check if the calling frame is an arguments adaptor frame.
  Label adaptor_frame, try_allocate, runtime;
  __ lw(a2, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ lw(a3, MemOperand(a2, StandardFrameConstants::kContextOffset));
  __ Branch(&adaptor_frame,
            eq,
            a3,
            Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));

  // Get the length from the frame.
  __ lw(a1, MemOperand(sp, 0));
  __ Branch(&try_allocate, al);

  // Patch the arguments.length and the parameters pointer.
  __ bind(&adaptor_frame);
  __ lw(a1, MemOperand(a2, ArgumentsAdaptorFrameConstants::kLengthOffset));
  __ sw(a1, MemOperand(sp, 0));
  __ sll(at, a1, kPointerSizeLog2 - kSmiTagSize);
  __ Addu(a3, a2, Operand(at));

  __ Addu(a3, a3, Operand(StandardFrameConstants::kCallerSPOffset));
  __ sw(a3, MemOperand(sp, 1 * kPointerSize));

  // Try the new space allocation. Start out with computing the size
  // of the arguments object and the elements array in words.
  Label add_arguments_object;
  __ bind(&try_allocate);
  __ Branch(&add_arguments_object, eq, a1, Operand(0));
  __ srl(a1, a1, kSmiTagSize);

  __ Addu(a1, a1, Operand(FixedArray::kHeaderSize / kPointerSize));
  __ bind(&add_arguments_object);
  __ Addu(a1, a1, Operand(Heap::kArgumentsObjectSize / kPointerSize));

  // Do the allocation of both objects in one go.
  __ AllocateInNewSpace(
      a1,
      v0,
      a2,
      a3,
      &runtime,
      static_cast<AllocationFlags>(TAG_OBJECT | SIZE_IN_WORDS));

  // Get the arguments boilerplate from the current (global) context.
  int offset = Context::SlotOffset(Context::ARGUMENTS_BOILERPLATE_INDEX);
  __ lw(t0, MemOperand(cp, Context::SlotOffset(Context::GLOBAL_INDEX)));
  __ lw(t0, FieldMemOperand(t0, GlobalObject::kGlobalContextOffset));
  __ lw(t0, MemOperand(t0, offset));

  // Copy the JS object part.
  for (int i = 0; i < JSObject::kHeaderSize; i += kPointerSize) {
    __ lw(a3, FieldMemOperand(t0, i));
    __ sw(a3, FieldMemOperand(v0, i));
  }

  // Setup the callee in-object property.
  ASSERT(Heap::arguments_callee_index == 0);
  __ lw(a3, MemOperand(sp, 2 * kPointerSize));
  __ sw(a3, FieldMemOperand(v0, JSObject::kHeaderSize));

  // Get the length (smi tagged) and set that as an in-object property too.
  ASSERT(Heap::arguments_length_index == 1);
  __ lw(a1, MemOperand(sp, 0 * kPointerSize));
  __ sw(a1, FieldMemOperand(v0, JSObject::kHeaderSize + kPointerSize));

  Label done;
  __ Branch(&done, eq, a1, Operand(0));

  // Get the parameters pointer from the stack.
  __ lw(a2, MemOperand(sp, 1 * kPointerSize));

  // Setup the elements pointer in the allocated arguments object and
  // initialize the header in the elements fixed array.
  __ Addu(t0, v0, Operand(Heap::kArgumentsObjectSize));
  __ sw(t0, FieldMemOperand(v0, JSObject::kElementsOffset));
  __ LoadRoot(a3, Heap::kFixedArrayMapRootIndex);
  __ sw(a3, FieldMemOperand(t0, FixedArray::kMapOffset));
  __ sw(a1, FieldMemOperand(t0, FixedArray::kLengthOffset));
  __ srl(a1, a1, kSmiTagSize);  // Untag the length for the loop.

  // Copy the fixed array slots.
  Label loop;
  // Setup t0 to point to the first array slot.
  __ Addu(t0, t0, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  __ bind(&loop);
  // Pre-decrement a2 with kPointerSize on each iteration.
  // Pre-decrement in order to skip receiver.
  __ Addu(a2, a2, Operand(-kPointerSize));
  __ lw(a3, MemOperand(a2));
  // Post-increment t0 with kPointerSize on each iteration.
  __ sw(a3, MemOperand(t0));
  __ Addu(t0, t0, Operand(kPointerSize));
  __ Subu(a1, a1, Operand(1));
  __ Branch(&loop, ne, a1, Operand(0));

  // Return and remove the on-stack parameters.
  __ bind(&done);
  __ Addu(sp, sp, Operand(3 * kPointerSize));
  __ Ret();

  // Do the runtime call to allocate the arguments object.
  __ bind(&runtime);
  __ TailCallRuntime(Runtime::kNewArgumentsFast, 3, 1);
}


void CallFunctionStub::Generate(MacroAssembler* masm) {
  Label slow;

  // If the receiver might be a value (string, number or boolean) check
  // for this and box it if it is.
  if (ReceiverMightBeValue()) {
    // Get the receiver from the stack.
    // function, receiver [, arguments]
    Label receiver_is_value, receiver_is_js_object;
    __ lw(a1, MemOperand(sp, argc_ * kPointerSize));

    // Check if receiver is a smi (which is a number value).
    __ BranchOnSmi(a1, &receiver_is_value);

    // Check if the receiver is a valid JS object.
    __ GetObjectType(a1, a2, a2);
    __ Branch(&receiver_is_js_object,
              ge,
              a2,
              Operand(FIRST_JS_OBJECT_TYPE));

    // Call the runtime to box the value.
    __ bind(&receiver_is_value);
    // We need natives to execute this.
    __ EnterInternalFrame();
    __ Push(a1);
    __ InvokeBuiltin(Builtins::TO_OBJECT, CALL_JS);
    __ LeaveInternalFrame();
    __ sw(v0, MemOperand(sp, argc_ * kPointerSize));

    __ bind(&receiver_is_js_object);
  }

  // Get the function to call from the stack.
  // function, receiver [, arguments]
  __ lw(a1, MemOperand(sp, (argc_ + 1) * kPointerSize));

  // Check that the function is really a JavaScript function.
  // a1: pushed function (to be verified)
  __ BranchOnSmi(a1, &slow);
  // Get the map of the function object.
  __ GetObjectType(a1, a2, a2);
  __ Branch(&slow, ne, a2, Operand(JS_FUNCTION_TYPE));

  // Fast-case: Invoke the function now.
  // a1: pushed function
  ParameterCount actual(argc_);
  __ InvokeFunction(a1, actual, JUMP_FUNCTION);

  // Slow-case: Non-function called.
  __ bind(&slow);
  // CALL_NON_FUNCTION expects the non-function callee as receiver (instead
  // of the original receiver from the call site).
  __ sw(a1, MemOperand(sp, argc_ * kPointerSize));
  __ li(a0, Operand(argc_));  // Setup the number of arguments.
  __ mov(a2, zero_reg);
  __ GetBuiltinEntry(a3, Builtins::CALL_NON_FUNCTION);
  __ Jump(Handle<Code>(Builtins::builtin(Builtins::ArgumentsAdaptorTrampoline)),
          RelocInfo::CODE_TARGET);
}


const char* CompareStub::GetName() {
  switch (cc_) {
    case lt: return "CompareStub_LT";
    case gt: return "CompareStub_GT";
    case le: return "CompareStub_LE";
    case ge: return "CompareStub_GE";
    case ne: {
      if (strict_) {
        if (never_nan_nan_) {
          return "CompareStub_NE_STRICT_NO_NAN";
        } else {
          return "CompareStub_NE_STRICT";
        }
      } else {
        if (never_nan_nan_) {
          return "CompareStub_NE_NO_NAN";
        } else {
          return "CompareStub_NE";
        }
      }
    }
    case eq: {
      if (strict_) {
        if (never_nan_nan_) {
          return "CompareStub_EQ_STRICT_NO_NAN";
        } else {
          return "CompareStub_EQ_STRICT";
        }
      } else {
        if (never_nan_nan_) {
          return "CompareStub_EQ_NO_NAN";
        } else {
          return "CompareStub_EQ";
        }
      }
    }
    default: return "CompareStub";
  }
}


int CompareStub::MinorKey() {
  // Encode the two parameters in a unique 16 bit value.
  ASSERT(static_cast<unsigned>(cc_) < (1 << 14));
  return ConditionField::encode(static_cast<unsigned>(cc_))
         | StrictField::encode(strict_)
         | NeverNanNanField::encode(cc_ == eq ? never_nan_nan_ : false);
}


// We fall into this code if the operands were Smis, but the result was
// not (eg. overflow).  We branch into this code (to the not_smi label) if
// the operands were not both Smi.  The operands are in a1 (x) and a0 (y).
// To call the C-implemented binary fp operation routines we need to end up
// with the double precision floating point operands in a0 and a1 (for the
// value in a1) and a2 and a3 (for the value in a0).

void GenericBinaryOpStub::HandleBinaryOpSlowCases(MacroAssembler* masm,
                                    Label* not_smi,
                                    Register lhs,
                                    Register rhs,
                                    const Builtins::JavaScript& builtin) {
  Label slow, slow_reverse, do_the_call;
  Label a0_is_smi, a1_is_smi, finished_loading_a0, finished_loading_a1;
  bool use_fp_registers = CpuFeatures::IsSupported(FPU);
  if (ShouldGenerateSmiCode()) {
    if (op_ == Token::MOD || op_ == Token::DIV) {
    // If the divisor is zero for MOD or DIV, go to
    // the builtin code to return NaN.
     __ Branch(lhs.is(a0) ? &slow_reverse : &slow, eq, rhs, Operand(zero_reg));
    }

    // Smi-smi case (overflow).
    // Since both are Smis there is no heap number to overwrite, so allocate.
    // The new heap number is in t0. t1 and t2 are scratch.
    __ AllocateHeapNumber(t0, t1, t2, lhs.is(a0) ? &slow_reverse : &slow);

    // If we have floating point hardware, inline ADD, SUB, MUL, and DIV,
    // using registers f12 and f14 for the double values.

    bool use_fp_registers = CpuFeatures::IsSupported(FPU);

     if (use_fp_registers) {
      CpuFeatures::Scope scope(FPU);
      // Convert a1 (x) to double in f12
      __ sra(t2, lhs, kSmiTagSize);
      __ mtc1(t2, f12);
      __ cvt_d_w(f12, f12);

      // Convert a0 (y) to double in f14
      __ sra(t2, rhs, kSmiTagSize);
      __ mtc1(t2, f14);
      __ cvt_d_w(f14, f14);

    } else {
      // Write Smi from a0 to a3 and a2 in double format. t1 is scratch.
      ConvertToDoubleStub stub1(a3, a2, rhs, t1);
      __ Push(ra);
      __ Call(stub1.GetCode(), RelocInfo::CODE_TARGET);

      // Write Smi from a1 to a1 and a0 in double format. t1 is scratch.
      // Needs a1 in temp (t2); cannot use same reg for src & dest.
      __ mov(t2, lhs);
      ConvertToDoubleStub stub2(a1, a0, t2, t1);
      __ Call(stub2.GetCode(), RelocInfo::CODE_TARGET);
      __ Pop(ra);
    }
    __ jmp(&do_the_call);  // Tail call.  No return.
  }

  // We branch here if at least one of a0 and a1 is not a Smi.
  __ bind(not_smi);

  // After this point we have the left hand side in a1 and the right hand side
  // in a0.
  Register scratch  = VirtualFrame::scratch0();
  if (lhs.is(a0)) {
    __ Swap(a0, a1, scratch);
  }

  if (ShouldGenerateFPCode()) {
    if (runtime_operands_type_ == BinaryOpIC::DEFAULT) {
      switch (op_) {
        case Token::ADD:
        case Token::SUB:
        case Token::MUL:
        case Token::DIV:
          GenerateTypeTransition(masm);
          break;

        default:
          break;
      }
    }

    if (mode_ == NO_OVERWRITE) {
      // In the case where there is no chance of an overwritable float we may as
      // well do the allocation immediately while a0 and a1 are untouched.
      __ AllocateHeapNumber(t0, t1, t2, &slow);
    }

    // Move a0 (y) to a double in a2-a3.
    __ And(t1, a0, Operand(kSmiTagMask));
    // If it is an Smi, don't check if it is a heap number.
    __ Branch(&a0_is_smi, eq, t1, Operand(zero_reg));
    __ GetObjectType(a0, t1, t1);
    __ Branch(&slow, ne, t1, Operand(HEAP_NUMBER_TYPE));

    if (mode_ == OVERWRITE_RIGHT) {
      __ mov(t0, a0);  // Overwrite this heap number.
    }
    if (use_fp_registers) {
      CpuFeatures::Scope scope(FPU);
      // Load the double from tagged HeapNumber a1 to f14.
      __ Subu(t1, a0, Operand(kHeapObjectTag));
      __ ldc1(f14, MemOperand(t1, HeapNumber::kValueOffset));
    } else {
      // Calling convention says that 'right' double (x) is in a2 and a3.
      __ lw(a2, FieldMemOperand(a0, HeapNumber::kValueOffset));
      __ lw(a3, FieldMemOperand(a0, HeapNumber::kValueOffset + 4));
    }
    __ jmp(&finished_loading_a0);
    __ bind(&a0_is_smi);
    if (mode_ == OVERWRITE_RIGHT) {
      // We can't overwrite a Smi so get address of new heap number into t0.
      __ AllocateHeapNumber(t0, t1, t2, &slow);
    }

    if (use_fp_registers) {
      CpuFeatures::Scope scope(FPU);
      // Convert smi in a0 to double in f14.
      __ sra(t2, a0, kSmiTagSize);
      __ mtc1(t2, f14);
      __ cvt_d_w(f14, f14);
    } else {
      // Write Smi from a0 to a3 and a2 in double format.
      __ mov(t1, a0);
      ConvertToDoubleStub stub3(a3, a2, t1, t2);
      __ Push(ra);
      __ Call(stub3.GetCode(), RelocInfo::CODE_TARGET);
      __ Pop(ra);
    }

    // HEAP_NUMBERS stub is slower than GENERIC on a pair of smis.
    // a0 is known to be a smi. If a1 is also a smi then switch to GENERIC.
    Label a1_is_not_smi;
    if (runtime_operands_type_ == BinaryOpIC::HEAP_NUMBERS) {
      __ And(at, a1, Operand(kSmiTagMask));
      __ Branch(&a1_is_not_smi, ne, at, Operand(zero_reg));
      GenerateTypeTransition(masm);
      __ jmp(&a1_is_smi);
    }

    __ bind(&finished_loading_a0);

    // Move a1 (x) to a double in a0-a1.
    __ And(t1, a1, Operand(kSmiTagMask));
    // If it is an Smi, don't check if it is a heap number.
    __ Branch(&a1_is_smi, eq, t1, Operand(zero_reg));
    __ bind(&a1_is_not_smi);
    __ GetObjectType(a1, t1, t1);
    __ Branch(&slow, ne, t1, Operand(HEAP_NUMBER_TYPE));
    if (mode_ == OVERWRITE_LEFT) {
      __ mov(t0, a1);  // Overwrite this heap number.
    }
    if (use_fp_registers) {
      CpuFeatures::Scope scope(FPU);
      // Load the double from tagged HeapNumber a1 to f12.
      __ Subu(t1, a1, Operand(kHeapObjectTag));
      __ ldc1(f12, MemOperand(t1, HeapNumber::kValueOffset));
    } else {
      __ lw(a0, FieldMemOperand(a1, HeapNumber::kValueOffset));
      __ lw(a1, FieldMemOperand(a1, HeapNumber::kValueOffset + 4));
    }
    __ jmp(&finished_loading_a1);
    __ bind(&a1_is_smi);
    if (mode_ == OVERWRITE_LEFT) {
      // We can't overwrite a Smi so get address of new heap number into t0.
      __ AllocateHeapNumber(t0, t1, t2, &slow);
    }

    if (use_fp_registers) {
      CpuFeatures::Scope scope(FPU);
      // Convert smi in a1 to double in f12.
      __ sra(t2, a1, kSmiTagSize);
      __ mtc1(t2, f12);
      __ cvt_d_w(f12, f12);

    } else {
      // Write Smi from a1 to a0 and a1 in double format.
      __ mov(t1, a1);
      ConvertToDoubleStub stub4(a1, a0, t1, t2);
      __ Push(ra);
      __ Call(stub4.GetCode(), RelocInfo::CODE_TARGET);
      __ Pop(ra);
    }

    __ bind(&finished_loading_a1);

    __ bind(&do_the_call);
    // If we are inlining the operation using FPU instructions for
    // add, subtract, multiply, or divide, the arguments are in f12 and f14.
    if (use_fp_registers) {
      CpuFeatures::Scope scope(FPU);
      // MIPS FPU instructions to implement
      // double precision, add, subtract, multiply, divide.
      if (Token::MUL == op_) {
        __ mul_d(f0, f12, f14);
      } else if (Token::DIV == op_) {
        __ div_d(f0, f12, f14);
      } else if (Token::ADD == op_) {
        __ add_d(f0, f12, f14);
      } else if (Token::SUB == op_) {
        __ sub_d(f0, f12, f14);
      } else if (Token::MOD == op_) {
        // This is special case because only here we go to run-time
        // while using FPU registers. Current ABI must be taken into account.
        __ Push(ra);
        __ Push(t0);
        __ PrepareCallCFunction(4, t1);  // Two doubles count as 4 arguments.
        if (IsMipsSoftFloatABI) {
          // We are using MIPS FPU instructions, and parameters for the
          // run-time function call are prepared in f12/f14 register pairs,
          // but function we are calling is compiled with soft-float flag and
          // expecting soft float ABI (parameters in a0-a3 registers). We need
          // to copy parameters from f12/f14 register pairs to a0-a3 registers.
          __ mfc1(a0, f12);
          __ mfc1(a1, f13);
          __ mfc1(a2, f14);
          __ mfc1(a3, f15);
        }
        __ CallCFunction(ExternalReference::double_fp_operation(op_), 4);
        __ Pop(t0);  // Address of heap number.
        __ Pop(ra);
        if (IsMipsSoftFloatABI) {
          // Store answer in the overwritable heap number.
          // Double returned is stored in registers v0 and v1 (function we
          // called is compiled with soft-float flag and uses soft-float ABI).
          __ sw(v0, FieldMemOperand(t0, HeapNumber::kValueOffset));
          __ sw(v1, FieldMemOperand(t0, HeapNumber::kValueOffset + 4));
          __ mov(v0, t0);  // Return object ptr to caller.
          __ Ret();
        }
      } else {
        UNREACHABLE();
      }
      __ Subu(v0, t0, Operand(kHeapObjectTag));
      __ sdc1(f0, MemOperand(v0, HeapNumber::kValueOffset));
      __ Addu(v0, v0, Operand(kHeapObjectTag));
      __ Ret();
    } else {
      // If we did not inline the operation, then the arguments are in:
      // a0: Left value (least significant part of mantissa).
      // a1: Left value (sign, exponent, top of mantissa).
      // a2: Right value (least significant part of mantissa).
      // a3: Right value (sign, exponent, top of mantissa).
      // t0: Address of heap number for result.

      __ Push(ra);
      __ Push(t0);    // Address of heap number to store the answer.

      __ PrepareCallCFunction(4, t1);  // Two doubles count as 4 arguments.
      // Call C routine that may not cause GC or other trouble.
      if (!IsMipsSoftFloatABI) {
        if (!use_fp_registers) {
          // We are not using MIPS FPU instructions, and parameters for the
          // run-time function call are prepared in a0-a3 registers, but the
          // function we are calling is compiled with hard-float flag and
          // expecting hard float ABI (parameters in f12/f14 registers).
          // We need to copy parameters from a0-a3 registers to f12/f14
          // register pairs.
          __ mtc1(a0, f12);
          __ mtc1(a1, f13);
          __ mtc1(a2, f14);
          __ mtc1(a3, f15);
        }
      }

      __ CallCFunction(ExternalReference::double_fp_operation(op_), 4);

      if (!IsMipsSoftFloatABI) {
        if (!use_fp_registers) {
          // Returned double value is stored in registers f0 and f1.
          // Function we called is compiled with hard-float flag and uses
          // hard-float ABI. Return value in the case when we are not using
          // MIPS FPU instructions has to be placed in v0/v1, so we need to
          // copy from f0/f1.
          __ mfc1(v0, f0);
          __ mfc1(v1, f1);
        }
      }
      __ Pop(t0);  // Address of heap number.
      // Store answer in the overwritable heap number.

      // Double returned in registers v0 and v1.
      __ sw(v0, FieldMemOperand(t0, HeapNumber::kValueOffset));
      __ sw(v1, FieldMemOperand(t0, HeapNumber::kValueOffset + 4));
      __ mov(v0, t0);  // Return object ptr to caller.

      // And we are done.
      __ Pop(ra);
      __ Ret();
    }
  }

  if (lhs.is(a0)) {
    __ b(&slow);
    __ bind(&slow_reverse);
    __ Swap(a0, a1, scratch);
  }
  // We jump to here if something goes wrong (one param is not a number of any
  // sort or new-space allocation fails).
  __ bind(&slow);

  // Push arguments to the stack
  __ Push(a1, a0);

  if (Token::ADD == op_) {
    // Test for string arguments before calling runtime.
    // a1 : first argument
    // a0 : second argument
    // sp[0] : second argument
    // sp[4] : first argument

    Label not_strings, not_string1, string1, string1_smi2;
    __ And(t0, a1, Operand(kSmiTagMask));
    __ Branch(&not_string1, eq, t0, Operand(zero_reg));

    __ GetObjectType(a1, t0, t0);
    __ Branch(&not_string1, ge, t0, Operand(FIRST_NONSTRING_TYPE));

    // First argument is a a string, test second.
    __ And(t0, a0, Operand(kSmiTagMask));
    __ Branch(&string1_smi2, eq, t0, Operand(zero_reg));

    __ GetObjectType(a0, t0, t0);
    __ Branch(&string1, ge, t0, Operand(FIRST_NONSTRING_TYPE));

    // First and second argument are strings.
    StringAddStub string_add_stub(NO_STRING_CHECK_IN_STUB);
    __ TailCallStub(&string_add_stub);

    __ bind(&string1_smi2);
    NumberToStringStub::GenerateLookupNumberStringCache(
        masm, a0, a2, t0, t1, t2, true, &string1);

    // Replace second argument on stack and tailcall string add stub to make
    // the result.
    __ sw(a2, MemOperand(sp, 0));
    __ TailCallStub(&string_add_stub);

    // Only first argument is a string.
    __ bind(&string1);
    __ InvokeBuiltin(Builtins::STRING_ADD_LEFT, JUMP_JS);

    // First argument was not a string, test second.
    __ bind(&not_string1);
    __ And(t0, a0, Operand(kSmiTagMask));
    __ Branch(&not_strings, eq, t0, Operand(zero_reg));

    __ GetObjectType(a0, t0, t0);
    __ Branch(&not_strings, ge, t0, Operand(FIRST_NONSTRING_TYPE));

    // Only second argument is a string.
    __ InvokeBuiltin(Builtins::STRING_ADD_RIGHT, JUMP_JS);

    __ bind(&not_strings);
  }
  __ InvokeBuiltin(builtin, JUMP_JS);  // Tail call.  No return.
}


// Tries to get a signed int32 out of a double precision floating point heap
// number.  Rounds towards 0.  Fastest for doubles that are in the ranges
// -0x7fffffff to -0x40000000 or 0x40000000 to 0x7fffffff.  This corresponds
// almost to the range of signed int32 values that are not Smis.  Jumps to the
// label 'slow' if the double isn't in the range -0x80000000.0 to 0x80000000.0
// (excluding the endpoints).
static void GetInt32(MacroAssembler* masm,
                     Register source,
                     Register dest,
                     Register scratch,
                     Register scratch2,
                     Label* slow) {
  Label right_exponent, done;
  // Get exponent word (ENDIAN issues).
  __ lw(scratch, FieldMemOperand(source, HeapNumber::kExponentOffset));
  // Get exponent alone in scratch2.
  __ And(scratch2, scratch, Operand(HeapNumber::kExponentMask));
  // Load dest with zero.  We use this either for the final shift or
  // for the answer.
  __ mov(dest, zero_reg);
  // Check whether the exponent matches a 32 bit signed int that is not a Smi.
  // A non-Smi integer is 1.xxx * 2^30 so the exponent is 30 (biased).  This is
  // the exponent that we are fastest at and also the highest exponent we can
  // handle here.
  const uint32_t non_smi_exponent =
      (HeapNumber::kExponentBias + 30) << HeapNumber::kExponentShift;
  // If we have a match of the int32-but-not-Smi exponent then skip some logic.
  __ Branch(&right_exponent, eq, scratch2, Operand(non_smi_exponent));
  // If the exponent is higher than that then go to slow case.  This catches
  // numbers that don't fit in a signed int32, infinities and NaNs.
  __ Branch(slow, gt, scratch2, Operand(non_smi_exponent));

  // We know the exponent is smaller than 30 (biased).  If it is less than
  // 0 (biased) then the number is smaller in magnitude than 1.0 * 2^0, ie
  // it rounds to zero.
  const uint32_t zero_exponent =
      (HeapNumber::kExponentBias + 0) << HeapNumber::kExponentShift;
  __ Subu(scratch2, scratch2, Operand(zero_exponent));
  // Dest already has a Smi zero.
  __ Branch(&done, lt, scratch2, Operand(zero_reg));
  if (!CpuFeatures::IsSupported(FPU)) {
    // We have a shifted exponent between 0 and 30 in scratch2.
    __ srl(dest, scratch2, HeapNumber::kExponentShift);
    // We now have the exponent in dest.  Subtract from 30 to get
    // how much to shift down.
    __ li(at, Operand(30));
    __ subu(dest, at, dest);
  }
  __ bind(&right_exponent);
  if (CpuFeatures::IsSupported(FPU)) {
    CpuFeatures::Scope scope(FPU);
    // MIPS FPU instructions implementing double precision to integer
    // conversion using round to zero. Since the FP value was qualified
    // above, the resulting integer should be a legal int32.
    // The original 'Exponent' word is still in scratch.
    __ lwc1(f12, FieldMemOperand(source, HeapNumber::kMantissaOffset));
    __ mtc1(scratch, f13);
    __ trunc_w_d(f0, f12);
    __ mfc1(dest, f0);
  } else {
    // On entry, dest has final downshift, scratch has original sign/exp/mant.
    // Save sign bit in top bit of dest.
    __ And(scratch2, scratch, Operand(0x80000000));
    __ Or(dest, dest, Operand(scratch2));
    // Put back the implicit 1, just above mantissa field.
    __ Or(scratch, scratch, Operand(1 << HeapNumber::kExponentShift));

    // Shift up the mantissa bits to take up the space the exponent used to
    // take. We just orred in the implicit bit so that took care of one and
    // we want to leave the sign bit 0 so we subtract 2 bits from the shift
    // distance. But we want to clear the sign-bit so shift one more bit
    // left, then shift right one bit.
    const int shift_distance = HeapNumber::kNonMantissaBitsInTopWord - 2;
    __ sll(scratch, scratch, shift_distance + 1);
    __ srl(scratch, scratch, 1);

    // Get the second half of the double. For some exponents we don't
    // actually need this because the bits get shifted out again, but
    // it's probably slower to test than just to do it.
    __ lw(scratch2, FieldMemOperand(source, HeapNumber::kMantissaOffset));
    // Extract the top 10 bits, and insert those bottom 10 bits of scratch.
    // The width of the field here is the same as the shift amount above.
    const int field_width = shift_distance;
    __ Ext(scratch2, scratch2, 32-shift_distance, field_width);
    __ Ins(scratch, scratch2, 0, field_width);
    // Move down according to the exponent.
    __ srlv(scratch, scratch, dest);
    // Prepare the negative version of our integer.
    __ subu(scratch2, zero_reg, scratch);
    // Trick to check sign bit (msb) held in dest, count leading zero.
    // 0 indicates negative, save negative version with conditional move.
    __ clz(dest, dest);
    __ movz(scratch, scratch2, dest);
    __ mov(dest, scratch);
  }
  __ bind(&done);
}



// For bitwise ops where the inputs are not both Smis we here try to determine
// whether both inputs are either Smis or at least heap numbers that can be
// represented by a 32 bit signed value.  We truncate towards zero as required
// by the ES spec.  If this is the case we do the bitwise op and see if the
// result is a Smi.  If so, great, otherwise we try to find a heap number to
// write the answer into (either by allocating or by overwriting).
// On entry the operands are in lhs (x) and rhs (y). (Result = x op y).
// On exit the result is in v0.
void GenericBinaryOpStub::HandleNonSmiBitwiseOp(MacroAssembler* masm,
                                                Register lhs,
                                                Register rhs) {
  Label slow, result_not_a_smi;
  Label rhs_is_smi, lhs_is_smi;
  Label done_checking_rhs, done_checking_lhs;

  __ And(t1, lhs, Operand(kSmiTagMask));
  __ Branch(&lhs_is_smi, eq, t1, Operand(zero_reg));
  __ GetObjectType(lhs, t4, t4);
  __ Branch(&slow, ne, t4, Operand(HEAP_NUMBER_TYPE));
  GetInt32(masm, lhs, a3, t2, t3, &slow);  // Convert HeapNum a1 to integer a3.
  __ b(&done_checking_lhs);
  __ nop();   // NOP_ADDED

  __ bind(&lhs_is_smi);
  __ sra(a3, lhs, kSmiTagSize);  // Remove tag from Smi.
  __ bind(&done_checking_lhs);

  __ And(t0, rhs, Operand(kSmiTagMask));
  __ Branch(&rhs_is_smi, eq, t0, Operand(zero_reg));
  __ GetObjectType(rhs, t4, t4);
  __ Branch(&slow, ne, t4, Operand(HEAP_NUMBER_TYPE));
  GetInt32(masm, rhs, a2, t2, t3, &slow);  // Convert HeapNum a0 to integer a2.
  __ b(&done_checking_rhs);
  __ nop();   // NOP_ADDED

  __ bind(&rhs_is_smi);
  __ sra(a2, rhs, kSmiTagSize);  // Remove tag from Smi.
  __ bind(&done_checking_rhs);

  // a1 (x) and a0 (y): Original operands (Smi or heap numbers).
  // a3 (x) and a2 (y): Signed int32 operands.

  switch (op_) {
    case Token::BIT_OR:  __ or_(v1, a3, a2); break;
    case Token::BIT_XOR: __ xor_(v1, a3, a2); break;
    case Token::BIT_AND: __ and_(v1, a3, a2); break;
    case Token::SAR:
      __ srav(v1, a3, a2);
      break;
    case Token::SHR:
      __ srlv(v1, a3, a2);
      // SHR is special because it is required to produce a positive answer.
      // The code below for writing into heap numbers isn't capable of writing
      // the register as an unsigned int so we go to slow case if we hit this
      // case.
      __ And(t3, v1, Operand(0x80000000));
      __ Branch(&slow, ne, t3, Operand(zero_reg));
      break;
    case Token::SHL:
        __ sllv(v1, a3, a2);
      break;
    default: UNREACHABLE();
  }
  // check that the *signed* result fits in a smi
  __ Addu(t3, v1, Operand(0x40000000));
  __ And(t3, t3, Operand(0x80000000));
  __ Branch(&result_not_a_smi, ne, t3, Operand(zero_reg));
  // Smi tag result.
  __ sll(v0, v1, kSmiTagMask);
  __ Ret();

  Label have_to_allocate, got_a_heap_number;
  __ bind(&result_not_a_smi);
  switch (mode_) {
    case OVERWRITE_RIGHT: {
      // t0 has not been changed since  __ andi(t0, a0, Operand(kSmiTagMask));
      __ Branch(&have_to_allocate, eq, t0, Operand(zero_reg));
      __ mov(t5, rhs);
      break;
    }
    case OVERWRITE_LEFT: {
      // t1 has not been changed since  __ andi(t1, a1, Operand(kSmiTagMask));
      __ Branch(&have_to_allocate, eq, t1, Operand(zero_reg));
      __ mov(t5, lhs);
      break;
    }
    case NO_OVERWRITE: {
      // Get a new heap number in t5.  t6 and t7 are scratch.
      __ AllocateHeapNumber(t5, t6, t7, &slow);
    }
    default: break;
  }

  __ bind(&got_a_heap_number);
  // v1: Result as signed int32.
  // t5: Heap number to write answer into.

  // Nothing can go wrong now, so move the heap number to v0, which is the
  // result.
  __ mov(v0, t5);

  // Tail call that writes the int32 in v1 to the heap number in v0, using
  // t0, t1 as scratch.  v0 is preserved and returned by the stub.
  WriteInt32ToHeapNumberStub stub(v1, v0, t0, t1);
  __ Jump(stub.GetCode(), RelocInfo::CODE_TARGET);

  if (mode_ != NO_OVERWRITE) {
    __ bind(&have_to_allocate);
    // Get a new heap number in t5.  t6 and t7 are scratch.
    __ AllocateHeapNumber(t5, t6, t7, &slow);
    __ b(&got_a_heap_number);
    __ nop();   // NOP_ADDED
  }

  // If all else failed then we go to the runtime system.
  __ bind(&slow);

  __ Push(lhs, rhs);  // restore stack
  __ li(rhs, Operand(1));  // 1 argument (not counting receiver).

  switch (op_) {
    case Token::BIT_OR:
      __ InvokeBuiltin(Builtins::BIT_OR, JUMP_JS);
      break;
    case Token::BIT_AND:
      __ InvokeBuiltin(Builtins::BIT_AND, JUMP_JS);
      break;
    case Token::BIT_XOR:
      __ InvokeBuiltin(Builtins::BIT_XOR, JUMP_JS);
      break;
    case Token::SAR:
      __ InvokeBuiltin(Builtins::SAR, JUMP_JS);
      break;
    case Token::SHR:
      __ InvokeBuiltin(Builtins::SHR, JUMP_JS);
      break;
    case Token::SHL:
      __ InvokeBuiltin(Builtins::SHL, JUMP_JS);
      break;
    default:
      UNREACHABLE();
  }
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
               "GenericBinaryOpStub_%s_%s%s_%s",
               op_name,
               overwrite_name,
               specialized_on_rhs_ ? "_ConstantRhs" : "",
               BinaryOpIC::GetName(runtime_operands_type_));
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
  // lhs_ : x
  // rhs_ : y
  // result : v0 = x op y

  Register result = v0;
  Register lhs = lhs_;
  Register rhs = rhs_;

  ASSERT(result.is(v0) &&
          (((lhs.is(a1)) && (rhs.is(a0))) || ((lhs.is(a0)) && (rhs.is(a1)))));

  Register smi_test_reg = VirtualFrame::scratch0();
  Register scratch = VirtualFrame::scratch1();

  // All ops need to know whether we are dealing with two Smis.  Set up t2 to
  // tell us that.
  if (ShouldGenerateSmiCode()) {
    __ Or(smi_test_reg, lhs, Operand(rhs));  // smi_test_reg = x | y;
  }

  switch (op_) {
    case Token::ADD: {
      Label not_smi;
      // Fast path.
      if (ShouldGenerateSmiCode()) {
        ASSERT(kSmiTag == 0);  // Adjust code below.
        __ And(t3, smi_test_reg, Operand(kSmiTagMask));
        __ Branch(&not_smi, ne, t3, Operand(zero_reg));
        __ addu(v0, a1, a0);    // Add y.
        // Check for overflow.
        __ xor_(t0, v0, a0);
        __ xor_(t1, v0, a1);
        __ and_(t0, t0, t1);    // Overflow occurred if result is negative.
        __ Ret(ge, t0, Operand(zero_reg));  // Return on NO overflow (ge 0).
      }
      // Fall thru on overflow, with a0 and a1 preserved.
      HandleBinaryOpSlowCases(masm,
                              &not_smi,
                              lhs,
                              rhs,
                              Builtins::ADD);
      break;
    }

    case Token::SUB: {
      Label not_smi;
      // Fast path.
      if (ShouldGenerateSmiCode()) {
        ASSERT(kSmiTag == 0);  // Adjust code below.
        __ And(t3, smi_test_reg, Operand(kSmiTagMask));
        __ Branch(&not_smi, ne, t3, Operand(zero_reg));
        if (lhs.is(a1)) {
          __ subu(v0, a1, a0);  // Subtract y.
           // Check for overflow of a1 - a0.
          __ xor_(t0, v0, a1);
          __ xor_(t1, a0, a1);
          __ and_(t0, t0, t1);    // Overflow occurred if result is negative.
          __ Ret(ge, t0, Operand(zero_reg));  // Return on NO overflow (ge 0).
        } else {
          __ subu(v0, a0, a1);
           // Check for overflow of a0 - a1.
          __ xor_(t0, v0, a0);
          __ xor_(t1, a0, a1);
          __ and_(t0, t0, t1);    // Overflow occurred if result is negative.
          __ Ret(ge, t0, Operand(zero_reg));  // Return on NO overflow (ge 0).
        }
      }

      // Fall thru on overflow, with a0 and a1 preserved.
      HandleBinaryOpSlowCases(masm,
                              &not_smi,
                              lhs,
                              rhs,
                              Builtins::SUB);
      break;
    }

    case Token::MUL: {
      Label not_smi, slow;
      if (ShouldGenerateSmiCode()) {
        ASSERT(kSmiTag == 0);  // Adjust code below.
        __ And(t3, smi_test_reg, Operand(kSmiTagMask));
        __ Branch(&not_smi, ne, t3, Operand(zero_reg));
        // Remove tag from one operand (but keep sign), so that result is Smi.
        __ sra(scratch, rhs, kSmiTagSize);
        // Do multiplication.
        __ mult(lhs, scratch);
        __ mflo(v0);
        __ mfhi(v1);

        // Go 'slow' on overflow, detected if top 33 bits are not same.
        __ sra(scratch, v0, 31);
        __ Branch(&slow, ne, scratch, Operand(v1));

        // Return if non-zero Smi result.
        __ Ret(ne, v0, Operand(zero_reg));

        // We can return 0, if we multiplied positive number by 0.
        // We know one of them was 0, so sign of sum is sign of other.
        // (note that result of 0 is already in v0, and Smi::FromInt(0) is 0.)
        __ addu(scratch, rhs, lhs);
        __ Ret(gt, scratch, Operand(zero_reg));
        // Else, fall thru to slow case to handle -0

        __ bind(&slow);
      }
      HandleBinaryOpSlowCases(masm,
                              &not_smi,
                              lhs,
                              rhs,
                              Builtins::MUL);
      break;
    }


    case Token::DIV: {
      Label not_smi, slow;
      if (ShouldGenerateSmiCode()) {
        ASSERT(kSmiTag == 0);  // Adjust code below.
        // smi_test_reg = x | y at entry.
        __ And(t3, smi_test_reg, Operand(kSmiTagMask));
        __ Branch(&not_smi, ne, t3, Operand(zero_reg));
        // Remove tags, preserving sign.
        __ sra(t0, rhs, kSmiTagSize);
        __ sra(t1, lhs, kSmiTagSize);
        // Check for divisor of 0.
        __ Branch(&slow, eq, t0, Operand(zero_reg));
        // Divide x by y.
        __ Div(t1, Operand(t0));
        __ mflo(v1);    // Integer (un-tagged) quotient.
        __ mfhi(v0);    // Integer remainder.

        // Go to slow (float) case if remainder is not 0.
        __ Branch(&slow, ne, v0, Operand(zero_reg));

        ASSERT(kSmiTag == 0 && kSmiTagSize == 1);
        __ sll(v0, v1, kSmiTagSize);  // Smi tag return value in v0.

        // Check for the corner case of dividing the most negative smi by -1.
        __ Branch(&slow, eq, v1, Operand(0x40000000));
        // Check for negative zero result.
        __ Ret(ne, v0, Operand(zero_reg));  // OK if result was non-zero.
        __ li(t0, Operand(0x80000000));
        __ And(smi_test_reg, smi_test_reg, Operand(t0));
        // Go slow if operands negative.
        __ Branch(&slow, eq, smi_test_reg, Operand(t0));
        __ Ret();

        __ bind(&slow);
      }
      HandleBinaryOpSlowCases(masm,
                              &not_smi,
                              lhs,
                              rhs,
                              Builtins::DIV);
      break;
    }

    case Token::MOD: {
      Label not_smi, slow;
      if (ShouldGenerateSmiCode()) {
        ASSERT(kSmiTag == 0);  // Adjust code below.
        // t2 = x | y at entry.
        __ And(t3, smi_test_reg, Operand(kSmiTagMask));
        __ Branch(&not_smi, ne, t3, Operand(zero_reg));
        Register scratch2 = smi_test_reg;
        smi_test_reg = no_reg;
        // Remove tags, preserving sign.
        __ sra(t0, rhs, kSmiTagSize);
        __ sra(t1, lhs, kSmiTagSize);
        // Check for divisor of 0.
        __ Branch(&slow, eq, t0, Operand(zero_reg));
        __ Div(t1, Operand(t0));
        __ mfhi(result);
        __ sll(result, result, kSmiTagSize);  // Smi tag return value.
        // Check for negative zero result.
        __ Ret(ne, result, Operand(zero_reg));  // OK if result was non-zero.
        __ li(t0, Operand(0x80000000));
        __ And(scratch2, scratch2, Operand(t0));
        // Go slow if operands negative.
        __ Branch(&slow, eq, scratch2, Operand(t0));
        __ Ret();

        __ bind(&slow);
      }
      HandleBinaryOpSlowCases(masm,
                              &not_smi,
                              lhs,
                              rhs,
                              Builtins::MOD);
      break;
    }


    case Token::BIT_OR:
    case Token::BIT_AND:
    case Token::BIT_XOR:
    case Token::SAR:
    case Token::SHR:
    case Token::SHL: {
      // Result (v0) = x (lhs) op y (rhs).
      // Untaged x: a3, untagged y: a2.
      Label slow;
      ASSERT(kSmiTag == 0);  // Adjust code below.
      __ And(t3, smi_test_reg, Operand(kSmiTagMask));
      __ Branch(&slow, ne, t3, Operand(zero_reg));
      Register scratch2 = smi_test_reg;
      smi_test_reg = no_reg;
      switch (op_) {
        case Token::BIT_OR:  __ Or(result, rhs, Operand(lhs)); break;
        case Token::BIT_AND: __ And(result, rhs, Operand(lhs)); break;
        case Token::BIT_XOR: __ Xor(result, rhs, Operand(lhs)); break;
        case Token::SAR:
          // Remove tags from operands.
          __ sra(a2, rhs, kSmiTagSize);  // y.
          __ sra(a3, lhs, kSmiTagSize);  // x.
          // Shift.
          __ srav(v0, a3, a2);
          // Smi tag result.
          __ sll(v0, v0, kSmiTagMask);
          break;
        case Token::SHR:
          // Remove tags from operands.
          __ sra(a2, rhs, kSmiTagSize);  // y.
          __ sra(a3, lhs, kSmiTagSize);  // x.
          // Shift.
          __ srlv(v0, a3, a2);
          // Unsigned shift is not allowed to produce a negative number, so
          // check the sign bit and the sign bit after Smi tagging.
          __ And(t3, v0, Operand(0xc0000000));
          __ Branch(&slow, ne, t3, Operand(zero_reg));
          // Smi tag result.
          __ sll(v0, v0, kSmiTagMask);
          break;
        case Token::SHL:
           // Remove tags from operands.
          __ sra(a2, rhs, kSmiTagSize);  // y.
          __ sra(a3, lhs, kSmiTagSize);  // x.
          // Shift.
          __ sllv(v0, a3, a2);
          // Check that the signed result fits in a Smi.
          __ Addu(t3, v0, Operand(0x40000000));
          __ Branch(&slow, lt, t3, Operand(zero_reg));
          // Smi tag result.
          __ sll(v0, v0, kSmiTagMask);
          break;
        default: UNREACHABLE();
      }
      __ Ret();
      __ bind(&slow);

      HandleNonSmiBitwiseOp(masm, lhs, rhs);
      break;
    }

    default: UNREACHABLE();
  }
  // This code should be unreachable.
  __ stop("Unreachable");

  // Generate an unreachable reference to the DEFAULT stub so that it can be
  // found at the end of this stub when clearing ICs at GC.
  // TODO(kaznacheev): Check performance impact and get rid of this.
  if (runtime_operands_type_ != BinaryOpIC::DEFAULT) {
    GenericBinaryOpStub uninit(MinorKey(), BinaryOpIC::DEFAULT);
    __ CallStub(&uninit);
  }
}


void GenericBinaryOpStub::GenerateTypeTransition(MacroAssembler* masm) {
  Label get_result;

  __ Push(a1, a0);

  // Internal frame is necessary to handle exceptions properly.
  __ EnterInternalFrame();
  // Call the stub proper to get the result in v0.
  __ Call(&get_result);
  __ LeaveInternalFrame();

  __ push(v0);

  __ li(a0, Operand(Smi::FromInt(MinorKey())));
  __ push(a0);
  __ li(a0, Operand(Smi::FromInt(op_)));
  __ push(a0);
  __ li(a0, Operand(Smi::FromInt(runtime_operands_type_)));
  __ push(a0);

  __ TailCallExternalReference(
      ExternalReference(IC_Utility(IC::kBinaryOp_Patch)),
      6,
      1);

  // The entry point for the result calculation is assumed to be immediately
  // after this sequence.
  __ bind(&get_result);
}


Handle<Code> GetBinaryOpStub(int key, BinaryOpIC::TypeInfo type_info) {
  GenericBinaryOpStub stub(key, type_info);
  return stub.GetCode();
}

void TranscendentalCacheStub::Generate(MacroAssembler* masm) {
  // Argument is a number and is on stack and in a0.
  Label runtime_call;
  Label input_not_smi;
  Label loaded;

  if (CpuFeatures::IsSupported(FPU)) {
    // Load argument and check if it is a smi.
    __ BranchOnNotSmi(a0, &input_not_smi);

    CpuFeatures::Scope scope(FPU);
    // Input is a smi. Convert to double and load the low and high words
    // of the double into a2, a3.
    __ sra(t0, a0, kSmiTagSize);
    __ mtc1(t0, f4);
    __ cvt_d_w(f4, f4);
    __ mfc1(a2, f4);
    __ mfc1(a3, f5);
    __ Branch(&loaded);

    __ bind(&input_not_smi);
    // Check if input is a HeapNumber.
    __ CheckMap(a0,
                a1,
                Heap::kHeapNumberMapRootIndex,
                &runtime_call,
                true);
    // Input is a HeapNumber. Store the
    // low and high words into a2, a3.
    __ lw(a2, FieldMemOperand(a0, HeapNumber::kValueOffset));
    __ lw(a3, FieldMemOperand(a0, HeapNumber::kValueOffset + 4));

    __ bind(&loaded);
    // a2 = low 32 bits of double value
    // a3 = high 32 bits of double value
    // Compute hash (the shifts are arithmetic):
    //   h = (low ^ high); h ^= h >> 16; h ^= h >> 8; h = h & (cacheSize - 1);
    __ Xor(a1, a2, a3);
    __ sra(t0, a1, 16);
    __ Xor(a1, a1, t0);
    __ sra(t0, a1, 8);
    __ Xor(a1, a1, t0);
    ASSERT(IsPowerOf2(TranscendentalCache::kCacheSize));
    __ And(a1, a1, Operand(TranscendentalCache::kCacheSize - 1));

    // a2 = low 32 bits of double value.
    // a3 = high 32 bits of double value.
    // a1 = TranscendentalCache::hash(double value).
    __ li(a0,
           Operand(ExternalReference::transcendental_cache_array_address()));
    // a0 points to cache array.
    __ lw(a0, MemOperand(a0, type_ * sizeof(TranscendentalCache::caches_[0])));
    // a0 points to the cache for the type type_.
    // If NULL, the cache hasn't been initialized yet, so go through runtime.
    __ Branch(&runtime_call, eq, a0, Operand(zero_reg));

#ifdef DEBUG
    // Check that the layout of cache elements match expectations.
    { TranscendentalCache::Element test_elem[2];
      char* elem_start = reinterpret_cast<char*>(&test_elem[0]);
      char* elem2_start = reinterpret_cast<char*>(&test_elem[1]);
      char* elem_in0 = reinterpret_cast<char*>(&(test_elem[0].in[0]));
      char* elem_in1 = reinterpret_cast<char*>(&(test_elem[0].in[1]));
      char* elem_out = reinterpret_cast<char*>(&(test_elem[0].output));
      CHECK_EQ(12, elem2_start - elem_start);  // Two uint_32's and a pointer.
      CHECK_EQ(0, elem_in0 - elem_start);
      CHECK_EQ(kIntSize, elem_in1 - elem_start);
      CHECK_EQ(2 * kIntSize, elem_out - elem_start);
    }
#endif

    // Find the address of the a1'st entry in the cache, i.e., &a0[a1*12].
    __ sll(t0, a1, 1);
    __ Addu(a1, a1, t0);
    __ sll(t0, a1, 2);
    __ Addu(a0, a0, t0);

    // Check if cache matches: Double value is stored in uint32_t[2] array.
    __ lw(t0, MemOperand(a0, 0));
    __ lw(t1, MemOperand(a0, 4));
    __ lw(t2, MemOperand(a0, 8));
    __ Addu(a0, a0, 12);
    __ Branch(&runtime_call, ne, a2, Operand(t0));
    __ Branch(&runtime_call, ne, a3, Operand(t1));
    // Cache hit. Load result, pop argument and return.
    __ mov(v0, t2);
    __ Pop();
    __ Ret();
  }

  __ bind(&runtime_call);
  __ TailCallExternalReference(ExternalReference(RuntimeFunction()), 1, 1);
}


Runtime::FunctionId TranscendentalCacheStub::RuntimeFunction() {
  switch (type_) {
    // Add more cases when necessary.
    case TranscendentalCache::SIN: return Runtime::kMath_sin;
    case TranscendentalCache::COS: return Runtime::kMath_cos;
    default:
      UNIMPLEMENTED();
      return Runtime::kAbort;
  }
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
    // Check whether the value is a smi.
    Label try_float;
    __ BranchOnNotSmi(a0, &try_float);

    // Go slow case if the value of the expression is zero
    // to make sure that we switch between 0 and -0.
    __ Branch(&slow, eq, a0, Operand(0));

    // The value of the expression is a smi that is not zero.  Try
    // optimistic subtraction '0 - value'.
    __ subu(v0, zero_reg, a0);
    // Check for overflow. For v=0-x, overflow only occurs on x=0x80000000.
    __ Branch(&slow, eq, a0, Operand(0x80000000));  // Go slow on overflow.

    // Return v0 result if no overflow.
    __ StubReturn(1);

    __ bind(&try_float);
    __ GetObjectType(a0, a1, a1);
    __ Branch(&slow, ne, a1, Operand(HEAP_NUMBER_TYPE));
    // a0 is a heap number.  Get a new heap number in a1.
    if (overwrite_) {
      __ lw(a2, FieldMemOperand(a0, HeapNumber::kExponentOffset));
      __ Xor(a2, a2, Operand(HeapNumber::kSignMask));  // Flip sign.
      __ sw(a2, FieldMemOperand(a0, HeapNumber::kExponentOffset));
    } else {
      __ AllocateHeapNumber(a1, a2, a3, &slow);
      __ lw(a3, FieldMemOperand(a0, HeapNumber::kMantissaOffset));
      __ lw(a2, FieldMemOperand(a0, HeapNumber::kExponentOffset));
      __ sw(a3, FieldMemOperand(a1, HeapNumber::kMantissaOffset));
      __ Xor(a2, a2, Operand(HeapNumber::kSignMask));  // Flip sign.
      __ sw(a2, FieldMemOperand(a1, HeapNumber::kExponentOffset));
      __ mov(v0, a1);
    }
  } else if (op_ == Token::BIT_NOT) {
    // Check if the operand is a heap number.
    __ GetObjectType(a0, a1, a1);
    __ Branch(&slow, ne, a1, Operand(HEAP_NUMBER_TYPE));

    // Convert the heap number in a0 to an untagged integer in a1.
    // Go slow if HeapNumber won't fit in 32-bit (untagged) int.
    GetInt32(masm, a0, a1, a2, a3, &slow);

    // Do the bitwise operation (use NOR) and check if the result
    // fits in a smi.
    Label try_float;
    __ nor(a1, a1, zero_reg);
    // check that the *signed* result fits in a smi
    __ Addu(a2, a1, Operand(0x40000000));
    __ Branch(&try_float, lt, a2, Operand(zero_reg));

    // Smi tag result.
    __ sll(v0, a1, kSmiTagMask);
    __ StubReturn(1);

    __ bind(&try_float);
    if (!overwrite_) {
      // Allocate a fresh heap number, but don't overwrite a0 in-case
      // we need to go slow. Return new heap number in v0.
      __ AllocateHeapNumber(a2, a3, t0, &slow);
      __ mov(v0, a2);
    }

    // WriteInt32ToHeapNumberStub does not trigger GC, so we do not
    // have to set up a frame.
    WriteInt32ToHeapNumberStub stub(a1, v0, a2, a3);
    __ Push(ra);
    __ Call(stub.GetCode(), RelocInfo::CODE_TARGET);
    __ Pop(ra);
    // Fall thru to done.
  } else {
    UNIMPLEMENTED();
    __ break_(__LINE__);
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

// StringCharCodeAtGenerator


void StringCharCodeAtGenerator::GenerateFast(MacroAssembler* masm) {
  Label flat_string;
  Label ascii_string;
  Label got_char_code;

  ASSERT(!t0.is(scratch_));
  ASSERT(!t0.is(index_));
  ASSERT(!t0.is(result_));
  ASSERT(!t0.is(object_));

  // If the receiver is a smi trigger the non-string case.
  __ BranchOnSmi(object_, receiver_not_string_);

  // Fetch the instance type of the receiver into result register.
  __ lw(result_, FieldMemOperand(object_, HeapObject::kMapOffset));
  __ lbu(result_, FieldMemOperand(result_, Map::kInstanceTypeOffset));
  // If the receiver is not a string trigger the non-string case.
  __ And(t0, result_, Operand(kIsNotStringMask));
  __ Branch(receiver_not_string_, ne, t0, Operand(zero_reg));

  // If the index is non-smi trigger the non-smi case.
  __ BranchOnNotSmi(index_, &index_not_smi_);

  // Put smi-tagged index into scratch register.
  __ mov(scratch_, index_);
  __ bind(&got_smi_index_);

  // Check for index out of range.
  __ lw(t0, FieldMemOperand(object_, String::kLengthOffset));
  __ Branch(index_out_of_range_, ls, t0, Operand(scratch_));

  // We need special handling for non-flat strings.
  ASSERT(kSeqStringTag == 0);
  __ And(t0, result_, Operand(kStringRepresentationMask));
  __ Branch(&flat_string, eq, t0, Operand(zero_reg));

  // Handle non-flat strings.
  __ And(t0, result_, Operand(kIsConsStringMask));
  __ Branch(&call_runtime_, eq, t0, Operand(zero_reg));

  // ConsString.
  // Check whether the right hand side is the empty string (i.e. if
  // this is really a flat string in a cons string). If that is not
  // the case we would rather go to the runtime system now to flatten
  // the string.
  __ lw(result_, FieldMemOperand(object_, ConsString::kSecondOffset));
  __ LoadRoot(t0, Heap::kEmptyStringRootIndex);
  __ Branch(&call_runtime_, ne, result_, Operand(t0));

  // Get the first of the two strings and load its instance type.
  __ lw(object_, FieldMemOperand(object_, ConsString::kFirstOffset));
  __ lw(result_, FieldMemOperand(object_, HeapObject::kMapOffset));
  __ lbu(result_, FieldMemOperand(result_, Map::kInstanceTypeOffset));
  // If the first cons component is also non-flat, then go to runtime.
  ASSERT(kSeqStringTag == 0);

  __ And(t0, result_, Operand(kStringRepresentationMask));
  __ Branch(&call_runtime_, ne, t0, Operand(zero_reg));

  // Check for 1-byte or 2-byte string.
  __ bind(&flat_string);
  ASSERT(kAsciiStringTag != 0);
  __ And(t0, result_, Operand(kStringEncodingMask));
  __ Branch(&ascii_string, ne, t0, Operand(zero_reg));

  // 2-byte string.
  // Load the 2-byte character code into the result register. We can
  // add without shifting since the smi tag size is the log2 of the
  // number of bytes in a two-byte character.
  ASSERT(kSmiTag == 0 && kSmiTagSize == 1 && kSmiShiftSize == 0);
  __ Addu(scratch_, object_, Operand(scratch_));
  __ lhu(result_, FieldMemOperand(scratch_, SeqTwoByteString::kHeaderSize));
  __ Branch(&got_char_code);

  // ASCII string.
  // Load the byte into the result register.
  __ bind(&ascii_string);

  __ srl(t0, scratch_, kSmiTagSize);
  __ Addu(scratch_, object_, t0);

  __ lbu(result_, FieldMemOperand(scratch_, SeqAsciiString::kHeaderSize));

  __ bind(&got_char_code);
  __ sll(result_, result_, kSmiTagSize);
  __ bind(&exit_);
}


void StringCharCodeAtGenerator::GenerateSlow(
    MacroAssembler* masm, const RuntimeCallHelper& call_helper) {
  __ Abort("Unexpected fallthrough to CharCodeAt slow case");

  // Index is not a smi.
  __ bind(&index_not_smi_);
  // If index is a heap number, try converting it to an integer.
  __ CheckMap(index_,
              scratch_,
              Heap::kHeapNumberMapRootIndex,
              index_not_number_,
              true);
  call_helper.BeforeCall(masm);
  // Consumed by runtime conversion function:
  __ Push(object_, index_, index_);
  if (index_flags_ == STRING_INDEX_IS_NUMBER) {
    __ CallRuntime(Runtime::kNumberToIntegerMapMinusZero, 1);
  } else {
    ASSERT(index_flags_ == STRING_INDEX_IS_ARRAY_INDEX);
    // NumberToSmi discards numbers that are not exact integers.
    __ CallRuntime(Runtime::kNumberToSmi, 1);
  }

  // Save the conversion result before the pop instructions below
  // have a chance to overwrite it.

  __ Move(scratch_, v0);

  __ Pop(index_);
  __ Pop(object_);
  // Reload the instance type.
  __ lw(result_, FieldMemOperand(object_, HeapObject::kMapOffset));
  __ lbu(result_, FieldMemOperand(result_, Map::kInstanceTypeOffset));
  call_helper.AfterCall(masm);
  // If index is still not a smi, it must be out of range.
  __ BranchOnNotSmi(scratch_, index_out_of_range_);
  // Otherwise, return to the fast path.
  __ Branch(&got_smi_index_);

  // Call runtime. We get here when the receiver is a string and the
  // index is a number, but the code of getting the actual character
  // is too complex (e.g., when the string needs to be flattened).
  __ bind(&call_runtime_);
  call_helper.BeforeCall(masm);
  __ Push(object_, index_);
  __ CallRuntime(Runtime::kStringCharCodeAt, 2);

  __ Move(result_, v0);

  call_helper.AfterCall(masm);
  __ jmp(&exit_);

  __ Abort("Unexpected fallthrough from CharCodeAt slow case");
}


// -------------------------------------------------------------------------
// StringCharFromCodeGenerator

void StringCharFromCodeGenerator::GenerateFast(MacroAssembler* masm) {
  // Fast case of Heap::LookupSingleCharacterStringFromCode.

  ASSERT(!t0.is(result_));
  ASSERT(!t0.is(code_));

  ASSERT(kSmiTag == 0);
  ASSERT(kSmiShiftSize == 0);
  ASSERT(IsPowerOf2(String::kMaxAsciiCharCode + 1));
  __ And(t0,
         code_,
         Operand(kSmiTagMask |
                 ((~String::kMaxAsciiCharCode) << kSmiTagSize)));
  __ Branch(&slow_case_, ne, t0, Operand(zero_reg));

  __ LoadRoot(result_, Heap::kSingleCharacterStringCacheRootIndex);
  // At this point code register contains smi tagged ascii char code.
  ASSERT(kSmiTag == 0);
  __ sll(t0, code_, kPointerSizeLog2 - kSmiTagSize);
  __ Addu(result_, result_, t0);
  __ lw(result_, FieldMemOperand(result_, FixedArray::kHeaderSize));
  __ LoadRoot(t0, Heap::kUndefinedValueRootIndex);
  __ Branch(&slow_case_, eq, result_, Operand(t0));
  __ bind(&exit_);
}


void StringCharFromCodeGenerator::GenerateSlow(
    MacroAssembler* masm, const RuntimeCallHelper& call_helper) {
  __ Abort("Unexpected fallthrough to CharFromCode slow case");

  __ bind(&slow_case_);
  call_helper.BeforeCall(masm);
  __ push(code_);
  __ CallRuntime(Runtime::kCharFromCode, 1);
  __ Move(v0, result_);

  call_helper.AfterCall(masm);
  __ Branch(&exit_);

  __ Abort("Unexpected fallthrough from CharFromCode slow case");
}


// -------------------------------------------------------------------------
// StringCharAtGenerator

void StringCharAtGenerator::GenerateFast(MacroAssembler* masm) {
  char_code_at_generator_.GenerateFast(masm);
  char_from_code_generator_.GenerateFast(masm);
}


void StringCharAtGenerator::GenerateSlow(
    MacroAssembler* masm, const RuntimeCallHelper& call_helper) {
  char_code_at_generator_.GenerateSlow(masm, call_helper);
  char_from_code_generator_.GenerateSlow(masm, call_helper);
}


void StringHelper::GenerateCopyCharacters(MacroAssembler* masm,
                                          Register dest,
                                          Register src,
                                          Register count,
                                          Register scratch,
                                          bool ascii) {
  Label loop;
  Label done;
  // This loop just copies one character at a time, as it is only used for
  // very short strings.
  if (!ascii) {
    __ addu(count, count, count);
  }
  __ Branch(&done, eq, count, Operand(zero_reg));
  __ addu(count, dest, count);  // Count now points to the last dest byte.

  __ bind(&loop);
  __ lbu(scratch, MemOperand(src));
  __ addiu(src, src, 1);
  __ sb(scratch, MemOperand(dest));
  __ addiu(dest, dest, 1);
  __ Branch(&loop, lt, dest, Operand(count));

  __ bind(&done);
}


enum CopyCharactersFlags {
  COPY_ASCII = 1,
  DEST_ALWAYS_ALIGNED = 2
};


void StringHelper::GenerateCopyCharactersLong(MacroAssembler* masm,
                                              Register dest,
                                              Register src,
                                              Register count,
                                              Register scratch1,
                                              Register scratch2,
                                              Register scratch3,
                                              Register scratch4,
                                              Register scratch5,
                                              int flags) {
  bool ascii = (flags & COPY_ASCII) != 0;
  bool dest_always_aligned = (flags & DEST_ALWAYS_ALIGNED) != 0;

  if (dest_always_aligned && FLAG_debug_code) {
    // Check that destination is actually word aligned if the flag says
    // that it is.
    __ And(scratch4, dest, Operand(kPointerAlignmentMask));
    __ Check(eq,
             "Destination of copy not aligned.",
             scratch4,
             Operand(zero_reg));
  }

  const int kReadAlignment = 4;
  const int kReadAlignmentMask = kReadAlignment - 1;
  // Ensure that reading an entire aligned word containing the last character
  // of a string will not read outside the allocated area (because we pad up
  // to kObjectAlignment).
  ASSERT(kObjectAlignment >= kReadAlignment);
  // Assumes word reads and writes are little endian.
  // Nothing to do for zero characters.
  Label done;

  if (!ascii) {
    __ addu(count, count, count);
  }
  __ Branch(&done, eq, count, Operand(zero_reg));

  Label byte_loop;
  // Must copy at least eight bytes, otherwise just do it one byte at a time.
  __ Subu(scratch1, count, Operand(8));
  __ Addu(count, dest, Operand(count));
  Register limit = count;  // Read until src equals this.
  __ Branch(&byte_loop, lt, scratch1, Operand(zero_reg));

  if (!dest_always_aligned) {
    // Align dest by byte copying. Copies between zero and three bytes.
    __ And(scratch4, dest, Operand(kReadAlignmentMask));
    Label dest_aligned;
    __ Branch(&dest_aligned, eq, scratch4, Operand(zero_reg));
    Label aligned_loop;
    __ bind(&aligned_loop);
    __ lbu(scratch1, MemOperand(src));
    __ addiu(src, src, 1);
    __ sb(scratch1, MemOperand(dest));
    __ addiu(dest, dest, 1);
    __ addiu(scratch4, scratch4, 1);
    __ Branch(&aligned_loop, le, scratch4, Operand(kReadAlignmentMask));
    __ bind(&dest_aligned);
  }

  Label simple_loop;

  __ And(scratch4, src, Operand(kReadAlignmentMask));
  __ Branch(&simple_loop, eq, scratch4, Operand(zero_reg));

  // Loop for src/dst that are not aligned the same way.
  // This loop uses lwl and lwr instructions. These instructions
  // depend on the endianness, and the implementation assumes little-endian.
  {
    Label loop;
    __ bind(&loop);
    __ lwr(scratch1, MemOperand(src));
    __ Addu(src, src, Operand(kReadAlignment));
    __ lwl(scratch1, MemOperand(src, -1));
    __ sw(scratch1, MemOperand(dest));
    __ Addu(dest, dest, Operand(kReadAlignment));
    __ Subu(scratch2, limit, dest);
    __ Branch(&loop, ge, scratch2, Operand(kReadAlignment));
  }

  __ Branch(&byte_loop, al);

  // Simple loop.
  // Copy words from src to dest, until less than four bytes left.
  // Both src and dest are word aligned.
  __ bind(&simple_loop);
  {
    Label loop;
    __ bind(&loop);
    __ lw(scratch1, MemOperand(src));
    __ Addu(src, src, Operand(kReadAlignment));
    __ sw(scratch1, MemOperand(dest));
    __ Addu(dest, dest, Operand(kReadAlignment));
    __ Subu(scratch2, limit, dest);
    __ Branch(&loop, ge, scratch2, Operand(kReadAlignment));
  }

  // Copy bytes from src to dest until dest hits limit.
  __ bind(&byte_loop);
  // Test if dest has already reached the limit
  __ Branch(&done, ge, dest, Operand(limit));
  __ lbu(scratch1, MemOperand(src));
  __ addiu(src, src, 1);
  __ sb(scratch1, MemOperand(dest));
  __ addiu(dest, dest, 1);
  __ Branch(&byte_loop, al);

  __ bind(&done);
}


void StringHelper::GenerateTwoCharacterSymbolTableProbe(MacroAssembler* masm,
                                                        Register c1,
                                                        Register c2,
                                                        Register scratch1,
                                                        Register scratch2,
                                                        Register scratch3,
                                                        Register scratch4,
                                                        Register scratch5,
                                                        Label* not_found) {
  // Register scratch3 is the general scratch register in this function.
  Register scratch = scratch3;

  // Make sure that both characters are not digits as such strings has a
  // different hash algorithm. Don't try to look for these in the symbol table.
  Label not_array_index;
  __ Subu(scratch, c1, Operand(static_cast<int>('0')));
  __ Branch(&not_array_index,
            Ugreater,
            scratch,
            Operand(static_cast<int>('9' - '0')));
  __ Subu(scratch, c2, Operand(static_cast<int>('0')));

  // If check failed combine both characters into single halfword.
  // This is required by the contract of the method: code at the
  // not_found branch expects this combination in c1 register
  Label tmp;
  __ sll(scratch1, c2, kBitsPerByte);
  __ Branch(&tmp, Ugreater, scratch, Operand(static_cast<int>('9' - '0')));
  __ Or(c1, c1, scratch1);
  __ bind(&tmp);
  __ Branch(not_found,
            Uless_equal,
            scratch,
            Operand(static_cast<int>('9' - '0')));

  __ bind(&not_array_index);
  // Calculate the two character string hash.
  Register hash = scratch1;
  StringHelper::GenerateHashInit(masm, hash, c1);
  StringHelper::GenerateHashAddCharacter(masm, hash, c2);
  StringHelper::GenerateHashGetHash(masm, hash);

  // Collect the two characters in a register.
  Register chars = c1;
  __ sll(scratch, c2, kBitsPerByte);
  __ Or(chars, chars, scratch);

  // chars: two character string, char 1 in byte 0 and char 2 in byte 1.
  // hash:  hash of two character string.

  // Load symbol table
  // Load address of first element of the symbol table.
  Register symbol_table = c2;
  __ LoadRoot(symbol_table, Heap::kSymbolTableRootIndex);

  // Load undefined value
  Register undefined = scratch4;
  __ LoadRoot(undefined, Heap::kUndefinedValueRootIndex);

  // Calculate capacity mask from the symbol table capacity.
  Register mask = scratch2;
  __ lw(mask, FieldMemOperand(symbol_table, SymbolTable::kCapacityOffset));
  __ sra(mask, mask, 1);
  __ Addu(mask, mask, -1);

  // Calculate untagged address of the first element of the symbol table.
  Register first_symbol_table_element = symbol_table;
  __ Addu(first_symbol_table_element, symbol_table,
         Operand(SymbolTable::kElementsStartOffset - kHeapObjectTag));

  // Registers
  // chars: two character string, char 1 in byte 0 and char 2 in byte 1.
  // hash:  hash of two character string
  // mask:  capacity mask
  // first_symbol_table_element: address of the first element of
  //                             the symbol table
  // scratch: -

  // Perform a number of probes in the symbol table.
  static const int kProbes = 4;
  Label found_in_symbol_table;
  Label next_probe[kProbes];
  Register candidate = scratch5;  // Scratch register contains candidate.
  for (int i = 0; i < kProbes; i++) {
    // Register candidate = scratch5;  // Scratch register contains candidate.

    // Calculate entry in symbol table.
    if (i > 0) {
      __ Addu(candidate, hash, Operand(SymbolTable::GetProbeOffset(i)));
    } else {
      __ mov(candidate, hash);
    }

    __ And(candidate, candidate, Operand(mask));

    // Load the entry from the symble table.
    ASSERT_EQ(1, SymbolTable::kEntrySize);
    __ sll(scratch, candidate, kPointerSizeLog2);
    __ Addu(scratch, scratch, first_symbol_table_element);
    __ lw(candidate, MemOperand(scratch));

    // If entry is undefined no string with this hash can be found.
    __ Branch(not_found, eq, candidate, Operand(undefined));

    // If length is not 2 the string is not a candidate.
    __ lw(scratch, FieldMemOperand(candidate, String::kLengthOffset));
    __ Branch(&next_probe[i], ne, scratch, Operand(Smi::FromInt(2)));

    // Check that the candidate is a non-external ascii string.
    __ lw(scratch, FieldMemOperand(candidate, HeapObject::kMapOffset));
    __ lbu(scratch, FieldMemOperand(scratch, Map::kInstanceTypeOffset));
    __ JumpIfInstanceTypeIsNotSequentialAscii(scratch, scratch, &next_probe[i]);

    // Check if the two characters match.
    // Assumes that word load is little endian.
    __ lhu(scratch, FieldMemOperand(candidate, SeqAsciiString::kHeaderSize));
    __ Branch(&found_in_symbol_table, eq, chars, Operand(scratch));
    __ bind(&next_probe[i]);
  }

  // No matching 2 character string found by probing.
  __ jmp(not_found);

  // Scratch register contains result when we fall through to here.
  Register result = candidate;
  __ bind(&found_in_symbol_table);
  __ mov(v0, result);
}


void StringHelper::GenerateHashInit(MacroAssembler* masm,
                                      Register hash,
                                      Register character) {
  // hash = character + (character << 10);
  __ sll(hash, character, 10);
  __ addu(hash, hash, character);
  // hash ^= hash >> 6;
  __ sra(at, hash, 6);
  __ xor_(hash, hash, at);
}


void StringHelper::GenerateHashAddCharacter(MacroAssembler* masm,
                                              Register hash,
                                              Register character) {
  // hash += character;
  __ addu(hash, hash, character);
  // hash += hash << 10;
  __ sll(at, hash, 10);
  __ addu(hash, hash, at);
  // hash ^= hash >> 6;
  __ sra(at, hash, 6);
  __ xor_(hash, hash, at);
}


void StringHelper::GenerateHashGetHash(MacroAssembler* masm,
                                         Register hash) {
  // hash += hash << 3;
  __ sll(at, hash, 3);
  __ addu(hash, hash, at);
  // hash ^= hash >> 11;
  __ sra(at, hash, 11);
  __ xor_(hash, hash, at);
  // hash += hash << 15;
  __ sll(at, hash, 15);
  __ addu(hash, hash, at);

  // if (hash == 0) hash = 27;
  __ ori(at, zero_reg, 27);
  __ movz(hash, at, hash);
}


void SubStringStub::Generate(MacroAssembler* masm) {
  Label sub_string_runtime;
  // Stack frame on entry.
  //  ra: return address
  //  sp[0]: to
  //  sp[4]: from
  //  sp[8]: string

  // This stub is called from the native-call %_SubString(...), so
  // nothing can be assumed about the arguments. It is tested that:
  //  "string" is a sequential string,
  //  both "from" and "to" are smis, and
  //  0 <= from <= to <= string.length.
  // If any of these assumptions fail, we call the runtime system.

  static const int kToOffset = 0 * kPointerSize;
  static const int kFromOffset = 1 * kPointerSize;
  static const int kStringOffset = 2 * kPointerSize;


  // Check bounds and smi-ness.
  __ lw(t3, MemOperand(sp, kToOffset));
  __ lw(t2, MemOperand(sp, kFromOffset));
  ASSERT_EQ(0, kSmiTag);
  ASSERT_EQ(1, kSmiTagSize + kSmiShiftSize);

  __ BranchOnNotSmi(t3, &sub_string_runtime);
  __ BranchOnNotSmi(t2, &sub_string_runtime);

  __ sra(a3, t2, kSmiTagSize);  // Remove smi tag.
  __ sra(t5, t3, kSmiTagSize);  // Remove smi tag.

  // a3: from index (untagged smi)
  // t5: to index (untagged smi)

  __ Branch(&sub_string_runtime, lt, a3, Operand(zero_reg));  // From < 0

  __ subu(a2, t5, a3);
  __ Branch(&sub_string_runtime, gt, a3, Operand(t5));  // Fail if from > to.

  // Special handling of sub-strings of length 1 and 2. One character strings
  // are handled in the runtime system (looked up in the single character
  // cache). Two character strings are looked for in the symbol cache.
  __ Branch(&sub_string_runtime, lt, a2, Operand(2));

  // a2: result string length
  // a3: from index (untagged smi)
  // t2: from (smi)
  // t3: to (smi)
  // t5: to index (untagged smi)

  // Make sure first argument is a sequential (or flat) string.
  __ lw(t1, MemOperand(sp, kStringOffset));
  __ Branch(&sub_string_runtime, eq, t1, Operand(kSmiTagMask));

  __ lw(a1, FieldMemOperand(t1, HeapObject::kMapOffset));
  __ lbu(a1, FieldMemOperand(a1, Map::kInstanceTypeOffset));
  __ And(t4, a1, Operand(kIsNotStringMask));

  __ Branch(&sub_string_runtime, ne, t4, Operand(zero_reg));

  // a1: instance type
  // a2: result string length
  // a3: from index (untagged smi)
  // t1: string
  // t2: from (smi)
  // t3: to (smi)
  // t5: to index (untagged smi)

  Label seq_string;
  __ And(t0, a1, Operand(kStringRepresentationMask));
  ASSERT(kSeqStringTag < kConsStringTag);
  ASSERT(kExternalStringTag > kConsStringTag);

  // External strings go to runtime.
  __ Branch(&sub_string_runtime, gt, t0, Operand(kConsStringTag));

  // Sequential strings are handled directly.
  __ Branch(&seq_string, lt, t0, Operand(kConsStringTag));

  // Cons string. Try to recurse (once) on the first substring.
  // (This adds a little more generality than necessary to handle flattened
  // cons strings, but not much).
  __ lw(t1, FieldMemOperand(t1, ConsString::kFirstOffset));
  __ lw(t0, FieldMemOperand(t1, HeapObject::kMapOffset));
  __ lbu(a1, FieldMemOperand(t0, Map::kInstanceTypeOffset));
  ASSERT_EQ(0, kSeqStringTag);
  // Cons and External strings go to runtime.
  __ Branch(&sub_string_runtime, ne, a1, Operand(kStringRepresentationMask));

  // Definitly a sequential string.
  __ bind(&seq_string);

  // a1: instance type
  // a2: result string length
  // a3: from index (untagged smi)
  // t1: string
  // t2: from (smi)
  // t3: to (smi)
  // t5: to index (untagged smi)

  __ lw(t0, FieldMemOperand(t1, String::kLengthOffset));
  __ Branch(&sub_string_runtime, lt, t0, Operand(t3));  // Fail if to > length.

  // a1: instance type
  // a2: result string length
  // a3: from index (untagged smi)
  // t1: string
  // t2: from (smi)
  // t3: to (smi)
  // t5: to index (untagged smi)

  // Check for flat ascii string.
  Label non_ascii_flat;
  ASSERT_EQ(0, kTwoByteStringTag);

  __ And(t4, a1, Operand(kStringEncodingMask));
  __ Branch(&non_ascii_flat, eq, t4, Operand(zero_reg));

  Label result_longer_than_two;
  __ Branch(&result_longer_than_two, gt, a2, Operand(2));

  // Sub string of length 2 requested.
  // Get the two characters forming the sub string.
  __ Addu(t1, t1, Operand(a3));
  __ lbu(a3, FieldMemOperand(t1, SeqAsciiString::kHeaderSize));
  __ lbu(t0, FieldMemOperand(t1, SeqAsciiString::kHeaderSize + 1));

  // Try to lookup two character string in symbol table.
  Label make_two_character_string;
  StringHelper::GenerateTwoCharacterSymbolTableProbe(
      masm, a3, t0, a1, t1, t2, t3, t4, &make_two_character_string);
  __ IncrementCounter(&Counters::sub_string_native, 1, a3, t0);
  __ Addu(sp, sp, Operand(3 * kPointerSize));
  __ Ret();


  // a2: result string length.
  // a3: two characters combined into halfword in little endian byte order.
  __ bind(&make_two_character_string);
  __ AllocateAsciiString(v0, a2, t0, t1, t4, &sub_string_runtime);
  __ sh(a3, FieldMemOperand(v0, SeqAsciiString::kHeaderSize));
  __ IncrementCounter(&Counters::sub_string_native, 1, a3, t0);
  __ Addu(sp, sp, Operand(3 * kPointerSize));
  __ Ret();

  __ bind(&result_longer_than_two);

  // Allocate the result.
  __ AllocateAsciiString(v0, a2, t4, t0, a1, &sub_string_runtime);

  // v0: result string.
  // a2: result string length.
  // a3: from index (untagged smi)
  // t1: string.
  // t2: from offset (smi)
  // Locate first character of result.
  __ Addu(a1, v0, Operand(SeqAsciiString::kHeaderSize - kHeapObjectTag));
  // Locate 'from' character of string.
  __ Addu(t1, t1, Operand(SeqAsciiString::kHeaderSize - kHeapObjectTag));
  __ Addu(t1, t1, Operand(a3));

  // v0: result string.
  // a1: first character of result string.
  // a2: result string length.
  // t1: first character of sub string to copy.
  ASSERT_EQ(0, SeqAsciiString::kHeaderSize & kObjectAlignmentMask);
  StringHelper::GenerateCopyCharactersLong(
      masm, a1, t1, a2, a3, t0, t2, t3, t4, COPY_ASCII | DEST_ALWAYS_ALIGNED);
  __ IncrementCounter(&Counters::sub_string_native, 1, a3, t0);
  __ Addu(sp, sp, Operand(3 * kPointerSize));
  __ Ret();

  __ bind(&non_ascii_flat);
  // a2: result string length.
  // t1: string.
  // t2: from offset (smi)
  // Check for flat two byte string.

  // Allocate the result.
  __ AllocateTwoByteString(v0, a2, a1, a3, t0, &sub_string_runtime);

  // v0: result string.
  // a2: result string length.
  // t1: string.
  // Locate first character of result.
  __ Addu(a1, v0, Operand(SeqTwoByteString::kHeaderSize - kHeapObjectTag));
  // Locate 'from' character of string.
  __ Addu(t1, t1, Operand(SeqTwoByteString::kHeaderSize - kHeapObjectTag));
  // As "from" is a smi it is 2 times the value which matches the size of a two
  // byte character.
  __ Addu(t1, t1, Operand(t2));

  // v0: result string.
  // a1: first character of result.
  // a2: result length.
  // t1: first character of string to copy.
  ASSERT_EQ(0, SeqTwoByteString::kHeaderSize & kObjectAlignmentMask);
  StringHelper::GenerateCopyCharactersLong(
      masm, a1, t1, a2, a3, t0, t2, t3, t4, DEST_ALWAYS_ALIGNED);
  __ IncrementCounter(&Counters::sub_string_native, 1, a3, t0);
  __ Addu(sp, sp, Operand(3 * kPointerSize));
  __ Ret();

  // Just jump to runtime to create the sub string.
  __ bind(&sub_string_runtime);
  __ TailCallRuntime(Runtime::kSubString, 3, 1);
}


void StringCompareStub::GenerateCompareFlatAsciiStrings(MacroAssembler* masm,
                                                        Register right,
                                                        Register left,
                                                        Register scratch1,
                                                        Register scratch2,
                                                        Register scratch3,
                                                        Register scratch4) {
  Label compare_lengths;
  // Find minimum length and length difference.
  __ lw(scratch1, FieldMemOperand(left, String::kLengthOffset));
  __ lw(scratch2, FieldMemOperand(right, String::kLengthOffset));
  Register length_delta = v0;   // This will later become result.
  __ subu(length_delta, scratch1, scratch2);
  Register min_length = scratch1;
  ASSERT(kSmiTag == 0);
  // set min_length to the smaller of the two string lengths.
  __ slt(scratch3, scratch1, scratch2);
  __ movz(min_length, scratch2, scratch3);

  // Untag smi.
  __ sra(min_length, min_length, kSmiTagSize);

  // Setup registers left and right to point to character[0].
  __ Addu(left, left, Operand(SeqAsciiString::kHeaderSize - kHeapObjectTag));
  __ Addu(right, right, Operand(SeqAsciiString::kHeaderSize - kHeapObjectTag));

  {
    // Compare loop.
    Label loop;
    __ bind(&loop);
    // Exit if remaining length is 0.
    __ Branch(&compare_lengths, eq, min_length, Operand(zero_reg));

    // Load chars.
    __ lbu(scratch2, MemOperand(left));
    __ addiu(left, left, 1);
    __ lbu(scratch4, MemOperand(right));
    __ addiu(right, right, 1);

    // Repeat loop while chars are equal. Use Branch-delay slot.
    __ Branch(false, &loop, eq, scratch2, Operand(scratch4));
    __ addiu(min_length, min_length, -1);  // In delay-slot.
  }

  // We fall thru here when the chars are not equal.
  // The result is <, =, >,  based on non-matching char, or
  // non-matching length.
  // Re-purpose the length_delta reg for char diff.
  Register result = length_delta;   // This is v0.
  __ subu(result, scratch2, scratch4);

  // We branch here when all 'min-length' chars are equal, and there is
  // a string-length difference in 'result' reg.
  // We fall in here when there is a character difference in 'result'.

  // A zero 'difference' is directly returned as EQUAL.
  ASSERT(Smi::FromInt(EQUAL) == static_cast<Smi*>(0));

  __ bind(&compare_lengths);

  // Branchless code converts negative value to LESS,
  // postive value to GREATER.
  __ li(scratch1, Operand(Smi::FromInt(LESS)));
  __ slt(scratch2, result, zero_reg);
  __ movn(result, scratch1, scratch2);
  __ li(scratch1, Operand(Smi::FromInt(GREATER)));
  __ slt(scratch2, zero_reg, result);
  __ movn(result, scratch1, scratch2);
  __ Ret();  // Result is (in) register v0.
}


void StringCompareStub::Generate(MacroAssembler* masm) {
  Label runtime;

  // Stack frame on entry.
  //  sp[0]: right string
  //  sp[4]: left string
  __ lw(a0, MemOperand(sp, 1 * kPointerSize));  // left
  __ lw(a1, MemOperand(sp, 0 * kPointerSize));  // right

  Label not_same;
  __ Branch(&not_same, ne, a0, Operand(a1));
  ASSERT_EQ(0, EQUAL);
  ASSERT_EQ(0, kSmiTag);
  __ li(v0, Operand(Smi::FromInt(EQUAL)));
  __ IncrementCounter(&Counters::string_compare_native, 1, a1, a2);
  __ Addu(sp, sp, Operand(2 * kPointerSize));
  __ Ret();

  __ bind(&not_same);

  // Check that both objects are sequential ascii strings.
  __ JumpIfNotBothSequentialAsciiStrings(a0, a1, a2, a3, &runtime);

  // Compare flat ascii strings natively. Remove arguments from stack first.
  __ IncrementCounter(&Counters::string_compare_native, 1, a2, a3);
  __ Addu(sp, sp, Operand(2 * kPointerSize));
  GenerateCompareFlatAsciiStrings(masm, a1, a0, a2, a3, t0, t1);

  __ bind(&runtime);
  __ TailCallRuntime(Runtime::kStringCompare, 2, 1);
}


void StringAddStub::Generate(MacroAssembler* masm) {
  Label string_add_runtime;
  // Stack on entry:
  // sp[0]: second argument.
  // sp[4]: first argument.

  // Load the two arguments.
  __ lw(a0, MemOperand(sp, 1 * kPointerSize));  // First argument.
  __ lw(a1, MemOperand(sp, 0 * kPointerSize));  // Second argument.

  // Make sure that both arguments are strings if not known in advance.
  if (string_check_) {
    ASSERT_EQ(0, kSmiTag);
    __ JumpIfEitherSmi(a0, a1, &string_add_runtime);
    // Load instance types.
    __ lw(t0, FieldMemOperand(a0, HeapObject::kMapOffset));
    __ lw(t1, FieldMemOperand(a1, HeapObject::kMapOffset));
    __ lbu(t0, FieldMemOperand(t0, Map::kInstanceTypeOffset));
    __ lbu(t1, FieldMemOperand(t1, Map::kInstanceTypeOffset));
    ASSERT_EQ(0, kStringTag);
    // If either is not a string, go to runtime.
    __ Or(t4, t0, Operand(t1));
    __ And(t4, t4, Operand(kIsNotStringMask));
    __ Branch(&string_add_runtime, ne, t4, Operand(zero_reg));
  }

  // Both arguments are strings.
  // a0: first string
  // a1: second string
  // t0: first string instance type (if string_check_)
  // t1: second string instance type (if string_check_)
  {
    Label strings_not_empty;
    // Check if either of the strings are empty. In that case return the other.
    // These tests use zero-length check on string-length whch is an Smi.
    // Assert that Smi::FromInt(0) is really 0.
    ASSERT(kSmiTag == 0);
    ASSERT(Smi::FromInt(0) == 0);
    __ lw(a2, FieldMemOperand(a0, String::kLengthOffset));
    __ lw(a3, FieldMemOperand(a1, String::kLengthOffset));
    __ mov(v0, a0);       // Assume we'll return first string (from a0).
    __ movz(v0, a1, a2);  // If first is empty, return second (from a1).
    __ slt(t4, zero_reg, a2);   // if (a2 > 0) t4 = 1.
    __ slt(t5, zero_reg, a3);   // if (a3 > 0) t5 = 1.
    __ and_(t4, t4, t5);        // Branch if both strings were non-empty.
    __ Branch(&strings_not_empty, ne, t4, Operand(zero_reg));

    __ IncrementCounter(&Counters::string_add_native, 1, a2, a3);
    __ Addu(sp, sp, Operand(2 * kPointerSize));
    __ Ret();

    __ bind(&strings_not_empty);
  }

  // Untag both string-lengths.
  __ sra(a2, a2, kSmiTagSize);
  __ sra(a3, a3, kSmiTagSize);

  // Both strings are non-empty.
  // a0: first string
  // a1: second string
  // a2: length of first string
  // a3: length of second string
  // t0: first string instance type (if string_check_)
  // t1: second string instance type (if string_check_)
  // Look at the length of the result of adding the two strings.
  Label string_add_flat_result, longer_than_two;
  // Adding two lengths can't overflow.
  ASSERT(String::kMaxLength * 2 > String::kMaxLength);
  __ Addu(t2, a2, Operand(a3));
  // Use the runtime system when adding two one character strings, as it
  // contains optimizations for this specific case using the symbol table.
  __ Branch(&longer_than_two, ne, t2, Operand(2));

  // Check that both strings are non-external ascii strings.
  if (!string_check_) {
    __ lw(t0, FieldMemOperand(a0, HeapObject::kMapOffset));
    __ lw(t1, FieldMemOperand(a1, HeapObject::kMapOffset));
    __ lbu(t0, FieldMemOperand(t0, Map::kInstanceTypeOffset));
    __ lbu(t1, FieldMemOperand(t1, Map::kInstanceTypeOffset));
  }
  __ JumpIfBothInstanceTypesAreNotSequentialAscii(t0, t1, t2, t3,
                                                 &string_add_runtime);

  // Get the two characters forming the sub string.
  __ lbu(a2, FieldMemOperand(a0, SeqAsciiString::kHeaderSize));
  __ lbu(a3, FieldMemOperand(a1, SeqAsciiString::kHeaderSize));

  // Try to lookup two character string in symbol table. If it is not found
  // just allocate a new one.
  Label make_two_character_string;
  StringHelper::GenerateTwoCharacterSymbolTableProbe(
      masm, a2, a3, t2, t3, t0, t1, t4, &make_two_character_string);
  __ IncrementCounter(&Counters::string_add_native, 1, a2, a3);
  __ Addu(sp, sp, Operand(2 * kPointerSize));
  __ Ret();

  __ bind(&make_two_character_string);
  // Resulting string has length 2 and first chars of two strings
  // are combined into single halfword in a2 register.
  // So we can fill resulting string without two loops by a single
  // halfword store instruction (which assumes that processor is
  // in a little endian mode)
  __ li(t2, Operand(2));
  __ AllocateAsciiString(v0, t2, t0, t1, t4, &string_add_runtime);
  __ sh(a2, FieldMemOperand(v0, SeqAsciiString::kHeaderSize));
  __ IncrementCounter(&Counters::string_add_native, 1, a2, a3);
  __ Addu(sp, sp, Operand(2 * kPointerSize));
  __ Ret();

  __ bind(&longer_than_two);
  // Check if resulting string will be flat.
  __ Branch(&string_add_flat_result, lt, t2,
           Operand(String::kMinNonFlatLength));
  // Handle exceptionally long strings in the runtime system.
  ASSERT((String::kMaxLength & 0x80000000) == 0);
  ASSERT(IsPowerOf2(String::kMaxLength + 1));
  // kMaxLength + 1 is representable as shifted literal, kMaxLength is not.
  __ Branch(&string_add_runtime, hs, t2, Operand(String::kMaxLength + 1));

  // If result is not supposed to be flat, allocate a cons string object.
  // If both strings are ascii the result is an ascii cons string.
  if (!string_check_) {
    __ lw(t0, FieldMemOperand(a0, HeapObject::kMapOffset));
    __ lw(t1, FieldMemOperand(a1, HeapObject::kMapOffset));
    __ lbu(t0, FieldMemOperand(t0, Map::kInstanceTypeOffset));
    __ lbu(t1, FieldMemOperand(t1, Map::kInstanceTypeOffset));
  }
  Label non_ascii, allocated, ascii_data;
  ASSERT_EQ(0, kTwoByteStringTag);
  // Branch to non_ascii if either string-encoding field is zero (non-ascii).
  __ And(t4, t0, Operand(t1));
  __ And(t4, t4, Operand(kStringEncodingMask));
  __ Branch(&non_ascii, eq, t4, Operand(zero_reg));

  // Allocate an ASCII cons string.
  __ bind(&ascii_data);
  __ AllocateAsciiConsString(t3, t2, t0, t1, &string_add_runtime);
  __ bind(&allocated);
  // Fill the fields of the cons string.
  __ sw(a0, FieldMemOperand(t3, ConsString::kFirstOffset));
  __ sw(a1, FieldMemOperand(t3, ConsString::kSecondOffset));
  __ mov(v0, t3);
  __ IncrementCounter(&Counters::string_add_native, 1, a2, a3);
  __ Addu(sp, sp, Operand(2 * kPointerSize));
  __ Ret();

  __ bind(&non_ascii);
  // At least one of the strings is two-byte. Check whether it happens
  // to contain only ascii characters.
  // t0: first instance type.
  // t1: second instance type.
  // Branch to if _both_ instances have kAsciiDataHintMask set.
  __ And(at, t0, Operand(kAsciiDataHintMask));
  __ and_(at, at, t1);
  __ Branch(&ascii_data, ne, at, Operand(zero_reg));

  __ xor_(t0, t0, t1);
  ASSERT(kAsciiStringTag != 0 && kAsciiDataHintTag != 0);
  __ And(t0, t0, Operand(kAsciiStringTag | kAsciiDataHintTag));
  __ Branch(&ascii_data, eq, t0, Operand(kAsciiStringTag | kAsciiDataHintTag));

  // Allocate a two byte cons string.
  __ AllocateTwoByteConsString(t3, t2, t0, t1, &string_add_runtime);
  __ Branch(al, &allocated);

  // Handle creating a flat result. First check that both strings are
  // sequential and that they have the same encoding.
  // a0: first string
  // a1: second string
  // a2: length of first string
  // a3: length of second string
  // t0: first string instance type (if string_check_)
  // t1: second string instance type (if string_check_)
  // t2: sum of lengths.
  __ bind(&string_add_flat_result);
  if (!string_check_) {
    __ lw(t0, FieldMemOperand(a0, HeapObject::kMapOffset));
    __ lw(t1, FieldMemOperand(a1, HeapObject::kMapOffset));
    __ lbu(t0, FieldMemOperand(t0, Map::kInstanceTypeOffset));
    __ lbu(t1, FieldMemOperand(t1, Map::kInstanceTypeOffset));
  }
  // Check that both strings are sequential, meaning that we
  // branch to runtime if either string tag is non-zero.
  ASSERT_EQ(0, kSeqStringTag);
  __ Or(t4, t0, Operand(t1));
  __ And(t4, t4, Operand(kStringRepresentationMask));
  __ Branch(&string_add_runtime, ne, t4, Operand(zero_reg));

  // Now check if both strings have the same encoding (ASCII/Two-byte).
  // a0: first string
  // a1: second string
  // a2: length of first string
  // a3: length of second string
  // t0: first string instance type
  // t1: second string instance type
  // t2: sum of lengths.
  Label non_ascii_string_add_flat_result;
  ASSERT(IsPowerOf2(kStringEncodingMask));  // Just one bit to test.
  __ xor_(t3, t1, t0);
  __ And(t3, t3, Operand(kStringEncodingMask));
  __ Branch(&string_add_runtime, ne, t3, Operand(zero_reg));
  // And see if it's ASCII (0) or two-byte (1).
  __ And(t3, t0, Operand(kStringEncodingMask));
  __ Branch(&non_ascii_string_add_flat_result, eq, t3, Operand(zero_reg));

  // Both strings are sequential ASCII strings. We also know that they are
  // short (since the sum of the lengths is less than kMinNonFlatLength).
  // t2: length of resulting flat string
  __ AllocateAsciiString(t3, t2, t0, t1, t4, &string_add_runtime);
  // Locate first character of result.
  __ Addu(t2, t3, Operand(SeqAsciiString::kHeaderSize - kHeapObjectTag));
  // Locate first character of first argument.
  __ Addu(a0, a0, Operand(SeqAsciiString::kHeaderSize - kHeapObjectTag));
  // a0: first character of first string.
  // a1: second string.
  // a2: length of first string.
  // a3: length of second string.
  // t2: first character of result.
  // t3: result string.
  StringHelper::GenerateCopyCharacters(masm, t2, a0, a2, t0, true);

  // Load second argument and locate first character.
  __ Addu(a1, a1, Operand(SeqAsciiString::kHeaderSize - kHeapObjectTag));
  // a1: first character of second string.
  // a3: length of second string.
  // t2: next character of result.
  // t3: result string.
  StringHelper::GenerateCopyCharacters(masm, t2, a1, a3, t0, true);
  __ mov(v0, t3);
  __ IncrementCounter(&Counters::string_add_native, 1, a2, a3);
  __ Addu(sp, sp, Operand(2 * kPointerSize));
  __ Ret();

  __ bind(&non_ascii_string_add_flat_result);
  // Both strings are sequential two byte strings.
  // a0: first string.
  // a1: second string.
  // a2: length of first string.
  // a3: length of second string.
  // t2: sum of length of strings.
  __ AllocateTwoByteString(t3, t2, t0, t1, t4, &string_add_runtime);
  // a0: first string.
  // a1: second string.
  // a2: length of first string.
  // a3: length of second string.
  // t3: result string.

  // Locate first character of result.
  __ Addu(t2, t3, Operand(SeqTwoByteString::kHeaderSize - kHeapObjectTag));
  // Locate first character of first argument.
  __ Addu(a0, a0, Operand(SeqTwoByteString::kHeaderSize - kHeapObjectTag));

  // a0: first character of first string.
  // a1: second string.
  // a2: length of first string.
  // a3: length of second string.
  // t2: first character of result.
  // t3: result string.
  StringHelper::GenerateCopyCharacters(masm, t2, a0, a2, t0, false);

  // Locate first character of second argument.
  __ Addu(a1, a1, Operand(SeqTwoByteString::kHeaderSize - kHeapObjectTag));

  // a1: first character of second string.
  // a3: length of second string.
  // t2: next character of result (after copy of first string).
  // t3: result string.
  StringHelper::GenerateCopyCharacters(masm, t2, a1, a3, t0, false);

  __ mov(v0, t3);
  __ IncrementCounter(&Counters::string_add_native, 1, a2, a3);
  __ Addu(sp, sp, Operand(2 * kPointerSize));
  __ Ret();

  // Just jump to runtime to add the two strings.
  __ bind(&string_add_runtime);
  __ TailCallRuntime(Runtime::kStringAdd, 2, 1);
}


#undef __

} }  // namespace v8::internal

#endif  // V8_TARGET_ARCH_MIPS
