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
#include "code-stubs.h"
#include "codegen-inl.h"
#include "regexp-macro-assembler.h"

namespace v8 {
namespace internal {


#define __ ACCESS_MASM(masm)

static void EmitIdenticalObjectComparison(MacroAssembler* masm,
                                          Label* slow,
                                          Condition cc,
                                          bool never_nan_nan);
static void EmitSmiNonsmiComparison(MacroAssembler* masm,
                                    Register lhs,
                                    Register rhs,
                                    Label* rhs_not_nan,
                                    Label* slow,
                                    bool strict);
static void EmitTwoNonNanDoubleComparison(MacroAssembler* masm, Condition cc);
static void EmitStrictTwoHeapObjectCompare(MacroAssembler* masm,
                                           Register lhs,
                                           Register rhs);


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

  // Initialize the code pointer in the function to be the one
  // found in the shared function info object.
  __ lw(a3, FieldMemOperand(a3, SharedFunctionInfo::kCodeOffset));
  __ Addu(a3, a3, Operand(Code::kHeaderSize - kHeapObjectTag));
  __ sw(a3, FieldMemOperand(v0, JSFunction::kCodeEntryOffset));

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

  if (FLAG_debug_code) {
    const char* message;
    Heap::RootListIndex expected_map_index;
    if (mode_ == CLONE_ELEMENTS) {
      message = "Expected (writable) fixed array";
      expected_map_index = Heap::kFixedArrayMapRootIndex;
    } else {
      ASSERT(mode_ == COPY_ON_WRITE_ELEMENTS);
      message = "Expected copy-on-write fixed array";
      expected_map_index = Heap::kFixedCOWArrayMapRootIndex;
    }
    __ Push(a3);
    __ lw(a3, FieldMemOperand(a3, JSArray::kElementsOffset));
    __ lw(a3, FieldMemOperand(a3, HeapObject::kMapOffset));
    __ LoadRoot(at, expected_map_index);
    __ Assert(eq, message, a3, Operand(at));
    __ Pop(a3);
  }

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
    __ CopyFields(a2, a3, a1.bit(), elements_size / kPointerSize);
  }

  // Return and remove the on-stack parameters.
  __ Addu(sp, sp, Operand(3 * kPointerSize));
  __ Ret();

