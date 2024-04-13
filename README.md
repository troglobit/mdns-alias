Publish mDNS Alias
==================

This is a small C daemon that publishes mDNS CNAMEs using Avahi.

Currently the program takes its CNAME aliases on the command line:

    mdns-alias foo.local bar.local

At least one name, including `.local` must be given on the command line.


Dependencies
------------

At run time: a running `avahi-daemon`

Build time: avahi client library and header files, and to detect it, the
`pkg-config` tool.  You also need a C compiler and `make`.

Ubuntu, Mint, or other Debian based Linux distributions:

    sudo apt install libavahi-client-dev pkg-config


Building
--------

This project is built using `configure` and `make`.  The configure
script generates the `Makefile` for your host system, or your embedded
[target system][2].  Usually setting `--host=toolchain-prefix-` is all
you need for the `configure` script to find your cross compiler.

    ./configure
    make

By default the configure script prepares for installing in `/usr/local`,
if you want to change that, please see `./configure --help`.  To install
type:

    sudo make install

Some systems no longer have `sudo`, alternatives include `su` to root,
or tools like `doas`.

> **Note:** if you do not like pre-packaged, and properly versioned,
> release tarballs, you need to have GNU autoconf and automake installed
> to be able run the `autogen.sh` script.  It creates the `configure`
> script and Makefile template, which are included in the portable
> release tarballs.


Motivation
----------

For some reason the de facto standard mDNS-SD daemon in Linux, Avahi,
does not support CNAME (DNS alias) records in the configuration files
or using any of its tools.  This has been covered in many repos, but
none did it better than [George Hawkins][1], I believe.

[1]: https://github.com/george-hawkins/avahi-aliases-notes
[2]: https://www.gnu.org/software/automake/manual/html_node/Cross_002dCompilation.html
