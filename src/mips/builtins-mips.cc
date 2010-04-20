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

#include "codegen-inl.h"
#include "debug.h"
#include "runtime.h"

namespace v8 {
namespace internal {


#define __ ACCESS_MASM(masm)


void Builtins::Generate_Adaptor(MacroAssembler* masm,
                                CFunctionId id,
                                BuiltinExtraArguments extra_args) {
  // a0                 : number of arguments excluding receiver
  // a1                 : called function (only guaranteed when
  //                      extra_args requires it)
  // cp                 : context
  // sp[0]              : last argument
  // ...
  // sp[4 * (argc - 1) + builtins arguments slots] : first argument
  // sp[4 * agc + builtins arguments slots]       : receiver

  // Insert extra arguments.
  int num_extra_args = 0;
  if (extra_args == NEEDS_CALLED_FUNCTION) {
    // We need to push a1 after the original arguments and before the arguments
    // slots.
    num_extra_args = 1;
    __ Add(sp, sp, -4);
    __ sw(a1, MemOperand(sp, StandardFrameConstants::kBArgsSlotsSize));
  } else {
    ASSERT(extra_args == NO_EXTRA_ARGUMENTS);
  }

  // JumpToExternalReference expects a0 to contain the number of arguments
  // including the receiver and the extra arguments.
  __ Add(a0, a0, Operand(num_extra_args + 1));
  __ JumpToExternalReference(ExternalReference(id));
}


void Builtins::Generate_ArrayCode(MacroAssembler* masm) {
  // Just jump to the generic array code.
  Code* code = Builtins::builtin(Builtins::ArrayCodeGeneric);
  Handle<Code> array_code(code);
  // We are already in a builtin and did not touch to the stack, so use a simple
  // Jump to call Builtins::ArrayCodeGeneric.
  __ Jump(array_code, RelocInfo::CODE_TARGET);
}


void Builtins::Generate_ArrayConstructCode(MacroAssembler* masm) {
  // Just jump to the generic construct code.
  Code* code = Builtins::builtin(Builtins::JSConstructStubGeneric);
  Handle<Code> generic_construct_stub(code);
  // We are already in a builtin and did not touch to the stack, so use a simple
  // Jump to call Builtins::JSConstructStubGeneric.
  __ Jump(generic_construct_stub, RelocInfo::CODE_TARGET);
}


void Builtins::Generate_JSConstructCall(MacroAssembler* masm) {
  // a0     : number of arguments
  // a1     : constructor function
  // ra     : return address
  // sp + args slots [...]: constructor arguments

  Label non_function_call;
  // Check that the function is not a smi.
  __ And(t0, a1, Operand(kSmiTagMask));
  __ Branch(eq, &non_function_call, t0, Operand(zero_reg));
  // Check that the function is a JSFunction.
  __ GetObjectType(a1, a2, a2);
  __ Branch(ne, &non_function_call, a2, Operand(JS_FUNCTION_TYPE));

  // Jump to the function-specific construct stub.
  __ lw(a2, FieldMemOperand(a1, JSFunction::kSharedFunctionInfoOffset));
  __ lw(a2, FieldMemOperand(a2, SharedFunctionInfo::kConstructStubOffset));
  __ Add(t9, a2, Operand(Code::kHeaderSize - kHeapObjectTag));
  __ Jump(Operand(t9));

  // a0: number of arguments
  // a1: called object
  __ bind(&non_function_call);
  // CALL_NON_FUNCTION expects the non-function constructor as receiver
  // (instead of the original receiver from the call site). The receiver is
  // stack element argc.
  __ sll(t0, a0, kPointerSizeLog2);
  __ Add(t0, t0, StandardFrameConstants::kBArgsSlotsSize);
  __ Add(t0, t0, sp);
  __ sw(a1, MemOperand(t0));
  // Set expected number of arguments to zero (not changing a0).
  __ li(a2, Operand(0));
  __ GetBuiltinEntry(a3, Builtins::CALL_NON_FUNCTION_AS_CONSTRUCTOR);
  // ra-dev: Already inside builtin, so don't need args slots?
  __ break_(0x94);
  __ JumpToBuiltin(Handle<Code>(builtin(ArgumentsAdaptorTrampoline)),
          RelocInfo::CODE_TARGET);
}


static void Generate_JSConstructStubHelper(MacroAssembler* masm,
                                           bool is_api_function) {
  // a0     : number of arguments
  // a1     : constructor function
  // ra     : return address
  // sp + args slots [...]: constructor arguments

  // Enter a construct frame.
  __ EnterConstructFrame();

  // Preserve the two incoming parameters on the stack.
  __ sll(a0, a0, kSmiTagSize);  // Tag arguments count.
  __ MultiPushReversed(a0.bit() | a1.bit());

  // Use t7 to hold undefined, which is used in several places below.
  __ LoadRoot(t7, Heap::kUndefinedValueRootIndex);

  Label rt_call, allocated;
  // Try to allocate the object without transitioning into C code. If any of the
  // preconditions is not met, the code bails out to the runtime call.
  if (FLAG_inline_new) {
    Label undo_allocation;
#ifdef ENABLE_DEBUGGER_SUPPORT
    UNIMPLEMENTED_MIPS();
#endif

    // Load the initial map and verify that it is in fact a map.
    // a1: constructor function
    // t7: undefined
    __ lw(a2, FieldMemOperand(a1, JSFunction::kPrototypeOrInitialMapOffset));
    __ And(t0, a2, Operand(kSmiTagMask));
    __ Branch(eq, &rt_call, t0, Operand(zero_reg));
    __ GetObjectType(a2, a3, t4);
    __ Branch(ne, &rt_call, t4, Operand(MAP_TYPE));

    // Check that the constructor is not constructing a JSFunction (see comments
    // in Runtime_NewObject in runtime.cc). In which case the initial map's
    // instance type would be JS_FUNCTION_TYPE.
    // a1: constructor function
    // a2: initial map
    // t7: undefined
    __ lbu(a3, FieldMemOperand(a2, Map::kInstanceTypeOffset));
    __ Branch(eq, &rt_call, a3, Operand(JS_FUNCTION_TYPE));

    // Now allocate the JSObject on the heap.
    // constructor function
    // a2: initial map
    // t7: undefined
    __ lbu(a3, FieldMemOperand(a2, Map::kInstanceSizeOffset));
    __ AllocateInNewSpace(a3, t4, t5, t6, &rt_call, NO_ALLOCATION_FLAGS);

    // Allocated the JSObject, now initialize the fields. Map is set to initial
    // map and properties and elements are set to empty fixed array.
    // a1: constructor function
    // a2: initial map
    // a3: object size
    // t4: JSObject (not tagged)
    // t7: undefined
    __ LoadRoot(t6, Heap::kEmptyFixedArrayRootIndex);
    __ mov(t5, t4);
    __ sw(a2, MemOperand(t5, JSObject::kMapOffset));
    __ sw(t6, MemOperand(t5, JSObject::kPropertiesOffset));
    __ sw(t6, MemOperand(t5, JSObject::kElementsOffset));
    __ Addu(t5, t5, Operand(3*kPointerSize));
    ASSERT_EQ(0 * kPointerSize, JSObject::kMapOffset);
    ASSERT_EQ(1 * kPointerSize, JSObject::kPropertiesOffset);
    ASSERT_EQ(2 * kPointerSize, JSObject::kElementsOffset);

    // Fill all the in-object properties with undefined.
    // a1: constructor function
    // a2: initial map
    // a3: object size (in words)
    // t4: JSObject (not tagged)
    // t5: First in-object property of JSObject (not tagged)
    // t7: undefined
    __ sll(t0, a3, kPointerSizeLog2);
    __ add(t6, t4, t0);   // End of object.
    ASSERT_EQ(3 * kPointerSize, JSObject::kHeaderSize);
    { Label loop, entry;
      __ jmp(&entry);
      __ bind(&loop);
      __ sw(t7, MemOperand(t5, 0));
      __ addiu(t5, t5, kPointerSize);
      __ bind(&entry);
      __ Branch(Uless, &loop, t5, Operand(t6));
    }

    // Add the object tag to make the JSObject real, so that we can continue and
    // jump into the continuation code at any time from now on. Any failures
    // need to undo the allocation, so that the heap is in a consistent state
    // and verifiable.
    __ Add(t4, t4, Operand(kHeapObjectTag));

    // Check if a non-empty properties array is needed. Continue with allocated
    // object if not fall through to runtime call if it is.
    // a1: constructor function
    // t4: JSObject
    // t5: start of next object (not tagged)
    // t7: undefined
    __ lbu(a3, FieldMemOperand(a2, Map::kUnusedPropertyFieldsOffset));
    // The field instance sizes contains both pre-allocated property fields and
    // in-object properties.
    __ lw(a0, FieldMemOperand(a2, Map::kInstanceSizesOffset));
    __ And(t6,
           a0,
           Operand(0x000000FF << Map::kPreAllocatedPropertyFieldsByte * 8));
    __ srl(t0, t6, Map::kPreAllocatedPropertyFieldsByte * 8);
    __ Addu(a3, a3, Operand(t0));
    __ And(t6, a0, Operand(0x000000FF << Map::kInObjectPropertiesByte * 8));
    __ srl(t0, t6, Map::kInObjectPropertiesByte * 8);
    __ sub(a3, a3, t0);

    // Done if no extra properties are to be allocated.
    __ Branch(eq, &allocated, a3, Operand(zero_reg));
    __ Assert(greater_equal, "Property allocation count failed.",
        a3, Operand(zero_reg));

    // Scale the number of elements by pointer size and add the header for
    // FixedArrays to the start of the next object calculation from above.
    // a1: constructor
    // a3: number of elements in properties array
    // t4: JSObject
    // t5: start of next object
    // t7: undefined
    __ Addu(a0, a3, Operand(FixedArray::kHeaderSize / kPointerSize));
    __ AllocateInNewSpace(a0,
                          t5,
                          t6,
                          a2,
                          &undo_allocation,
                          RESULT_CONTAINS_TOP);

    // Initialize the FixedArray.
    // a1: constructor
    // a3: number of elements in properties array
    // t4: JSObject
    // t5: start of next object
    // t7: undefined
    __ LoadRoot(t6, Heap::kFixedArrayMapRootIndex);
    __ mov(a2, t5);
    __ sw(t6, MemOperand(a2, JSObject::kMapOffset));
    __ sw(a3, MemOperand(a2, Array::kLengthOffset));
    __ Addu(a2, a2, Operand(2 * kPointerSize));

    ASSERT_EQ(0 * kPointerSize, JSObject::kMapOffset);
    ASSERT_EQ(1 * kPointerSize, Array::kLengthOffset);

    // Initialize the fields to undefined.
    // a1: constructor
    // a2: First element of FixedArray (not tagged)
    // a3: number of elements in properties array
    // t4: JSObject
    // t5: FixedArray (not tagged)
    // t7: undefined
    __ sll(t3, a3, kPointerSizeLog2);
    __ addu(t6, a2, t3);  // End of object.
    ASSERT_EQ(2 * kPointerSize, FixedArray::kHeaderSize);
    { Label loop, entry;
      __ jmp(&entry);
      __ bind(&loop);
      __ sw(t7, MemOperand(a2));
      __ addiu(a2, a2, kPointerSize);
      __ bind(&entry);
      __ Branch(less, &loop, a2, Operand(t6));
    }

    // Store the initialized FixedArray into the properties field of
    // the JSObject
    // a1: constructor function
    // t4: JSObject
    // t5: FixedArray (not tagged)
    __ Add(t5, t5, Operand(kHeapObjectTag));  // Add the heap tag.
    __ sw(t5, FieldMemOperand(t4, JSObject::kPropertiesOffset));

    // Continue with JSObject being successfully allocated
    // a1: constructor function
    // a4: JSObject
    __ jmp(&allocated);

    // Undo the setting of the new top so that the heap is verifiable. For
    // example, the map's unused properties potentially do not match the
    // allocated objects unused properties.
    // t4: JSObject (previous new top)
    __ bind(&undo_allocation);
    __ UndoAllocationInNewSpace(t4, t5);
  }

  __ bind(&rt_call);
  // Allocate the new receiver object using the runtime call.
  // a1: constructor function
  __ Push(a1);  // Argument for Runtime_NewObject.
  __ CallRuntime(Runtime::kNewObject, 1);
  __ mov(t4, v0);

  // Receiver for constructor call allocated.
  // t4: JSObject
  __ bind(&allocated);
  __ Push(t4);

  // Push the function and the allocated receiver from the stack.
  // sp[0]: receiver (newly allocated object)
  // sp[1]: constructor function
  // sp[2]: number of arguments (smi-tagged)
  __ lw(a1, MemOperand(sp, kPointerSize));
  __ MultiPushReversed(a1.bit() | t4.bit());

  // Reload the number of arguments from the stack.
  // a1: constructor function
  // sp[0]: receiver
  // sp[1]: constructor function
  // sp[2]: receiver
  // sp[3]: constructor function
  // sp[4]: number of arguments (smi-tagged)
  __ lw(a3, MemOperand(sp, 4 * kPointerSize));

  // Setup pointer to last argument.
  __ Add(a2, fp, Operand(StandardFrameConstants::kCallerSPOffset
                          + StandardFrameConstants::kBArgsSlotsSize));

  // Setup number of arguments for function call below
  __ srl(a0, a3, kSmiTagSize);

  // Copy arguments and receiver to the expression stack.
  // a0: number of arguments
  // a1: constructor function
  // a2: address of last argument (caller sp)
  // a3: number of arguments (smi-tagged)
  // sp[0]: receiver
  // sp[1]: constructor function
  // sp[2]: receiver
  // sp[3]: constructor function
  // sp[4]: number of arguments (smi-tagged)
  Label loop, entry;
  __ jmp(&entry);
  __ bind(&loop);
  __ sll(t0, a3, kPointerSizeLog2 - kSmiTagSize);
  __ Addu(t0, a2, Operand(t0));
  __ lw(t1, MemOperand(t0));
  __ Push(t1);
  __ bind(&entry);
  __ Addu(a3, a3, Operand(-2));
  __ Branch(greater_equal, &loop, a3, Operand(zero_reg));

  // Call the function.
  // a0: number of arguments
  // a1: constructor function
  if (is_api_function) {
    __ lw(cp, FieldMemOperand(a1, JSFunction::kContextOffset));
    Handle<Code> code = Handle<Code>(
        Builtins::builtin(Builtins::HandleApiCallConstruct));
    ParameterCount expected(0);
    __ InvokeCode(code, expected, expected,
                  RelocInfo::CODE_TARGET, CALL_FUNCTION);
  } else {
    ParameterCount actual(a0);
    __ InvokeFunction(a1, actual, CALL_FUNCTION);
  }

  // Pop the function from the stack.
  // v0: result
  // sp[0]: constructor function
  // sp[2]: receiver
  // sp[3]: constructor function
  // sp[4]: number of arguments (smi-tagged)
  __ Pop();

  // Restore context from the frame.
  __ lw(cp, MemOperand(fp, StandardFrameConstants::kContextOffset));

  // If the result is an object (in the ECMA sense), we should get rid
  // of the receiver and use the result; see ECMA-262 section 13.2.2-7
  // on page 74.
  Label use_receiver, exit;

  // If the result is a smi, it is *not* an object in the ECMA sense.
  // v0: result
  // sp[0]: receiver (newly allocated object)
  // sp[1]: constructor function
  // sp[2]: number of arguments (smi-tagged)
  __ And(t0, v0, Operand(kSmiTagMask));
  __ Branch(eq, &use_receiver, t0, Operand(zero_reg));

  // If the type of the result (stored in its map) is less than
  // FIRST_JS_OBJECT_TYPE, it is not an object in the ECMA sense.
  __ GetObjectType(v0, a3, a3);
  __ Branch(greater_equal, &exit, a3, Operand(FIRST_JS_OBJECT_TYPE));

  // Throw away the result of the constructor invocation and use the
  // on-stack receiver as the result.
  __ bind(&use_receiver);
  __ lw(v0, MemOperand(sp));

  // Remove receiver from the stack, remove caller arguments, and
  // return.
  __ bind(&exit);
  // v0: result
  // sp[0]: receiver (newly allocated object)
  // sp[1]: constructor function
  // sp[2]: number of arguments (smi-tagged)
  __ lw(a1, MemOperand(sp, 2 * kPointerSize));
  __ LeaveConstructFrame();
  __ sll(t0, a1, kPointerSizeLog2 - 1);
  __ Add(sp, sp, t0);
  __ Add(sp, sp, kPointerSize);
  __ IncrementCounter(&Counters::constructed_objects, 1, a1, a2);
  __ Ret();
}


void Builtins::Generate_JSConstructStubGeneric(MacroAssembler* masm) {
  Generate_JSConstructStubHelper(masm, false);
}


void Builtins::Generate_JSConstructStubApi(MacroAssembler* masm) {
  Generate_JSConstructStubHelper(masm, true);
}


static void Generate_JSEntryTrampolineHelper(MacroAssembler* masm,
                                             bool is_construct) {
  // Called from JSEntryStub::GenerateBody

  // Registers:
  // a0: entry_address
  // a1: function
  // a2: reveiver_pointer
  // a3: argc
  // s0: argv
  //
  // Stack:
  // arguments slots
  // handler frame
  // entry frame
  // callee saved registers + ra
  // 4 args slots
  // args

  // Clear the context before we push it when entering the JS frame.
  __ li(cp, Operand(0));

  // Enter an internal frame.
  __ EnterInternalFrame();

  // Set up the context from the function argument.
  __ lw(cp, FieldMemOperand(a1, JSFunction::kContextOffset));

  // Set up the roots register.
  ExternalReference roots_address = ExternalReference::roots_address();
  __ li(s6, Operand(roots_address));

  // Push the function and the receiver onto the stack.
  __ MultiPushReversed(a1.bit() | a2.bit());

  // Copy arguments to the stack in a loop.
  // a3: argc
  // s0: argv, ie points to first arg
  Label loop, entry;
  __ sll(t0, a3, kPointerSizeLog2);
  __ addu(t2, s0, t0);
  __ b(&entry);
  __ nop();   // Branch delay slot nop.
  // t2 points past last arg.
  __ bind(&loop);
  __ lw(t0, MemOperand(s0));  // Read next parameter.
  __ addiu(s0, s0, kPointerSize);
  __ lw(t0, MemOperand(t0));  // Dereference handle.
  __ Push(t0);  // Push parameter.
  __ bind(&entry);
  __ Branch(ne, &loop, s0, Operand(t2));

  // Registers:
  // a0: entry_address
  // a1: function
  // a2: reveiver_pointer
  // a3: argc
  // s0: argv
  // s6: roots_address
  //
  // Stack:
  // arguments
  // receiver
  // function
  // arguments slots
  // handler frame
  // entry frame
  // callee saved registers + ra
  // 4 args slots
  // args

  // Initialize all JavaScript callee-saved registers, since they will be seen
  // by the garbage collector as part of handlers.
  __ LoadRoot(t0, Heap::kUndefinedValueRootIndex);
  __ mov(s1, t0);
  __ mov(s2, t0);
  __ mov(s3, t0);
  __ mov(s4, s0);
  __ mov(s5, t0);
  // s6 holds the root address. Do not clobber.
  // s7 is cp. Do not init.

  // Invoke the code and pass argc as a0.
  __ mov(a0, a3);
  if (is_construct) {
    __ CallBuiltin(Handle<Code>(Builtins::builtin(Builtins::JSConstructCall)),
            RelocInfo::CODE_TARGET);
  } else {
    ParameterCount actual(a0);
    __ InvokeFunction(a1, actual, CALL_FUNCTION);
  }

  __ LeaveInternalFrame();

  __ Jump(ra);
}


void Builtins::Generate_JSEntryTrampoline(MacroAssembler* masm) {
  Generate_JSEntryTrampolineHelper(masm, false);
}


void Builtins::Generate_JSConstructEntryTrampoline(MacroAssembler* masm) {
  Generate_JSEntryTrampolineHelper(masm, true);
}


void Builtins::Generate_FunctionCall(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void Builtins::Generate_FunctionApply(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


static void EnterArgumentsAdaptorFrame(MacroAssembler* masm) {
  __ sll(a0, a0, kSmiTagSize);
  __ li(t0, Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));
  __ MultiPush(a0.bit() | a1.bit() | t0.bit() | fp.bit() | ra.bit());
  __ Add(fp, sp, Operand(3 * kPointerSize));
}


static void LeaveArgumentsAdaptorFrame(MacroAssembler* masm) {
  // v0 : result being passed through
  // Get the number of arguments passed (as a smi), tear down the frame and
  // then tear down the parameters.
  __ lw(a1, MemOperand(fp, -3 * kPointerSize));
  __ mov(sp, fp);
  __ MultiPop(fp.bit() | ra.bit());
  __ sll(t0, a1, kPointerSizeLog2 - kSmiTagSize);
  __ Addu(sp, sp, t0);
  // Adjust for the receiver and arguments slots.
  __ Addu(sp, sp,
      Operand(kPointerSize + StandardFrameConstants::kBArgsSlotsSize));
}


void Builtins::Generate_ArgumentsAdaptorTrampoline(MacroAssembler* masm) {
  // State setup as expected by MacroAssembler::InvokePrologue.
  // a0: actual arguments count
  // a1: function (passed through to callee)
  // a2: expected arguments count
  // a3: callee code entry

  Label invoke, dont_adapt_arguments;

  Label enough, too_few;
  __ Branch(eq, &dont_adapt_arguments,
      a2, Operand(SharedFunctionInfo::kDontAdaptArgumentsSentinel));
  // We use Uless as the number of argument should always be greater than 0.
  __ Branch(Uless, &too_few, a0, Operand(a2));

  {  // Enough parameters: actual >= expected.
    // a0: actual number of arguments as a smi
    // a1: function
    // a2: expected number of arguments
    // a3: code entry to call
    __ bind(&enough);
    EnterArgumentsAdaptorFrame(masm);

    // Calculate copy start address into a0 and copy end address into a2.
    __ sll(a0, a0, kPointerSizeLog2 - kSmiTagSize);
    __ Addu(a0, a0, StandardFrameConstants::kBArgsSlotsSize);
    __ Addu(a0, fp, a0);
    // Adjust for return address and receiver.
    __ Addu(a0, a0, Operand(2 * kPointerSize));
    // Compute copy end address.
    __ sll(a2, a2, kPointerSizeLog2);
    __ subu(a2, a0, a2);

    // Copy the arguments (including the receiver) to the new stack frame.
    // a0: copy start address
    // a1: function
    // a2: copy end address
    // a3: code entry to call

    Label copy;
    __ bind(&copy);
    __ lw(t0, MemOperand(a0));
    __ Push(t0);
    // Use the branch delay slot to update a0.
    __ Branch(false, ne, &copy, a0, Operand(a2));
    __ addiu(a0, a0, -kPointerSize);

    __ jmp(&invoke);
  }

  {  // Too few parameters: Actual < expected
    __ bind(&too_few);
    EnterArgumentsAdaptorFrame(masm);

    // TODO(MIPS): Optimize these loops.

    // Calculate copy start address into a0.
    // Copy end address is not in fp, as we have allocated arguments slots.
    // a0: actual number of arguments as a smi
    // a1: function
    // a2: expected number of arguments
    // a3: code entry to call
    __ sll(a0, a0, kPointerSizeLog2 - kSmiTagSize);
    __ Addu(a0, a0, StandardFrameConstants::kBArgsSlotsSize);
    __ Addu(a0, fp, a0);
    // Adjust for return address and receiver.
    __ Addu(a0, a0, Operand(2 * kPointerSize));
    // Compute copy end address. Also adjust for return address.
    __ Addu(t1, fp, StandardFrameConstants::kBArgsSlotsSize + kPointerSize);

    // Copy the arguments (including the receiver) to the new stack frame.
    // a0: copy start address
    // a1: function
    // a2: expected number of arguments
    // a3: code entry to call
    // t1: copy end address
    Label copy;
    __ bind(&copy);
    // Adjust load for return address and receiver.
    __ lw(t0, MemOperand(a0));
    __ Push(t0);
    __ Addu(a0, a0, -kPointerSize);
    __ Branch(ne, &copy, a0, Operand(t1));

    // Fill the remaining expected arguments with undefined.
    // a1: function
    // a2: expected number of arguments
    // a3: code entry to call
    __ LoadRoot(t0, Heap::kUndefinedValueRootIndex);
    __ sll(t2, a2, kPointerSizeLog2);
    __ Subu(a2, fp, Operand(t2));
    __ Addu(a2, a2, Operand(-4 * kPointerSize));  // Adjust for frame.

    Label fill;
    __ bind(&fill);
    __ Push(t0);
    __ Branch(ne, &fill, sp, Operand(a2));
  }

  // Call the entry point.
  __ bind(&invoke);

  __ Call(a3);

  // Exit frame and return.
  LeaveArgumentsAdaptorFrame(masm);
  __ Ret();


  // -------------------------------------------
  // Don't adapt arguments.
  // -------------------------------------------
  __ bind(&dont_adapt_arguments);
  __ Add(sp,  sp,  StandardFrameConstants::kBArgsSlotsSize);
  __ Jump(a3);
}


#undef __

} }  // namespace v8::internal

