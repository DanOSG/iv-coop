#pragma once
// UPnP port mapping for hosting by IP (no Steam relay): asks the router to forward our UDP
// port to this PC, using Windows' built-in NAT traversal COM API (natupnp.h), so there's
// no third-party dependency. COM calls can block for seconds (router discovery), so
// everything runs on a private worker thread; the game thread only queues work and polls.

#include <cstdint>
#include <optional>
#include <string>

namespace ivc::upnp {

// Try to open `port` (UDP). Returns at once; the outcome arrives through poll().
void open(std::uint16_t port);
// Remove the mapping we added (if any). Returns at once.
void close();
// The outcome of the last open(), once, as a line for the phone ("Port 41300 opened on your
// router ..." / "Couldn't open the port: ..."); empty optional while still working or when
// already reported.
std::optional<std::string> poll();
// True while a mapping we created is in place.
bool mapped();

} // namespace ivc::upnp
