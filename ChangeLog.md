ChangeLog
=========

All notable changes to the project are documented in this file.

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

[v1.1]:       https://github.com/troglobit/mdns-alias/compare/v1.0...v1.1
[UNRELEASED]: https://github.com/troglobit/mdns-alias/compare/v1.1...HEAD
[v1.0]:       https://github.com/troglobit/mdns-alias/compare/TAIL...v1.0
