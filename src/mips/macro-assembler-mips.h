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

#ifndef V8_MIPS_MACRO_ASSEMBLER_MIPS_H_
#define V8_MIPS_MACRO_ASSEMBLER_MIPS_H_

#include "assembler.h"
#include "mips/assembler-mips.h"

namespace v8 {
namespace internal {

// Forward declaration.
class JumpTarget;

// Reserved Register Usage Summary.
//
// Registers t8, t9, and at are reserved for use by the MacroAssembler.
//
// The programmer should know that the MacroAssembler may clobber these two,
// but won't touch other registers except in special cases.
//
// Per the MIPS ABI, register t9 must be used for indirect function call
// via 'jalr t9' or 'jr t9' instructions. This is relied upon by gcc when
// trying to update gp register for position-independent-code. Whenever
// MIPS generated code calls C code, it must be via t9 register.

// Registers aliases
// cp is assumed to be a callee saved register.
const Register roots = s6;  // Roots array pointer.
const Register cp = s7;     // JavaScript context pointer
const Register fp = s8_fp;  // Alias fp
// Register used for condition evaluation.
const Register condReg1 = s4;
const Register condReg2 = s5;

enum InvokeJSFlags {
  CALL_JS,
  JUMP_JS
};


// Flags used for the AllocateInNewSpace functions.
enum AllocationFlags {
  // No special flags.
  NO_ALLOCATION_FLAGS = 0,
  // Return the pointer to the allocated already tagged as a heap object.
  TAG_OBJECT = 1 << 0,
  // The content of the result register already contains the allocation top in
  // new space.
  RESULT_CONTAINS_TOP = 1 << 1,
  // Specify that the requested size of the space to allocate is specified in
  // words instead of bytes.
  SIZE_IN_WORDS = 1 << 2
};


// MacroAssembler implements a collection of frequently used macros.
class MacroAssembler: public Assembler {
 public:
  MacroAssembler(void* buffer, int size);

// Arguments macros
#define COND_TYPED_ARGS Condition cond, Register r1, const Operand& r2
#define COND_ARGS cond, r1, r2

// ** Prototypes

// * Prototypes for functions with no target (eg Ret()).
#define DECLARE_NOTARGET_PROTOTYPE(Name) \
  void Name(bool ProtectBranchDelaySlot = true); \
  void Name(COND_TYPED_ARGS, bool ProtectBranchDelaySlot = true); \
  inline void Name(bool ProtectBranchDelaySlot, COND_TYPED_ARGS) { \
    Name(COND_ARGS, ProtectBranchDelaySlot); \
  }

// * Prototypes for functions with a target.

// Cases when relocation may be needed.
#define DECLARE_RELOC_PROTOTYPE(Name, target_type) \
  void Name(target_type target, \
            RelocInfo::Mode rmode, \
            bool ProtectBranchDelaySlot = true); \
  inline void Name(bool ProtectBranchDelaySlot, \
                   target_type target, \
                   RelocInfo::Mode rmode) { \
    Name(target, rmode, ProtectBranchDelaySlot); \
  } \
  void Name(target_type target, \
            RelocInfo::Mode rmode, \
            COND_TYPED_ARGS, \
            bool ProtectBranchDelaySlot = true); \
  inline void Name(bool ProtectBranchDelaySlot, \
                   target_type target, \
                   RelocInfo::Mode rmode, \
                   COND_TYPED_ARGS) { \
    Name(target, rmode, COND_ARGS, ProtectBranchDelaySlot); \
  }

// Cases when relocation is not needed.
#define DECLARE_NORELOC_PROTOTYPE(Name, target_type) \
  void Name(target_type target, bool ProtectBranchDelaySlot = true); \
  inline void Name(bool ProtectBranchDelaySlot, target_type target) { \
    Name(target, ProtectBranchDelaySlot); \
  } \
  void Name(target_type target, \
            COND_TYPED_ARGS, \
            bool ProtectBranchDelaySlot = true); \
  inline void Name(bool ProtectBranchDelaySlot, \
                   target_type target, \
                   COND_TYPED_ARGS) { \
    Name(target, COND_ARGS, ProtectBranchDelaySlot); \
  }

// ** Target prototypes.

#define DECLARE_JUMP_CALL_PROTOTYPES(Name) \
  DECLARE_NORELOC_PROTOTYPE(Name, Register) \
  DECLARE_NORELOC_PROTOTYPE(Name, const Operand&) \
  DECLARE_RELOC_PROTOTYPE(Name, byte*) \
  DECLARE_RELOC_PROTOTYPE(Name, Handle<Code>)

#define DECLARE_BRANCH_PROTOTYPES(Name) \
  DECLARE_NORELOC_PROTOTYPE(Name, Label*) \
  DECLARE_NORELOC_PROTOTYPE(Name, int16_t)


DECLARE_JUMP_CALL_PROTOTYPES(Jump)
DECLARE_JUMP_CALL_PROTOTYPES(Call)

DECLARE_BRANCH_PROTOTYPES(Branch)
DECLARE_BRANCH_PROTOTYPES(BranchAndLink)

DECLARE_NOTARGET_PROTOTYPE(Ret)

#undef COND_TYPED_ARGS
#undef COND_ARGS
#undef DECLARE_NOTARGET_PROTOTYPE
#undef DECLARE_NORELOC_PROTOTYPE
#undef DECLARE_RELOC_PROTOTYPE
#undef DECLARE_JUMP_CALL_PROTOTYPES
#undef DECLARE_BRANCH_PROTOTYPES

