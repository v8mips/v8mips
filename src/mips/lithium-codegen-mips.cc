// Copyright 2011 the V8 project authors. All rights reserved.
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

#include "mips/lithium-codegen-mips.h"
#include "mips/lithium-gap-resolver-mips.h"
#include "code-stubs.h"
#include "stub-cache.h"

namespace v8 {
namespace internal {


class SafepointGenerator : public CallWrapper {
 public:
  SafepointGenerator(LCodeGen* codegen,
                     LPointerMap* pointers,
                     int deoptimization_index)
      : codegen_(codegen),
        pointers_(pointers),
        deoptimization_index_(deoptimization_index) { }
  virtual ~SafepointGenerator() { }

  virtual void BeforeCall(int call_size) const {
    ASSERT(call_size >= 0);
    // Ensure that we have enough space after the previous safepoint position
    // for the generated code there.
    int call_end = codegen_->masm()->pc_offset() + call_size;
    int prev_jump_end =
        codegen_->LastSafepointEnd() + Deoptimizer::patch_size();
    if (call_end < prev_jump_end) {
      int padding_size = prev_jump_end - call_end;
      ASSERT_EQ(0, padding_size % Assembler::kInstrSize);
      while (padding_size > 0) {
        codegen_->masm()->nop();
        padding_size -= Assembler::kInstrSize;
      }
    }
  }

  virtual void AfterCall() const {
    codegen_->RecordSafepoint(pointers_, deoptimization_index_);
  }

