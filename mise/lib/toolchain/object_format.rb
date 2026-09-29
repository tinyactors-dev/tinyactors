# frozen_string_literal: true

module Toolchain
  # What differs between the object formats the targets produce: how a C
  # name is spelled in the symbol table, what a shared library is called
  # and how it names itself, what the linker and the platform's runtime
  # add. A Target has one; a Binary finds its own (ObjectFormat.of).
  # Callers never branch on the format: they call it, or a Target or
  # Binary that does.
  module ObjectFormat
    # The format of the object whose `llvm-readobj --file-headers` output
    # is `headers`; an archive answers its members' format.
    def self.of(headers)
      case headers[/^Format: (.*)$/, 1].to_s
      when /\Aelf/ then ELF
      when /\AMach-O/ then MachO
      when 'WASM' then WASM
      else raise Error, "an unknown object format:\n#{headers}"
      end
    end

    # ELF: Linux. A C name is its symbol; a shared library exports through
    # its dynamic symbol table.
    module ELF
      module_function

      def to_s = 'ELF'
      def c_name(symbol) = symbol
      def symbol(c_name) = c_name
      def shared?(headers) = headers.match?(/^\s*Type: SharedObject/)
      def symbol_table(shared) = shared ? ['-D'] : []
      def linker_defined = []
      def runtime_undefined(shared: false) = []
      def archive_flags = []
      def position_independent = ['-fPIC']

      def shared_library_file(library, version) = "lib#{library}.so.#{version}"
      def shared_library_glob = 'lib*.so.*'

      def identity_flags(file) = ["-Wl,-soname,#{file}"]
      # The name a program that links `file` records for it.
      def install_name(file) = file
      def runtime_libraries = []
      # A library links no C library: its libc functions bind to the
      # process's (ADR 0001 decision 10). -S drops the debug info zig cc
      # emits by default.
      def links_libc = false
      def shared_flags(exports: nil) = exports ? %w[-Wl,--gc-sections -Wl,-S] : %w[-Wl,-S]
      # ELF leaves a shared object's undefined symbols to the loader.
      def plugin_flags = []
      def host_plugin_flags = %w[-shared]

      # The development name beside a versioned library (lib<name>.so).
      def install_links(file, library, dir)
        File.symlink(file, dir.join("lib#{library}.so"))
      end

      # One relocatable object of `objects`, keeping only what `keep`
      # reaches when it is given.
      def combine(_target, objects, out, keep: nil)
        roots = keep ? ['--gc-sections', *keep.flat_map { |name| ['-u', name] }] : []
        Tool[:zig].run('ld.lld', '-r', *roots, *objects, '-o', out)
      end

      def identity(path)
        Tool[:llvm_readobj].run('--dynamic-table', path)[/Library soname: \[([^\]]+)\]/, 1]
      end

      def dependencies(path)
        Tool[:llvm_readobj].run('--needed-libs', path)[/NeededLibraries \[(.*?)\]/m, 1].to_s.split
      end

      def minimum_os(_path) = nil
    end

    # Mach-O: macOS. Every C name has a leading underscore in the symbol
    # table; every library links libSystem, and ld64 defines a few symbols
    # in each one.
    module MachO
      module_function

      def to_s = 'Mach-O'
      def c_name(symbol) = symbol.delete_prefix('_')
      def symbol(c_name) = "_#{c_name}"
      def shared?(headers) = headers.match?(/FileType: DynamicLibrary/)
      def symbol_table(_shared) = []
      def linker_defined = %w[__dso_handle _mh_dylib_header]

      # What a Mach-O binary may leave to libSystem, which every macOS
      # process has, whatever its code: compiler-rt calls the first two,
      # and Darwin's clang lowers zeroing to bzero (__bzero on x86_64)
      # whatever the flags (-fno-builtin-bzero does not stop it). A dylib
      # with stubs also calls the lazy-binding helper.
      def runtime_undefined(shared: false)
        %w[_availability_version_check sys_icache_invalidate bzero __bzero] + (shared ? %w[dyld_stub_binder] : [])
      end

      def archive_flags = ['--format=darwin']
      def position_independent = ['-fPIC']

      def shared_library_file(library, version) = "lib#{library}.#{version}.dylib"
      def shared_library_glob = 'lib*.*.dylib'

      def identity_flags(file) = ["-Wl,-install_name,#{install_name(file)}"]
      def install_name(file) = "@rpath/#{file}"
      # Every process has libSystem; every dylib loads it.
      def runtime_libraries = %w[/usr/lib/libSystem.B.dylib]
      def links_libc = true

      # ld64 garbage-collects with -dead_strip and hides what is not listed.
      def shared_flags(exports: nil)
        exports ? ['-Wl,-dead_strip', "-Wl,-exported_symbols_list,#{exports}"] : []
      end

      # A Mach-O dylib must name where every symbol comes from, unless told
      # the loading process provides it (a Node-API addon's napi_*).
      def plugin_flags = %w[-Wl,-undefined,dynamic_lookup]
      def host_plugin_flags = %w[-bundle -undefined dynamic_lookup]

      def install_links(_file, _library, _dir) = nil

      # zig has no ld64.lld to garbage-collect a relocatable link, so `keep`
      # roots nothing; a dylib's -dead_strip drops what the API does not
      # reach.
      def combine(target, objects, out, keep: nil)
        Tool[:zig].run('cc', '-target', target.triple, '-r', *objects, '-o', out)
      end

      def identity(path) = Tool[:llvm_otool].run('-D', path).lines.drop(1).first.to_s.strip

      # The libraries it loads; the list llvm-readobj gives starts with its
      # own install name.
      def dependencies(path)
        own = identity(path)
        Tool[:llvm_readobj].run('--needed-libs', path)[/NeededLibraries \[(.*?)\]/m, 1].to_s.split - [own]
      end

      def minimum_os(path)
        Tool[:llvm_otool].run('-l', path)[/cmd LC_BUILD_VERSION.*?minos (\S+)/m, 1]
      end
    end

    # WebAssembly: a module has no shared libraries and nothing to mangle.
    module WASM
      module_function

      def to_s = 'wasm'
      def c_name(symbol) = symbol
      def symbol(c_name) = c_name
      def shared?(_headers) = false
      def symbol_table(_shared) = []
      def linker_defined = []
      # A wasm object refers to the stack pointer global, and a call through
      # a function pointer (a vtable) to the indirect function table; the
      # linker defines both when it makes the module.
      def runtime_undefined(shared: false) = %w[__stack_pointer __indirect_function_table]
      def archive_flags = []
      def position_independent = []

      def shared_library_file(*) = raise(Error, 'wasm has no shared library; link a module (Target#wasm_module)')
      def combine(*) = raise(Error, 'wasm objects are linked into a module (Target#wasm_module), not combined')
      def install_name(file) = file
      def runtime_libraries = []
      def identity(_path) = nil
      def dependencies(_path) = []
      def minimum_os(_path) = nil
    end
  end
end
