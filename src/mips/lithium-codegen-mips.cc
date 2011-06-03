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
      __ CallRuntime(Runtime::kNewContext, 1);
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

  if (cc == al) {
    if (FLAG_trap_on_deopt) __ stop("trap_on_deopt");
    __ Jump(entry, RelocInfo::RUNTIME_ENTRY);
  } else {
    if (FLAG_trap_on_deopt) {
      Label done;
      __ Branch(&done, NegateCondition(cc), src1, src2);
      __ stop("trap_on_deopt");
      __ Jump(entry, RelocInfo::RUNTIME_ENTRY);
      __ bind(&done);
    } else {
      __ Jump(entry, RelocInfo::RUNTIME_ENTRY, cc, src1, src2);
    }
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
  if (!FLAG_debug_info || position == RelocInfo::kNoPosition) return;
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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoSubI(LSubI* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
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


void LCodeGen::DoBranch(LBranch* instr) {
  int true_block = chunk_->LookupDestination(instr->true_block_id());
  int false_block = chunk_->LookupDestination(instr->false_block_id());

  Representation r = instr->hydrogen()->representation();
  if (r.IsInteger32()) {
    Register reg = ToRegister(instr->InputAt(0));
    EmitBranch(true_block, false_block, ne, reg, Operand(0));
  } else if (r.IsDouble()) {
    Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
    // DoubleRegister reg = ToDoubleRegister(instr->InputAt(0));
    // Register scratch = scratch0();
    // 
    // // Test the double value. Zero and NaN are false.
    // __ VFPCompareAndLoadFlags(reg, 0.0, scratch);
    // __ tst(scratch, Operand(kVFPZConditionFlagBit | kVFPVConditionFlagBit));
    // EmitBranch(true_block, false_block, ne);
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
      __ Branch(false_label, eq, reg, Operand(0));
      __ And(at, reg, Operand(kSmiTagMask));
      __ Branch(true_label, eq, at, Operand(0));
    
      // Test double values. Zero and NaN are false.

      // TODO(plind): I think this is optimization, and stub below
      // can handle everything.

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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoIsNullAndBranch(LIsNullAndBranch* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // ASSERT(instr->hydrogen()->value()->representation().IsTagged());
  // Register result = ToRegister(instr->result());
  // Register input_reg = EmitLoadRegister(instr->InputAt(0), ip);
  // __ tst(input_reg, Operand(kSmiTagMask));
  // __ LoadRoot(result, Heap::kTrueValueRootIndex);
  // Label done;
  // __ b(eq, &done);
  // __ LoadRoot(result, Heap::kFalseValueRootIndex);
  // __ bind(&done);
}


void LCodeGen::DoIsSmiAndBranch(LIsSmiAndBranch* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // int true_block = chunk_->LookupDestination(instr->true_block_id());
  // int false_block = chunk_->LookupDestination(instr->false_block_id());
  //
  // Register input_reg = EmitLoadRegister(instr->InputAt(0), ip);
  // __ tst(input_reg, Operand(kSmiTagMask));
  // EmitBranch(true_block, false_block, eq);
}


void LCodeGen::DoIsUndetectable(LIsUndetectable* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoIsUndetectableAndBranch(LIsUndetectableAndBranch* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // Register input = ToRegister(instr->InputAt(0));
  // Register temp = ToRegister(instr->TempAt(0));
  //
  // int true_block = chunk_->LookupDestination(instr->true_block_id());
  // int false_block = chunk_->LookupDestination(instr->false_block_id());
  //
  // __ JumpIfSmi(input, chunk_->GetAssemblyLabel(false_block));
  // __ ldr(temp, FieldMemOperand(input, HeapObject::kMapOffset));
  // __ ldrb(temp, FieldMemOperand(temp, Map::kBitFieldOffset));
  // __ tst(temp, Operand(1 << Map::kIsUndetectable));
  // EmitBranch(true_block, false_block, ne);
}


// TODO(plind): These 2 routine not called yet, commenting out.
// static InstanceType TestType(HHasInstanceType* instr) {
//   InstanceType from = instr->from();
//   InstanceType to = instr->to();
//   if (from == FIRST_TYPE) return to;
//   ASSERT(from == to || to == LAST_TYPE);
//   return from;
// }
//
//
// static Condition BranchCondition(HHasInstanceType* instr) {
//   InstanceType from = instr->from();
//   InstanceType to = instr->to();
//   if (from == to) return eq;
//   if (to == LAST_TYPE) return hs;
//   if (from == FIRST_TYPE) return ls;
//   UNREACHABLE();
//   return eq;
// }


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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // Register scratch = scratch0();
  // Register input = ToRegister(instr->InputAt(0));
  //
  // int true_block = chunk_->LookupDestination(instr->true_block_id());
  // int false_block = chunk_->LookupDestination(instr->false_block_id());
  //
  // Label* false_label = chunk_->GetAssemblyLabel(false_block);
  //
  // __ tst(input, Operand(kSmiTagMask));
  // __ b(eq, false_label);
  //
  // __ CompareObjectType(input, scratch, scratch, TestType(instr->hydrogen()));
  // EmitBranch(true_block, false_block, BranchCondition(instr->hydrogen()));
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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // Register reg = ToRegister(instr->InputAt(0));
  // Register temp = ToRegister(instr->TempAt(0));
  // int true_block = instr->true_block_id();
  // int false_block = instr->false_block_id();
  //
  // __ ldr(temp, FieldMemOperand(reg, HeapObject::kMapOffset));
  // __ cmp(temp, Operand(instr->map()));
  // EmitBranch(true_block, false_block, eq);
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


void LCodeGen::DoInstanceOfAndBranch(LInstanceOfAndBranch* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // ASSERT(ToRegister(instr->InputAt(0)).is(r0));  // Object is in r0.
  // ASSERT(ToRegister(instr->InputAt(1)).is(r1));  // Function is in r1.
  //
  // int true_block = chunk_->LookupDestination(instr->true_block_id());
  // int false_block = chunk_->LookupDestination(instr->false_block_id());
  //
  // InstanceofStub stub(InstanceofStub::kArgsInRegisters);
  // CallCode(stub.GetCode(), RelocInfo::CODE_TARGET, instr);
  // __ cmp(r0, Operand(0));
  // EmitBranch(true_block, false_block, eq);
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

  Condition condition = ComputeCompareCondition(op);
  if (op == Token::GT || op == Token::LTE) {
    condition = ReverseCondition(condition);
  }
  __ LoadRoot(ToRegister(instr->result()),
              Heap::kTrueValueRootIndex,
              condition,
              v0,
              Operand(0));
  __ LoadRoot(ToRegister(instr->result()),
              Heap::kFalseValueRootIndex,
              NegateCondition(condition),
              v0,
              Operand(0));
}


void LCodeGen::DoCmpTAndBranch(LCmpTAndBranch* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // Token::Value op = instr->op();
  // int true_block = chunk_->LookupDestination(instr->true_block_id());
  // int false_block = chunk_->LookupDestination(instr->false_block_id());
  //
  // Handle<Code> ic = CompareIC::GetUninitialized(op);
  // CallCode(ic, RelocInfo::CODE_TARGET, instr);
  //
  // // The compare stub expects compare condition and the input operands
  // // reversed for GT and LTE.
  // Condition condition = ComputeCompareCondition(op);
  // if (op == Token::GT || op == Token::LTE) {
  //   condition = ReverseCondition(condition);
  // }
  // __ cmp(r0, Operand(0));
  // EmitBranch(true_block, false_block, condition);
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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoLoadNamedFieldPolymorphic(LLoadNamedFieldPolymorphic* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  // Register result = ToRegister(instr->result());
  // Register input = ToRegister(instr->InputAt(0));
  // Register scratch = scratch0();
  //
  // __ ldr(result, FieldMemOperand(input, JSObject::kElementsOffset));
  // if (FLAG_debug_code) {
  //   Label done;
  //   __ ldr(scratch, FieldMemOperand(result, HeapObject::kMapOffset));
  //   __ LoadRoot(ip, Heap::kFixedArrayMapRootIndex);
  //   __ cmp(scratch, ip);
  //   __ b(eq, &done);
  //   __ LoadRoot(ip, Heap::kFixedCOWArrayMapRootIndex);
  //   __ cmp(scratch, ip);
  //   __ b(eq, &done);
  //   __ ldr(scratch, FieldMemOperand(result, HeapObject::kMapOffset));
  //   __ ldrb(scratch, FieldMemOperand(scratch, Map::kInstanceTypeOffset));
  //   __ sub(scratch, scratch, Operand(FIRST_EXTERNAL_ARRAY_TYPE));
  //   __ cmp(scratch, Operand(kExternalArrayTypeCount));
  //   __ Check(cc, "Check for fast elements failed.");
  //   __ bind(&done);
  // }
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
        MemOperand(context, Context::SlotOffset(Context::CLOSURE_INDEX)));
  __ lw(result, FieldMemOperand(result, JSFunction::kContextOffset));
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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoCallConstantFunction(LCallConstantFunction* instr) {
  ASSERT(ToRegister(instr->result()).is(a0));
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
  ASSERT(ToRegister(instr->result()).is(a0));

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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoDeferredNumberTagI(LNumberTagI* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
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
                                LEnvironment* env) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoTaggedToI(LTaggedToI* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoNumberUntagD(LNumberUntagD* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoDoubleToI(LDoubleToI* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoCheckSmi(LCheckSmi* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoCheckNonSmi(LCheckNonSmi* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoCheckInstanceType(LCheckInstanceType* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoCheckFunction(LCheckFunction* instr) {
  ASSERT(instr->InputAt(0)->IsRegister());
  Register reg = ToRegister(instr->InputAt(0));
  DeoptimizeIf(ne, instr->environment(), reg,
               Operand(instr->hydrogen()->target()));
}


void LCodeGen::DoCheckMap(LCheckMap* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoCheckPrototypeMaps(LCheckPrototypeMaps* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


void LCodeGen::DoArrayLiteral(LArrayLiteral* instr) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


Condition LCodeGen::EmitTypeofIs(Label* true_label,
                                 Label* false_label,
                                 Register input,
                                 Handle<String> type_name) {
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
  return al;
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
  Abort("Unimplemented: %s (line %d)", __func__, __LINE__);
}


#undef __

} }  // namespace v8::internal
