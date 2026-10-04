# pocketparse @VERSION@ — @PLATFORM@

This tarball unpacks to a prefix-shaped tree:

    bin/pocketparse                 the command-line tool
    lib/libpocketparse.a            static library
    lib/libpocketparse.{so,dylib}   shared library
    include/pocketparse.h           the public header
    share/man/man1/pocketparse.1    the manual page

## Install

Copy the tree into a prefix. System-wide:

    sudo cp -r * /usr/local/

Into your home prefix:

    cp -r * ~/.local/

After installing, `pocketparse --help` prints the usage summary and
`man pocketparse` shows the manual page (make sure the prefix's
`share/man` is on your `MANPATH`).

## Linking against the library

    cc myprog.c -lpocketparse        # add -L/-I if not in a default prefix

Include only `pocketparse.h`; the ax engine that powers the library is an
internal detail and exports no symbols of its own.