  // Emit code to discard a non-negative number of pointer-sized elements
  // from the stack, clobbering only the sp register.
  void Drop(int count, Condition cond = cc_always);

  // Swap two registers.  If the scratch register is omitted then a slightly
  // less efficient form using xor instead of mov is emitted.
  void Swap(Register reg1, Register reg2, Register scratch = no_reg);

  void Call(Label* target);
  // May do nothing if the registers are identical.
  void Move(Register dst, Register src);


  // Jump unconditionally to given label.
  // We NEED a nop in the branch delay slot, as it used by v8, for example in
  // CodeGenerator::ProcessDeferred().
  // Currently the branch delay slot is filled by the MacroAssembler.
  // Use rather b(Label) for code generation.
  void jmp(Label* L) {
    Branch(L);
  }

  // Load an object from the root table.
  void LoadRoot(Register destination,
                Heap::RootListIndex index);
  void LoadRoot(Register destination,
                Heap::RootListIndex index,
                Condition cond, Register src1, const Operand& src2);

  // Store an object to the root table.
  void StoreRoot(Register source,
                 Heap::RootListIndex index);
  void StoreRoot(Register source,
                 Heap::RootListIndex index,
                 Condition cond, Register src1, const Operand& src2);

  // Load an external reference.
  void LoadExternalReference(Register reg, ExternalReference ext) {
    li(reg, Operand(ext));
  }



  // Check if object is in new space.
  // scratch can be object itself, but it will be clobbered.
  void InNewSpace(Register object,
                  Register scratch,
                  Condition cc,  // eq for new space, ne otherwise
                  Label* branch);


  // Set the remembered set bit for an offset into an
  // object. RecordWriteHelper only works if the object is not in new
  // space.
  void RecordWriteHelper(Register object, Register offset, Register scracth);

  // Sets the remembered set bit for [address+offset].
  void RecordWrite(Register object, Register offset, Register scratch);


  // ---------------------------------------------------------------------------
  // Inline caching support

  // Generates code that verifies that the maps of objects in the
  // prototype chain of object hasn't changed since the code was
  // generated and branches to the miss label if any map has. If
  // necessary the function also generates code for security check
  // in case of global object holders. The scratch and holder
  // registers are always clobbered, but the object register is only
  // clobbered if it the same as the holder register. The function
  // returns a register containing the holder - either object_reg or
  // holder_reg.
  // The function can optionally (when save_at_depth !=
  // kInvalidProtoDepth) save the object at the given depth by moving
  // it to [sp].
  Register CheckMaps(JSObject* object, Register object_reg,
                     JSObject* holder, Register holder_reg,
                     Register scratch,
                     int save_at_depth,
                     Label* miss);

  // Generate code for checking access rights - used for security checks
  // on access to global objects across environments. The holder register
  // is left untouched, whereas both scratch registers are clobbered.
  void CheckAccessGlobalProxy(Register holder_reg,
                              Register scratch,
                              Label* miss);


