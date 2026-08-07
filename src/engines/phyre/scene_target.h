// SPDX-License-Identifier: MIT
#pragma once

// Which bind carries Ayesha's 3D scene.
//
// This is PhyreEngine knowledge, which is why it is here and not in
// `src/core/scene_pass.cpp`. Observing the render-target binds is genuine
// D3D11 mechanism and holds for any renderer; the question "of the several
// dozen render-target binds a frame issues, which one is the scene" is a
// property of a particular engine and has to be answered per engine.
// `src/core/scene_pass.h` states the general form of that argument on
// SceneTargetTest.
//
// Getting it wrong is quiet rather than loud: a rule that matches a shadow map
// or a UI layer sends the pre-UI SMAA pass and supersampling's downscale at the
// wrong surface and leaves the scene exactly as it was, while every counter
// reports success. That failure mode has cost this project whole sessions, so
// the rule is stated narrowly and its evidence is recorded rather than assumed.
namespace dusk {

// Registers Ayesha's rule with the core scene-pass module. Safe to call more
// than once. Costs nothing when neither SMAA nor supersampling is on -- core
// only consults the rule when one of them is asking.
void registerPhyreSceneTarget();

}  // namespace dusk
