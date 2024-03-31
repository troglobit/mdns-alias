dist_doc_DATA       = README.md LICENSE
sbin_PROGRAMS       = mdns-alias

mdns_alias_SOURCES  = mdns-alias.c
mdns_alias_CFLAGS   = -W -Wall -Wextra -Wno-unused-parameter
mdns_alias_CFLAGS  += $(avahi_CFLAGS)
mdns_alias_LDADD    = $(avahi_LIBS)