  __ bind(&slow_case);
  __ TailCallRuntime(Runtime::kCreateArrayLiteralShallow, 3, 1);
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
  STATIC_ASSERT(HeapNumber::kSignMask == 0x80000000u);
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
  STATIC_ASSERT(HeapNumber::kSignMask == 0x80000000u);
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
  __ mov(scratch_, zero_reg);
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
    __ mov(v0, zero_reg);         // Things are <=, >=, ==, === themselves.
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


static void EmitSmiNonsmiComparison(MacroAssembler* masm,
                                    Register lhs,
                                    Register rhs,
                                    Label* both_loaded_as_doubles,
                                    Label* slow,
                                    bool strict) {
  ASSERT((lhs.is(a0) && rhs.is(a1)) ||
         (lhs.is(a1) && rhs.is(a0)));

  Label lhs_is_smi;
  __ And(t0, lhs, Operand(kSmiTagMask));
  __ Branch(&lhs_is_smi, eq, t0, Operand(zero_reg));
  // Rhs is a Smi.
  // Check whether the non-smi is a heap number.
  __ GetObjectType(lhs, t4, t4);
  if (strict) {
    // If lhs was not a number and rhs was a Smi then strict equality cannot
    // succeed. Return non-equal (lhs is already not zero)
    __ mov(v0, lhs);
    __ Ret(ne, t4, Operand(HEAP_NUMBER_TYPE));
  } else {
    // Smi compared non-strictly with a non-Smi non-heap-number. Call
    // the runtime.
    __ Branch(slow, ne, t4, Operand(HEAP_NUMBER_TYPE));
  }

  // Rhs is a smi, lhs is a number.
  // Convert smi rhs to double.
  if (CpuFeatures::IsSupported(FPU)) {
    CpuFeatures::Scope scope(FPU);
    __ sra(at, rhs, kSmiTagSize);
    __ mtc1(at, f14);
    __ cvt_d_w(f14, f14);
    __ ldc1(f12, FieldMemOperand(lhs, HeapNumber::kValueOffset));
  } else {
    // Load lhs to a double in a2, a3.
    __ lw(a3, FieldMemOperand(lhs, HeapNumber::kValueOffset + 4));
    __ lw(a2, FieldMemOperand(lhs, HeapNumber::kValueOffset));

    // Write Smi from rhs to a1 and a0 in double format. t5 is scratch.
    __ mov(t6, rhs);
    ConvertToDoubleStub stub1(a1, a0, t6, t5);
    __ Push(ra);
    __ Call(stub1.GetCode(), RelocInfo::CODE_TARGET);

    __ Pop(ra);
  }

  // We now have both loaded as doubles.
  __ jmp(both_loaded_as_doubles);

  __ bind(&lhs_is_smi);
  // Lhs is a Smi.  Check whether the non-smi is a heap number.
  __ GetObjectType(rhs, t4, t4);
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
  // Convert smi lhs to double.
  if (CpuFeatures::IsSupported(FPU)) {
    CpuFeatures::Scope scope(FPU);
    __ sra(at, lhs, kSmiTagSize);
    __ mtc1(at, f12);
    __ cvt_d_w(f12, f12);
    __ ldc1(f14, FieldMemOperand(rhs, HeapNumber::kValueOffset));
  } else {
    // Convert lhs to a double format. t5 is scratch.
    __ mov(t6, lhs);
    ConvertToDoubleStub stub2(a3, a2, t6, t5);
    __ Push(ra);
    __ Call(stub2.GetCode(), RelocInfo::CODE_TARGET);
    __ Pop(ra);
    // Load rhs to a double in a1, a0.
    if (rhs.is(a0)) {
      __ lw(a1, FieldMemOperand(rhs, HeapNumber::kValueOffset + 4));
      __ lw(a0, FieldMemOperand(rhs, HeapNumber::kValueOffset));
    } else {
      __ lw(a0, FieldMemOperand(rhs, HeapNumber::kValueOffset));
      __ lw(a1, FieldMemOperand(rhs, HeapNumber::kValueOffset + 4));
    }
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


static void EmitStrictTwoHeapObjectCompare(MacroAssembler* masm,
                                           Register lhs,
                                           Register rhs) {
    // If either operand is a JSObject or an oddball value, then they are
    // not equal since their pointers are different.
    // There is no test for undetectability in strict equality.
    STATIC_ASSERT(LAST_TYPE == JS_FUNCTION_TYPE);
    Label first_non_object;
    // Get the type of the first operand into a2 and compare it with
    // FIRST_JS_OBJECT_TYPE.
    __ GetObjectType(lhs, a2, a2);
    __ Branch(&first_non_object, less, a2, Operand(FIRST_JS_OBJECT_TYPE));

    // Return non-zero.
    Label return_not_equal;
    __ bind(&return_not_equal);
    __ li(v0, Operand(1));
    __ Ret();

    __ bind(&first_non_object);
    // Check for oddballs: true, false, null, undefined.
    __ Branch(&return_not_equal, eq, a2, Operand(ODDBALL_TYPE));

    __ GetObjectType(rhs, a3, a3);
    __ Branch(&return_not_equal, greater, a3, Operand(FIRST_JS_OBJECT_TYPE));

    // Check for oddballs: true, false, null, undefined.
    __ Branch(&return_not_equal, eq, a3, Operand(ODDBALL_TYPE));

    // Now that we have the types we might as well check for symbol-symbol.
    // Ensure that no non-strings have the symbol bit set.
    STATIC_ASSERT(LAST_TYPE < kNotStringTag + kIsSymbolMask);
    STATIC_ASSERT(kSymbolTag != 0);
    __ And(t2, a2, Operand(a3));
    __ And(t0, t2, Operand(kIsSymbolMask));
    __ Branch(&return_not_equal, ne, t0, Operand(zero_reg));
}


static void EmitCheckForTwoHeapNumbers(MacroAssembler* masm,
                                       Register lhs,
                                       Register rhs,
                                       Label* both_loaded_as_doubles,
                                       Label* not_heap_numbers,
                                       Label* slow) {
  __ GetObjectType(lhs, a3, a2);
  __ Branch(not_heap_numbers, ne, a2, Operand(HEAP_NUMBER_TYPE));
  __ lw(a2, FieldMemOperand(rhs, HeapObject::kMapOffset));
  // If first was a heap number & second wasn't, go to slow case.
  __ Branch(slow, ne, a3, Operand(a2));

  // Both are heap numbers. Load them up then jump to the code we have
  // for that.
  if (CpuFeatures::IsSupported(FPU)) {
    CpuFeatures::Scope scope(FPU);
    __ ldc1(f12, FieldMemOperand(lhs, HeapNumber::kValueOffset));
    __ ldc1(f14, FieldMemOperand(rhs, HeapNumber::kValueOffset));
  } else {
    __ lw(a2, FieldMemOperand(lhs, HeapNumber::kValueOffset));
    __ lw(a3, FieldMemOperand(lhs, HeapNumber::kValueOffset + 4));
    if (rhs.is(a0)) {
      __ lw(a1, FieldMemOperand(rhs, HeapNumber::kValueOffset + 4));
      __ lw(a0, FieldMemOperand(rhs, HeapNumber::kValueOffset));
    } else {
      __ lw(a0, FieldMemOperand(rhs, HeapNumber::kValueOffset));
      __ lw(a1, FieldMemOperand(rhs, HeapNumber::kValueOffset + 4));
    }
  }
  __ jmp(both_loaded_as_doubles);
}


// Fast negative check for symbol-to-symbol equality.
static void EmitCheckForSymbolsOrObjects(MacroAssembler* masm,
                                         Register lhs,
                                         Register rhs,
                                         Label* possible_strings,
                                         Label* not_both_strings) {
  ASSERT((lhs.is(a0) && rhs.is(a1)) ||
         (lhs.is(a1) && rhs.is(a0)));

  // a2 is object type of lhs.
  // Ensure that no non-strings have the symbol bit set.
  Label object_test;
  STATIC_ASSERT(kSymbolTag != 0);
  __ And(at, a2, Operand(kIsNotStringMask));
  __ Branch(&object_test, ne, at, Operand(zero_reg));
  __ And(at, a2, Operand(kIsSymbolMask));
  __ Branch(possible_strings, eq, at, Operand(zero_reg));
  __ GetObjectType(rhs, a3, a3);
  __ Branch(not_both_strings, ge, a3, Operand(FIRST_NONSTRING_TYPE));
  __ And(at, a3, Operand(kIsSymbolMask));
  __ Branch(possible_strings, eq, at, Operand(zero_reg));

  // Both are symbols. We already checked they weren't the same pointer
  // so they are not equal.
  __ li(v0, Operand(1));   // Non-zero indicates not equal.
  __ Ret();

  __ bind(&object_test);
  __ Branch(not_both_strings, lt, a2, Operand(FIRST_JS_OBJECT_TYPE));
  __ GetObjectType(rhs, a2, a3);
  __ Branch(not_both_strings, lt, a3, Operand(FIRST_JS_OBJECT_TYPE));

  // If both objects are undetectable, they are equal.  Otherwise, they
  // are not equal, since they are different objects and an object is not
  // equal to undefined.
  __ lw(a3, FieldMemOperand(lhs, HeapObject::kMapOffset));
  __ lbu(a2, FieldMemOperand(a2, Map::kBitFieldOffset));
  __ lbu(a3, FieldMemOperand(a3, Map::kBitFieldOffset));
  __ and_(a0, a2, a3);
  __ And(a0, a0, Operand(1 << Map::kIsUndetectable));
  __ Xor(v0, a0, Operand(1 << Map::kIsUndetectable));
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

      STATIC_ASSERT(8 == kDoubleSize);
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


// On entry lhs_ (lhs) and rhs_ (rhs) are the things to be compared.
// On exit, v0 is 0, positive, or negative (smi) to indicate the result
// of the comparison.
void CompareStub::Generate(MacroAssembler* masm) {
  Label slow;  // Call builtin.
  Label not_smis, both_loaded_as_doubles;


  if (include_smi_compare_) {
    Label not_two_smis, smi_done;
    __ Or(a2, a1, a0);
    __ BranchOnNotSmi(a2, &not_two_smis);
    __ sra(a1, a1, 1);
    __ sra(a0, a0, 1);
    __ Subu(v0, a1, a0);
    __ Ret();
    __ bind(&not_two_smis);
  } else if (FLAG_debug_code) {
    __ Or(a2, a1, a0);
    __ And(a2, a2, kSmiTagMask);
    __ Assert(ne, "CompareStub: unexpected smi operands.",
        a2, Operand(zero_reg));
  }


  // NOTICE! This code is only reached after a smi-fast-case check, so
  // it is certain that at least one operand isn't a smi.

  // Handle the case where the objects are identical.  Either returns the answer
  // or goes to slow.  Only falls through if the objects were not identical.
  EmitIdenticalObjectComparison(masm, &slow, cc_, never_nan_nan_);

  // If either is a Smi (we know that not both are), then they can only
  // be strictly equal if the other is a HeapNumber.
  STATIC_ASSERT(kSmiTag == 0);
  ASSERT_EQ(0, Smi::FromInt(0));
  __ And(t2, lhs_, Operand(rhs_));
  __ BranchOnNotSmi(t2, &not_smis, t0);
  // One operand is a smi. EmitSmiNonsmiComparison generates code that can:
  // 1) Return the answer.
  // 2) Go to slow.
  // 3) Fall through to both_loaded_as_doubles.
  // 4) Jump to rhs_not_nan.
  // In cases 3 and 4 we have found out we were dealing with a number-number
  // comparison and the numbers have been loaded into f12 and f14 as doubles,
  // or in GP registers (a0, a1, a2, a3) depending on the presence of the FPU.
  EmitSmiNonsmiComparison(masm, lhs_, rhs_,
                          &both_loaded_as_doubles, &slow, strict_);

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
  // and neither of them is a Smi. The objects are in lhs_ and rhs_.
  if (strict_) {
    // This returns non-equal for some object types, or falls through if it
    // was not lucky.
    EmitStrictTwoHeapObjectCompare(masm, lhs_, rhs_);
  }

  Label check_for_symbols;
  Label flat_string_check;
  // Check for heap-number-heap-number comparison. Can jump to slow case,
  // or load both doubles and jump to the code that handles
  // that case. If the inputs are not doubles then jumps to check_for_symbols.
  // In this case a2 will contain the type of lhs_.
  EmitCheckForTwoHeapNumbers(masm,
                             lhs_,
                             rhs_,
                             &both_loaded_as_doubles,
                             &check_for_symbols,
                             &flat_string_check);

  __ bind(&check_for_symbols);
  if (cc_ == eq && !strict_) {
    // Returns an answer for two symbols or two detectable objects.
    // Otherwise jumps to string case or not both strings case.
    // Assumes that a2 is the type of lhs_ on entry.
    EmitCheckForSymbolsOrObjects(masm, lhs_, rhs_, &flat_string_check, &slow);
  }

  // Check for both being sequential ASCII strings, and inline if that is the
  // case.
  __ bind(&flat_string_check);

  __ JumpIfNonSmisNotBothSequentialAsciiStrings(lhs_, rhs_, a2, a3, &slow);

  __ IncrementCounter(&Counters::string_compare_native, 1, a2, a3);
  StringCompareStub::GenerateCompareFlatAsciiStrings(masm,
                                                     rhs_,
                                                     lhs_,
                                                     a2,
                                                     a3,
                                                     t0,
                                                     t1);
  // Never falls through to here.

  __ bind(&slow);
  // Prepare for call to builtin. Push object pointers, a0 (lhs) first,
  // a1 (rhs) second.
  __ Push(lhs_, rhs_);
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


// This stub does not handle the inlined cases (Smis, Booleans, undefined).
// The stub returns zero for false, and a non-zero value for true.
void ToBooleanStub::Generate(MacroAssembler* masm) {
  Label false_result;
  Label not_heap_number;
  Register scratch0 = VirtualFrame::scratch0();

  // HeapNumber => false iff +0, -0, or NaN.
  __ lw(scratch0, FieldMemOperand(tos_, HeapObject::kMapOffset));
  __ LoadRoot(at, Heap::kHeapNumberMapRootIndex);
  __ Branch(&not_heap_number, ne, scratch0, Operand(at));

  __ Subu(at, tos_, Operand(kHeapObjectTag));
  __ ldc1(f12, MemOperand(at, HeapNumber::kValueOffset));
  __ fcmp(f12, 0.0, UEQ);

  // "tos_" is a register, and contains a non zero value by default.
  // Hence we only need to overwrite "tos_" with zero to return false for
  // FP_ZERO or FP_NAN cases. Otherwise, by default it returns true.
  __ movt(tos_, zero_reg);
  __ Ret();

  __ bind(&not_heap_number);

  // Check if the value is 'null'.
  // 'null' => false.
  __ LoadRoot(at, Heap::kNullValueRootIndex);
  __ Branch(&false_result, eq, tos_, Operand(at));

  // It can be an undetectable object.
  // Undetectable => false.
  __ lw(at, FieldMemOperand(tos_, HeapObject::kMapOffset));
  __ lbu(scratch0, FieldMemOperand(at, Map::kBitFieldOffset));
  __ And(scratch0, scratch0, Operand(1 << Map::kIsUndetectable));
  __ Branch(&false_result, eq, scratch0, Operand(1 << Map::kIsUndetectable));

  // JavaScript object => true.
  __ lw(scratch0, FieldMemOperand(tos_, HeapObject::kMapOffset));
  __ lbu(scratch0, FieldMemOperand(scratch0, Map::kInstanceTypeOffset));

  // "tos_" is a register and contains a non-zero value.
  // Hence we implicitly return true if the greater than
  // condition is satisfied.
  __ Ret(gt, scratch0, Operand(FIRST_JS_OBJECT_TYPE));

  // Check for string
  __ lw(scratch0, FieldMemOperand(tos_, HeapObject::kMapOffset));
  __ lbu(scratch0, FieldMemOperand(scratch0, Map::kInstanceTypeOffset));
  // "tos_" is a register and contains a non-zero value.
  // Hence we implicitly return true if the greater than
  // condition is satisfied.
  __ Ret(gt, scratch0, Operand(FIRST_NONSTRING_TYPE));

  // String value => false iff empty, i.e., length is zero
  __ lw(tos_, FieldMemOperand(tos_, String::kLengthOffset));
  // If length is zero, "tos_" contains zero ==> false.
  // If length is not zero, "tos_" contains a non-zero value ==> true.
  __ Ret();

  // Return 0 in "tos_" for false .
  __ bind(&false_result);
  __ mov(tos_, zero_reg);
  __ Ret();
}


// We fall into this code if the operands were Smis, but the result was
// not (eg. overflow).  We branch into this code (to the not_smi label) if
// the operands were not both Smi.  The operands are in lhs and rhs.
// To call the C-implemented binary fp operation routines we need to end up
// with the double precision floating point operands in a0 and a1 (for the
// value in a1) and a2 and a3 (for the value in a0).
void GenericBinaryOpStub::HandleBinaryOpSlowCases(MacroAssembler* masm,
                                    Label* not_smi,
                                    Register lhs,
                                    Register rhs,
                                    const Builtins::JavaScript& builtin) {
  Label slow, slow_reverse, do_the_call;
  bool use_fp_registers = CpuFeatures::IsSupported(FPU) && Token::MOD != op_;

  ASSERT((lhs.is(a0) && rhs.is(a1)) || (lhs.is(a1) && rhs.is(a0)));
  Register heap_number_map = t2;

  if (ShouldGenerateSmiCode()) {
    if (op_ == Token::MOD || op_ == Token::DIV) {
      // If the divisor is zero for MOD or DIV, go to
      // the builtin code to return NaN.
      __ Branch(lhs.is(a0) ? &slow_reverse : &slow, eq, rhs, Operand(zero_reg));
    }
    // Smi-smi case (overflow).
    // Since both are Smis there is no heap number to overwrite, so allocate.
    // The new heap number is in t1. a3 and t3 are scratch.
    __ LoadRoot(heap_number_map, Heap::kHeapNumberMapRootIndex);
    __ AllocateHeapNumber(
        t1, a3, t3, heap_number_map, lhs.is(a0) ? &slow_reverse : &slow);

    // If we have floating point hardware, inline ADD, SUB, MUL, and DIV,
    // using registers f12 and f14 for the double values.
    if (CpuFeatures::IsSupported(FPU)) {
      CpuFeatures::Scope scope(FPU);
      // Convert lhs to double in f12
      __ sra(t3, lhs, kSmiTagSize);
      __ mtc1(t3, f12);
      __ cvt_d_w(f12, f12);

      // Convert rhs to double in f14
      __ sra(t3, rhs, kSmiTagSize);
      __ mtc1(t3, f14);
      __ cvt_d_w(f14, f14);

      if (!use_fp_registers) {
        __ mfc1(a2, f14);   // a2, a3 get rhs.
        __ mfc1(a3, f15);
        __ mfc1(a0, f12);   // a0, a1 get lhs.
        __ mfc1(a1, f13);
      }

    } else {
      // Write Smi from rhs to a3 and a2 in double format. t5 is scratch.
      __ mov(t3, rhs);
      ConvertToDoubleStub stub1(a3, a2, t3, t5);
      __ Push(ra);
      __ Call(stub1.GetCode(), RelocInfo::CODE_TARGET);

      // Write Smi from lhs to a1 and a0 in double format. t5 is scratch.
      __ mov(t3, lhs);
      ConvertToDoubleStub stub2(a1, a0, t3, t5);
      __ Call(stub2.GetCode(), RelocInfo::CODE_TARGET);
      __ Pop(ra);
    }
    __ jmp(&do_the_call);  // Tail call.  No return.
  }

  // We branch here if at least one of a0 and a1 is not a Smi.
  __ bind(not_smi);
  __ LoadRoot(heap_number_map, Heap::kHeapNumberMapRootIndex);

  // After this point we have the left hand side in a1 and the right hand side
  // in a0.
  Register scratch  = VirtualFrame::scratch0();
  if (lhs.is(a0)) {
    __ Swap(a0, a1, scratch);
  }

  // The type transition also calculates the answer.
  bool generate_code_to_calculate_answer = true;

  if (ShouldGenerateFPCode()) {
    if (runtime_operands_type_ == BinaryOpIC::DEFAULT) {
      switch (op_) {
        case Token::ADD:
        case Token::SUB:
        case Token::MUL:
        case Token::DIV:
          GenerateTypeTransition(masm);   // Tail call.
          generate_code_to_calculate_answer = false;
          break;

        default:
          break;
      }
    }

    if (generate_code_to_calculate_answer) {
      Label a0_is_smi, a1_is_smi, finished_loading_a0, finished_loading_a1;
      if (mode_ == NO_OVERWRITE) {
        // In the case where there is no chance of an overwritable float we may
        // as well do the allocation immediately while a0 and a1 are untouched.
        __ AllocateHeapNumber(t1, a3, t3, heap_number_map, &slow);
      }

      // Move a0 (rhs) to a double in a2-a3.
      // If it's a Smi don't check if it's a heap number.
      __ BranchOnSmi(a0, &a0_is_smi);

      __ lw(t0, FieldMemOperand(a0, HeapObject::kMapOffset));
      __ AssertRegisterIsRoot(heap_number_map, Heap::kHeapNumberMapRootIndex);
      __ Branch(&slow, ne, t0, Operand(heap_number_map));
      if (mode_ == OVERWRITE_RIGHT) {
        __ mov(t1, a0);  // Overwrite this heap number.
      }
      if (use_fp_registers) {
        CpuFeatures::Scope scope(FPU);
        // Load the double from tagged HeapNumber a0 (rhs) to f14.
        __ ldc1(f14, FieldMemOperand(a0, HeapNumber::kValueOffset));
      } else {
        // Calling convention says that second double is in a2 and a3.
        __ lw(a2, FieldMemOperand(a0, HeapNumber::kValueOffset));
        __ lw(a3, FieldMemOperand(a0, HeapNumber::kValueOffset + 4));
      }
      __ jmp(&finished_loading_a0);
      __ bind(&a0_is_smi);
      if (mode_ == OVERWRITE_RIGHT) {
        // We can't overwrite a Smi so get address of new heap number into t1.
      __ AllocateHeapNumber(t1, t0, t3, heap_number_map, &slow);
      }

      if (CpuFeatures::IsSupported(FPU)) {
       CpuFeatures::Scope scope(FPU);
       // Convert smi in a0 (rhs) to double in f14.
       __ sra(t3, a0, kSmiTagSize);
       __ mtc1(t3, f14);
       __ cvt_d_w(f14, f14);
       if (!use_fp_registers) {
         __ mfc1(a2, f14);
         __ mfc1(a3, f15);
       }
      } else {
       // Write Smi from a0 (rhs) to a3 and a2 in double format.
       __ mov(t3, a0);
       ConvertToDoubleStub stub3(a3, a2, t3, t0);
       __ Push(ra);
       __ Call(stub3.GetCode(), RelocInfo::CODE_TARGET);
       __ Pop(ra);
      }

      // HEAP_NUMBERS stub is slower than GENERIC on a pair of smis.
      // a0 is known to be a smi. If a1 is also a smi then switch to GENERIC.
      Label a1_is_not_smi;
      if (runtime_operands_type_ == BinaryOpIC::HEAP_NUMBERS) {
        __ BranchOnNotSmi(a1, &a1_is_not_smi);
        GenerateTypeTransition(masm);  // Tail call.
      }

      __ bind(&finished_loading_a0);

      // Move a1 (lhs) to a double in a0-a1.
      // If it's a Smi don't check if it's a heap number.
      __ BranchOnSmi(a1, &a1_is_smi);
      __ bind(&a1_is_not_smi);
      __ lw(t0, FieldMemOperand(a1, HeapNumber::kMapOffset));
      __ AssertRegisterIsRoot(heap_number_map, Heap::kHeapNumberMapRootIndex);
      __ Branch(&slow, ne, t0, Operand(heap_number_map));
      if (mode_ == OVERWRITE_LEFT) {
        __ mov(t1, a1);  // Overwrite this heap number.
      }
      if (use_fp_registers) {
        CpuFeatures::Scope scope(FPU);
        // Load the double from tagged HeapNumber a1 (lhs) to f12.
        __ ldc1(f12, FieldMemOperand(a1, HeapNumber::kValueOffset));
      } else {
        // Calling convention says that first double (lhs) is in a0 and a1.
        __ lw(a0, FieldMemOperand(a1, HeapNumber::kValueOffset));
        __ lw(a1, FieldMemOperand(a1, HeapNumber::kValueOffset + 4));
      }
      __ jmp(&finished_loading_a1);
      __ bind(&a1_is_smi);
      if (mode_ == OVERWRITE_LEFT) {
        // We can't overwrite a Smi so get address of new heap number into t1.
      __ AllocateHeapNumber(t1, t0, t3, heap_number_map, &slow);
      }

      if (CpuFeatures::IsSupported(FPU)) {
        CpuFeatures::Scope scope(FPU);
        // Convert smi in a1 (lhs) to double in f12.
        __ sra(t3, a1, kSmiTagSize);
        __ mtc1(t3, f12);
        __ cvt_d_w(f12, f12);
        if (!use_fp_registers) {
          __ mfc1(a0, f12);
          __ mfc1(a1, f13);
        }
      } else {
        // Write Smi from a1 (lhs) to a1 and a0 in double format.
        __ mov(t3, a1);
        ConvertToDoubleStub stub4(a1, a0, t3, t5);
        __ Push(ra);
        __ Call(stub4.GetCode(), RelocInfo::CODE_TARGET);
        __ Pop(ra);
      }

      __ bind(&finished_loading_a1);
    }

    if (generate_code_to_calculate_answer || do_the_call.is_linked()) {
      __ bind(&do_the_call);
      // If we are inlining the operation using FPU instructions for
      // add, subtract, multiply, or divide, the arguments are in f12 and f14.
      if (use_fp_registers) {
        CpuFeatures::Scope scope(FPU);
        // MIPS32 FPU instructions to implement
        // double precision, add, subtract, multiply, divide.

        if (Token::MUL == op_) {
          __ mul_d(f0, f12, f14);
        } else if (Token::DIV == op_) {
          __ div_d(f0, f12, f14);
        } else if (Token::ADD == op_) {
          __ add_d(f0, f12, f14);
        } else if (Token::SUB == op_) {
          __ sub_d(f0, f12, f14);
        } else {
          UNREACHABLE();
        }
        __ sdc1(f0, FieldMemOperand(t1, HeapNumber::kValueOffset));
        __ mov(v0, t1);
        __ Ret();
      } else {
        // If we did not inline the operation, then the arguments are in:
        // a0: Left value (least significant part of mantissa).
        // a1: Left value (sign, exponent, top of mantissa).
        // a2: Right value (least significant part of mantissa).
        // a3: Right value (sign, exponent, top of mantissa).
        // t1: Address of heap number for result.

        __ Push(ra);
        __ Push(t1);
        __ PrepareCallCFunction(4, t0);  // Two doubles count as 4 arguments.
        if (!IsMipsSoftFloatABI) {
          // We are not using MIPS FPU instructions, and parameters for the
          // run-time function call are prepared in a0-a3 registers, but the
          // function we are calling is compiled with hard-float flag and
          // expecting hard float ABI (parameters in f12/f14 registers).
          // Copy parameters from a0-a3 registers to f12/f14 register pairs.
          __ mtc1(a0, f12);
          __ mtc1(a1, f13);
          __ mtc1(a2, f14);
          __ mtc1(a3, f15);
        }
        // Call C routine that may not cause GC or other trouble.
        __ CallCFunction(ExternalReference::double_fp_operation(op_), 4);
        __ Pop(t1);
        __ Pop(ra);
        // Store answer in the overwritable heap number.
        if (!IsMipsSoftFloatABI) {
          // Double returned in fp coprocessor register f0 and f1.
          __ sdc1(f0, FieldMemOperand(t1, HeapNumber::kValueOffset));
        } else {
          // Double returned in registers v0 and v1.
          __ sw(v0, FieldMemOperand(t1, HeapNumber::kValueOffset));
          __ sw(v1, FieldMemOperand(t1, HeapNumber::kValueOffset + 4));
        }
        __ mov(v0, t1);
        // And we are done.
        __ Ret();
      }
    }
  }

  if (!generate_code_to_calculate_answer &&
      !slow_reverse.is_linked() &&
      !slow.is_linked()) {
    return;
  }

  if (lhs.is(a0)) {
    __ Branch(&slow);
    __ bind(&slow_reverse);
    __ Swap(a0, a1, scratch);
  }

  heap_number_map = no_reg;  // Don't use this any more from here on.

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

  Register heap_number_map = t6;
  __ LoadRoot(heap_number_map, Heap::kHeapNumberMapRootIndex);

  __ And(t1, lhs, Operand(kSmiTagMask));
  __ Branch(&lhs_is_smi, eq, t1, Operand(zero_reg));

  __ lw(t4, FieldMemOperand(lhs, HeapNumber::kMapOffset));
  __ Branch(&slow, ne, t4, Operand(heap_number_map));
  // Convert HeapNum a1 to integer a3.
  __ ConvertToInt32(lhs, a3, t2, t3, &slow);
  __ b(&done_checking_lhs);
  __ nop();   // NOP_ADDED

  __ bind(&lhs_is_smi);
  __ sra(a3, lhs, kSmiTagSize);  // Remove tag from Smi.
  __ bind(&done_checking_lhs);

  __ And(t0, rhs, Operand(kSmiTagMask));
  __ Branch(&rhs_is_smi, eq, t0, Operand(zero_reg));
  __ lw(t4, FieldMemOperand(rhs, HeapNumber::kMapOffset));
  __ Branch(&slow, ne, t4, Operand(heap_number_map));
  // Convert HeapNum a0 to integer a2.
  __ ConvertToInt32(rhs, a2, t2, t3, &slow);
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
      if (CpuFeatures::IsSupported(FPU)) {
        CpuFeatures::Scope scope(FPU);
        __ Branch(&result_not_a_smi, ne, t3, Operand(zero_reg));
      } else {
        __ Branch(&slow, ne, t3, Operand(zero_reg));
      }
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
      // Get a new heap number in t5.  t4 and t7 are scratch.
      __ AllocateHeapNumber(t5, t4, t7, heap_number_map, &slow);
    }
    default: break;
  }

  __ bind(&got_a_heap_number);
  // v1: Result as signed int32.
  // t5: Heap number to write answer into.

  // Nothing can go wrong now, so move the heap number to v0, which is the
  // result.
  __ mov(v0, t5);

  if (CpuFeatures::IsSupported(FPU)) {
    CpuFeatures::Scope scope(FPU);
    __ mov(s0, v1);
    if (op_ == Token::SHR) {
      __ Cvt_d_uw(f0, s0);
    } else {
      __ mtc1(s0, f0);
      __ cvt_d_w(f0, f0);
    }
    __ Subu(t3, v0, Operand(kHeapObjectTag));
    __ sdc1(f0, MemOperand(t3, HeapNumber::kValueOffset));
    __ Ret();

  } else {
  // Tail call that writes the int32 in v1 to the heap number in v0, using
  // t0, t1 as scratch.  v0 is preserved and returned by the stub.
  WriteInt32ToHeapNumberStub stub(v1, v0, t0, t1);
  __ Jump(stub.GetCode(), RelocInfo::CODE_TARGET);
  }

  if (mode_ != NO_OVERWRITE) {
    __ bind(&have_to_allocate);
    // Get a new heap number in t5.  t4 and t7 are scratch.
    __ AllocateHeapNumber(t5, t4, t7, heap_number_map, &slow);
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
        STATIC_ASSERT(kSmiTag == 0);  // Adjust code below.
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
        STATIC_ASSERT(kSmiTag == 0);  // Adjust code below.
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
        STATIC_ASSERT(kSmiTag == 0);  // Adjust code below.
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
        STATIC_ASSERT(kSmiTag == 0);  // Adjust code below.
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

        STATIC_ASSERT(kSmiTag == 0 && kSmiTagSize == 1);
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
        STATIC_ASSERT(kSmiTag == 0);  // Adjust code below.
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
      STATIC_ASSERT(kSmiTag == 0);  // Adjust code below.
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

  __ li(a2, Operand(Smi::FromInt(MinorKey())));
  __ li(a1, Operand(Smi::FromInt(op_)));
  __ li(a0, Operand(Smi::FromInt(runtime_operands_type_)));
  __ Push(a2, a1, a0);

  __ TailCallExternalReference(
      ExternalReference(IC_Utility(IC::kBinaryOp_Patch)),
      5,
      1);
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
  __ TailCallRuntime(Runtime::kStackGuard, 0, 1);
}


void GenericUnaryOpStub::Generate(MacroAssembler* masm) {
  Label slow, done;

  Register heap_number_map = t6;
  __ LoadRoot(heap_number_map, Heap::kHeapNumberMapRootIndex);

  if (op_ == Token::SUB) {
    if (include_smi_code_) {
      // Check whether the value is a smi.
      Label try_float;
      __ BranchOnNotSmi(a0, &try_float);

      // Go slow case if the value of the expression is zero
      // to make sure that we switch between 0 and -0.
      if (negative_zero_ == kStrictNegativeZero) {
        // If we have to check for zero, then we can check for the max negative
        // smi while we are at it. (This is kind of expensive on mips, and
        // it seems that we should be able to find a more optimal test.)
        __ And(at, a0, Operand(~0x80000000));  // Emit 3 instr: lui, ori, and.
        __ Branch(&slow, eq, at, Operand(zero_reg));
        __ subu(v0, zero_reg, a0);
        __ Ret();
      } else {
        // The value of the expression is a smi and 0 is OK for -0.  Try
        // optimistic subtraction '0 - value'.
        __ subu(v0, zero_reg, a0);
        // Check for overflow. For v=0-x, overflow only occurs on x=0x80000000.
        // We don't have to reverse the optimistic neg since we did not
        // change input register a0.
        __ Branch(&slow, eq, a0, Operand(0x80000000));  // Go slow on overflow.
        __ Ret();
      }
      __ bind(&try_float);
    } else if (FLAG_debug_code) {
      Register scratch = VirtualFrame::scratch0();
      __ And(scratch, a0, kSmiTagMask);
      __ Assert(ne, "Unexpected smi operand.", scratch, Operand(zero_reg));
    }

    __ lw(a1, FieldMemOperand(a0, HeapObject::kMapOffset));
    __ AssertRegisterIsRoot(heap_number_map, Heap::kHeapNumberMapRootIndex);
    __ Branch(&slow, ne, a1, Operand(heap_number_map));
    // a0 is a heap number.  Get a new heap number in a1.
    if (overwrite_ == UNARY_OVERWRITE) {
      __ lw(a2, FieldMemOperand(a0, HeapNumber::kExponentOffset));
      __ Xor(a2, a2, Operand(HeapNumber::kSignMask));  // Flip sign.
      __ sw(a2, FieldMemOperand(a0, HeapNumber::kExponentOffset));
    } else {
      __ AllocateHeapNumber(a1, a2, a3, heap_number_map, &slow);
      __ lw(a3, FieldMemOperand(a0, HeapNumber::kMantissaOffset));
      __ lw(a2, FieldMemOperand(a0, HeapNumber::kExponentOffset));
      __ sw(a3, FieldMemOperand(a1, HeapNumber::kMantissaOffset));
      __ Xor(a2, a2, Operand(HeapNumber::kSignMask));  // Flip sign.
      __ sw(a2, FieldMemOperand(a1, HeapNumber::kExponentOffset));
      __ mov(v0, a1);
    }
  } else if (op_ == Token::BIT_NOT) {
    if (include_smi_code_) {
      Label non_smi;
      __ BranchOnNotSmi(a0, &non_smi);
      __ srl(v0, a0, kSmiTagSize);
      __ nor(v0, v0, v0);
      __ sll(v0, v0, kSmiTagSize);
      __ Ret();
      __ bind(&non_smi);
    } else if (FLAG_debug_code) {
      Register scratch = VirtualFrame::scratch0();
      __ And(scratch, a0, kSmiTagMask);
      __ Assert(ne, "Unexpected smi operand.",
          scratch, Operand(zero_reg));
    }
    // Check if the operand is a heap number.
    __ lw(a1, FieldMemOperand(a0, HeapObject::kMapOffset));
    __ AssertRegisterIsRoot(heap_number_map, Heap::kHeapNumberMapRootIndex);
    __ Branch(&slow, ne, a1, Operand(heap_number_map));

    // Convert the heap number in a0 to an untagged integer in a1.
    // Go slow if HeapNumber won't fit in 32-bit (untagged) int.
    __ ConvertToInt32(a0, a1, a2, a3, &slow);

    // Do the bitwise operation (use NOR) and check if the result
    // fits in a smi.
    Label try_float;
    __ nor(a1, a1, zero_reg);
    // check that the *signed* result fits in a smi
    __ Addu(a2, a1, Operand(0x40000000));
    __ Branch(&try_float, lt, a2, Operand(zero_reg));

    // Smi tag result.
    __ sll(v0, a1, kSmiTagMask);
    __ Ret();

    __ bind(&try_float);
    if (!overwrite_ == UNARY_OVERWRITE) {
      // Allocate a fresh heap number, but don't overwrite a0 in-case
      // we need to go slow. Return new heap number in v0.
      __ AllocateHeapNumber(a2, a3, t0, heap_number_map, &slow);
      __ mov(v0, a2);
    }

    if (CpuFeatures::IsSupported(FPU)) {
      CpuFeatures::Scope scope(FPU);
      __ mov(s0, a1);
      __ mtc1(s0, f0);
      __ cvt_d_w(f0, f0);

      __ Subu(t3, v0, Operand(kHeapObjectTag));
      __ sdc1(f0, MemOperand(t3, HeapNumber::kValueOffset));
      __ Ret();
    } else {
    // WriteInt32ToHeapNumberStub does not trigger GC, so we do not
    // have to set up a frame.
    WriteInt32ToHeapNumberStub stub(a1, v0, a2, a3);
    __ Push(ra);
    __ Call(stub.GetCode(), RelocInfo::CODE_TARGET);
    __ Pop(ra);
    // Fall thru to done.
    }
  } else {
    UNIMPLEMENTED();
    __ break_(__LINE__);
  }

  __ bind(&done);
  __ Ret();

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
  // v0 holds the exception.

  // Adjust this code if not the case.
  STATIC_ASSERT(StackHandlerConstants::kSize == 4 * kPointerSize);

  // Drop the sp to the top of the handler.
  __ li(a3, Operand(ExternalReference(Top::k_handler_address)));
  __ lw(sp, MemOperand(a3));

  // Restore the next handler and frame pointer, discard handler state.
  STATIC_ASSERT(StackHandlerConstants::kNextOffset == 0);
  __ Pop(a2);
  __ sw(a2, MemOperand(a3));
  STATIC_ASSERT(StackHandlerConstants::kFPOffset == 2 * kPointerSize);
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

  STATIC_ASSERT(StackHandlerConstants::kPCOffset == 3 * kPointerSize);
  __ Pop(t9);
  __ Jump(t9);
}


void CEntryStub::GenerateThrowUncatchable(MacroAssembler* masm,
                                          UncatchableExceptionType type) {
  // Adjust this code if not the case.
  STATIC_ASSERT(StackHandlerConstants::kSize == 4 * kPointerSize);

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
  STATIC_ASSERT(StackHandlerConstants::kNextOffset == 0);
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
  STATIC_ASSERT(StackHandlerConstants::kFPOffset == 2 * kPointerSize);
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
  STATIC_ASSERT(StackHandlerConstants::kPCOffset == 3 * kPointerSize);
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
  STATIC_ASSERT(((kFailureTag + 1) & kFailureTagMask) == 0);
  __ addiu(a2, v0, 1);
  __ andi(t0, a2, kFailureTagMask);
  __ Branch(&failure_returned, eq, t0, Operand(zero_reg));

  // Exit C frame and return.
  // v0:v1: result
  // sp: stack pointer
  // fp: frame pointer
  __ LeaveExitFrame();

  // Check if we should retry or throw exception.
  Label retry;
  __ bind(&failure_returned);
  STATIC_ASSERT(Failure::RETRY_AFTER_GC == 0);
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
  __ EnterExitFrame(s0, s1, s2);

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

  // Check index (a1) against formal parameters count limit passed in
  // through register a0. Use unsigned comparison to get negative
  // check for free.
  __ Branch(&slow, Ugreater_equal, a1, Operand(a0));

  // Read the argument from the stack and return it.
  __ subu(a3, a0, a1);
  __ sll(t3, a3, kPointerSizeLog2 - kSmiTagSize);
  __ Addu(a3, fp, Operand(t3));
  __ lw(v0, MemOperand(a3, kDisplacement));
  __ Ret();

  // Arguments adaptor case: Check index (a1) against actual arguments
  // limit found in the arguments adaptor frame. Use unsigned
  // comparison to get negative check for free.
  __ bind(&adaptor);
  __ lw(a0, MemOperand(a2, ArgumentsAdaptorFrameConstants::kLengthOffset));
  __ Branch(&slow, Ugreater_equal, a1, Operand(a0));

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
  __ Branch(&add_arguments_object, eq, a1, Operand(zero_reg));
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
  __ CopyFields(v0, t0, a3.bit(), JSObject::kHeaderSize / kPointerSize);

  // Setup the callee in-object property.
  ASSERT(Heap::arguments_callee_index == 0);
  __ lw(a3, MemOperand(sp, 2 * kPointerSize));
  __ sw(a3, FieldMemOperand(v0, JSObject::kHeaderSize));

  // Get the length (smi tagged) and set that as an in-object property too.
  ASSERT(Heap::arguments_length_index == 1);
  __ lw(a1, MemOperand(sp, 0 * kPointerSize));
  __ sw(a1, FieldMemOperand(v0, JSObject::kHeaderSize + kPointerSize));

  Label done;
  __ Branch(&done, eq, a1, Operand(zero_reg));

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
  __ Branch(&loop, ne, a1, Operand(zero_reg));

  // Return and remove the on-stack parameters.
  __ bind(&done);
  __ Addu(sp, sp, Operand(3 * kPointerSize));
  __ Ret();

  // Do the runtime call to allocate the arguments object.
  __ bind(&runtime);
  __ TailCallRuntime(Runtime::kNewArgumentsFast, 3, 1);
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
  STATIC_ASSERT(kSmiTag == 0);
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
  STATIC_ASSERT(kSmiTag == 0);
  STATIC_ASSERT(kSmiTagSize + kSmiShiftSize == 1);
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
  STATIC_ASSERT(kStringTag == 0);
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
  STATIC_ASSERT((kStringTag | kSeqStringTag) == 0);
  __ Branch(&seq_string, eq, at, Operand(zero_reg));

  // subject: Subject string
  // a0: instance type if Subject string
  // regexp_data: RegExp data (FixedArray)
  // Check for flat cons string.
  // A flat cons string is a cons string where the second part is the empty
  // string. In that case the subject string is just the first part of the cons
  // string. Also in this case the first part of the cons string is known to be
  // a sequential string or an external string.
  STATIC_ASSERT(kExternalStringTag != 0);
  STATIC_ASSERT((kConsStringTag & kExternalStringTag) == 0);
  __ And(at, a0, Operand(kIsNotStringMask | kExternalStringTag));
  __ Branch(&runtime, ne, at, Operand(zero_reg));
  __ lw(a0, FieldMemOperand(subject, ConsString::kSecondOffset));
  __ LoadRoot(a1, Heap::kEmptyStringRootIndex);
  __ Branch(&runtime, ne, a0, Operand(a1));
  __ lw(subject, FieldMemOperand(subject, ConsString::kFirstOffset));
  __ lw(a0, FieldMemOperand(subject, HeapObject::kMapOffset));
  __ lbu(a0, FieldMemOperand(a0, Map::kInstanceTypeOffset));
  // Is first part a flat string?
  STATIC_ASSERT(kSeqStringTag == 0);
  __ And(at, a0, Operand(kStringRepresentationMask));
  __ Branch(&runtime, ne, at, Operand(zero_reg));

  __ bind(&seq_string);
  // subject: Subject string
  // regexp_data: RegExp data (FixedArray)
  // a0: Instance type of subject string
  STATIC_ASSERT(kStringEncodingMask == 4);
  STATIC_ASSERT(kAsciiStringTag == 4);
  STATIC_ASSERT(kTwoByteStringTag == 0);
  // Find the code object based on the assumptions above.
  __ And(a0, a0, Operand(kStringEncodingMask));  // Non-zero for ascii.
  __ lw(t9, FieldMemOperand(regexp_data, JSRegExp::kDataAsciiCodeOffset));
  __ sra(a3, a0, 2);  // a3 is 1 for ascii, 0 for UC16 (usyed below).
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
  STATIC_ASSERT(SeqAsciiString::kHeaderSize == SeqTwoByteString::kHeaderSize);
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
  STATIC_ASSERT(kSmiTag == 0);
  STATIC_ASSERT(kSmiTagSize + kSmiShiftSize == 1);
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


// Unfortunately you have to run without snapshots to see most of these
// names in the profile since most compare stubs end up in the snapshot.
const char* CompareStub::GetName() {
  ASSERT((lhs_.is(a0) && rhs_.is(a1)) ||
         (lhs_.is(a1) && rhs_.is(a0)));

  if (name_ != NULL) return name_;
  const int kMaxNameLength = 100;
  name_ = Bootstrapper::AllocateAutoDeletedArray(kMaxNameLength);
  if (name_ == NULL) return "OOM";

  const char* cc_name;
  switch (cc_) {
    case lt: cc_name = "LT"; break;
    case gt: cc_name = "GT"; break;
    case le: cc_name = "LE"; break;
    case ge: cc_name = "GE"; break;
    case eq: cc_name = "EQ"; break;
    case ne: cc_name = "NE"; break;
    default: cc_name = "UnknownCondition"; break;
  }

  const char* lhs_name = lhs_.is(a0) ? "_a0" : "_a1";
  const char* rhs_name = rhs_.is(a0) ? "_a0" : "_a1";

  const char* strict_name = "";
  if (strict_ && (cc_ == eq || cc_ == ne)) {
    strict_name = "_STRICT";
  }

  const char* never_nan_nan_name = "";
  if (never_nan_nan_ && (cc_ == eq || cc_ == ne)) {
    never_nan_nan_name = "_NO_NAN";
  }

  const char* include_number_compare_name = "";
  if (!include_number_compare_) {
    include_number_compare_name = "_NO_NUMBER";
  }

  const char* include_smi_compare_name = "";
  if (!include_smi_compare_) {
    include_smi_compare_name = "_NO_SMI";
  }

  OS::SNPrintF(Vector<char>(name_, kMaxNameLength),
               "CompareStub_%s%s%s%s%s%s",
               cc_name,
               lhs_name,
               rhs_name,
               strict_name,
               never_nan_nan_name,
               include_number_compare_name,
               include_smi_compare_name);
  return name_;
}


int CompareStub::MinorKey() {
  // Encode the two parameters in a unique 16 bit value.
  ASSERT(static_cast<unsigned>(cc_) < (1 << 14));
  ASSERT((lhs_.is(a0) && rhs_.is(a1)) ||
         (lhs_.is(a1) && rhs_.is(a0)));
  return ConditionField::encode(static_cast<unsigned>(cc_))
         | RegisterField::encode(lhs_.is(a0))
         | StrictField::encode(strict_)
         | NeverNanNanField::encode(cc_ == eq ? never_nan_nan_ : false)
         | IncludeSmiCompareField::encode(include_smi_compare_);
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
  STATIC_ASSERT(kSeqStringTag == 0);
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
  STATIC_ASSERT(kSeqStringTag == 0);

  __ And(t0, result_, Operand(kStringRepresentationMask));
  __ Branch(&call_runtime_, ne, t0, Operand(zero_reg));

  // Check for 1-byte or 2-byte string.
  __ bind(&flat_string);
  STATIC_ASSERT(kAsciiStringTag != 0);
  __ And(t0, result_, Operand(kStringEncodingMask));
  __ Branch(&ascii_string, ne, t0, Operand(zero_reg));

  // 2-byte string.
  // Load the 2-byte character code into the result register. We can
  // add without shifting since the smi tag size is the log2 of the
  // number of bytes in a two-byte character.
  STATIC_ASSERT(kSmiTag == 0 && kSmiTagSize == 1 && kSmiShiftSize == 0);
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

  STATIC_ASSERT(kSmiTag == 0);
  STATIC_ASSERT(kSmiShiftSize == 0);
  ASSERT(IsPowerOf2(String::kMaxAsciiCharCode + 1));
  __ And(t0,
         code_,
         Operand(kSmiTagMask |
                 ((~String::kMaxAsciiCharCode) << kSmiTagSize)));
  __ Branch(&slow_case_, ne, t0, Operand(zero_reg));

  __ LoadRoot(result_, Heap::kSingleCharacterStringCacheRootIndex);
  // At this point code register contains smi tagged ascii char code.
  STATIC_ASSERT(kSmiTag == 0);
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
  __ Move(result_, v0);

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


class StringHelper : public AllStatic {
 public:
  // Generate code for copying characters using a simple loop. This should only
  // be used in places where the number of characters is small and the
  // additional setup and checking in GenerateCopyCharactersLong adds too much
  // overhead. Copying of overlapping regions is not supported.
  // Dest register ends at the position after the last character written.
  static void GenerateCopyCharacters(MacroAssembler* masm,
                                     Register dest,
                                     Register src,
                                     Register count,
                                     Register scratch,
                                     bool ascii);

  // Generate code for copying a large number of characters. This function
  // is allowed to spend extra time setting up conditions to make copying
  // faster. Copying of overlapping regions is not supported.
  // Dest register ends at the position after the last character written.
  static void GenerateCopyCharactersLong(MacroAssembler* masm,
                                         Register dest,
                                         Register src,
                                         Register count,
                                         Register scratch1,
                                         Register scratch2,
                                         Register scratch3,
                                         Register scratch4,
                                         Register scratch5,
                                         int flags);


  // Probe the symbol table for a two character string. If the string is
  // not found by probing a jump to the label not_found is performed. This jump
  // does not guarantee that the string is not in the symbol table. If the
  // string is found the code falls through with the string in register r0.
  // Contents of both c1 and c2 registers are modified. At the exit c1 is
  // guaranteed to contain halfword with low and high bytes equal to
  // initial contents of c1 and c2 respectively.
  static void GenerateTwoCharacterSymbolTableProbe(MacroAssembler* masm,
                                                   Register c1,
                                                   Register c2,
                                                   Register scratch1,
                                                   Register scratch2,
                                                   Register scratch3,
                                                   Register scratch4,
                                                   Register scratch5,
                                                   Label* not_found);

  // Generate string hash.
  static void GenerateHashInit(MacroAssembler* masm,
                               Register hash,
                               Register character);

  static void GenerateHashAddCharacter(MacroAssembler* masm,
                                       Register hash,
                                       Register character);

  static void GenerateHashGetHash(MacroAssembler* masm,
                                  Register hash);

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(StringHelper);
};


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
  STATIC_ASSERT(kObjectAlignment >= kReadAlignment);
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
    STATIC_ASSERT(SymbolTable::kEntrySize == 1);
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

  Register to = t2;
  Register from = t3;

  // Check bounds and smi-ness.
  __ lw(to, MemOperand(sp, kToOffset));
  __ lw(from, MemOperand(sp, kFromOffset));
  STATIC_ASSERT(kFromOffset == kToOffset + 4);
  STATIC_ASSERT(kSmiTag == 0);
  STATIC_ASSERT(kSmiTagSize + kSmiShiftSize == 1);

  __ BranchOnNotSmi(from, &sub_string_runtime);
  __ BranchOnNotSmi(to, &sub_string_runtime);

  __ sra(a3, from, kSmiTagSize);  // Remove smi tag.
  __ sra(t5, to, kSmiTagSize);  // Remove smi tag.

  // a3: from index (untagged smi)
  // t5: to index (untagged smi)

  __ Branch(&sub_string_runtime, lt, a3, Operand(zero_reg));  // From < 0

  __ subu(a2, t5, a3);
  __ Branch(&sub_string_runtime, gt, a3, Operand(t5));  // Fail if from > to.

  // Special handling of sub-strings of length 1 and 2. One character strings
  // are handled in the runtime system (looked up in the single character
  // cache). Two character strings are looked for in the symbol cache.
  __ Branch(&sub_string_runtime, lt, a2, Operand(2));

  // Both to and from are smis.

  // a2: result string length
  // a3: from index (untagged smi)
  // t2: (a.k.a. to): to (smi)
  // t3: (a.k.a. from): from offset (smi)
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
  // t2: (a.k.a. to): to (smi)
  // t3: (a.k.a. from): from offset (smi)
  // t5: to index (untagged smi)

  Label seq_string;
  __ And(t0, a1, Operand(kStringRepresentationMask));
  STATIC_ASSERT(kSeqStringTag < kConsStringTag);
  STATIC_ASSERT(kConsStringTag < kExternalStringTag);

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
  STATIC_ASSERT(kSeqStringTag == 0);
  // Cons and External strings go to runtime.
  __ Branch(&sub_string_runtime, ne, a1, Operand(kStringRepresentationMask));

  // Definitly a sequential string.
  __ bind(&seq_string);

  // a1: instance type
  // a2: result string length
  // a3: from index (untagged smi)
  // t1: string
  // t2: (a.k.a. to): to (smi)
  // t3: (a.k.a. from): from offset (smi)
  // t5: to index (untagged smi)

  __ lw(t0, FieldMemOperand(t1, String::kLengthOffset));
  __ Branch(&sub_string_runtime, lt, t0, Operand(to));  // Fail if to > length.
  to = no_reg;

  // a1: instance type
  // a2: result string length
  // a3: from index (untagged smi)
  // t1: string
  // t3: (a.k.a. from): from offset (smi)
  // t5: to index (untagged smi)

  // Check for flat ascii string.
  Label non_ascii_flat;
  STATIC_ASSERT(kTwoByteStringTag == 0);

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
  // t3: (a.k.a. from): from offset (smi)
  // Locate first character of result.
  __ Addu(a1, v0, Operand(SeqAsciiString::kHeaderSize - kHeapObjectTag));
  // Locate 'from' character of string.
  __ Addu(t1, t1, Operand(SeqAsciiString::kHeaderSize - kHeapObjectTag));
  __ Addu(t1, t1, Operand(a3));

  // v0: result string.
  // a1: first character of result string.
  // a2: result string length.
  // t1: first character of sub string to copy.
  STATIC_ASSERT((SeqAsciiString::kHeaderSize & kObjectAlignmentMask) == 0);
  StringHelper::GenerateCopyCharactersLong(
      masm, a1, t1, a2, a3, t0, t2, t3, t4, COPY_ASCII | DEST_ALWAYS_ALIGNED);
  __ IncrementCounter(&Counters::sub_string_native, 1, a3, t0);
  __ Addu(sp, sp, Operand(3 * kPointerSize));
  __ Ret();

  __ bind(&non_ascii_flat);
  // a2: result string length.
  // t1: string.
  // t3: (a.k.a. from): from offset (smi)
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
  __ Addu(t1, t1, Operand(from));
  from = no_reg;

  // v0: result string.
  // a1: first character of result.
  // a2: result length.
  // t1: first character of string to copy.
  STATIC_ASSERT((SeqTwoByteString::kHeaderSize & kObjectAlignmentMask) == 0);
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
  STATIC_ASSERT(kSmiTag == 0);
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
  __ lw(a1, MemOperand(sp, 1 * kPointerSize));  // left
  __ lw(a0, MemOperand(sp, 0 * kPointerSize));  // right

  Label not_same;
  __ Branch(&not_same, ne, a0, Operand(a1));
  STATIC_ASSERT(EQUAL == 0);
  STATIC_ASSERT(kSmiTag == 0);
  __ li(v0, Operand(Smi::FromInt(EQUAL)));
  __ IncrementCounter(&Counters::string_compare_native, 1, a1, a2);
  __ Addu(sp, sp, Operand(2 * kPointerSize));
  __ Ret();

  __ bind(&not_same);

  // Check that both objects are sequential ascii strings.
  __ JumpIfNotBothSequentialAsciiStrings(a1, a0, a2, a3, &runtime);

  // Compare flat ascii strings natively. Remove arguments from stack first.
  __ IncrementCounter(&Counters::string_compare_native, 1, a2, a3);
  __ Addu(sp, sp, Operand(2 * kPointerSize));
  GenerateCompareFlatAsciiStrings(masm, a0, a1, a2, a3, t0, t1);

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
    STATIC_ASSERT(kSmiTag == 0);
    __ JumpIfEitherSmi(a0, a1, &string_add_runtime);
    // Load instance types.
    __ lw(t0, FieldMemOperand(a0, HeapObject::kMapOffset));
    __ lw(t1, FieldMemOperand(a1, HeapObject::kMapOffset));
    __ lbu(t0, FieldMemOperand(t0, Map::kInstanceTypeOffset));
    __ lbu(t1, FieldMemOperand(t1, Map::kInstanceTypeOffset));
    STATIC_ASSERT(kStringTag == 0);
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
    STATIC_ASSERT(kSmiTag == 0);
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
  STATIC_ASSERT(String::kMaxLength < String::kMaxLength * 2);
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
  STATIC_ASSERT((String::kMaxLength & 0x80000000) == 0);
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
  STATIC_ASSERT(kTwoByteStringTag == 0);
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
  STATIC_ASSERT(kAsciiStringTag != 0 && kAsciiDataHintTag != 0);
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
  STATIC_ASSERT(kSeqStringTag == 0);
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

