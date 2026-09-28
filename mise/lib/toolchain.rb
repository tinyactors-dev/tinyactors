# frozen_string_literal: true

require 'pathname'

# Everything in the build that differs between hosts, in one place, so a
# build script gives the same answer on Linux and on macOS:
#
#   Toolchain::Tool          a program pinned in the root mise.toml (zig,
#                            clang, LLVM's binutils, GNU tar), found through
#                            mise; never whatever is on PATH.
#   Toolchain::Target        what the build compiles for: a release target
#                            (zig cc, docs/adr/0001), the check target (the
#                            pinned clang, x86_64 Linux ELF), or the host ($CC).
#   Toolchain::ObjectFormat  what differs between ELF, Mach-O and wasm.
#   Toolchain::Binary        an object, archive or library on disk that reads
#                            its own symbols as C names and checks itself.
#   Toolchain::Host          the machine the build runs on.
#
# Only mise/lib/toolchain/ runs a host-dependent tool by name, reads $CC,
# RUBY_PLATFORM or RbConfig's host, or branches on macos?, linux? or wasm?;
# `mise run check-portability` rejects any other script that does
# (mise/ast-grep/rules/). `mise run toolchain-test` builds and checks every
# target's objects on whatever host runs it.
module Toolchain
  ROOT = Pathname.new(__dir__).join('../..').expand_path

  class Error < StandardError; end
end

require_relative 'toolchain/tool'
require_relative 'toolchain/object_format'
require_relative 'toolchain/binary'
require_relative 'toolchain/target'
require_relative 'toolchain/host'
