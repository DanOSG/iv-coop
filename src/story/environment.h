#pragma once
// Clock and weather for joiners: the host's world drives both.
//
// The host sends its time of day and current weather every 2 s. A joiner sets its clock when it is
// more than a game minute off (SET_TIME_OF_DAY; the clock keeps running locally in between) and forces
// the host's weather: snapped on the first update (FORCE_WEATHER_NOW), afterwards FORCE_WEATHER so the
// change blends in as it does for the host. Leaving releases the weather back to the local cycle.

#include <cstdint>
#include <span>

#include "net/session.h"

namespace ivc::story::environment {

void host_tick(bool hosting, net::Session& net);
void client_apply(std::span<const std::uint8_t> msg);
void client_tick();
void clear();

} // namespace ivc::story::environment
