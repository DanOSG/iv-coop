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
// Keys (on the station you're tuned to; they start items directly on the stream slot, see start_item):
//   Ctrl+Shift+1  the next item of the playing list (entry + 8), from its start
//   Ctrl+Shift+2  the playing item again, 30 s further on (start_item with an offset)
//   Ctrl+Shift+3  the playing item again, from the start
//   Ctrl+Shift+4  every station not playing aloud: its next item, lined up 60 s in (heard on tuning to it)

namespace ivc::radio_probe {

void configure(bool enabled);
void tick();

} // namespace ivc::radio_probe
