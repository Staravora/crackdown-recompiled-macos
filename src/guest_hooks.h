#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace crackdown::hooks {

using GuestFunction = void (*)(PPCContext&, uint8_t*);

// Replacements for guest functions. The host code behind them is the same in every recompiled guest; the bindings in
// guest_bindings_base.cpp and guest_bindings_tu6.cpp say which guest function each one replaces, and `original` is that
// function's own code.
void FilterPadState(PPCContext&, uint8_t*, GuestFunction original);
void CameraConstructed(PPCContext&, uint8_t*, GuestFunction original);
void AudioBarrier(PPCContext&, uint8_t*, GuestFunction original);
void AudioResetEvent(PPCContext&, uint8_t*);
void AudioMixerCallback(PPCContext&, uint8_t*, GuestFunction original);
void WaveInstanceSetup(PPCContext&, uint8_t*, GuestFunction original);
void AudioReleaseHandle(PPCContext&, uint8_t*, GuestFunction original);

void DamageEntry(PPCContext&, uint8_t*, GuestFunction original);
void FireWeaponTracked(PPCContext&, uint8_t*, GuestFunction original);
void VehicleDamage(PPCContext&, uint8_t*, GuestFunction original);
void EngineTorque(PPCContext&, uint8_t*, GuestFunction original);
void WalkVelocity(PPCContext&, uint8_t*, GuestFunction original);
void JumpLaunch(PPCContext&, uint8_t*, GuestFunction original);

void CrtMemcpy(PPCContext&, uint8_t*, GuestFunction original);
void CrtMemmove(PPCContext&, uint8_t*, GuestFunction original);
void CrtMemset(PPCContext&, uint8_t*, GuestFunction original);

void EntityDestroyed(PPCContext&, uint8_t*, GuestFunction original);

void FrameLoop(PPCContext&, uint8_t*, GuestFunction original);
void SimulationUpdate(PPCContext&, uint8_t*, GuestFunction original);
void FrameDelta(PPCContext&, uint8_t*);
void TimingUpdate(PPCContext&, uint8_t*, GuestFunction original);
void FrameTimer(PPCContext&, uint8_t*, GuestFunction original);
void PresentInterval(PPCContext&, uint8_t*, GuestFunction original);
void SwapPacket(PPCContext&, uint8_t*, GuestFunction original);
void SwapCallback(PPCContext&, uint8_t*, GuestFunction original);
void SwapBuilder(PPCContext&, uint8_t*, GuestFunction original);

void CensusBeginVertices(PPCContext&, uint8_t*, GuestFunction original);
void CensusBeginIndexedVertices(PPCContext&, uint8_t*, GuestFunction original);
void CensusDrawVertices(PPCContext&, uint8_t*, GuestFunction original);
void CensusDrawIndexed(PPCContext&, uint8_t*, GuestFunction original);
void RingSegmentEnd(PPCContext&, uint8_t*, GuestFunction original);
void CensusClear(PPCContext&, uint8_t*, GuestFunction original);
void CensusResolve(PPCContext&, uint8_t*, GuestFunction original);
void CensusBeginTiling(PPCContext&, uint8_t*, GuestFunction original);
void CensusEndTiling(PPCContext&, uint8_t*, GuestFunction original);
void CensusRunCommandBuffer(PPCContext&, uint8_t*, GuestFunction original);
void CensusQueryIssue(PPCContext&, uint8_t*, GuestFunction original);
void CensusQueryData(PPCContext&, uint8_t*, GuestFunction original);
void CensusTextureLock(PPCContext&, uint8_t*, GuestFunction original);
void CensusCreateTexture(PPCContext&, uint8_t*, GuestFunction original);
void CensusCacheFlush(PPCContext&, uint8_t*, GuestFunction original);

void GpuFence(PPCContext&, uint8_t*, GuestFunction original);
void GpuPoll(PPCContext&, uint8_t*, GuestFunction original);

void MenuPopupOpen(PPCContext&, uint8_t*, GuestFunction original);
void MenuPopupYes(PPCContext&, uint8_t*, GuestFunction original);
void MenuPopupNo(PPCContext&, uint8_t*, GuestFunction original);
void MenuPopupDestroyed(PPCContext&, uint8_t*, GuestFunction original);
void MenuStringById(PPCContext&, uint8_t*, GuestFunction original);
void MenuBlockDestroyed(PPCContext&, uint8_t*, GuestFunction original);

void AimUpdate(PPCContext&, uint8_t*, GuestFunction original);
void CameraUpdate(PPCContext&, uint8_t*, GuestFunction original);

void EngineMovie(PPCContext&, uint8_t*, GuestFunction original);
void PlayVideo(PPCContext&, uint8_t*, GuestFunction original);
void SequenceFrame(PPCContext&, uint8_t*, GuestFunction original);

void ApplyLanguage(PPCContext&, uint8_t*, GuestFunction original);
void VehicleAction(PPCContext&, uint8_t*, GuestFunction original);

void OrbConstructed(PPCContext&, uint8_t*, GuestFunction original);
void OrbDestroyed(PPCContext&, uint8_t*, GuestFunction original);
void OrbMapViewConstructed(PPCContext&, uint8_t*, GuestFunction original);
void OrbMapViewDestroyed(PPCContext&, uint8_t*, GuestFunction original);
void OrbFrontendScreenConstructed(PPCContext&, uint8_t*, GuestFunction original);
void OrbFrontendScreenDestroyed(PPCContext&, uint8_t*, GuestFunction original);
void OrbListDescriptorShown(PPCContext&, uint8_t*, GuestFunction original);
void OrbListDescriptorPosition(PPCContext&, uint8_t*, GuestFunction original);
void OrbMinimapProjection(PPCContext&, uint8_t*, GuestFunction original);

// Guest functions the hooks call that no hook replaces; each build's bindings supply their own (GuestImage::calls).
struct GuestCalls {
  GuestFunction movie_singleton;
  GuestFunction sequence_advance;
};

}  // namespace crackdown::hooks
