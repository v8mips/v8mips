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
  V(MipsSub)                        \
  V(MipsMul)                        \
  V(MipsAnd)                        \
  V(MipsOr)                         \
  V(MipsXor)                        \
  V(MipsShl)                        \
  V(MipsShr)                        \
  V(MipsSar)                        \
  V(MipsRor)                        \
  V(MipsMov)                        \
  V(MipsCmp)                        \
  V(MipsCallCodeObject)             \
  V(MipsCallJSFunction)             \
  V(MipsCallAddress)                \
  V(MipsPush)                       \
  V(MipsDrop)                       \
  V(MipsLbu)                        \
  V(MipsSb)                         \
  V(MipsLhu)                        \
  V(MipsSh)                         \
  V(MipsLw)                         \
  V(MipsSw)                         \
  V(MipsLdc1)                       \
  V(MipsSdc1)                       \
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
//
// We use the following local notation for addressing modes:
//
// R = register
// O = register or stack slot
// D = double register
// I = immediate (handle, external, int32)
// MRI = [register + immediate]
// MRR = [register + register]
// TODO(plind): Add the new r6 address modes.
#define TARGET_ADDRESSING_MODE_LIST(V) \
  V(MRI) /* [%r0 + K] */               \
  V(MRR) /* [%r0 + %r1] */


}  // namespace compiler
}  // namespace internal
}  // namespace v8

#endif  // V8_COMPILER_MIPS_INSTRUCTION_CODES_MIPS_H_