  // ---------------------------------------------------------------------------
  // Allocation support

  // Allocate an object in new space. The object_size is specified in words (not
  // bytes). If the new space is exhausted control continues at the gc_required
  // label. The allocated object is returned in result. If the flag
  // tag_allocated_object is true the result is tagged as as a heap object. All
  // registers are clobbered also when control continues at the gc_required
  // label.
  void AllocateInNewSpace(int object_size,
                          Register result,
                          Register scratch1,
                          Register scratch2,
                          Label* gc_required,
                          AllocationFlags flags);
  void AllocateInNewSpace(Register object_size,
                          Register result,
                          Register scratch1,
                          Register scratch2,
                          Label* gc_required,
                          AllocationFlags flags);

  // Undo allocation in new space. The object passed and objects allocated after
  // it will no longer be allocated. The caller must make sure that no pointers
  // are left to the object(s) no longer allocated as they would be invalid when
  // allocation is undone.
  void UndoAllocationInNewSpace(Register object, Register scratch);


  void AllocateTwoByteString(Register result,
                             Register length,
                             Register scratch1,
                             Register scratch2,
                             Register scratch3,
                             Label* gc_required);
  void AllocateAsciiString(Register result,
                           Register length,
                           Register scratch1,
                           Register scratch2,
                           Register scratch3,
                           Label* gc_required);
  void AllocateTwoByteConsString(Register result,
                                 Register length,
                                 Register scratch1,
                                 Register scratch2,
                                 Label* gc_required);
  void AllocateAsciiConsString(Register result,
                               Register length,
                               Register scratch1,
                               Register scratch2,
                               Label* gc_required);

  // Allocates a heap number or jumps to the gc_required label if the young
  // space is full and a scavenge is needed. All registers are clobbered also
  // when control continues at the gc_required label.
  void AllocateHeapNumber(Register result,
                          Register scratch1,
                          Register scratch2,
                          Label* gc_required);

  // ---------------------------------------------------------------------------
  // Instruction macros

#define DEFINE_INSTRUCTION(instr)                                              \
  void instr(Register rd, Register rs, const Operand& rt);                     \
  void instr(Register rd, Register rs, Register rt) {                          \
    instr(rd, rs, Operand(rt));                                                \
  }                                                                            \
  void instr(Register rs, Register rt, int32_t j) {                            \
    instr(rs, rt, Operand(j));                                                 \
  }

#define DEFINE_INSTRUCTION2(instr)                                             \
  void instr(Register rs, const Operand& rt);                                  \
  void instr(Register rs, Register rt) {                                       \
    instr(rs, Operand(rt));                                                    \
  }                                                                            \
  void instr(Register rs, int32_t j) {                                         \
    instr(rs, Operand(j));                                                     \
  }

  DEFINE_INSTRUCTION(Addu);
  DEFINE_INSTRUCTION(Subu);
  DEFINE_INSTRUCTION(Mul);
  DEFINE_INSTRUCTION2(Mult);
  DEFINE_INSTRUCTION2(Multu);
  DEFINE_INSTRUCTION2(Div);
  DEFINE_INSTRUCTION2(Divu);

  DEFINE_INSTRUCTION(And);
  DEFINE_INSTRUCTION(Or);
  DEFINE_INSTRUCTION(Xor);
  DEFINE_INSTRUCTION(Nor);

  DEFINE_INSTRUCTION(Slt);
  DEFINE_INSTRUCTION(Sltu);

#undef DEFINE_INSTRUCTION
#undef DEFINE_INSTRUCTION2


  //------------Pseudo-instructions-------------

  void mov(Register rd, Register rt) { or_(rd, rt, zero_reg); }


  // load int32 in the rd register
  void li(Register rd, Operand j, bool gen2instr = false);
  inline void li(Register rd, int32_t j, bool gen2instr = false) {
    li(rd, Operand(j), gen2instr);
  }
  inline void li(Register dst, Handle<Object> value, bool gen2instr = false) {
    li(dst, Operand(value), gen2instr);
  }

  // Exception-generating instructions and debugging support
  void stop(const char* msg);


  // Push multiple registers on the stack.
  // Registers are saved in numerical order, with higher numbered registers
  // saved in higher memory addresses
  void MultiPush(RegList regs);
  void MultiPushReversed(RegList regs);

