# tinyparse @VERSION@ — @PLATFORM@

This tarball unpacks to a prefix-shaped tree:

    bin/tinyparse                 the command-line tool
    lib/libtinyparse.a            static library
    lib/libtinyparse.{so,dylib}   shared library
    include/tinyparse.h           the public header
    share/man/man1/tinyparse.1    the manual page

## Install

Copy the tree into a prefix. System-wide:

    sudo cp -r * /usr/local/

Into your home prefix:

    cp -r * ~/.local/

After installing, `tinyparse --help` prints the usage summary and
`man tinyparse` shows the manual page (make sure the prefix's
`share/man` is on your `MANPATH`).

## Linking against the library

    cc myprog.c -ltinyparse        # add -L/-I if not in a default prefix

Include only `tinyparse.h`; the library has no dependencies of its own.
