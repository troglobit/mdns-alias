sbin_PROGRAMS       = mdns-alias

mdns_alias_SOURCES  = mdns-alias.c
mdns_alias_CFLAGS   = -W -Wall -Wextra -Wno-unused-parameter -std=gnu99
mdns_alias_CFLAGS  += $(avahi_CFLAGS) $(uev_CFLAGS)
mdns_alias_LDADD    = $(avahi_LIBS) $(uev_LIBS) $(LIBS) $(LIBOBJS)

AUTOMAKE_OPTIONS    = subdir-objects
