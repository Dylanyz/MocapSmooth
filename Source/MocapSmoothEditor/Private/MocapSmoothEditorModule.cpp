// SPDX-License-Identifier: Apache-2.0

#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "MocapSmoothFilter.h"

/**
 * `MocapSmooth.SelfTest` in the console runs the same six checks as the reference
 * implementation's self-test (slider mapping, the 0.802*fc knee, zero phase, Gaussian never
 * overshooting, Butterworth ringing ~3.4%, quaternion norm across a sign flip) and logs each one.
 * Cheap, and the fastest way to confirm a build of the filter is sound without touching an asset.
 */
static FAutoConsoleCommand GMocapSmoothSelfTest(
	TEXT("MocapSmooth.SelfTest"),
	TEXT("Run the mocap smoothing filter self-test and log the results."),
	FConsoleCommandDelegate::CreateStatic([]() { MocapSmoothFilter::SelfTest(); }));

IMPLEMENT_MODULE(FDefaultModuleImpl, MocapSmoothEditor);