  void Push(Register src) {
    Addu(sp, sp, Operand(-kPointerSize));
    sw(src, MemOperand(sp, 0));
  }

  // Push two registers.  Pushes leftmost register first (to highest address).
  void Push(Register src1, Register src2, Condition cond = al) {
    ASSERT(cond == al);  // Do not support conditional versions yet.
    ASSERT(!src1.is(src2));
    Addu(sp, sp, Operand(2 * -kPointerSize));
    sw(src1, MemOperand(sp, 1 * kPointerSize));
    sw(src2, MemOperand(sp, 0 * kPointerSize));
  }

  // Push three registers.  Pushes leftmost register first (to highest address).
  void Push(Register src1, Register src2, Register src3, Condition cond = al) {
    ASSERT(cond == al);  // Do not support conditional versions yet.
    ASSERT(!src1.is(src2));
    ASSERT(!src2.is(src3));
    ASSERT(!src1.is(src3));
    Addu(sp, sp, Operand(3 * -kPointerSize));
    sw(src1, MemOperand(sp, 2 * kPointerSize));
    sw(src2, MemOperand(sp, 1 * kPointerSize));
    sw(src3, MemOperand(sp, 0 * kPointerSize));
  }

  // Push four registers.  Pushes leftmost register first (to highest address).
  void Push(Register src1, Register src2,
            Register src3, Register src4, Condition cond = al) {
    ASSERT(cond == al);  // Do not support conditional versions yet.
    ASSERT(!src1.is(src2));
    ASSERT(!src2.is(src3));
    ASSERT(!src1.is(src3));
    ASSERT(!src1.is(src4));
    ASSERT(!src2.is(src4));
    ASSERT(!src3.is(src4));
    Addu(sp, sp, Operand(4 * -kPointerSize));
    sw(src1, MemOperand(sp, 3 * kPointerSize));
    sw(src2, MemOperand(sp, 2 * kPointerSize));
    sw(src3, MemOperand(sp, 1 * kPointerSize));
    sw(src4, MemOperand(sp, 0 * kPointerSize));
  }
  
  inline void push(Register src) { Push(src); }

  void Push(Register src, Condition cond, Register tst1, Register tst2) {
    // Since we don't have conditionnal execution we use a Branch.
    Branch(3, cond, tst1, Operand(tst2));
    Addu(sp, sp, Operand(-kPointerSize));
    sw(src, MemOperand(sp, 0));
  }


  // Pops multiple values from the stack and load them in the
  // registers specified in regs. Pop order is the opposite as in MultiPush.
  void MultiPop(RegList regs);
  void MultiPopReversed(RegList regs);
  void Pop(Register dst) {
    lw(dst, MemOperand(sp, 0));
    Addu(sp, sp, Operand(kPointerSize));
  }
  void Pop(uint32_t count = 1) {
    Addu(sp, sp, Operand(count * kPointerSize));
  }


  // -------------------------------------------------------------------------
  // Activation frames

  void EnterInternalFrame() { EnterFrame(StackFrame::INTERNAL); }
  void LeaveInternalFrame() { LeaveFrame(StackFrame::INTERNAL); }

  void EnterConstructFrame() { EnterFrame(StackFrame::CONSTRUCT); }
  void LeaveConstructFrame() { LeaveFrame(StackFrame::CONSTRUCT); }

  // Enter specific kind of exit frame; either EXIT or
  // EXIT_DEBUG. Expects the number of arguments in register a0 and
  // the builtin function to call in register a1.
  // On output hold_argc, hold_function, and hold_argv are setup.
  void EnterExitFrame(ExitFrame::Mode mode,
                      Register hold_argc,
                      Register hold_argv,
                      Register hold_function);

  // Leave the current exit frame. Expects the return value in v0.
  void LeaveExitFrame(ExitFrame::Mode mode);

  // Align the stack by optionally pushing a Smi zero.
  void AlignStack(int offset);    // TODO(REBASE) : remove this function.

  // Get the actual activation frame alignment for target environment.
  static int ActivationFrameAlignment();

  // -------------------------------------------------------------------------
  // JavaScript invokes

  // Invoke the JavaScript function code by either calling or jumping.
  void InvokeCode(Register code,
                  const ParameterCount& expected,
                  const ParameterCount& actual,
                  InvokeFlag flag);

