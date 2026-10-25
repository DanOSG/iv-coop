#pragma once
// Radio research probe (docs/research-radio.md, experiments E1-E4). Logging plus three debug keys;
// on only with developer keys.
//
// Every second it reads each station's playing stream slot (current slot +0x1917; in the slot,
// category +0xBC0, track number +0xBC4, track-list entry +0x960 - the exact values ForceTrack
// 0x945120 writes) and logs changes, naming the track list by matching the pointer against
// metadata lookups of candidate list names.
//
// Keys (on the station you're tuned to):
//   Ctrl+Shift+1  freeze, force the next track number of the playing list, unfreeze
//   Ctrl+Shift+2  same, but stay frozen (E4: what does unfreezing change?)
//   Ctrl+Shift+3  freeze, force the playing track again (restarts it?), unfreeze

namespace ivc::radio_probe {

void configure(bool enabled);
void tick();

} // namespace ivc::radio_probe
