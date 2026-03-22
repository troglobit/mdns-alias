ChangeLog
=========

All notable changes to the project are documented in this file.

[v1.2][] - 2026-03-22
---------------------

### Changes

- Switch event loop from `AvahiSimplePoll` to [libuev][], implementing
  the `AvahiPoll` vtable directly.  This removes the libavahi-common
  poll helper dependency and integrates cleanly with libuev signals and
  timers
- Add `SIGHUP` support: when running with `-H`, send `SIGHUP` to reload
  the system hostname and republish CNAMEs without restarting the daemon
- Handle Avahi daemon startup race and runtime restarts gracefully using
  `AVAHI_CLIENT_NO_FAIL`: the daemon now waits for avahi-daemon to
  appear on the bus rather than failing immediately, and automatically
  reconnects if avahi-daemon is restarted

[v1.1][] - 2026-03-03
---------------------

### Changes

- Replace `stdio` logging with `syslog(3)`
- Add `-l LEVEL` command-line option to control log verbosity at runtime
- Add `-H` command-line option to get CNAME from `gethostname()` API

### Bug Fixes

- Fix mDNS CNAME conflict resolution: remove `AVAHI_PUBLISH_ALLOW_MULTIPLE`
  flag so standard mDNS probing (RFC 6762 §8) is used and only one host
  wins a given CNAME.  Handle the resulting `AVAHI_ENTRY_GROUP_COLLISION`
  state gracefully
- Fix CNAME target after Avahi hostname collision: replace `gethostname(2)`
  with `avahi_client_get_host_name()` so the published CNAME target always
  reflects the name Avahi is currently advertising, including after a
  collision-driven rename
- Fix missing `return` on invalid CNAME argument: the daemon previously
  logged the error but continued running with bad input

[v1.0][] - 2024-04-14
---------------------

Initial public release.  Basic support for publishing cname0.local
.. cnameN.local using the Avahi client API.  Restricted to .local
for now.

[v1.2]:       https://github.com/troglobit/mdns-alias/compare/v1.1...v1.2
[v1.1]:       https://github.com/troglobit/mdns-alias/compare/v1.0...v1.1
[UNRELEASED]: https://github.com/troglobit/mdns-alias/compare/v1.2...HEAD
[v1.0]:       https://github.com/troglobit/mdns-alias/compare/TAIL...v1.0
[libuev]:     https://github.com/troglobit/libuev
