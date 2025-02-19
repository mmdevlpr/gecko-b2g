/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm64_SharedICHelpers_arm64_h
#define jit_arm64_SharedICHelpers_arm64_h

#include "jit/BaselineIC.h"
#include "jit/JitFrames.h"
#include "jit/MacroAssembler.h"
#include "jit/SharedICRegisters.h"

namespace js {
namespace jit {

// Distance from sp to the top Value inside an IC stub (no return address on the
// stack on ARM).
static const size_t ICStackValueOffset = 0;

inline void EmitRestoreTailCallReg(MacroAssembler& masm) {
  // No-op on ARM because link register is always holding the return address.
}

inline void EmitRepushTailCallReg(MacroAssembler& masm) {
  // No-op on ARM because link register is always holding the return address.
}

inline void EmitCallIC(MacroAssembler& masm, CodeOffset* callOffset) {
  // The stub pointer must already be in ICStubReg.
  // Load stubcode pointer from the ICStub.
  // R2 won't be active when we call ICs, so we can use r0.
  static_assert(R2 == ValueOperand(r0));
  masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), r0);

  // Call the stubcode via a direct branch-and-link.
  masm.Blr(x0);
  *callOffset = CodeOffset(masm.currentOffset());
}

inline void EmitReturnFromIC(MacroAssembler& masm) {
  masm.abiret();  // Defaults to lr.
}

inline void EmitBaselineLeaveStubFrame(MacroAssembler& masm) {
  vixl::UseScratchRegisterScope temps(&masm.asVIXL());
  const ARMRegister scratch64 = temps.AcquireX();

  masm.moveToStackPtr(FramePointer);

  // Pop values, discarding the frame descriptor.
  masm.pop(FramePointer, ICStubReg, ICTailCallReg, scratch64.asUnsized());

  // Stack should remain 16-byte aligned.
  masm.checkStackAlignment();
}

template <typename AddrType>
inline void EmitPreBarrier(MacroAssembler& masm, const AddrType& addr,
                           MIRType type) {
  // On AArch64, lr is clobbered by guardedCallPreBarrier. Save it first.
  masm.push(lr);
  masm.guardedCallPreBarrier(addr, type);
  masm.pop(lr);
}

inline void EmitStubGuardFailure(MacroAssembler& masm) {
  // Load next stub into ICStubReg.
  masm.loadPtr(Address(ICStubReg, ICCacheIRStub::offsetOfNext()), ICStubReg);

  // Return address is already loaded, just jump to the next stubcode.
  masm.jump(Address(ICStubReg, ICStub::offsetOfStubCode()));
}

}  // namespace jit
}  // namespace js

#endif  // jit_arm64_SharedICHelpers_arm64_h