  void InvokeCode(Handle<Code> code,
                  const ParameterCount& expected,
                  const ParameterCount& actual,
                  RelocInfo::Mode rmode,
                  InvokeFlag flag);

  // Invoke the JavaScript function in the given register. Changes the
  // current context to the context in the function before invoking.
  void InvokeFunction(Register function,
                      const ParameterCount& actual,
                      InvokeFlag flag);

  void InvokeFunction(JSFunction* function,
                      const ParameterCount& actual,
                      InvokeFlag flag);


#ifdef ENABLE_DEBUGGER_SUPPORT
  // -------------------------------------------------------------------------
  // Debugger Support

  void SaveRegistersToMemory(RegList regs);
  void RestoreRegistersFromMemory(RegList regs);
  void CopyRegistersFromMemoryToStack(Register base, RegList regs);
  void CopyRegistersFromStackToMemory(Register base,
                                      Register scratch,
                                      RegList regs);
  void DebugBreak();
#endif


  // -------------------------------------------------------------------------
  // Exception handling

  // Push a new try handler and link into try handler chain.
  // The return address must be passed in register ra.
  // Clobber t0, t1, t2.
  void PushTryHandler(CodeLocation try_location, HandlerType type);

  // Unlink the stack handler on top of the stack from the try handler chain.
  // Must preserve the result register.
  void PopTryHandler();


  // -------------------------------------------------------------------------
  // Support functions.

  // Try to get function prototype of a function and puts the value in
  // the result register. Checks that the function really is a
  // function and jumps to the miss label if the fast checks fail. The
  // function register will be untouched; the other registers may be
  // clobbered.
  void TryGetFunctionPrototype(Register function,
                               Register result,
                               Register scratch,
                               Label* miss);

  void GetObjectType(Register function,
                     Register map,
                     Register type_reg);

  // Check if the map of an object is equal to a specified map and
  // branch to label if not. Skip the smi check if not required
  // (object is known to be a heap object).
  void CheckMap(Register obj,
                Register scratch,
                Handle<Map> map,
                Label* fail,
                bool is_heap_object);

  inline void BranchOnSmi(Register value, Label* smi_label,
                          Register scratch = at) {
    ASSERT_EQ(0, kSmiTag);
    andi(scratch, value, kSmiTagMask);
    Branch(smi_label, eq, scratch, Operand(zero_reg));
  }


  inline void BranchOnNotSmi(Register value, Label* not_smi_label,
                             Register scratch = at) {
    ASSERT_EQ(0, kSmiTag);
    andi(scratch, value, kSmiTagMask);
    Branch(not_smi_label, ne, scratch, Operand(zero_reg));
  }

  void CallBuiltin(ExternalReference builtin_entry);
  void CallBuiltin(Register target);
  void CallBuiltin(Handle<Code> code, RelocInfo::Mode rmode);
  void JumpToBuiltin(ExternalReference builtin_entry);
  void JumpToBuiltin(Register target);
  void JumpToBuiltin(Handle<Code> code, RelocInfo::Mode rmode);

  // Generates code for reporting that an illegal operation has
  // occurred.
  void IllegalOperation(int num_arguments);


  // -------------------------------------------------------------------------
  // Runtime calls

  // Call a code stub.
  void CallStub(CodeStub* stub, Condition cond = cc_always,
                Register r1 = zero_reg, const Operand& r2 = Operand(zero_reg));

  // Tail call a code stub (jump).
  void TailCallStub(CodeStub* stub);

  void CallJSExitStub(CodeStub* stub);

  // Return from a code stub after popping its arguments.
  void StubReturn(int argc);

  // Call a runtime routine.
  void CallRuntime(Runtime::Function* f, int num_arguments);

  // Convenience function: Same as above, but takes the fid instead.
  void CallRuntime(Runtime::FunctionId fid, int num_arguments);

  // Convenience function: call an external reference.
  void CallExternalReference(const ExternalReference& ext,
                             int num_arguments);

  // Tail call of a runtime routine (jump).
  // Like JumpToExternalReference, but also takes care of passing the number
  // of parameters.
  void TailCallExternalReference(const ExternalReference& ext,
                                 int num_arguments,
                                 int result_size);

