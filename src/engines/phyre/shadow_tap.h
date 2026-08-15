// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>

// PCF tap rescale for the enlarged shadow map on Ayesha.
//
// WHAT IT IS FOR. `core/shadow_res.h` allocates a larger shadow map, but the
// receiver's filter offset is stated in UV rather than in texels, so enlarging
// the map alone leaves the blur covering exactly the same world area it covered
// at 1024. The picture gains sampling precision and no visible sharpness. This
// is the half that makes the enlargement show: rescale the offset by
// 1024/newSize so the kernel stays one texel wide, which is what the Arland
// project's `tapScalePatch` does for Rorona, Totori and Meruru.
//
// This was measured rather than argued. A shot indoors at multiplier 8 against
// the same shot at 1 gave a shadow edge transition of 11 px against 9 px, which
// is no change; the same comparison on Arland, where the rescale is applied, is
// obvious to the eye. Without this, ShadowMultiplier is very nearly a no-op.
//
// WHY A ONE-TIME PATCH, WHERE ARLAND INTERCEPTS EVERY WRITE. Arland reaches the
// value through an 880-byte receiver material and has to patch it at both
// constant-buffer write paths, on every write, forever. Ayesha does not need
// any of that: the engine sets the shader parameter by name from a single site,
// and there is exactly one reference to the `tapScale` string in the whole
// executable. Patch that site once at startup and every material that later
// asks for the parameter by name receives the corrected value.
//
//     movss xmm2, [rip+disp]      ; disp -> a float holding 1/1024
//     lea   rdx, [rip+disp]       ; -> "tapScale"
//     call  <set parameter by name>
//
// WHY THE FLOAT ITSELF IS NOT PATCHED, which would be a four-byte write and no
// instruction rewriting at all. The compiler pooled the literal. Its other user
// multiplies an integer by it twice, which is a bytes-to-megabytes conversion,
// so editing the constant in place would silently corrupt a size readout
// somewhere in the game. The instruction is repointed instead and the engine's
// own constant is left alone.
//
// WHY A MOD-OWNED PAGE AND NOT SPARE ROOM IN THE IMAGE. `.rdata` carries a
// 331 KB run of zeros that looks like free space and was rejected as a home for
// the replacement value. A scan for displacements that resolve into that range
// returns thousands of candidates, and while a scan like that cannot tell an
// instruction from a coincidence, it also cannot establish the region is dead.
// Writing a value into memory the game may read is not worth saving an
// allocation. The page below is the mod's own, and the only thing that can read
// it is the instruction this file repoints.
//
// THE REACH IS CHECKED, NOT ASSUMED. A rip-relative operand carries a signed
// 32-bit displacement, so the replacement value has to land within 2 GB of the
// instruction. The allocation asks for an address near the module and the patch
// declines if what it gets is out of range, which leaves the vanilla constant
// in place and the enlarged map behaving as it did before this file existed.
namespace atfix {

// Repoints Ayesha's `tapScale` load at the mod's own rescaled value. Does
// nothing unless the shadow multiplier is above 1, since at 1 the engine's own
// constant is already correct. Gated on the recognized build's expected byte
// window; declines and logs if the window does not match, if the allocation is
// out of rip-relative reach, or if the page protection cannot be changed.
//
// Idempotent. Returns true when the patch is in place.
bool installShadowTapScale(BYTE* base, uint8_t exeBuild);

}  // namespace atfix
