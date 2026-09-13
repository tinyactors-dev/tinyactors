require "mkmf"

$INCFLAGS << " -I$(srcdir)/../../src"

abort "Ruby headers are required" unless have_header("ruby.h")

# Build the extension against the same runtime sources as the repository's
# native tests.  Keeping this here makes the checkout usable without a
# separately installed libtinyactors.
$srcdir = File.expand_path("../../src", __dir__)
$VPATH << $srcdir
$INCFLAGS << " -I$(srcdir)/../../src"
runtime = Dir[File.join($srcdir, "*.c")].reject { |f| File.basename(f) == "compiler_runtime.c" }
$srcs = ["tinyactors.c"] + runtime
$objs = $srcs.map { |f| "#{File.basename(f, '.c')}.o" }

create_makefile("tinyactors/tinyactors")