  // Convenience function: tail call a runtime routine (jump).
  void TailCallRuntime(Runtime::FunctionId fid,
                       int num_arguments,
                       int result_size);

  // Before calling a C-function from generated code, align arguments on stack
  // and add space for the four mips argument slots.
  // After aligning the frame, non-register arguments must be stored in
  // sp[kCFuncArg_5], sp[kCFuncArg_6], etc., not pushed.
  // The argument count assumes all arguments are word sized.
  // Some compilers/platforms require the stack to be aligned when calling
  // C++ code.
  // Needs a scratch register to do some arithmetic. This register will be
  // trashed.
  void PrepareCallCFunction(int num_arguments, Register scratch);

  // Arguments 1-4 are placed in registers a0 thru a3 respectively.
  // Arguments 5..n are stored to stack using following constants:
  //  sw(t0, MemOperand(sp, MacroAssembler::kCFuncArg_5));
  static const int kCFuncArg_5 =
      (0 * kPointerSize + StandardFrameConstants::kCArgsSlotsSize);
  static const int kCFuncArg_6 =
      (1 * kPointerSize + StandardFrameConstants::kCArgsSlotsSize);
  static const int kCFuncArg_7 =
      (2 * kPointerSize + StandardFrameConstants::kCArgsSlotsSize);
  static const int kCFuncArg_8 =
      (3 * kPointerSize + StandardFrameConstants::kCArgsSlotsSize);

  // Calls a C function and cleans up the space for arguments allocated
  // by PrepareCallCFunction. The called function is not allowed to trigger a
  // garbage collection, since that might move the code and invalidate the
  // return address (unless this is somehow accounted for by the called
  // function).
  void CallCFunction(ExternalReference function, int num_arguments);
  void CallCFunction(Register function, int num_arguments);

  // Jump to the builtin routine.
  void JumpToExternalReference(const ExternalReference& builtin);

  // Invoke specified builtin JavaScript function. Adds an entry to
  // the unresolved list if the name does not resolve.
  void InvokeBuiltin(Builtins::JavaScript id, InvokeJSFlags flags);

  // Store the code object for the given builtin in the target register and
  // setup the function in a1.
  void GetBuiltinEntry(Register target, Builtins::JavaScript id);

  struct Unresolved {
    int pc;
    uint32_t flags;  // see Bootstrapper::FixupFlags decoders/encoders.
    const char* name;
  };

  Handle<Object> CodeObject() { return code_object_; }


  // -------------------------------------------------------------------------
  // Stack limit support

  void StackLimitCheck(Label* on_stack_limit_hit);


  // -------------------------------------------------------------------------
  // StatsCounter support

  void SetCounter(StatsCounter* counter, int value,
                  Register scratch1, Register scratch2);
  void IncrementCounter(StatsCounter* counter, int value,
                        Register scratch1, Register scratch2);
  void DecrementCounter(StatsCounter* counter, int value,
                        Register scratch1, Register scratch2);


  // -------------------------------------------------------------------------
  // Debugging

  // Calls Abort(msg) if the condition cc is not satisfied.
  // Use --debug_code to enable.
  void Assert(Condition cc, const char* msg, Register rs, Operand rt);

  // Like Assert(), but always enabled.
  void Check(Condition cc, const char* msg, Register rs, Operand rt);

  // Print a message to stdout and abort execution.
  void Abort(const char* msg);

  // Verify restrictions about code generated in stubs.
  void set_generating_stub(bool value) { generating_stub_ = value; }
  bool generating_stub() { return generating_stub_; }
  void set_allow_stub_calls(bool value) { allow_stub_calls_ = value; }
  bool allow_stub_calls() { return allow_stub_calls_; }

  // -------------------------------------------------------------------------
  // Smi utilities

  // Jump if either of the registers contain a non-smi.
  void JumpIfNotBothSmi(Register reg1, Register reg2, Label* on_not_both_smi);
  // Jump if either of the registers contain a smi.
  void JumpIfEitherSmi(Register reg1, Register reg2, Label* on_either_smi);

  // -------------------------------------------------------------------------
  // String utilities

  // Checks if both instance types are sequential ASCII strings and jumps to
  // label if either is not.
  void JumpIfBothInstanceTypesAreNotSequentialAscii(
      Register first_object_instance_type,
      Register second_object_instance_type,
      Register scratch1,
      Register scratch2,
      Label* failure);