 private:
  LCodeGen* codegen_;
  LPointerMap* pointers_;
  int deoptimization_index_;
};


#define __ masm()->

bool LCodeGen::GenerateCode() {
  HPhase phase("Code generation", chunk());
  ASSERT(is_unused());
  status_ = GENERATING;
  CpuFeatures::Scope scope(FPU);
  return GeneratePrologue() &&
      GenerateBody() &&
      GenerateDeferredCode() &&
      GenerateSafepointTable();
}


void LCodeGen::FinishCode(Handle<Code> code) {
  ASSERT(is_done());
  code->set_stack_slots(GetStackSlotCount());
  code->set_safepoint_table_offset(safepoints_.GetCodeOffset());
  PopulateDeoptimizationData(code);
  Deoptimizer::EnsureRelocSpaceForLazyDeoptimization(code);
}


void LCodeGen::Abort(const char* format, ...) {
  if (FLAG_trace_bailout) {
    SmartPointer<char> name(info()->shared_info()->DebugName()->ToCString());
    PrintF("Aborting LCodeGen in @\"%s\": ", *name);
    va_list arguments;
    va_start(arguments, format);
    OS::VPrint(format, arguments);
    va_end(arguments);
    PrintF("\n");
  }
  status_ = ABORTED;
}


void LCodeGen::Comment(const char* format, ...) {
  if (!FLAG_code_comments) return;
  char buffer[4 * KB];
  StringBuilder builder(buffer, ARRAY_SIZE(buffer));
  va_list arguments;
  va_start(arguments, format);
  builder.AddFormattedList(format, arguments);
  va_end(arguments);

  // Copy the string before recording it in the assembler to avoid
  // issues when the stack allocated buffer goes out of scope.
  size_t length = builder.position();
  Vector<char> copy = Vector<char>::New(length + 1);
  memcpy(copy.start(), builder.Finalize(), copy.length());
  masm()->RecordComment(copy.start());
}


bool LCodeGen::GeneratePrologue() {
  ASSERT(is_generating());

#ifdef DEBUG
  if (strlen(FLAG_stop_at) > 0 &&
      info_->function()->name()->IsEqualTo(CStrVector(FLAG_stop_at))) {
    __ stop("stop_at");
  }
#endif

  // a1: Callee's JS function.
  // cp: Callee's context.
  // fp: Caller's frame pointer.
  // lr: Caller's pc.

  // Strict mode functions and builtins need to replace the receiver
  // with undefined when called as functions (without an explicit
  // receiver object). r5 is zero for method calls and non-zero for
  // function calls.
  if (info_->is_strict_mode() || info_->is_native()) {
    Label ok;
    __ Branch(&ok, eq, t1, Operand(zero_reg));

    int receiver_offset = scope()->num_parameters() * kPointerSize;
    __ LoadRoot(a2, Heap::kUndefinedValueRootIndex);
    __ sw(a2, MemOperand(sp, receiver_offset));
    __ bind(&ok);
  }

  __ Push(ra, fp, cp, a1);
  __ Addu(fp, sp, Operand(2 * kPointerSize));  // Adj. FP to point to saved FP.

  // Reserve space for the stack slots needed by the code.
  int slots = GetStackSlotCount();
  if (slots > 0) {
    if (FLAG_debug_code) {
      __ li(a0, Operand(slots));
      __ li(a2, Operand(kSlotsZapValue));
      Label loop;
      __ bind(&loop);
      __ push(a2);
      __ Subu(a0, a0, 1);
      __ Branch(&loop, ne, a0, Operand(zero_reg));
    } else {
      __ Subu(sp, sp, Operand(slots * kPointerSize));
    }
  }

  // Possibly allocate a local context.
  int heap_slots = scope()->num_heap_slots() - Context::MIN_CONTEXT_SLOTS;
  if (heap_slots > 0) {
    Comment(";;; Allocate local context");
    // Argument to NewContext is the function, which is in a1.
    __ push(a1);
    if (heap_slots <= FastNewContextStub::kMaximumSlots) {
      FastNewContextStub stub(heap_slots);
      __ CallStub(&stub);
    } else {
      __ CallRuntime(Runtime::kNewFunctionContext, 1);
    }
    RecordSafepoint(Safepoint::kNoDeoptimizationIndex);
    // Context is returned in both v0 and cp.  It replaces the context
    // passed to us.  It's saved in the stack and kept live in cp.
    __ sw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
    // Copy any necessary parameters into the context.
    int num_parameters = scope()->num_parameters();
    for (int i = 0; i < num_parameters; i++) {
      Slot* slot = scope()->parameter(i)->AsSlot();
      if (slot != NULL && slot->type() == Slot::CONTEXT) {
        int parameter_offset = StandardFrameConstants::kCallerSPOffset +
            (num_parameters - 1 - i) * kPointerSize;
        // Load parameter from stack.
        __ lw(a0, MemOperand(fp, parameter_offset));
        // Store it in the context.
        __ li(a1, Operand(Context::SlotOffset(slot->index())));
        __ addu(at, cp, a1);
        __ sw(a0, MemOperand(at));
        // Update the write barrier. This clobbers all involved
        // registers, so we have to use two more registers to avoid
        // clobbering cp.
        __ mov(a2, cp);
        __ RecordWrite(a2, Operand(a1), a3, a0);
      }
    }
    Comment(";;; End allocate local context");
  }

  // Trace the call.
  if (FLAG_trace) {
    __ CallRuntime(Runtime::kTraceEnter, 0);
  }
  return !is_aborted();
}


bool LCodeGen::GenerateBody() {
  ASSERT(is_generating());
  bool emit_instructions = true;
  for (current_instruction_ = 0;
       !is_aborted() && current_instruction_ < instructions_->length();
       current_instruction_++) {
    LInstruction* instr = instructions_->at(current_instruction_);
    if (instr->IsLabel()) {
      LLabel* label = LLabel::cast(instr);
      emit_instructions = !label->HasReplacement();
    }

    if (emit_instructions) {
      Comment(";;; @%d: %s.", current_instruction_, instr->Mnemonic());
      instr->CompileToNative(this);
    }
  }
  return !is_aborted();
}


LInstruction* LCodeGen::GetNextInstruction() {
  if (current_instruction_ < instructions_->length() - 1) {
    return instructions_->at(current_instruction_ + 1);
  } else {
    return NULL;
  }
}


bool LCodeGen::GenerateDeferredCode() {
  ASSERT(is_generating());
  for (int i = 0; !is_aborted() && i < deferred_.length(); i++) {
    LDeferredCode* code = deferred_[i];
    __ bind(code->entry());
    code->Generate();
    __ jmp(code->exit());
  }
  // Deferred code is the last part of the instruction sequence. Mark
  // the generated code as done unless we bailed out.
  if (!is_aborted()) status_ = DONE;  return !is_aborted();
}


bool LCodeGen::GenerateDeoptJumpTable() {
  // TODO(plind): this will need a different implementation for MIPS.
  // Skipping it for now. Raised issue #100 for this.
  Abort("Unimplemented: %s", "EmitLoadDoubleRegister");
  return false;
}


bool LCodeGen::GenerateSafepointTable() {
  ASSERT(is_done());
  safepoints_.Emit(masm(), GetStackSlotCount());
  return !is_aborted();
}


Register LCodeGen::ToRegister(int index) const {
  return Register::FromAllocationIndex(index);
}


DoubleRegister LCodeGen::ToDoubleRegister(int index) const {
  return DoubleRegister::FromAllocationIndex(index);
}


Register LCodeGen::ToRegister(LOperand* op) const {
  ASSERT(op->IsRegister());
  return ToRegister(op->index());
}


Register LCodeGen::EmitLoadRegister(LOperand* op, Register scratch) {
  if (op->IsRegister()) {
    return ToRegister(op->index());
  } else if (op->IsConstantOperand()) {
    __ li(scratch, ToOperand(op));
    return scratch;
  } else if (op->IsStackSlot() || op->IsArgument()) {
    __ lw(scratch, ToMemOperand(op));
    return scratch;
  }
  UNREACHABLE();
  return scratch;
}


DoubleRegister LCodeGen::ToDoubleRegister(LOperand* op) const {
  ASSERT(op->IsDoubleRegister());
  return ToDoubleRegister(op->index());
}


DoubleRegister LCodeGen::EmitLoadDoubleRegister(LOperand* op,
                                                FloatRegister flt_scratch,
                                                DoubleRegister dbl_scratch) {
  Abort("Unimplemented: %s", "EmitLoadDoubleRegister");
  return dbl_scratch;
}


int LCodeGen::ToInteger32(LConstantOperand* op) const {
  Handle<Object> value = chunk_->LookupLiteral(op);
  ASSERT(chunk_->LookupLiteralRepresentation(op).IsInteger32());
  ASSERT(static_cast<double>(static_cast<int32_t>(value->Number())) ==
      value->Number());
  return static_cast<int32_t>(value->Number());
}


Operand LCodeGen::ToOperand(LOperand* op) {
  if (op->IsConstantOperand()) {
    LConstantOperand* const_op = LConstantOperand::cast(op);
    Handle<Object> literal = chunk_->LookupLiteral(const_op);
    Representation r = chunk_->LookupLiteralRepresentation(const_op);
    if (r.IsInteger32()) {
      ASSERT(literal->IsNumber());
      return Operand(static_cast<int32_t>(literal->Number()));
    } else if (r.IsDouble()) {
      Abort("ToOperand Unsupported double immediate.");
    }
    ASSERT(r.IsTagged());
    return Operand(literal);
  } else if (op->IsRegister()) {
    return Operand(ToRegister(op));
  } else if (op->IsDoubleRegister()) {
    Abort("ToOperand IsDoubleRegister unimplemented");
    return Operand(0);
  }
  // Stack slots not implemented, use ToMemOperand instead.
  UNREACHABLE();
  return Operand(0);
}


MemOperand LCodeGen::ToMemOperand(LOperand* op) const {
  ASSERT(!op->IsRegister());
  ASSERT(!op->IsDoubleRegister());
  ASSERT(op->IsStackSlot() || op->IsDoubleStackSlot());
  int index = op->index();
  if (index >= 0) {
    // Local or spill slot. Skip the frame pointer, function, and
    // context in the fixed part of the frame.
    return MemOperand(fp, -(index + 3) * kPointerSize);
  } else {
    // Incoming parameter. Skip the return address.
    return MemOperand(fp, -(index - 1) * kPointerSize);
  }
}


MemOperand LCodeGen::ToHighMemOperand(LOperand* op) const {
  ASSERT(op->IsDoubleStackSlot());
  int index = op->index();
  if (index >= 0) {
    // Local or spill slot. Skip the frame pointer, function, context,
    // and the first word of the double in the fixed part of the frame.
    return MemOperand(fp, -(index + 3) * kPointerSize + kPointerSize);
  } else {
    // Incoming parameter. Skip the return address and the first word of
    // the double.
    return MemOperand(fp, -(index - 1) * kPointerSize + kPointerSize);
  }
}


void LCodeGen::WriteTranslation(LEnvironment* environment,
                                Translation* translation) {
  if (environment == NULL) return;

  // The translation includes one command per value in the environment.
  int translation_size = environment->values()->length();
  // The output frame height does not include the parameters.
  int height = translation_size - environment->parameter_count();

  WriteTranslation(environment->outer(), translation);
  int closure_id = DefineDeoptimizationLiteral(environment->closure());
  translation->BeginFrame(environment->ast_id(), closure_id, height);
  for (int i = 0; i < translation_size; ++i) {
    LOperand* value = environment->values()->at(i);
    // spilled_registers_ and spilled_double_registers_ are either
    // both NULL or both set.
    if (environment->spilled_registers() != NULL && value != NULL) {
      if (value->IsRegister() &&
          environment->spilled_registers()[value->index()] != NULL) {
        translation->MarkDuplicate();
        AddToTranslation(translation,
                         environment->spilled_registers()[value->index()],
                         environment->HasTaggedValueAt(i));
      } else if (
          value->IsDoubleRegister() &&
          environment->spilled_double_registers()[value->index()] != NULL) {
        translation->MarkDuplicate();
        AddToTranslation(
            translation,
            environment->spilled_double_registers()[value->index()],
            false);
      }
    }

    AddToTranslation(translation, value, environment->HasTaggedValueAt(i));
  }
}


void LCodeGen::AddToTranslation(Translation* translation,
                                LOperand* op,
                                bool is_tagged) {
  if (op == NULL) {
    // TODO(twuerthinger): Introduce marker operands to indicate that this value
    // is not present and must be reconstructed from the deoptimizer. Currently
    // this is only used for the arguments object.
    translation->StoreArgumentsObject();
  } else if (op->IsStackSlot()) {
    if (is_tagged) {
      translation->StoreStackSlot(op->index());
    } else {
      translation->StoreInt32StackSlot(op->index());
    }
  } else if (op->IsDoubleStackSlot()) {
    translation->StoreDoubleStackSlot(op->index());
  } else if (op->IsArgument()) {
    ASSERT(is_tagged);
    int src_index = GetStackSlotCount() + op->index();
    translation->StoreStackSlot(src_index);
  } else if (op->IsRegister()) {
    Register reg = ToRegister(op);
    if (is_tagged) {
      translation->StoreRegister(reg);
    } else {
      translation->StoreInt32Register(reg);
    }
  } else if (op->IsDoubleRegister()) {
    DoubleRegister reg = ToDoubleRegister(op);
    translation->StoreDoubleRegister(reg);
  } else if (op->IsConstantOperand()) {
    Handle<Object> literal = chunk()->LookupLiteral(LConstantOperand::cast(op));
    int src_index = DefineDeoptimizationLiteral(literal);
    translation->StoreLiteral(src_index);
  } else {
    UNREACHABLE();
  }
}


void LCodeGen::CallCode(Handle<Code> code,
                        RelocInfo::Mode mode,
                        LInstruction* instr) {
  CallCodeGeneric(code, mode, instr, RECORD_SIMPLE_SAFEPOINT);
}


void LCodeGen::CallCodeGeneric(Handle<Code> code,
                               RelocInfo::Mode mode,
                               LInstruction* instr,
                               SafepointMode safepoint_mode) {
  ASSERT(instr != NULL);
  LPointerMap* pointers = instr->pointer_map();
  RecordPosition(pointers->position());
  __ Call(code, mode);
  RegisterLazyDeoptimization(instr, safepoint_mode);
}


void LCodeGen::CallRuntime(const Runtime::Function* function,
                           int num_arguments,
                           LInstruction* instr) {
  ASSERT(instr != NULL);
  LPointerMap* pointers = instr->pointer_map();
  ASSERT(pointers != NULL);
  RecordPosition(pointers->position());

  __ CallRuntime(function, num_arguments);
  RegisterLazyDeoptimization(instr, RECORD_SIMPLE_SAFEPOINT);
}


void LCodeGen::CallRuntimeFromDeferred(Runtime::FunctionId id,
                                       int argc,
                                       LInstruction* instr) {
  __ CallRuntimeSaveDoubles(id);
  RecordSafepointWithRegisters(
      instr->pointer_map(), argc, Safepoint::kNoDeoptimizationIndex);
}


void LCodeGen::RegisterLazyDeoptimization(LInstruction* instr,
                                          SafepointMode safepoint_mode) {
  // Create the environment to bailout to. If the call has side effects
  // execution has to continue after the call otherwise execution can continue
  // from a previous bailout point repeating the call.
  LEnvironment* deoptimization_environment;
  if (instr->HasDeoptimizationEnvironment()) {
    deoptimization_environment = instr->deoptimization_environment();
  } else {
    deoptimization_environment = instr->environment();
  }

  RegisterEnvironmentForDeoptimization(deoptimization_environment);
  if (safepoint_mode == RECORD_SIMPLE_SAFEPOINT) {
    RecordSafepoint(instr->pointer_map(),
                    deoptimization_environment->deoptimization_index());
  } else {
    ASSERT(safepoint_mode == RECORD_SAFEPOINT_WITH_REGISTERS_AND_NO_ARGUMENTS);
    RecordSafepointWithRegisters(
        instr->pointer_map(),
        0,
        deoptimization_environment->deoptimization_index());
  }
}


void LCodeGen::RegisterEnvironmentForDeoptimization(LEnvironment* environment) {
  if (!environment->HasBeenRegistered()) {
    // Physical stack frame layout:
    // -x ............. -4  0 ..................................... y
    // [incoming arguments] [spill slots] [pushed outgoing arguments]

    // Layout of the environment:
    // 0 ..................................................... size-1
    // [parameters] [locals] [expression stack including arguments]

    // Layout of the translation:
    // 0 ........................................................ size - 1 + 4
    // [expression stack including arguments] [locals] [4 words] [parameters]
    // |>------------  translation_size ------------<|

    int frame_count = 0;
    for (LEnvironment* e = environment; e != NULL; e = e->outer()) {
      ++frame_count;
    }
    Translation translation(&translations_, frame_count);
    WriteTranslation(environment, &translation);
    int deoptimization_index = deoptimizations_.length();
    environment->Register(deoptimization_index, translation.index());
    deoptimizations_.Add(environment);
  }
}


void LCodeGen::DeoptimizeIf(Condition cc,
                            LEnvironment* environment,
                            Register src1,
                            const Operand& src2) {
  RegisterEnvironmentForDeoptimization(environment);
  ASSERT(environment->HasBeenRegistered());
  int id = environment->deoptimization_index();
  Address entry = Deoptimizer::GetDeoptimizationEntry(id, Deoptimizer::EAGER);
  ASSERT(entry != NULL);
  if (entry == NULL) {
    Abort("bailout was not prepared");
    return;
  }

  ASSERT(FLAG_deopt_every_n_times < 2);  // Other values not supported on MIPS.

  if (FLAG_deopt_every_n_times == 1 &&
      info_->shared_info()->opt_count() == id) {
    __ Jump(entry, RelocInfo::RUNTIME_ENTRY);
    return;
  }

  if (FLAG_trap_on_deopt) {
    Label skip;
    if (cc != al) {
      __ Branch(&skip, NegateCondition(cc), src1, src2);
    }
    __ stop("trap_on_deopt");
    __ bind(&skip);
  }

  if (cc == al) {
    __ Jump(entry, RelocInfo::RUNTIME_ENTRY);
  } else {
    // We often have several deopts to the same entry, reuse the last
    // jump entry if this is the case.
    Comment(";;; Jump to deopt entry");
    // TODO(kalmard): This was taken from ARM. Seems to create infinite
    // loops like this:
    // beq s3, at, -1
#if 0
    if (deopt_jump_table_.is_empty() ||
        (deopt_jump_table_.last().address != entry)) {
      deopt_jump_table_.Add(JumpTableEntry(entry));
    }
    __ Branch(&deopt_jump_table_.last().label, cc, src1, src2);
#else
    __ Jump(entry, RelocInfo::RUNTIME_ENTRY, cc, src1, src2);
#endif
  }
}


void LCodeGen::PopulateDeoptimizationData(Handle<Code> code) {
  int length = deoptimizations_.length();
  if (length == 0) return;
  ASSERT(FLAG_deopt);
  Handle<DeoptimizationInputData> data =
      factory()->NewDeoptimizationInputData(length, TENURED);

  Handle<ByteArray> translations = translations_.CreateByteArray();
  data->SetTranslationByteArray(*translations);
  data->SetInlinedFunctionCount(Smi::FromInt(inlined_function_count_));

  Handle<FixedArray> literals =
      factory()->NewFixedArray(deoptimization_literals_.length(), TENURED);
  for (int i = 0; i < deoptimization_literals_.length(); i++) {
    literals->set(i, *deoptimization_literals_[i]);
  }
  data->SetLiteralArray(*literals);

  data->SetOsrAstId(Smi::FromInt(info_->osr_ast_id()));
  data->SetOsrPcOffset(Smi::FromInt(osr_pc_offset_));

  // Populate the deoptimization entries.
  for (int i = 0; i < length; i++) {
    LEnvironment* env = deoptimizations_[i];
    data->SetAstId(i, Smi::FromInt(env->ast_id()));
    data->SetTranslationIndex(i, Smi::FromInt(env->translation_index()));
    data->SetArgumentsStackHeight(i,
                                  Smi::FromInt(env->arguments_stack_height()));
  }
  code->set_deoptimization_data(*data);
}


int LCodeGen::DefineDeoptimizationLiteral(Handle<Object> literal) {
  int result = deoptimization_literals_.length();
  for (int i = 0; i < deoptimization_literals_.length(); ++i) {
    if (deoptimization_literals_[i].is_identical_to(literal)) return i;
  }
  deoptimization_literals_.Add(literal);
  return result;
}


void LCodeGen::PopulateDeoptimizationLiteralsWithInlinedFunctions() {
  ASSERT(deoptimization_literals_.length() == 0);

  const ZoneList<Handle<JSFunction> >* inlined_closures =
      chunk()->inlined_closures();

  for (int i = 0, length = inlined_closures->length();
       i < length;
       i++) {
    DefineDeoptimizationLiteral(inlined_closures->at(i));
  }

  inlined_function_count_ = deoptimization_literals_.length();
}


void LCodeGen::RecordSafepoint(
    LPointerMap* pointers,
    Safepoint::Kind kind,
    int arguments,
    int deoptimization_index) {
  ASSERT(expected_safepoint_kind_ == kind);

  const ZoneList<LOperand*>* operands = pointers->operands();
  Safepoint safepoint = safepoints_.DefineSafepoint(masm(),
      kind, arguments, deoptimization_index);
  for (int i = 0; i < operands->length(); i++) {
    LOperand* pointer = operands->at(i);
    if (pointer->IsStackSlot()) {
      safepoint.DefinePointerSlot(pointer->index());
    } else if (pointer->IsRegister() && (kind & Safepoint::kWithRegisters)) {
      safepoint.DefinePointerRegister(ToRegister(pointer));
    }
  }
  if (kind & Safepoint::kWithRegisters) {
    // Register cp always contains a pointer to the context.
    safepoint.DefinePointerRegister(cp);
  }
}


void LCodeGen::RecordSafepoint(LPointerMap* pointers,
                               int deoptimization_index) {
  RecordSafepoint(pointers, Safepoint::kSimple, 0, deoptimization_index);
}


void LCodeGen::RecordSafepoint(int deoptimization_index) {
  LPointerMap empty_pointers(RelocInfo::kNoPosition);
  RecordSafepoint(&empty_pointers, deoptimization_index);
}


void LCodeGen::RecordSafepointWithRegisters(LPointerMap* pointers,
                                            int arguments,
                                            int deoptimization_index) {
  RecordSafepoint(pointers, Safepoint::kWithRegisters, arguments,
      deoptimization_index);
}


void LCodeGen::RecordSafepointWithRegistersAndDoubles(
    LPointerMap* pointers,
    int arguments,
    int deoptimization_index) {
  RecordSafepoint(pointers, Safepoint::kWithRegistersAndDoubles, arguments,
      deoptimization_index);
}


void LCodeGen::RecordPosition(int position) {
  if (position == RelocInfo::kNoPosition) return;
  masm()->positions_recorder()->RecordPosition(position);
}


void LCodeGen::DoLabel(LLabel* label) {
  if (label->is_loop_header()) {
    Comment(";;; B%d - LOOP entry", label->block_id());
  } else {
    Comment(";;; B%d", label->block_id());
  }
  __ bind(label->label());
  current_block_ = label->block_id();
  DoGap(label);
}


void LCodeGen::DoParallelMove(LParallelMove* move) {
  resolver_.Resolve(move);
}


void LCodeGen::DoGap(LGap* gap) {
  for (int i = LGap::FIRST_INNER_POSITION;
       i <= LGap::LAST_INNER_POSITION;
       i++) {
    LGap::InnerPosition inner_pos = static_cast<LGap::InnerPosition>(i);
    LParallelMove* move = gap->GetParallelMove(inner_pos);
    if (move != NULL) DoParallelMove(move);
  }

  LInstruction* next = GetNextInstruction();
  if (next != NULL && next->IsLazyBailout()) {
    int pc = masm()->pc_offset();
    safepoints_.SetPcAfterGap(pc);
  }
}


void LCodeGen::DoInstructionGap(LInstructionGap* instr) {
  DoGap(instr);
}


void LCodeGen::DoParameter(LParameter* instr) {
  // Nothing to do.
}


void LCodeGen::DoCallStub(LCallStub* instr) {
  ASSERT(ToRegister(instr->result()).is(v0));
  switch (instr->hydrogen()->major_key()) {
    case CodeStub::RegExpConstructResult: {
      RegExpConstructResultStub stub;
      CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::RegExpExec: {
      RegExpExecStub stub;
      CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::SubString: {
      SubStringStub stub;
      CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::NumberToString: {
      NumberToStringStub stub;
      CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::StringAdd: {
      StringAddStub stub(NO_STRING_ADD_FLAGS);
      CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::StringCompare: {
      StringCompareStub stub;
      CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
      break;
    }
    case CodeStub::TranscendentalCache: {
      __ lw(a0, MemOperand(sp, 0));
      TranscendentalCacheStub stub(instr->transcendental_type(),
                                   TranscendentalCacheStub::TAGGED);
      CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
      break;
    }
    default:
      UNREACHABLE();
  }
}


void LCodeGen::DoUnknownOSRValue(LUnknownOSRValue* instr) {
  // Nothing to do.
}


void LCodeGen::DoModI(LModI* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoDivI(LDivI* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


template<int T>
void LCodeGen::DoDeferredBinaryOpStub(LTemplateInstruction<1, 2, T>* instr,
                                      Token::Value op) {
  Register left = ToRegister(instr->InputAt(0));
  Register right = ToRegister(instr->InputAt(1));

  PushSafepointRegistersScope scope(this, Safepoint::kWithRegistersAndDoubles);
  // Move left to a1 and right to a0 for the stub call.
  if (left.is(a1)) {
    __ Move(a0, right);
  } else if (left.is(a0) && right.is(a1)) {
    __ Swap(a0, a1, a2);
  } else if (left.is(a0)) {
    ASSERT(!right.is(a1));
    __ mov(a1, a0);
    __ mov(a0, right);
  } else {
    ASSERT(!left.is(a0) && !right.is(a0));
    __ mov(a0, right);
    __ mov(a1, left);
  }
  BinaryOpStub stub(op, OVERWRITE_LEFT);
  __ CallStub(&stub);
  RecordSafepointWithRegistersAndDoubles(instr->pointer_map(),
                                         0,
                                         Safepoint::kNoDeoptimizationIndex);
  // Overwrite the stored value of v0 with the result of the stub.
  // TODO(plind): validate this is correct.....
  __ StoreToSafepointRegistersAndDoublesSlot(v0, v0);
}


void LCodeGen::DoMulI(LMulI* instr) {
  ASSERT(instr->result()->Equals(instr->InputAt(0)));
  Register scratch = scratch0();
  Register result = ToRegister(instr->result());
  Register left = ToRegister(instr->InputAt(0));
  LOperand* right_op = instr->InputAt(1);

  bool can_overflow = instr->hydrogen()->CheckFlag(HValue::kCanOverflow);
  bool bailout_on_minus_zero =
    instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero);

  if (right_op->IsConstantOperand() && !can_overflow) {
    // Use optimized code for specific constants.
    int32_t constant = ToInteger32(LConstantOperand::cast(right_op));

    if (bailout_on_minus_zero && (constant < 0)) {
      // The case of a null constant will be handled separately.
      // If constant is negative and left is null, the result should be -0.
      DeoptimizeIf(eq, instr->environment(), left, Operand(zero_reg));
    }

    switch (constant) {
      case -1:
        __ Subu(result, zero_reg, left);
        break;
      case 0:
        if (bailout_on_minus_zero) {
          // If left is strictly negative and the constant is null, the
          // result is -0. Deoptimize if required, otherwise return 0.
          DeoptimizeIf(lt, instr->environment(), left, Operand(zero_reg));
        }
        __ mov(result, zero_reg);
        break;
      case 1:
        // Nothing to do.
        break;
      default:
        // Multiplying by powers of two and powers of two plus or minus
        // one can be done faster with shifted operands.
        // For other constants we emit standard code.
        int32_t mask = constant >> 31;
        uint32_t constant_abs = (constant + mask) ^ mask;

        if (IsPowerOf2(constant_abs) ||
            IsPowerOf2(constant_abs - 1) ||
            IsPowerOf2(constant_abs + 1)) {
          if (IsPowerOf2(constant_abs)) {
            int32_t shift = WhichPowerOf2(constant_abs);
            __ sll(result, left, shift);
          } else if (IsPowerOf2(constant_abs - 1)) {
            int32_t shift = WhichPowerOf2(constant_abs - 1);
            __ sll(result, left, shift);
            __ Addu(result, result, left);
          } else if (IsPowerOf2(constant_abs + 1)) {
            int32_t shift = WhichPowerOf2(constant_abs + 1);
            __ sll(result, left, shift);
            __ Subu(result, result, left);
          }

          // Correct the sign of the result is the constant is negative.
          if (constant < 0)  {
            __ Subu(result, zero_reg, result);
          }

        } else {
          // Generate standard code.
          __ li(at, constant);
          __ mul(result, left, at);
        }
    }

  } else {
    Register right = EmitLoadRegister(right_op, scratch);
    if (bailout_on_minus_zero) {
      __ Or(ToRegister(instr->TempAt(0)), left, right);
    }

    if (can_overflow) {
      // hi:lo = left * right.
      __ mult(left, right);
      __ mfhi(scratch);
      __ mflo(result);
      __ sra(at, result, 31);
      // TODO(kalmard): is this correct? Shouldn't we specifically compare both
      // sides with zero?
      DeoptimizeIf(ne, instr->environment(), scratch, Operand(at));
    } else {
      __ mul(result, left, right);
    }

    if (bailout_on_minus_zero) {
      // Bail out if the result is supposed to be negative zero.
      Label done;
      __ Branch(&done, ne, result, Operand(zero_reg));
      DeoptimizeIf(lt,
                   instr->environment(),
                   ToRegister(instr->TempAt(0)),
                   Operand(zero_reg));
      __ bind(&done);
    }
  }
}


void LCodeGen::DoBitI(LBitI* instr) {
  LOperand* left = instr->InputAt(0);
  LOperand* right = instr->InputAt(1);
  ASSERT(left->Equals(instr->result()));
  ASSERT(left->IsRegister());
  Register result = ToRegister(left);
  Operand right_operand(no_reg);

  if (right->IsStackSlot() || right->IsArgument()) {
    Register right_reg = EmitLoadRegister(right, at);
    right_operand = Operand(right_reg);
  } else {
    ASSERT(right->IsRegister() || right->IsConstantOperand());
    right_operand = ToOperand(right);
  }

  switch (instr->op()) {
    case Token::BIT_AND:
      __ And(result, ToRegister(left), right_operand);
      break;
    case Token::BIT_OR:
      __ Or(result, ToRegister(left), right_operand);
      break;
    case Token::BIT_XOR:
      __ Xor(result, ToRegister(left), right_operand);
      break;
    default:
      UNREACHABLE();
      break;
  }
}


void LCodeGen::DoShiftI(LShiftI* instr) {
  Register scratch = scratch0();
  LOperand* left = instr->InputAt(0);
  LOperand* right = instr->InputAt(1);
  ASSERT(left->Equals(instr->result()));
  ASSERT(left->IsRegister());
  Register result = ToRegister(left);
  if (right->IsRegister()) {
    // Mask the right operand.
    // TODO(kalmard): This is probably not needed on MIPS, at least not in all
    // cases.
    __ And(scratch, ToRegister(right), Operand(0x1F));
    switch (instr->op()) {
      case Token::SAR:
        __ srav(result, result, scratch);
        break;
      case Token::SHR:
        __ srlv(result, result, scratch);
        if (instr->can_deopt()) {
          DeoptimizeIf(lt, instr->environment(), result, Operand(zero_reg));
        }
        break;
      case Token::SHL:
        __ sllv(result, result, scratch);
        break;
      default:
        UNREACHABLE();
        break;
    }
  } else {
    int value = ToInteger32(LConstantOperand::cast(right));
    uint8_t shift_count = static_cast<uint8_t>(value & 0x1F);
    switch (instr->op()) {
      case Token::SAR:
        if (shift_count != 0) {
          __ sra(result, result, shift_count);
        }
        break;
      case Token::SHR:
        if (shift_count == 0 && instr->can_deopt()) {
          __ And(at, result, Operand(0x80000000));
          DeoptimizeIf(ne, instr->environment(), at, Operand(zero_reg));
        } else {
          __ srl(result, result, shift_count);
        }
        break;
      case Token::SHL:
        if (shift_count != 0) {
          __ sll(result, result, shift_count);
        }
        break;
      default:
        UNREACHABLE();
        break;
    }
  }
}


void LCodeGen::DoSubI(LSubI* instr) {
  LOperand* left = instr->InputAt(0);
  LOperand* right = instr->InputAt(1);
  ASSERT(left->Equals(instr->result()));
  bool can_overflow = instr->hydrogen()->CheckFlag(HValue::kCanOverflow);

  if (!can_overflow) {
    if (right->IsStackSlot() || right->IsArgument()) {
      Register right_reg = EmitLoadRegister(right, at);
      __ Subu(ToRegister(left), ToRegister(left), Operand(right_reg));
    } else {
      ASSERT(right->IsRegister() || right->IsConstantOperand());
      __ Subu(ToRegister(left), ToRegister(left), ToOperand(right));
    }
  } else {  // can_overflow.
    Register overflow = scratch0();
    Register scratch = scratch1();
    if (right->IsStackSlot() ||
        right->IsArgument() ||
        right->IsConstantOperand()) {
      Register right_reg = EmitLoadRegister(right, scratch);
      __ SubuAndCheckForOverflow(ToRegister(left),
                                 ToRegister(left),
                                 right_reg,
                                 overflow);  // Reg at also used as scratch.
    } else {
      ASSERT(right->IsRegister());
      // Due to overflow check macros not supporting constant operands,
      // handling the IsConstantOperand case was moved to prev if clause.
      __ SubuAndCheckForOverflow(ToRegister(left),
                                 ToRegister(left),
                                 ToRegister(right),
                                 overflow);  // Reg at also used as scratch.
    }
    DeoptimizeIf(lt, instr->environment(), overflow, Operand(zero_reg));
  }
}


void LCodeGen::DoConstantI(LConstantI* instr) {
  ASSERT(instr->result()->IsRegister());
  __ li(ToRegister(instr->result()), Operand(instr->value()));
}


void LCodeGen::DoConstantD(LConstantD* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoConstantT(LConstantT* instr) {
  ASSERT(instr->result()->IsRegister());
  __ li(ToRegister(instr->result()), Operand(instr->value()));
}


void LCodeGen::DoJSArrayLength(LJSArrayLength* instr) {
  Register result = ToRegister(instr->result());
  Register array = ToRegister(instr->InputAt(0));
  __ lw(result, FieldMemOperand(array, JSArray::kLengthOffset));
}


void LCodeGen::DoExternalArrayLength(LExternalArrayLength* instr) {
  Register result = ToRegister(instr->result());
  Register array = ToRegister(instr->InputAt(0));
  __ lw(result, FieldMemOperand(array, ExternalArray::kLengthOffset));
}


void LCodeGen::DoFixedArrayLength(LFixedArrayLength* instr) {
  Register result = ToRegister(instr->result());
  Register array = ToRegister(instr->InputAt(0));
  __ lw(result, FieldMemOperand(array, FixedArray::kLengthOffset));
}


void LCodeGen::DoValueOf(LValueOf* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoBitNotI(LBitNotI* instr) {
  LOperand* input = instr->InputAt(0);
  ASSERT(input->Equals(instr->result()));
  __ Nor(ToRegister(input), zero_reg, Operand(ToRegister(input)));
}


void LCodeGen::DoThrow(LThrow* instr) {
  Register input_reg = EmitLoadRegister(instr->InputAt(0), at);
  __ push(input_reg);
  CallRuntime(Runtime::kThrow, 1, instr);

  if (FLAG_debug_code) {
    __ stop("Unreachable code.");
  }
}


void LCodeGen::DoAddI(LAddI* instr) {
  LOperand* left = instr->InputAt(0);
  LOperand* right = instr->InputAt(1);
  ASSERT(left->Equals(instr->result()));
  bool can_overflow = instr->hydrogen()->CheckFlag(HValue::kCanOverflow);

  if (!can_overflow) {
    if (right->IsStackSlot() || right->IsArgument()) {
      Register right_reg = EmitLoadRegister(right, at);
      __ Addu(ToRegister(left), ToRegister(left), Operand(right_reg));
    } else {
      ASSERT(right->IsRegister() || right->IsConstantOperand());
      __ Addu(ToRegister(left), ToRegister(left), ToOperand(right));
    }
  } else {  // can_overflow.
    Register overflow = scratch0();
    Register scratch = scratch1();
    if (right->IsStackSlot() ||
        right->IsArgument() ||
        right->IsConstantOperand()) {
      Register right_reg = EmitLoadRegister(right, scratch);
      __ AdduAndCheckForOverflow(ToRegister(left),
                                 ToRegister(left),
                                 right_reg,
                                 overflow);  // Reg at also used as scratch.
    } else {
      ASSERT(right->IsRegister());
      // Due to overflow check macros not supporting constant operands,
      // handling the IsConstantOperand case was moved to prev if clause.
      __ AdduAndCheckForOverflow(ToRegister(left),
                                 ToRegister(left),
                                 ToRegister(right),
                                 overflow);  // Reg at also used as scratch.
    }
    DeoptimizeIf(lt, instr->environment(), overflow, Operand(zero_reg));
  }
}


void LCodeGen::DoArithmeticD(LArithmeticD* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoArithmeticT(LArithmeticT* instr) {
  ASSERT(ToRegister(instr->InputAt(0)).is(a1));
  ASSERT(ToRegister(instr->InputAt(1)).is(a0));
  ASSERT(ToRegister(instr->result()).is(v0));

  BinaryOpStub stub(instr->op(), NO_OVERWRITE);
  CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
}


int LCodeGen::GetNextEmittedBlock(int block) {
  for (int i = block + 1; i < graph()->blocks()->length(); ++i) {
    LLabel* label = chunk_->GetLabel(i);
    if (!label->HasReplacement()) return i;
  }
  return -1;
}


void LCodeGen::EmitBranch(int left_block, int right_block,
                          Condition cc, Register src1, const Operand& src2) {
  int next_block = GetNextEmittedBlock(current_block_);
  right_block = chunk_->LookupDestination(right_block);
  left_block = chunk_->LookupDestination(left_block);
  if (right_block == left_block) {
    EmitGoto(left_block);
  } else if (left_block == next_block) {
    __ Branch(chunk_->GetAssemblyLabel(right_block),
              NegateCondition(cc), src1, src2);
  } else if (right_block == next_block) {
    __ Branch(chunk_->GetAssemblyLabel(left_block), cc, src1, src2);
  } else {
    __ Branch(chunk_->GetAssemblyLabel(left_block), cc, src1, src2);
    __ Branch(chunk_->GetAssemblyLabel(right_block));
  }
}


// This is just a wrapper that sets up a proper EmitBranchF based on the non-fpu
// condition code in cc.
void LCodeGen::EmitBranchF(int left_block, int right_block,
                           Condition cc, FPURegister src1, FPURegister src2) {
  right_block = chunk_->LookupDestination(right_block);
  left_block = chunk_->LookupDestination(left_block);

  bool should_negate = false;
  FPUCondition cond = ToFPUCondition(cc, should_negate);
  ASSERT(cond != kNoFPUCondition);
  if (should_negate) {
    EmitBranchF(right_block, left_block, cond, src1, src2);
  } else {
    EmitBranchF(left_block, right_block, cond, src1, src2);
  }
}


void LCodeGen::EmitBranchF(int left_block,
                           int right_block,
                           FPUCondition cc,
                           FPURegister src1,
                           FPURegister src2) {
  int next_block = GetNextEmittedBlock(current_block_);
  right_block = chunk_->LookupDestination(right_block);
  left_block = chunk_->LookupDestination(left_block);
  if (right_block == left_block) {
    EmitGoto(left_block);
  } else if (left_block == next_block) {
    // Note: the labels are reversed here.
    __ BranchF(NULL, chunk_->GetAssemblyLabel(right_block), cc, src1, src2);
  } else if (right_block == next_block) {
    __ BranchF(chunk_->GetAssemblyLabel(left_block), NULL, cc, src1, src2);
  } else {
    __ BranchF(chunk_->GetAssemblyLabel(left_block),
               chunk_->GetAssemblyLabel(right_block),cc, src1, src2);
  }
}


void LCodeGen::DoBranch(LBranch* instr) {
  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  Representation r = instr->hydrogen()->representation();
  if (r.IsInteger32()) {
    Register reg = ToRegister(instr->InputAt(0));
    EmitBranch(true_block, false_block, ne, reg, Operand(zero_reg));
  } else if (r.IsDouble()) {
    FPURegister scratch = double_scratch0();
    __ Move(scratch, zero_reg, zero_reg);
    DoubleRegister reg = ToDoubleRegister(instr->InputAt(0));
    // Test the double value. Zero and NaN are false.
    EmitBranchF(false_block, true_block, UEQ, reg, scratch);
  } else {
    ASSERT(r.IsTagged());

    // TODO(plind): this function VERY QUICKLY written, and POORLY tested. Review it.....

    Register reg = ToRegister(instr->InputAt(0));
    if (instr->hydrogen()->type().IsBoolean()) {
      __ LoadRoot(at, Heap::kTrueValueRootIndex);
      EmitBranch(true_block, false_block, eq, reg, Operand(at));
    } else {
      Label* true_label = chunk_->GetAssemblyLabel(true_block);
      Label* false_label = chunk_->GetAssemblyLabel(false_block);

      __ LoadRoot(at, Heap::kUndefinedValueRootIndex);
      __ Branch(false_label, eq, reg, Operand(at));
      __ LoadRoot(at, Heap::kTrueValueRootIndex);
      __ Branch(true_label, eq, reg, Operand(at));
      __ LoadRoot(at, Heap::kFalseValueRootIndex);
      __ Branch(false_label, eq, reg, Operand(at));
      __ Branch(false_label, eq, reg, Operand(zero_reg));
      __ JumpIfSmi(reg, true_label);

      // Test double values. Zero and NaN are false.

      // TODO(plind): I think this is optimization, and stub below
      // can handle everything.
      // TODO(kalmard): This could be ported to MIPS using BranchF but would
      // require two double scratches and lithium only has one. Can we get
      // another one without too much hassle?

      Label call_stub;
      // DoubleRegister dbl_scratch = d0;
      // Register scratch = scratch0();
      // __ ldr(scratch, FieldMemOperand(reg, HeapObject::kMapOffset));
      // __ LoadRoot(ip, Heap::kHeapNumberMapRootIndex);
      // __ cmp(scratch, Operand(ip));
      // __ b(ne, &call_stub);
      // __ sub(ip, reg, Operand(kHeapObjectTag));
      // __ vldr(dbl_scratch, ip, HeapNumber::kValueOffset);
      // __ VFPCompareAndLoadFlags(dbl_scratch, 0.0, scratch);
      // __ tst(scratch, Operand(kVFPZConditionFlagBit | kVFPVConditionFlagBit));
      // __ b(ne, false_label);
      // __ b(true_label);

      // The conversion stub doesn't cause garbage collections so it's
      // safe to not record a safepoint after the call.
      __ bind(&call_stub);
      ToBooleanStub stub(reg);
      RegList saved_regs = kJSCallerSaved | kCalleeSaved;
      __ MultiPush(saved_regs);
      __ CallStub(&stub);
      __ mov(at, v0);  // Save result.
      __ MultiPop(saved_regs);
      EmitBranch(true_block, false_block, ne, at, Operand(zero_reg));
    }
  }
}


void LCodeGen::EmitGoto(int block, LDeferredCode* deferred_stack_check) {
  block = chunk_->LookupDestination(block);
  int next_block = GetNextEmittedBlock(current_block_);
  if (block != next_block) {
    // Perform stack overflow check if this goto needs it before jumping.
    if (deferred_stack_check != NULL) {
      __ LoadRoot(at, Heap::kStackLimitRootIndex);
      __ Branch(chunk_->GetAssemblyLabel(block), hs, sp, Operand(at));
      __ jmp(deferred_stack_check->entry());
      deferred_stack_check->SetExit(chunk_->GetAssemblyLabel(block));
    } else {
      __ jmp(chunk_->GetAssemblyLabel(block));
    }
  }
}


void LCodeGen::DoDeferredStackCheck(LGoto* instr) {
  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);
  CallRuntimeFromDeferred(Runtime::kStackGuard, 0, instr);
}


void LCodeGen::DoGoto(LGoto* instr) {
  class DeferredStackCheck: public LDeferredCode {
   public:
    DeferredStackCheck(LCodeGen* codegen, LGoto* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredStackCheck(instr_); }
   private:
    LGoto* instr_;
  };

  DeferredStackCheck* deferred = NULL;
  if (instr->include_stack_check()) {
    deferred = new DeferredStackCheck(this, instr);
  }
  EmitGoto(instr->block_id(), deferred);
}


Condition LCodeGen::TokenToCondition(Token::Value op, bool is_unsigned) {
  Condition cond = kNoCondition;
  switch (op) {
    case Token::EQ:
    case Token::EQ_STRICT:
      cond = eq;
      break;
    case Token::LT:
      cond = is_unsigned ? lo : lt;
      break;
    case Token::GT:
      cond = is_unsigned ? hi : gt;
      break;
    case Token::LTE:
      cond = is_unsigned ? ls : le;
      break;
    case Token::GTE:
      cond = is_unsigned ? hs : ge;
      break;
    case Token::IN:
    case Token::INSTANCEOF:
    default:
      UNREACHABLE();
  }
  return cond;
}


void LCodeGen::EmitCmpI(LOperand* left, LOperand* right) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoCmpID(LCmpID* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoCmpIDAndBranch(LCmpIDAndBranch* instr) {
  LOperand* left = instr->InputAt(0);
  LOperand* right = instr->InputAt(1);
  int false_block = chunk_->LookupDestination(instr->false_block_id());
  int true_block = chunk_->LookupDestination(instr->true_block_id());

  Condition cc = TokenToCondition(instr->op(), instr->is_double());

  if (instr->is_double()) {
    // Compare left and right as doubles and load the
    // resulting flags into the normal status register.
    FPURegister left_reg = ToDoubleRegister(left);
    FPURegister right_reg = ToDoubleRegister(right);

    // If a NaN is involved, i.e. the result is unordered,
    // jump to false block label.
    // TODO(kalmard): This may not be necessary, EmitBranchF probably jumps to
    // false on NaN.

    __ BranchF(chunk_->GetAssemblyLabel(false_block), NULL, CHECK_NAN,
               left_reg, right_reg);

    EmitBranchF(true_block, false_block, cc, left_reg, right_reg);
  } else {
    // EmitCmpI cannot be used on MIPS.
    // EmitCmpI(left, right);
    EmitBranch(true_block,
               false_block,
               cc,
               ToRegister(left),
               Operand(ToRegister(right)));
  }
}


void LCodeGen::DoCmpJSObjectEq(LCmpJSObjectEq* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // Register left = ToRegister(instr->InputAt(0));
  // Register right = ToRegister(instr->InputAt(1));
  // Register result = ToRegister(instr->result());
  //
  // __ cmp(left, Operand(right));
  // __ LoadRoot(result, Heap::kTrueValueRootIndex, eq);
  // __ LoadRoot(result, Heap::kFalseValueRootIndex, ne);
}


void LCodeGen::DoCmpJSObjectEqAndBranch(LCmpJSObjectEqAndBranch* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // Register left = ToRegister(instr->InputAt(0));
  // Register right = ToRegister(instr->InputAt(1));
  // int false_block = chunk_->LookupDestination(instr->false_block_id());
  // int true_block = chunk_->LookupDestination(instr->true_block_id());
  //
  // __ cmp(left, Operand(right));
  // EmitBranch(true_block, false_block, eq);
}


void LCodeGen::DoCmpSymbolEq(LCmpSymbolEq* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // Register left = ToRegister(instr->InputAt(0));
  // Register right = ToRegister(instr->InputAt(1));
  // Register result = ToRegister(instr->result());
  //
  // __ cmp(left, Operand(right));
  // __ LoadRoot(result, Heap::kTrueValueRootIndex, eq);
  // __ LoadRoot(result, Heap::kFalseValueRootIndex, ne);
}


void LCodeGen::DoCmpSymbolEqAndBranch(LCmpSymbolEqAndBranch* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // Register left = ToRegister(instr->InputAt(0));
  // Register right = ToRegister(instr->InputAt(1));
  // int false_block = chunk_->LookupDestination(instr->false_block_id());
  // int true_block = chunk_->LookupDestination(instr->true_block_id());
  //
  // __ cmp(left, Operand(right));
  // EmitBranch(true_block, false_block, eq);
}


void LCodeGen::DoIsNull(LIsNull* instr) {
  Register reg = ToRegister(instr->InputAt(0));
  Register result = ToRegister(instr->result());

  // TODO(plind): maybe too conservative here .... avoiding potential problem
  // of conditional LoadRoot overwriting input regs.
  Register input;
  if (result.is(reg)) {
    input = scratch0();
    __ mov(input, reg);
  } else {
    input = reg;
  }
  __ LoadRoot(at, Heap::kNullValueRootIndex);
  if (instr->is_strict()) {
    __ LoadRoot(result, Heap::kTrueValueRootIndex, eq, input, Operand(at));
    __ LoadRoot(result, Heap::kFalseValueRootIndex, ne, input, Operand(at));
  } else {
    Label true_value, false_value, done;
    __ Branch(&true_value, eq, reg, Operand(at));
    __ LoadRoot(at, Heap::kUndefinedValueRootIndex);
    __ Branch(&true_value, eq, reg, Operand(at));
    __ andi(at, reg, kSmiTagMask);
    __ Branch(&false_value, eq, at, Operand(zero_reg));

    // Check for undetectable objects by looking in the bit field in
    // the map. The object has already been smi checked.
    Register scratch = result;
    __ lw(scratch, FieldMemOperand(reg, HeapObject::kMapOffset));
    __ lbu(scratch, FieldMemOperand(scratch, Map::kBitFieldOffset));
    __ And(at, scratch, Operand(1 << Map::kIsUndetectable));
    __ Branch(&true_value, ne, at, Operand(zero_reg));

    __ bind(&false_value);
    __ LoadRoot(result, Heap::kFalseValueRootIndex);
    __ jmp(&done);
    __ bind(&true_value);
    __ LoadRoot(result, Heap::kTrueValueRootIndex);
    __ bind(&done);
  }
}


void LCodeGen::DoIsNullAndBranch(LIsNullAndBranch* instr) {
  Register scratch = scratch0();
  Register reg = ToRegister(instr->InputAt(0));

  // TODO(fsc): If the expression is known to be a smi, then it's
  // definitely not null. Jump to the false block.

  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  __ LoadRoot(at, Heap::kNullValueRootIndex);
  if (instr->is_strict()) {
    EmitBranch(true_block, false_block, eq, reg, Operand(at));
  } else {
    Label* true_label = chunk_->GetAssemblyLabel(true_block);
    Label* false_label = chunk_->GetAssemblyLabel(false_block);
    __ Branch(USE_DELAY_SLOT, true_label, eq, reg, Operand(at));
    __ LoadRoot(at, Heap::kUndefinedValueRootIndex);  // In the delay slot.
    __ Branch(USE_DELAY_SLOT, true_label, eq, reg, Operand(at));
    __ JumpIfSmi(reg, false_label);  // In the delay slot.
    // Check for undetectable objects by looking in the bit field in
    // the map. The object has already been smi checked.
    __ lw(scratch, FieldMemOperand(reg, HeapObject::kMapOffset));
    __ lbu(scratch, FieldMemOperand(scratch, Map::kBitFieldOffset));
    __ And(scratch, scratch, 1 << Map::kIsUndetectable);
    EmitBranch(true_block, false_block, ne, scratch, Operand(zero_reg));
  }
}


Condition LCodeGen::EmitIsObject(Register input,
                                 Register temp1,
                                 Register temp2,
                                 Label* is_not_object,
                                 Label* is_object) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  return al;
}


void LCodeGen::DoIsObject(LIsObject* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoIsObjectAndBranch(LIsObjectAndBranch* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoIsSmi(LIsSmi* instr) {
  ASSERT(instr->hydrogen()->value()->representation().IsTagged());
  Register result = ToRegister(instr->result());
  Register input_reg = EmitLoadRegister(instr->InputAt(0), at);
  __ LoadRoot(result, Heap::kTrueValueRootIndex);
  Label done;
  __ JumpIfSmi(input_reg, &done);
  __ LoadRoot(result, Heap::kFalseValueRootIndex);
  __ bind(&done);
}


void LCodeGen::DoIsSmiAndBranch(LIsSmiAndBranch* instr) {
  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  Register input_reg = EmitLoadRegister(instr->InputAt(0), at);
  __ And(at, input_reg, kSmiTagMask);
  EmitBranch(true_block, false_block, eq, at, Operand(zero_reg));
}


void LCodeGen::DoIsUndetectable(LIsUndetectable* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoIsUndetectableAndBranch(LIsUndetectableAndBranch* instr) {
  Register input = ToRegister(instr->InputAt(0));
  Register temp = ToRegister(instr->TempAt(0));

  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  __ JumpIfSmi(input, chunk_->GetAssemblyLabel(false_block));
  __ lw(temp, FieldMemOperand(input, HeapObject::kMapOffset));
  __ lbu(temp, FieldMemOperand(temp, Map::kBitFieldOffset));
  __ And(at, temp, Operand(1 << Map::kIsUndetectable));
  EmitBranch(true_block, false_block, ne, at, Operand(zero_reg));
}


static InstanceType TestType(HHasInstanceType* instr) {
  InstanceType from = instr->from();
  InstanceType to = instr->to();
  if (from == FIRST_TYPE) return to;
  ASSERT(from == to || to == LAST_TYPE);
  return from;
}


static Condition BranchCondition(HHasInstanceType* instr) {
  InstanceType from = instr->from();
  InstanceType to = instr->to();
  if (from == to) return eq;
  if (to == LAST_TYPE) return hs;
  if (from == FIRST_TYPE) return ls;
  UNREACHABLE();
  return eq;
}


void LCodeGen::DoHasInstanceType(LHasInstanceType* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // Register input = ToRegister(instr->InputAt(0));
  // Register result = ToRegister(instr->result());
  //
  // ASSERT(instr->hydrogen()->value()->representation().IsTagged());
  // Label done;
  // __ tst(input, Operand(kSmiTagMask));
  // __ LoadRoot(result, Heap::kFalseValueRootIndex, eq);
  // __ b(eq, &done);
  // __ CompareObjectType(input, result, result, TestType(instr->hydrogen()));
  // Condition cond = BranchCondition(instr->hydrogen());
  // __ LoadRoot(result, Heap::kTrueValueRootIndex, cond);
  // __ LoadRoot(result, Heap::kFalseValueRootIndex, NegateCondition(cond));
  // __ bind(&done);
}


void LCodeGen::DoHasInstanceTypeAndBranch(LHasInstanceTypeAndBranch* instr) {
  Register scratch = scratch0();
  Register input = ToRegister(instr->InputAt(0));

  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  Label* false_label = chunk_->GetAssemblyLabel(false_block);

  __ JumpIfSmi(input, false_label);

  __ GetObjectType(input, scratch, scratch);
  EmitBranch(true_block,
             false_block,
             BranchCondition(instr->hydrogen()),
             scratch,
             Operand(TestType(instr->hydrogen())));
}


void LCodeGen::DoGetCachedArrayIndex(LGetCachedArrayIndex* instr) {
  Register input = ToRegister(instr->InputAt(0));
  Register result = ToRegister(instr->result());

  if (FLAG_debug_code) {
    __ AbortIfNotString(input);
  }

  __ lw(result, FieldMemOperand(input, String::kHashFieldOffset));
  __ IndexFromHash(result, result);
}


void LCodeGen::DoHasCachedArrayIndex(LHasCachedArrayIndex* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // Register input = ToRegister(instr->InputAt(0));
  // Register result = ToRegister(instr->result());
  // Register scratch = scratch0();
  //
  // ASSERT(instr->hydrogen()->value()->representation().IsTagged());
  // __ ldr(scratch,
  //        FieldMemOperand(input, String::kHashFieldOffset));
  // __ tst(scratch, Operand(String::kContainsCachedArrayIndexMask));
  // __ LoadRoot(result, Heap::kTrueValueRootIndex, eq);
  // __ LoadRoot(result, Heap::kFalseValueRootIndex, ne);
}


void LCodeGen::DoHasCachedArrayIndexAndBranch(
    LHasCachedArrayIndexAndBranch* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // Register input = ToRegister(instr->InputAt(0));
  // Register scratch = scratch0();
  //
  // int true_block = chunk_->LookupDestination(instr->true_block_id());
  // int false_block = chunk_->LookupDestination(instr->false_block_id());
  //
  // __ ldr(scratch,
  //        FieldMemOperand(input, String::kHashFieldOffset));
  // __ tst(scratch, Operand(String::kContainsCachedArrayIndexMask));
  // EmitBranch(true_block, false_block, eq);
}


// Branches to a label or falls through with the answer in flags.  Trashes
// the temp registers, but not the input.  Only input and temp2 may alias.
void LCodeGen::EmitClassOfTest(Label* is_true,
                               Label* is_false,
                               Handle<String>class_name,
                               Register input,
                               Register temp,
                               Register temp2) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoClassOfTest(LClassOfTest* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // Register input = ToRegister(instr->InputAt(0));
  // Register result = ToRegister(instr->result());
  // ASSERT(input.is(result));
  // Handle<String> class_name = instr->hydrogen()->class_name();
  //
  // Label done, is_true, is_false;
  //
  // EmitClassOfTest(&is_true, &is_false, class_name, input, scratch0(), input);
  // __ b(ne, &is_false);
  //
  // __ bind(&is_true);
  // __ LoadRoot(result, Heap::kTrueValueRootIndex);
  // __ jmp(&done);
  //
  // __ bind(&is_false);
  // __ LoadRoot(result, Heap::kFalseValueRootIndex);
  // __ bind(&done);
}


void LCodeGen::DoClassOfTestAndBranch(LClassOfTestAndBranch* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // Register input = ToRegister(instr->InputAt(0));
  // Register temp = scratch0();
  // Register temp2 = ToRegister(instr->TempAt(0));
  // Handle<String> class_name = instr->hydrogen()->class_name();
  //
  // int true_block = chunk_->LookupDestination(instr->true_block_id());
  // int false_block = chunk_->LookupDestination(instr->false_block_id());
  //
  // Label* true_label = chunk_->GetAssemblyLabel(true_block);
  // Label* false_label = chunk_->GetAssemblyLabel(false_block);
  //
  // EmitClassOfTest(true_label, false_label, class_name, input, temp, temp2);
  //
  // EmitBranch(true_block, false_block, eq);
}


void LCodeGen::DoCmpMapAndBranch(LCmpMapAndBranch* instr) {
  Register reg = ToRegister(instr->InputAt(0));
  Register temp = ToRegister(instr->TempAt(0));
  int true_block = instr->true_block_id();
  int false_block = instr->false_block_id();

  __ lw(temp, FieldMemOperand(reg, HeapObject::kMapOffset));
  EmitBranch(true_block, false_block, eq, temp, Operand(instr->map()));
}


void LCodeGen::DoInstanceOf(LInstanceOf* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // ASSERT(ToRegister(instr->InputAt(0)).is(r0));  // Object is in r0.
  // ASSERT(ToRegister(instr->InputAt(1)).is(r1));  // Function is in r1.
  //
  // InstanceofStub stub(InstanceofStub::kArgsInRegisters);
  // CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
  //
  // __ cmp(r0, Operand(0));
  // __ mov(r0, Operand(factory()->false_value()), LeaveCC, ne);
  // __ mov(r0, Operand(factory()->true_value()), LeaveCC, eq);
}


void LCodeGen::DoInstanceOfKnownGlobal(LInstanceOfKnownGlobal* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoDeferredLInstanceOfKnownGlobal(LInstanceOfKnownGlobal* instr,
                                                Label* map_check) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


static Condition ComputeCompareCondition(Token::Value op) {
  switch (op) {
    case Token::EQ_STRICT:
    case Token::EQ:
      return eq;
    case Token::LT:
      return lt;
    case Token::GT:
      return gt;
    case Token::LTE:
      return le;
    case Token::GTE:
      return ge;
    default:
      UNREACHABLE();
      return kNoCondition;
  }
}


void LCodeGen::DoCmpT(LCmpT* instr) {
  Token::Value op = instr->op();

  Handle<Code> ic = CompareIC::GetUninitialized(op);
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
  __ nop();  // This instruction signals no smi code inlined.
  // TODO(plind): Understand what conditions will let this be patched.

  Condition condition = ComputeCompareCondition(op);
  if (op == Token::GT || op == Token::LTE) {
    condition = ReverseCondition(condition);
  }
  // TODO(plind): optimize this a bit.
  __ mov(at, v0);
  __ LoadRoot(ToRegister(instr->result()),
              Heap::kTrueValueRootIndex,
              condition,
              at,
              Operand(zero_reg));
  __ LoadRoot(ToRegister(instr->result()),
              Heap::kFalseValueRootIndex,
              NegateCondition(condition),
              at,
              Operand(zero_reg));
}


void LCodeGen::DoReturn(LReturn* instr) {
  if (FLAG_trace) {
    // Push the return value on the stack as the parameter.
    // Runtime::TraceExit returns its parameter in v0.
    __ push(v0);
    __ CallRuntime(Runtime::kTraceExit, 1);
  }
  int32_t sp_delta = (GetParameterCount() + 1) * kPointerSize;
  __ mov(sp, fp);
  __ Pop(ra, fp);
  __ Addu(sp, sp, Operand(sp_delta));
  __ Jump(ra);
}


void LCodeGen::DoLoadGlobalCell(LLoadGlobalCell* instr) {
  Register result = ToRegister(instr->result());
  __ li(at, Operand(Handle<Object>(instr->hydrogen()->cell())));
  __ lw(result, FieldMemOperand(at, JSGlobalPropertyCell::kValueOffset));
  if (instr->hydrogen()->check_hole_value()) {
    __ LoadRoot(at, Heap::kTheHoleValueRootIndex);
    DeoptimizeIf(eq, instr->environment(), result, Operand(at));
  }
}


void LCodeGen::DoLoadGlobalGeneric(LLoadGlobalGeneric* instr) {
  ASSERT(ToRegister(instr->global_object()).is(a0));
  ASSERT(ToRegister(instr->result()).is(v0));

  __ li(a2, Operand(instr->name()));
  RelocInfo::Mode mode = instr->for_typeof() ? RelocInfo::CODE_TARGET
                                             : RelocInfo::CODE_TARGET_CONTEXT;
  Handle<Code> ic = isolate()->builtins()->LoadIC_Initialize();
  CallCode(ic, mode, instr);
}


void LCodeGen::DoStoreGlobalCell(LStoreGlobalCell* instr) {
  Register value = ToRegister(instr->InputAt(0));
  Register scratch = scratch0();

  // Load the cell.
  __ li(scratch, Operand(Handle<Object>(instr->hydrogen()->cell())));

  // If the cell we are storing to contains the hole it could have
  // been deleted from the property dictionary. In that case, we need
  // to update the property details in the property dictionary to mark
  // it as no longer deleted.
  if (instr->hydrogen()->check_hole_value()) {
    Register scratch2 = ToRegister(instr->TempAt(0));
    __ lw(scratch2,
          FieldMemOperand(scratch, JSGlobalPropertyCell::kValueOffset));
    __ LoadRoot(at, Heap::kTheHoleValueRootIndex);
    DeoptimizeIf(eq, instr->environment(), scratch2, Operand(at));
  }

  // Store the value.
  __ sw(value, FieldMemOperand(scratch, JSGlobalPropertyCell::kValueOffset));
}


void LCodeGen::DoStoreGlobalGeneric(LStoreGlobalGeneric* instr) {
  ASSERT(ToRegister(instr->global_object()).is(a1));
  ASSERT(ToRegister(instr->value()).is(a0));

  __ li(a2, Operand(instr->name()));
  Handle<Code> ic = instr->strict_mode()
      ? isolate()->builtins()->StoreIC_Initialize_Strict()
      : isolate()->builtins()->StoreIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET_CONTEXT, instr);
}


void LCodeGen::DoLoadContextSlot(LLoadContextSlot* instr) {
  Register context = ToRegister(instr->context());
  Register result = ToRegister(instr->result());
  __ lw(result, ContextOperand(context, instr->slot_index()));
}


void LCodeGen::DoStoreContextSlot(LStoreContextSlot* instr) {
  Register context = ToRegister(instr->context());
  Register value = ToRegister(instr->value());
  __ sw(value, ContextOperand(context, instr->slot_index()));
  if (instr->needs_write_barrier()) {
    int offset = Context::SlotOffset(instr->slot_index());
    __ RecordWrite(context, Operand(offset), value, scratch0());
  }
}


void LCodeGen::DoLoadNamedField(LLoadNamedField* instr) {
  Register object = ToRegister(instr->InputAt(0));
  Register result = ToRegister(instr->result());
  if (instr->hydrogen()->is_in_object()) {
    __ lw(result, FieldMemOperand(object, instr->hydrogen()->offset()));
  } else {
    __ lw(result, FieldMemOperand(object, JSObject::kPropertiesOffset));
    __ lw(result, FieldMemOperand(result, instr->hydrogen()->offset()));
  }
}


void LCodeGen::EmitLoadFieldOrConstantFunction(Register result,
                                               Register object,
                                               Handle<Map> type,
                                               Handle<String> name) {
  LookupResult lookup;
  type->LookupInDescriptors(NULL, *name, &lookup);
  ASSERT(lookup.IsProperty() &&
         (lookup.type() == FIELD || lookup.type() == CONSTANT_FUNCTION));
  if (lookup.type() == FIELD) {
    int index = lookup.GetLocalFieldIndexFromMap(*type);
    int offset = index * kPointerSize;
    if (index < 0) {
      // Negative property indices are in-object properties, indexed
      // from the end of the fixed part of the object.
      __ lw(result, FieldMemOperand(object, offset + type->instance_size()));
    } else {
      // Non-negative property indices are in the properties array.
      __ lw(result, FieldMemOperand(object, JSObject::kPropertiesOffset));
      __ lw(result, FieldMemOperand(result, offset + FixedArray::kHeaderSize));
    }
  } else {
    Handle<JSFunction> function(lookup.GetConstantFunctionFromMap(*type));
    LoadHeapObject(result, Handle<HeapObject>::cast(function));
  }
}


void LCodeGen::DoLoadNamedFieldPolymorphic(LLoadNamedFieldPolymorphic* instr) {
  Register object = ToRegister(instr->object());
  Register result = ToRegister(instr->result());
  Register scratch = scratch0();
  int map_count = instr->hydrogen()->types()->length();
  Handle<String> name = instr->hydrogen()->name();
  if (map_count == 0) {
    ASSERT(instr->hydrogen()->need_generic());
    __ li(a2, Operand(name));
    Handle<Code> ic = isolate()->builtins()->LoadIC_Initialize();
    CallCode(ic, RelocInfo::CODE_TARGET, instr);
  } else {
    Label done;
    __ lw(scratch, FieldMemOperand(object, HeapObject::kMapOffset));
    for (int i = 0; i < map_count - 1; ++i) {
      Handle<Map> map = instr->hydrogen()->types()->at(i);
      Label next;
      __ Branch(&next, ne, scratch, Operand(map));
      EmitLoadFieldOrConstantFunction(result, object, map, name);
      __ Branch(&done);
      __ bind(&next);
    }
    Handle<Map> map = instr->hydrogen()->types()->last();
    if (instr->hydrogen()->need_generic()) {
      Label generic;
      __ Branch(&generic, ne, scratch, Operand(map));
      EmitLoadFieldOrConstantFunction(result, object, map, name);
      __ Branch(&done);
      __ bind(&generic);
      __ li(a2, Operand(name));
      Handle<Code> ic = isolate()->builtins()->LoadIC_Initialize();
      CallCode(ic, RelocInfo::CODE_TARGET, instr);
    } else {
      DeoptimizeIf(ne, instr->environment(), scratch, Operand(map));
      EmitLoadFieldOrConstantFunction(result, object, map, name);
    }
    __ bind(&done);
  }
}


void LCodeGen::DoLoadNamedGeneric(LLoadNamedGeneric* instr) {
  ASSERT(ToRegister(instr->object()).is(a0));
  ASSERT(ToRegister(instr->result()).is(v0));

  // Name is always in a2.
  __ li(a2, Operand(instr->name()));
  Handle<Code> ic = isolate()->builtins()->LoadIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoLoadFunctionPrototype(LLoadFunctionPrototype* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoLoadElements(LLoadElements* instr) {
  Register result = ToRegister(instr->result());
  Register input = ToRegister(instr->InputAt(0));
  Register scratch = scratch0();

  __ lw(result, FieldMemOperand(input, JSObject::kElementsOffset));
  if (FLAG_debug_code) {
    Label done;
    __ lw(scratch, FieldMemOperand(result, HeapObject::kMapOffset));
    __ LoadRoot(at, Heap::kFixedArrayMapRootIndex);
    __ Branch(USE_DELAY_SLOT, &done, eq, scratch, Operand(at));
    __ LoadRoot(at, Heap::kFixedCOWArrayMapRootIndex);  // In the delay slot.
    __ Branch(USE_DELAY_SLOT, &done, eq, scratch, Operand(at));
    __ GetObjectType(result, scratch, scratch);  // In the delay slot.
    __ Subu(scratch, scratch, Operand(FIRST_EXTERNAL_ARRAY_TYPE));
    __ Check(lo,
             "Check for fast elements failed.",
             scratch,
             Operand(kExternalArrayTypeCount));
    __ bind(&done);
  }
}


void LCodeGen::DoLoadExternalArrayPointer(
    LLoadExternalArrayPointer* instr) {
  Register to_reg = ToRegister(instr->result());
  Register from_reg  = ToRegister(instr->InputAt(0));
  __ lw(to_reg, FieldMemOperand(from_reg,
                                ExternalArray::kExternalPointerOffset));
}


void LCodeGen::DoAccessArgumentsAt(LAccessArgumentsAt* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoLoadKeyedFastElement(LLoadKeyedFastElement* instr) {
  Register elements = ToRegister(instr->elements());
  Register key = EmitLoadRegister(instr->key(), scratch0());
  Register result = ToRegister(instr->result());
  Register scratch = scratch0();
  // TODO(plind): expect this ASSERT to break on mips.......
  ASSERT(result.is(elements));

  // Load the result.
  __ sll(scratch, key, kPointerSizeLog2);  // Key indexes words.
  __ addu(scratch, elements, scratch);
  __ lw(result, FieldMemOperand(scratch, FixedArray::kHeaderSize));

  // Check for the hole value.
  if (instr->hydrogen()->RequiresHoleCheck()) {
    __ LoadRoot(scratch, Heap::kTheHoleValueRootIndex);
    DeoptimizeIf(eq, instr->environment(), result, Operand(scratch));
  }
}


void LCodeGen::DoLoadKeyedSpecializedArrayElement(
    LLoadKeyedSpecializedArrayElement* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoLoadKeyedGeneric(LLoadKeyedGeneric* instr) {
  ASSERT(ToRegister(instr->object()).is(a1));
  ASSERT(ToRegister(instr->key()).is(a0));

  Handle<Code> ic = isolate()->builtins()->KeyedLoadIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoArgumentsElements(LArgumentsElements* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoArgumentsLength(LArgumentsLength* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoApplyArguments(LApplyArguments* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoPushArgument(LPushArgument* instr) {
  LOperand* argument = instr->InputAt(0);
  if (argument->IsDoubleRegister() || argument->IsDoubleStackSlot()) {
    Abort("DoPushArgument not implemented for double type.");
  } else {
    Register argument_reg = EmitLoadRegister(argument, at);
    __ push(argument_reg);
  }
}


void LCodeGen::DoThisFunction(LThisFunction* instr) {
  Register result = ToRegister(instr->result());
  __ lw(result, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
}


void LCodeGen::DoContext(LContext* instr) {
  Register result = ToRegister(instr->result());
  __ mov(result, cp);
}


void LCodeGen::DoOuterContext(LOuterContext* instr) {
  Register context = ToRegister(instr->context());
  Register result = ToRegister(instr->result());
  __ lw(result,
        MemOperand(context, Context::SlotOffset(Context::PREVIOUS_INDEX)));

}


void LCodeGen::DoGlobalObject(LGlobalObject* instr) {
  Register context = ToRegister(instr->context());
  Register result = ToRegister(instr->result());
  __ lw(result, ContextOperand(cp, Context::GLOBAL_INDEX));
}


void LCodeGen::DoGlobalReceiver(LGlobalReceiver* instr) {
  Register global = ToRegister(instr->global());
  Register result = ToRegister(instr->result());
  __ lw(result, FieldMemOperand(global, GlobalObject::kGlobalReceiverOffset));
}


void LCodeGen::CallKnownFunction(Handle<JSFunction> function,
                                 int arity,
                                 LInstruction* instr,
                                 CallKind call_kind) {
  // Change context if needed.
  bool change_context =
      (info()->closure()->context() != function->context()) ||
      scope()->contains_with() ||
      (scope()->num_heap_slots() > 0);
  if (change_context) {
    __ lw(cp, FieldMemOperand(a1, JSFunction::kContextOffset));
  }

  // Set a0 to arguments count if adaption is not needed. Assumes that a0
  // is available to write to at this point.
  if (!function->NeedsArgumentsAdaption()) {
    __ li(a0, Operand(arity));
  }

  LPointerMap* pointers = instr->pointer_map();
  RecordPosition(pointers->position());

  // Invoke function.
  __ SetCallKind(t1, call_kind);
  __ lw(at, FieldMemOperand(a1, JSFunction::kCodeEntryOffset));
  __ Call(at);

  // Setup deoptimization.
  RegisterLazyDeoptimization(instr, RECORD_SIMPLE_SAFEPOINT);

  // Restore context.
  __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoCallConstantFunction(LCallConstantFunction* instr) {
  ASSERT(ToRegister(instr->result()).is(v0));
  __ mov(a0, v0);
  __ li(a1, Operand(instr->function()));
  CallKnownFunction(instr->function(), instr->arity(), instr, CALL_AS_METHOD);
}


void LCodeGen::DoDeferredMathAbsTaggedHeapNumber(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::EmitIntegerMathAbs(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoMathAbs(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoMathFloor(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoMathRound(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoMathSqrt(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoMathPowHalf(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoPower(LPower* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoMathLog(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoMathCos(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoMathSin(LUnaryMathOperation* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoUnaryMathOperation(LUnaryMathOperation* instr) {
  switch (instr->op()) {
    case kMathAbs:
      DoMathAbs(instr);
      break;
    case kMathFloor:
      DoMathFloor(instr);
      break;
    case kMathRound:
      DoMathRound(instr);
      break;
    case kMathSqrt:
      DoMathSqrt(instr);
      break;
    case kMathPowHalf:
      DoMathPowHalf(instr);
      break;
    case kMathCos:
      DoMathCos(instr);
      break;
    case kMathSin:
      DoMathSin(instr);
      break;
    case kMathLog:
      DoMathLog(instr);
      break;
    default:
      Abort("Unimplemented type of LUnaryMathOperation.");
      UNREACHABLE();
  }
}


void LCodeGen::DoInvokeFunction(LInvokeFunction* instr) {
  ASSERT(ToRegister(instr->function()).is(a1));
  ASSERT(instr->HasPointerMap());
  ASSERT(instr->HasDeoptimizationEnvironment());
  LPointerMap* pointers = instr->pointer_map();
  LEnvironment* env = instr->deoptimization_environment();
  RecordPosition(pointers->position());
  RegisterEnvironmentForDeoptimization(env);
  SafepointGenerator generator(this, pointers, env->deoptimization_index());
  ParameterCount count(instr->arity());
  __ InvokeFunction(a1, count, CALL_FUNCTION, generator, CALL_AS_METHOD);
  __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoCallKeyed(LCallKeyed* instr) {
  ASSERT(ToRegister(instr->result()).is(v0));

  int arity = instr->arity();
  Handle<Code> ic =
      isolate()->stub_cache()->ComputeKeyedCallInitialize(arity, NOT_IN_LOOP);
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
  __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoCallNamed(LCallNamed* instr) {
  ASSERT(ToRegister(instr->result()).is(v0));

  int arity = instr->arity();
  RelocInfo::Mode mode = RelocInfo::CODE_TARGET;
  Handle<Code> ic =
      isolate()->stub_cache()->ComputeCallInitialize(arity, NOT_IN_LOOP, mode);
  __ li(a2, Operand(instr->name()));
  CallCode(ic, mode, instr);
  // Restore context register.
  __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoCallFunction(LCallFunction* instr) {
  ASSERT(ToRegister(instr->result()).is(v0));

  int arity = instr->arity();
  CallFunctionStub stub(arity, NOT_IN_LOOP, RECEIVER_MIGHT_BE_IMPLICIT);
  CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
  __ Drop(1);
  __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoCallGlobal(LCallGlobal* instr) {
  ASSERT(ToRegister(instr->result()).is(v0));

  int arity = instr->arity();
  RelocInfo::Mode mode = RelocInfo::CODE_TARGET_CONTEXT;
  Handle<Code> ic =
      isolate()->stub_cache()->ComputeCallInitialize(arity, NOT_IN_LOOP, mode);
  __ li(a2, Operand(instr->name()));
  CallCode(ic, mode, instr);
  __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));
}


void LCodeGen::DoCallKnownGlobal(LCallKnownGlobal* instr) {
  ASSERT(ToRegister(instr->result()).is(v0));
  __ li(a1, Operand(instr->target()));
  CallKnownFunction(instr->target(), instr->arity(), instr, CALL_AS_FUNCTION);
}


void LCodeGen::DoCallNew(LCallNew* instr) {
  ASSERT(ToRegister(instr->InputAt(0)).is(a1));
  ASSERT(ToRegister(instr->result()).is(v0));

  Handle<Code> builtin = isolate()->builtins()->JSConstructCall();
  __ li(a0, Operand(instr->arity()));
  CallCode(builtin, RelocInfo::CONSTRUCT_CALL, instr);
}


void LCodeGen::DoCallRuntime(LCallRuntime* instr) {
  CallRuntime(instr->function(), instr->arity(), instr);
}


void LCodeGen::DoStoreNamedField(LStoreNamedField* instr) {
  Register object = ToRegister(instr->object());
  Register value = ToRegister(instr->value());
  Register scratch = scratch0();
  int offset = instr->offset();

  ASSERT(!object.is(value));

  if (!instr->transition().is_null()) {
    __ li(scratch, Operand(instr->transition()));
    __ sw(scratch, FieldMemOperand(object, HeapObject::kMapOffset));
  }

  // Do the store.
  if (instr->is_in_object()) {
    __ sw(value, FieldMemOperand(object, offset));
    if (instr->needs_write_barrier()) {
      // Update the write barrier for the object for in-object properties.
      __ RecordWrite(object, Operand(offset), value, scratch);
    }
  } else {
    __ lw(scratch, FieldMemOperand(object, JSObject::kPropertiesOffset));
    __ sw(value, FieldMemOperand(scratch, offset));
    if (instr->needs_write_barrier()) {
      // Update the write barrier for the properties array.
      // object is used as a scratch register.
      __ RecordWrite(scratch, Operand(offset), value, object);
    }
  }
}


void LCodeGen::DoStoreNamedGeneric(LStoreNamedGeneric* instr) {
  ASSERT(ToRegister(instr->object()).is(a1));
  ASSERT(ToRegister(instr->value()).is(a0));

  // Name is always in a2.
  __ li(a2, Operand(instr->name()));
  Handle<Code> ic = instr->strict_mode()
      ? isolate()->builtins()->StoreIC_Initialize_Strict()
      : isolate()->builtins()->StoreIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoBoundsCheck(LBoundsCheck* instr) {
  DeoptimizeIf(hs,
               instr->environment(),
               ToRegister(instr->index()),
               Operand(ToRegister(instr->length())));
}


void LCodeGen::DoStoreKeyedFastElement(LStoreKeyedFastElement* instr) {
  Register value = ToRegister(instr->value());
  Register elements = ToRegister(instr->object());
  Register key = instr->key()->IsRegister() ? ToRegister(instr->key()) : no_reg;
  Register scratch = scratch0();

  // Do the store.
  if (instr->key()->IsConstantOperand()) {
    ASSERT(!instr->hydrogen()->NeedsWriteBarrier());
    LConstantOperand* const_operand = LConstantOperand::cast(instr->key());
    int offset =
        ToInteger32(const_operand) * kPointerSize + FixedArray::kHeaderSize;
    __ sw(value, FieldMemOperand(elements, offset));
  } else {
    __ sll(scratch, key, kPointerSizeLog2);
    __ addu(scratch, elements, scratch);
    __ sw(value, FieldMemOperand(scratch, FixedArray::kHeaderSize));
  }

  if (instr->hydrogen()->NeedsWriteBarrier()) {
    // Compute address of modified element and store it into key register.
    __ Addu(key, scratch, Operand(FixedArray::kHeaderSize));
    __ RecordWrite(elements, key, value);
  }
}


void LCodeGen::DoStoreKeyedSpecializedArrayElement(
    LStoreKeyedSpecializedArrayElement* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoStoreKeyedGeneric(LStoreKeyedGeneric* instr) {
  ASSERT(ToRegister(instr->object()).is(a2));
  ASSERT(ToRegister(instr->key()).is(a1));
  ASSERT(ToRegister(instr->value()).is(a0));

  Handle<Code> ic = instr->strict_mode()
      ? isolate()->builtins()->KeyedStoreIC_Initialize_Strict()
      : isolate()->builtins()->KeyedStoreIC_Initialize();
  CallCode(ic, RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoStringAdd(LStringAdd* instr) {
  __ push(ToRegister(instr->left()));
  __ push(ToRegister(instr->right()));
  StringAddStub stub(NO_STRING_CHECK_IN_STUB);
  CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
}


void LCodeGen::DoStringCharCodeAt(LStringCharCodeAt* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoDeferredStringCharCodeAt(LStringCharCodeAt* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoStringCharFromCode(LStringCharFromCode* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoDeferredStringCharFromCode(LStringCharFromCode* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoStringLength(LStringLength* instr) {
  Register string = ToRegister(instr->InputAt(0));
  Register result = ToRegister(instr->result());
  __ lw(result, FieldMemOperand(string, String::kLengthOffset));
}


void LCodeGen::DoInteger32ToDouble(LInteger32ToDouble* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoNumberTagI(LNumberTagI* instr) {
  class DeferredNumberTagI: public LDeferredCode {
   public:
    DeferredNumberTagI(LCodeGen* codegen, LNumberTagI* instr)
        : LDeferredCode(codegen), instr_(instr) { }
    virtual void Generate() { codegen()->DoDeferredNumberTagI(instr_); }
   private:
    LNumberTagI* instr_;
  };

  LOperand* input = instr->InputAt(0);
  ASSERT(input->IsRegister() && input->Equals(instr->result()));
  Register reg = ToRegister(input);
  Register overflow = scratch0();

  DeferredNumberTagI* deferred = new DeferredNumberTagI(this, instr);
  __ SmiTagCheckOverflow(reg, overflow);
  __ BranchOnOverflow(deferred->entry(), overflow);
  __ bind(deferred->exit());
}


void LCodeGen::DoDeferredNumberTagI(LNumberTagI* instr) {

  Label slow;
  Register reg = ToRegister(instr->InputAt(0));
  FPURegister dbl_scratch = lithiumScratchDouble;

  // Preserve the value of all registers.
  PushSafepointRegistersScope scope(this, Safepoint::kWithRegisters);

  // There was overflow, so bits 30 and 31 of the original integer
  // disagree. Try to allocate a heap number in new space and store
  // the value in there. If that fails, call the runtime system.
  Label done;
  __ SmiUntag(reg);
  __ Xor(reg, reg, Operand(0x80000000));
  __ mtc1(reg, dbl_scratch);
  __ cvt_d_w(dbl_scratch, dbl_scratch);
  if (FLAG_inline_new) {
    // TODO(plind): why did they choose r5, r3, r4, r6 for Arm version?
    __ LoadRoot(t2, Heap::kHeapNumberMapRootIndex);
    __ AllocateHeapNumber(t1, a3, t0, t2, &slow);
    if (!reg.is(t1)) __ mov(reg, t1);
    __ b(&done);
  }

  // Slow case: Call the runtime system to do the number allocation.
  __ bind(&slow);

  // TODO(3095996): Put a valid pointer value in the stack slot where the result
  // register is stored, as this register is in the pointer map, but contains an
  // integer value.
  __ StoreToSafepointRegisterSlot(zero_reg, reg);
  CallRuntimeFromDeferred(Runtime::kAllocateHeapNumber, 0, instr);
  if (!reg.is(v0)) __ mov(reg, v0);

  // Done. Put the value in dbl_scratch into the value of the allocated heap
  // number.
  __ bind(&done);
  __ sdc1(dbl_scratch, FieldMemOperand(reg, HeapNumber::kValueOffset));
  __ StoreToSafepointRegisterSlot(reg, reg);
}


void LCodeGen::DoNumberTagD(LNumberTagD* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoDeferredNumberTagD(LNumberTagD* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoSmiTag(LSmiTag* instr) {
  LOperand* input = instr->InputAt(0);
  ASSERT(input->IsRegister() && input->Equals(instr->result()));
  ASSERT(!instr->hydrogen_value()->CheckFlag(HValue::kCanOverflow));
  __ SmiTag(ToRegister(input));
}


void LCodeGen::DoSmiUntag(LSmiUntag* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::EmitNumberUntagD(Register input_reg,
                                DoubleRegister result_reg,
                                bool deoptimize_on_undefined,
                                LEnvironment* env) {
  Register scratch = scratch0();

  Label load_smi, heap_number, done;

  // Smi check.
  __ JumpIfSmi(input_reg, &load_smi);

  // Heap number map check.
  __ lw(scratch, FieldMemOperand(input_reg, HeapObject::kMapOffset));
  __ LoadRoot(at, Heap::kHeapNumberMapRootIndex);
  if (deoptimize_on_undefined) {
    DeoptimizeIf(ne, env, scratch, Operand(at));
  } else {
    Label heap_number;
    __ Branch(&heap_number, eq, scratch, Operand(at));

    __ LoadRoot(at, Heap::kUndefinedValueRootIndex);
    DeoptimizeIf(ne, env, input_reg, Operand(at));

    // Convert undefined to NaN.
    __ LoadRoot(at, Heap::kNanValueRootIndex);
    __ ldc1(result_reg, FieldMemOperand(at, HeapNumber::kValueOffset));
    __ Branch(&done);

    __ bind(&heap_number);
  }
  // Heap number to double register conversion.
  __ ldc1(result_reg, FieldMemOperand(at, HeapNumber::kValueOffset));
  __ Branch(&done);

  // Smi to double register conversion
  __ bind(&load_smi);
  __ SmiUntag(input_reg);  // Untag smi before converting to float.
  __ mtc1(input_reg, result_reg);
  __ cvt_d_w(result_reg, result_reg);
  __ SmiTag(input_reg);  // Retag smi.
  __ bind(&done);
}


class DeferredTaggedToI: public LDeferredCode {
 public:
  DeferredTaggedToI(LCodeGen* codegen, LTaggedToI* instr)
      : LDeferredCode(codegen), instr_(instr) { }
  virtual void Generate() { codegen()->DoDeferredTaggedToI(instr_); }
 private:
  LTaggedToI* instr_;
};


void LCodeGen::DoDeferredTaggedToI(LTaggedToI* instr) {
  Register input_reg = ToRegister(instr->InputAt(0));
  Register scratch1 = scratch0();
  Register scratch2 = ToRegister(instr->TempAt(0));
  FPURegister double_scratch = double_scratch0();
  FPURegister single_scratch = double_scratch.low();

  ASSERT(!scratch1.is(input_reg) && !scratch1.is(scratch2));
  ASSERT(!scratch2.is(input_reg) && !scratch2.is(scratch1));

  Label done;

  // The input is a tagged HeapObject.
  // Heap number map check.
  __ lw(scratch1, FieldMemOperand(input_reg, HeapObject::kMapOffset));
  __ LoadRoot(at, Heap::kHeapNumberMapRootIndex);
  // This 'at' value and scratch1 map value are used for tests in both clauses
  // of the if.

  if (instr->truncating()) {
    Register scratch3 = ToRegister(instr->TempAt(1));
    FPURegister double_scratch2 = ToDoubleRegister(instr->TempAt(2));
    ASSERT(!scratch3.is(input_reg) &&
           !scratch3.is(scratch1) &&
           !scratch3.is(scratch2));
    // Performs a truncating conversion of a floating point number as used by
    // the JS bitwise operations.
    Label heap_number;
    __ Branch(&heap_number, eq, scratch1, Operand(at));  // HeapNumber map?
    // Check for undefined. Undefined is converted to zero for truncating
    // conversions.
    __ LoadRoot(at, Heap::kUndefinedValueRootIndex);
    DeoptimizeIf(ne, instr->environment(), input_reg, Operand(at));
    __ mov(input_reg, zero_reg);  // TODO(plind): result really in input reg?
    __ b(&done);

    __ bind(&heap_number);
    __ ldc1(double_scratch2,
            FieldMemOperand(input_reg, HeapNumber::kValueOffset));
    __ EmitECMATruncate(input_reg,
                        double_scratch2,
                        single_scratch,
                        scratch1,
                        scratch2,
                        scratch3);

  } else {
    // TODO(plind): if this scope is needed, then we also need one above?
    // I don't think it is needed at all, we have a top-level FPU scope.
    CpuFeatures::Scope scope(FPU);

    // Deoptimize if we don't have a heap number.
    DeoptimizeIf(ne, instr->environment(), scratch1, Operand(at));

    // Load the double value.
    __ ldc1(double_scratch,
            FieldMemOperand(input_reg, HeapNumber::kValueOffset));

    // TODO(plind): ARM uses a MacroAssembler function here (EmitVFPTruncate).
    // On MIPS a lot of things cannot be implemented the same way so right
    // now it makes more sense to just do things manually.

    // TODO(plind): this code is still untested.

    // Save FCSR.
    __ cfc1(scratch1, FCSR);
    // Disable FPU exceptions.
    __ ctc1(zero_reg, FCSR);
    // TODO(plind): it appears this instuction rounds towards zero regardless
    // of the rounding mode in FCSR. Verify, fix if needed, and remove comment.
    __ trunc_w_d(single_scratch, double_scratch);
    // Retrieve FCSR.
    __ cfc1(scratch2, FCSR);
    // Restore FCSR.
    __ ctc1(scratch1, FCSR);

    // Check for inexact conversion or exception (non-zero flags).
    __ And(scratch2, scratch2, kFCSRFlagMask);

    // Deopt if the operation did not succeed.
    DeoptimizeIf(ne, instr->environment(), scratch2, Operand(zero_reg));

    // Load the result.
    __ mfc1(input_reg, single_scratch);

    if (instr->hydrogen()->CheckFlag(HValue::kBailoutOnMinusZero)) {
      // __ cmp(input_reg, Operand(0));
      // __ b(ne, &done);
      __ Branch(&done, ne, input_reg, Operand(zero_reg));

      __ mfc1(scratch1, double_scratch.high());
      __ And(scratch1, scratch1, Operand(HeapNumber::kSignMask));
      DeoptimizeIf(ne, instr->environment(), scratch1, Operand(zero_reg));
    }
  }
  __ bind(&done);
}


void LCodeGen::DoTaggedToI(LTaggedToI* instr) {
  LOperand* input = instr->InputAt(0);
  ASSERT(input->IsRegister());
  ASSERT(input->Equals(instr->result()));  // TODO(plind): maybe bad assumption for mips?

  Register input_reg = ToRegister(input);

  DeferredTaggedToI* deferred = new DeferredTaggedToI(this, instr);

  // Let the deferred code handle the HeapObject case.
  __ JumpIfNotSmi(input_reg, deferred->entry());

  // Smi to int32 conversion.
  __ SmiUntag(input_reg);
  __ bind(deferred->exit());
}


void LCodeGen::DoNumberUntagD(LNumberUntagD* instr) {
  LOperand* input = instr->InputAt(0);
  ASSERT(input->IsRegister());
  LOperand* result = instr->result();
  ASSERT(result->IsDoubleRegister());

  Register input_reg = ToRegister(input);
  DoubleRegister result_reg = ToDoubleRegister(result);

  EmitNumberUntagD(input_reg, result_reg,
                   instr->hydrogen()->deoptimize_on_undefined(),
                   instr->environment());
}


void LCodeGen::DoDoubleToI(LDoubleToI* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoCheckSmi(LCheckSmi* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoCheckNonSmi(LCheckNonSmi* instr) {
  LOperand* input = instr->InputAt(0);
  __ And(at, ToRegister(input), Operand(kSmiTagMask));
  DeoptimizeIf(eq, instr->environment(), at, Operand(zero_reg));
}


void LCodeGen::DoCheckInstanceType(LCheckInstanceType* instr) {
  Register input = ToRegister(instr->InputAt(0));
  Register scratch = scratch0();

  __ GetObjectType(input, scratch, scratch);

  if (instr->hydrogen()->is_interval_check()) {
    InstanceType first;
    InstanceType last;
    instr->hydrogen()->GetCheckInterval(&first, &last);

    // If there is only one type in the interval check for equality.
    if (first == last) {
      DeoptimizeIf(ne, instr->environment(), scratch, Operand(first));
    } else {
      DeoptimizeIf(lo, instr->environment(), scratch, Operand(first));
      // Omit check for the last type.
      if (last != LAST_TYPE) {
        DeoptimizeIf(hi, instr->environment(), scratch, Operand(last));
      }
    }
  } else {
    uint8_t mask;
    uint8_t tag;
    instr->hydrogen()->GetCheckMaskAndTag(&mask, &tag);

    if (IsPowerOf2(mask)) {
      ASSERT(tag == 0 || IsPowerOf2(tag));
      __ And(at, scratch, mask);
      DeoptimizeIf(tag == 0 ? ne : eq, instr->environment(),
          at, Operand(zero_reg));
    } else {
      __ And(scratch, scratch, Operand(mask));
      DeoptimizeIf(ne, instr->environment(), scratch, Operand(tag));
    }
  }
}


void LCodeGen::DoCheckFunction(LCheckFunction* instr) {
  ASSERT(instr->InputAt(0)->IsRegister());
  Register reg = ToRegister(instr->InputAt(0));
  DeoptimizeIf(ne, instr->environment(), reg,
               Operand(instr->hydrogen()->target()));
}


void LCodeGen::DoCheckMap(LCheckMap* instr) {
  Register scratch = scratch0();
  LOperand* input = instr->InputAt(0);
  ASSERT(input->IsRegister());
  Register reg = ToRegister(input);
  __ lw(scratch, FieldMemOperand(reg, HeapObject::kMapOffset));
  DeoptimizeIf(ne,
               instr->environment(),
               scratch,
               Operand(instr->hydrogen()->map()));
}


void LCodeGen::DoClampDToUint8(LClampDToUint8* instr) {
  DoubleRegister value_reg = ToDoubleRegister(instr->unclamped());
  Register result_reg = ToRegister(instr->result());
  DoubleRegister temp_reg = ToDoubleRegister(instr->TempAt(0));
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // __ ClampDoubleToUint8(result_reg, value_reg, temp_reg);
}


void LCodeGen::DoClampIToUint8(LClampIToUint8* instr) {
  Register unclamped_reg = ToRegister(instr->unclamped());
  Register result_reg = ToRegister(instr->result());
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // __ ClampUint8(result_reg, unclamped_reg);
}


void LCodeGen::DoClampTToUint8(LClampTToUint8* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::LoadHeapObject(Register result,
                              Handle<HeapObject> object) {
  if (heap()->InNewSpace(*object)) {
    Handle<JSGlobalPropertyCell> cell =
        factory()->NewJSGlobalPropertyCell(object);
    __ li(result, Operand(cell));
    __ lw(result, FieldMemOperand(result, JSGlobalPropertyCell::kValueOffset));
  } else {
    __ li(result, Operand(object));
  }
}


void LCodeGen::DoCheckPrototypeMaps(LCheckPrototypeMaps* instr) {
  Register temp1 = ToRegister(instr->TempAt(0));
  Register temp2 = ToRegister(instr->TempAt(1));

  Handle<JSObject> holder = instr->holder();
  Handle<JSObject> current_prototype = instr->prototype();

  // Load prototype object.
  LoadHeapObject(temp1, current_prototype);

  // Check prototype maps up to the holder.
  while (!current_prototype.is_identical_to(holder)) {
    __ lw(temp2, FieldMemOperand(temp1, HeapObject::kMapOffset));
    DeoptimizeIf(ne,
                 instr->environment(),
                 temp2,
                 Operand(Handle<Map>(current_prototype->map())));
    current_prototype =
        Handle<JSObject>(JSObject::cast(current_prototype->GetPrototype()));
    // Load next prototype object.
    LoadHeapObject(temp1, current_prototype);
  }

  // Check the holder map.
  __ lw(temp2, FieldMemOperand(temp1, HeapObject::kMapOffset));
  DeoptimizeIf(ne,
               instr->environment(),
               temp2,
               Operand(Handle<Map>(current_prototype->map())));
}


void LCodeGen::DoArrayLiteral(LArrayLiteral* instr) {
  __ lw(a3, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
  __ lw(a3, FieldMemOperand(a3, JSFunction::kLiteralsOffset));
  __ li(a2, Operand(Smi::FromInt(instr->hydrogen()->literal_index())));
  __ li(a1, Operand(instr->hydrogen()->constant_elements()));
  __ Push(a3, a2, a1);

  // Pick the right runtime function or stub to call.
  int length = instr->hydrogen()->length();
  if (instr->hydrogen()->IsCopyOnWrite()) {
    ASSERT(instr->hydrogen()->depth() == 1);
    FastCloneShallowArrayStub::Mode mode =
        FastCloneShallowArrayStub::COPY_ON_WRITE_ELEMENTS;
    FastCloneShallowArrayStub stub(mode, length);
    CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
  } else if (instr->hydrogen()->depth() > 1) {
    CallRuntime(Runtime::kCreateArrayLiteral, 3, instr);
  } else if (length > FastCloneShallowArrayStub::kMaximumClonedLength) {
    CallRuntime(Runtime::kCreateArrayLiteralShallow, 3, instr);
  } else {
    FastCloneShallowArrayStub::Mode mode =
        FastCloneShallowArrayStub::CLONE_ELEMENTS;
    FastCloneShallowArrayStub stub(mode, length);
    CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
  }
}


void LCodeGen::DoObjectLiteral(LObjectLiteral* instr) {
  ASSERT(ToRegister(instr->result()).is(v0));
  __ lw(t0, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
  __ lw(t0, FieldMemOperand(t0, JSFunction::kLiteralsOffset));
  __ li(a3, Operand(Smi::FromInt(instr->hydrogen()->literal_index())));
  __ li(a2, Operand(instr->hydrogen()->constant_properties()));
  __ li(a1, Operand(Smi::FromInt(instr->hydrogen()->fast_elements() ? 1 : 0)));
  __ Push(t0, a3, a2, a1);

  // Pick the right runtime function to call.
  if (instr->hydrogen()->depth() > 1) {
    CallRuntime(Runtime::kCreateObjectLiteral, 4, instr);
  } else {
    CallRuntime(Runtime::kCreateObjectLiteralShallow, 4, instr);
  }
}


void LCodeGen::DoToFastProperties(LToFastProperties* instr) {
  ASSERT(ToRegister(instr->InputAt(0)).is(a0));
  ASSERT(ToRegister(instr->result()).is(v0));
  __ push(a0);
  CallRuntime(Runtime::kToFastProperties, 1, instr);
}


void LCodeGen::DoRegExpLiteral(LRegExpLiteral* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoFunctionLiteral(LFunctionLiteral* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoTypeof(LTypeof* instr) {
  ASSERT(ToRegister(instr->result()).is(v0));
  Register input = ToRegister(instr->InputAt(0));
  __ push(input);
  CallRuntime(Runtime::kTypeof, 1, instr);
}


void LCodeGen::DoTypeofIs(LTypeofIs* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoTypeofIsAndBranch(LTypeofIsAndBranch* instr) {
  Register input = ToRegister(instr->InputAt(0));
  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());
  Label* true_label = chunk_->GetAssemblyLabel(true_block);
  Label* false_label = chunk_->GetAssemblyLabel(false_block);

  Register cmp1 = no_reg;
  Operand cmp2 = Operand(no_reg);

  Condition final_branch_condition = EmitTypeofIs(true_label,
                                                  false_label,
                                                  input,
                                                  instr->type_literal(),
                                                  cmp1,
                                                  cmp2);

  ASSERT(cmp1.is_valid());
  ASSERT(!cmp2.is_reg() || cmp2.rm().is_valid());

  EmitBranch(true_block, false_block, final_branch_condition, cmp1, cmp2);
}


Condition LCodeGen::EmitTypeofIs(Label* true_label,
                                 Label* false_label,
                                 Register input,
                                 Handle<String> type_name,
                                 Register& cmp1,
                                 Operand& cmp2) {
  // TODO(kalmard): This function utilizes the delay slot heavily. Verify that
  // it doesn't cause problems and add some comments/explanation. Try to find a
  // simple "name" that describes this behavior and use it in comments.
  Condition final_branch_condition = kNoCondition;
  Register scratch = scratch0();
  if (type_name->Equals(heap()->number_symbol())) {
    __ JumpIfSmi(input, true_label);
    __ lw(input, FieldMemOperand(input, HeapObject::kMapOffset));
    __ LoadRoot(at, Heap::kHeapNumberMapRootIndex);
    cmp1 = input;
    cmp2 = Operand(at);
    final_branch_condition = eq;

  } else if (type_name->Equals(heap()->string_symbol())) {
    __ JumpIfSmi(input, false_label);
    __ GetObjectType(input, input, scratch);
    __ Branch(USE_DELAY_SLOT, false_label,
              ge, scratch, Operand(FIRST_NONSTRING_TYPE));
    __ lbu(at, FieldMemOperand(input, Map::kBitFieldOffset));
    __ And(at, at, 1 << Map::kIsUndetectable);
    cmp1 = at;
    cmp2 = Operand(zero_reg);
    final_branch_condition = eq;

  } else if (type_name->Equals(heap()->boolean_symbol())) {
    __ LoadRoot(at, Heap::kTrueValueRootIndex);
    __ Branch(USE_DELAY_SLOT, true_label, eq, at, Operand(input));
    __ LoadRoot(at, Heap::kFalseValueRootIndex);
    cmp1 = at;
    cmp2 = Operand(input);
    final_branch_condition = eq;

  } else if (type_name->Equals(heap()->undefined_symbol())) {
    __ LoadRoot(at, Heap::kUndefinedValueRootIndex);
    __ Branch(USE_DELAY_SLOT, true_label, eq, at, Operand(input));
    __ JumpIfSmi(input, false_label);
    // Check for undetectable objects => true.
    __ lw(input, FieldMemOperand(input, HeapObject::kMapOffset));
    __ lbu(at, FieldMemOperand(input, Map::kBitFieldOffset));
    __ And(at, at, 1 << Map::kIsUndetectable);
    cmp1 = at;
    cmp2 = Operand(zero_reg);
    final_branch_condition = ne;

  } else if (type_name->Equals(heap()->function_symbol())) {
    __ JumpIfSmi(input, false_label);
    __ GetObjectType(input, input, scratch);
    cmp1 = scratch;
    cmp2 = Operand(FIRST_CALLABLE_SPEC_OBJECT_TYPE);
    final_branch_condition = ge;

  } else if (type_name->Equals(heap()->object_symbol())) {
    __ JumpIfSmi(input, false_label);
    __ LoadRoot(at, Heap::kNullValueRootIndex);
    __ Branch(USE_DELAY_SLOT, true_label, eq, at, Operand(input));
    __ GetObjectType(input, input, scratch);
    __ Branch(USE_DELAY_SLOT, false_label,
              lt, scratch, Operand(FIRST_NONCALLABLE_SPEC_OBJECT_TYPE));
    __ lbu(scratch, FieldMemOperand(input, Map::kInstanceTypeOffset));
    __ Branch(USE_DELAY_SLOT, false_label,
              gt, scratch, Operand(LAST_NONCALLABLE_SPEC_OBJECT_TYPE));
    // Check for undetectable objects => false.
    __ lbu(at, FieldMemOperand(input, Map::kBitFieldOffset));
    __ And(at, at, 1 << Map::kIsUndetectable);
    cmp1 = at;
    cmp2 = Operand(zero_reg);
    final_branch_condition = eq;

  } else {
    cmp1 = at;
    cmp2 = Operand(zero_reg);  // Set to valid regs, to avoid caller assertion.
    final_branch_condition = ne;
    __ Branch(false_label);
    // A dead branch instruction will be generated after this point.
  }

  return final_branch_condition;
}


void LCodeGen::DoIsConstructCall(LIsConstructCall* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoIsConstructCallAndBranch(LIsConstructCallAndBranch* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::EmitIsConstructCall(Register temp1, Register temp2) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoLazyBailout(LLazyBailout* instr) {
  // No code for lazy bailout instruction. Used to capture environment after a
  // call for populating the safepoint data with deoptimization data.
}


void LCodeGen::DoDeoptimize(LDeoptimize* instr) {
  DeoptimizeIf(al, instr->environment(), zero_reg, Operand(zero_reg));
}


void LCodeGen::DoDeleteProperty(LDeleteProperty* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoIn(LIn* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoStackCheck(LStackCheck* instr) {
  // Perform stack overflow check.
  Label ok;
  __ LoadRoot(at, Heap::kStackLimitRootIndex);
  __ Branch(&ok, hs, sp, Operand(at));
  StackCheckStub stub;
  CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
  __ bind(&ok);
}


void LCodeGen::DoOsrEntry(LOsrEntry* instr) {
  // This is a pseudo-instruction that ensures that the environment here is
  // properly registered for deoptimization and records the assembler's PC
  // offset.
  LEnvironment* environment = instr->environment();
  environment->SetSpilledRegisters(instr->SpilledRegisterArray(),
                                   instr->SpilledDoubleRegisterArray());

  // If the environment were already registered, we would have no way of
  // backpatching it with the spill slot operands.
  ASSERT(!environment->HasBeenRegistered());
  RegisterEnvironmentForDeoptimization(environment);
  ASSERT(osr_pc_offset_ == -1);
  osr_pc_offset_ = masm()->pc_offset();
}


#undef __

} }  // namespace v8::internal
