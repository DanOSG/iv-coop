#pragma once
// Radio research probe (docs/research-radio.md, experiments E1-E4). Logging plus three debug keys;
// on only with developer keys.
//
// Every second it reads each station's playing stream slot (current slot +0x1917; in the slot,
// category +0xBC0, track number +0xBC4, track-list entry +0x960 - the exact values ForceTrack
// 0x945120 writes) and logs changes, naming the track list by matching the pointer against
// metadata lookups of candidate list names. Both slots are logged (crossfades, DJ talk over intros),
// and the tuned slot is scanned for a steadily rising counter (the play position, for mid-item joins).
//
// Keys (on the station you're tuned to; each traces the slot for 4 s afterwards):
//   Ctrl+Shift+1  the next item of the playing category, started 30 s in
//   Ctrl+Shift+2  the same, from its start (control)
//   Ctrl+Shift+3  the playing item again, 30 s further on

namespace ivc::radio_probe {

void configure(bool enabled);
void tick();

} // namespace ivc::radio_probe
