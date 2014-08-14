// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_COMPILER_MIPS_INSTRUCTION_CODES_MIPS_H_
#define V8_COMPILER_MIPS_INSTRUCTION_CODES_MIPS_H_

namespace v8 {
namespace internal {
namespace compiler {

// MIPS-specific opcodes that specify which assembly sequence to emit.
// Most opcodes specify a single instruction.
#define TARGET_ARCH_OPCODE_LIST(V) \
  V(MipsAdd)                        \
  V(MipsAnd)                        \
  V(MipsCmp)                        \
  V(MipsOrr)                        \
  V(MipsSub)                        \
  V(MipsMul)                        \
  V(MipsMov)                        \
  V(MipsCallCodeObject)             \
  V(MipsCallJSFunction)             \
  V(MipsCallAddress)                \
  V(MipsPush)                       \
  V(MipsDrop)                       \
  V(MipsLoadWord8)                  \
  V(MipsStoreWord8)                 \
  V(MipsLoadWord16)                 \
  V(MipsStoreWord16)                \
  V(MipsLoadWord32)                 \
  V(MipsStoreWord32)                \
  V(MipsStoreWriteBarrier)

/*** plind
  V(MipsBic)                        \
  V(MipsCmn)                        \
  V(MipsTst)                        \
  V(MipsTeq)                        \
  V(MipsEor)                        \
  V(MipsRsb)                        \
  V(MipsMla)                        \
  V(MipsMls)                        \
  V(MipsSdiv)                       \
  V(MipsUdiv)                       \
  V(MipsMvn)                        \
  V(MipsBfc)                        \
  V(MipsUbfx)                       \

  V(MipsVcmpF64)                    \
  V(MipsVaddF64)                    \
  V(MipsVsubF64)                    \
  V(MipsVmulF64)                    \
  V(MipsVmlaF64)                    \
  V(MipsVmlsF64)                    \
  V(MipsVdivF64)                    \
  V(MipsVmodF64)                    \
  V(MipsVnegF64)                    \
  V(MipsVcvtF64S32)                 \
  V(MipsVcvtF64U32)                 \
  V(MipsVcvtS32F64)                 \
  V(MipsVcvtU32F64)                 \
  V(MipsFloat64Load)                \
  V(MipsFloat64Store)               \
***/



// Addressing modes represent the "shape" of inputs to an instruction.
// Many instructions support multiple addressing modes. Addressing modes
// are encoded into the InstructionCode of the instruction and tell the
// code generator after register allocation which assembler method to call.
#define TARGET_ADDRESSING_MODE_LIST(V)  \
  V(Offset_RI)        /* [%r0 + K] */   \
  V(Offset_RR)        /* [%r0 + %r1] */ \
  V(Operand2_I)       /* K */           \
  V(Operand2_R)       /* %r0 */         \
  V(Operand2_R_ASR_I) /* %r0 ASR K */   \
  V(Operand2_R_LSL_I) /* %r0 LSL K */   \
  V(Operand2_R_LSR_I) /* %r0 LSR K */   \
  V(Operand2_R_ROR_I) /* %r0 ROR K */   \
  V(Operand2_R_ASR_R) /* %r0 ASR %r1 */ \
  V(Operand2_R_LSL_R) /* %r0 LSL %r1 */ \
  V(Operand2_R_LSR_R) /* %r0 LSR %r1 */ \
  V(Operand2_R_ROR_R) /* %r0 ROR %r1 */

}  // namespace compiler
}  // namespace internal
}  // namespace v8

#endif  // V8_COMPILER_MIPS_INSTRUCTION_CODES_MIPS_H_