  // Check if instance type is sequential ASCII string and jump to label if
  // it is not.
  void JumpIfInstanceTypeIsNotSequentialAscii(Register type,
                                              Register scratch,
                                              Label* failure);

  // Test that both first and second are sequential ASCII strings.
  // Assume that they are non-smis.
  void JumpIfNonSmisNotBothSequentialAsciiStrings(Register first,
                                                  Register second,
                                                  Register scratch1,
                                                  Register scratch2,
                                                  Label* failure);

  // Test that both first and second are sequential ASCII strings.
  // Check that they are non-smis.
  void JumpIfNotBothSequentialAsciiStrings(Register first,
                                           Register second,
                                           Register scratch1,
                                           Register scratch2,
                                           Label* failure);

 private:
  void Jump(intptr_t target, RelocInfo::Mode rmode,
            bool ProtectBranchDelaySlot = true);
  void Jump(intptr_t target, RelocInfo::Mode rmode, Condition cond = cc_always,
            Register r1 = zero_reg, const Operand& r2 = Operand(zero_reg),
            bool ProtectBranchDelaySlot = true);
  void Call(intptr_t target, RelocInfo::Mode rmode,
            bool ProtectBranchDelaySlot = true);
  void Call(intptr_t target, RelocInfo::Mode rmode, Condition cond = cc_always,
            Register r1 = zero_reg, const Operand& r2 = Operand(zero_reg),
            bool ProtectBranchDelaySlot = true);

  // Helper functions for generating invokes.
  void InvokePrologue(const ParameterCount& expected,
                      const ParameterCount& actual,
                      Handle<Code> code_constant,
                      Register code_reg,
                      Label* done,
                      InvokeFlag flag);

  // Get the code for the given builtin. Returns if able to resolve
  // the function in the 'resolved' flag.
  Handle<Code> ResolveBuiltin(Builtins::JavaScript id, bool* resolved);

  // Activation support.
  void EnterFrame(StackFrame::Type type);
  void LeaveFrame(StackFrame::Type type);

  void InitializeNewString(Register string,
                           Register length,
                           Heap::RootListIndex map_index,
                           Register scratch1,
                           Register scratch2);


  bool generating_stub_;
  bool allow_stub_calls_;
  // This handle will be patched with the code object on installation.
  Handle<Object> code_object_;
};


#ifdef ENABLE_DEBUGGER_SUPPORT
// The code patcher is used to patch (typically) small parts of code e.g. for
// debugging and other types of instrumentation. When using the code patcher
// the exact number of bytes specified must be emitted. It is not legal to emit
// relocation information. If any of these constraints are violated it causes
// an assertion to fail.
class CodePatcher {
 public:
  CodePatcher(byte* address, int instructions);
  virtual ~CodePatcher();

  // Macro assembler to emit code.
  MacroAssembler* masm() { return &masm_; }

  // Emit an instruction directly.
  void Emit(Instr x);

  // Emit an address directly.
  void Emit(Address addr);

 private:
  byte* address_;  // The address of the code being patched.
  int instructions_;  // Number of instructions of the expected patch size.
  int size_;  // Number of bytes of the expected patch size.
  MacroAssembler masm_;  // Macro assembler used to generate the code.
};
#endif  // ENABLE_DEBUGGER_SUPPORT



// -----------------------------------------------------------------------------
// Static helper functions.

// Generate a MemOperand for loading a field from an object.
static inline MemOperand FieldMemOperand(Register object, int offset) {
  return MemOperand(object, offset - kHeapObjectTag);
}



#ifdef GENERATED_CODE_COVERAGE
#define CODE_COVERAGE_STRINGIFY(x) #x
#define CODE_COVERAGE_TOSTRING(x) CODE_COVERAGE_STRINGIFY(x)
#define __FILE_LINE__ __FILE__ ":" CODE_COVERAGE_TOSTRING(__LINE__)
#define ACCESS_MASM(masm) masm->stop(__FILE_LINE__); masm->
#else
#define ACCESS_MASM(masm) masm->
#endif

} }  // namespace v8::internal

#endif  // V8_MIPS_MACRO_ASSEMBLER_MIPS_H_

