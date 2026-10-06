# frozen_string_literal: true

require 'json'
require 'pathname'
require 'tmpdir'
require 'toolchain'

# The API description of a C library whose header needs no annotations
# beyond its API macro (tinyactors-core's libraries): what the release
# exports and checks (CRelease.api_functions), and what a changelog compares
# between releases (Changelog::APISurface), as JSON:
#
#   functions  every function the header declares with the API macro: its
#              return and parameter types, whether an amalgamation defines it
#              (`implemented`), and `platform` when only some targets have it
#   types      every type the header names with the prefix: a struct's or
#              union's fields, an enum's constants, an alias's type, and the
#              size, alignment and field offsets on each of LAYOUTS
#   constants  every enum constant and integer macro with the prefix
#
# The header is read with the pinned clang in freestanding mode for fixed
# targets, so the description does not depend on the machine that writes it.
# A function the header declares for wasm only (`#if defined(__wasm__)`) has
# platform `wasm`; one it declares for every target but freestanding wasm
# (`#if !defined(__wasm__) || defined(__wasi__)`) has platform `hosted`
# (PlatformFiles). tinyxml has its own, annotation-driven description.
module CAPI
  Error = Class.new(StandardError)

  FORMAT = 'c-api/1'
  # Where types are read, and the hosted platform's functions.
  NATIVE = 'x86_64-unknown-linux-gnu'
  # Freestanding wasm: the wasm platform's functions.
  WASM = 'wasm32-unknown-unknown'
  # The targets every struct has a layout for.
  LAYOUTS = { 'x86_64' => NATIVE, 'aarch64' => 'aarch64-unknown-linux-gnu', 'wasm32' => WASM }.freeze

  module_function

  # The description of `header`, whose names begin with `prefix` (`ta_`,
  # `TA_`), whose functions are declared with `macro`. `sources` maps a
  # platform (nil for portable) to the amalgamation that implements it;
  # `includes` are directories the header needs. `exclude` are names a
  # library it builds on already describes (its header includes theirs).
  # Each source comes with the Toolchain::Target that compiles it here, and
  # the flags it needs beyond C11.
  def describe(header:, macro:, prefix:, sources:, includes: [], exclude: [], version: nil)
    header = Pathname.new(header)
    flags = ->(target) { clang_flags(target, macro, includes) }
    native = AST.new(json(flags.call(NATIVE), header), prefix, exclude)
    wasm = AST.new(json(flags.call(WASM), header), prefix, exclude)
    implemented = sources.to_h do |platform, (source, target, flags)|
      [platform, exported(source, target, macro, includes, flags || [])]
    end
    functions = native.functions.map do |function|
      wasm.function?(function['name']) ? function : function.merge('platform' => 'hosted')
    end
    functions += wasm.functions.reject { |function| native.function?(function['name']) }
                     .map { |function| function.merge('platform' => 'wasm') }
    functions.each do |function|
      function['implemented'] = Array(implemented[function['platform']]).include?(function['name']) ||
                                Array(implemented[nil]).include?(function['name'])
    end
    types = native.types
    {
      'format' => FORMAT,
      'header' => header.basename.to_s,
      **(version ? { 'version' => version } : {}),
      'targets' => LAYOUTS.keys,
      'constants' => constants(header, native, flags, prefix, exclude),
      'types' => layouts(header, types, flags),
      'functions' => functions.sort_by { |function| function['name'] }
    }
  end

  def render(description) = "#{JSON.pretty_generate(description)}\n"

  def clang_flags(target, macro, includes)
    ["--target=#{target}", '-ffreestanding', '-std=c11', '-xc', "-D#{macro}=__attribute__((annotate(\"api\")))",
     *includes.map { |dir| "-I#{dir}" }]
  end

  def run(*arguments, input: nil)
    output, errors, status = Toolchain::Tool[:clang].capture(*arguments.map(&:to_s), stdin: input)
    raise Error, "clang #{arguments.join(' ')} failed:\n#{errors}" unless status.success?

    output
  end

  def json(flags, header) = JSON.parse(run(*flags, '-Xclang', '-ast-dump=json', '-fsyntax-only', header))

  # The functions the amalgamation `source` defines and exports, compiled by
  # `target` with the API at default visibility and everything else hidden.
  def exported(source, target, macro, includes, flags = [])
    Dir.mktmpdir('c-api') do |dir|
      object = target.compile(source.to_s, into: File.join(dir, 'library.o'), includes: [*includes, File.dirname(source)],
                                           defines: ["#{macro}=__attribute__((visibility(\"default\")))"],
                                           flags: [*%w[-std=c11 -w -fvisibility=hidden], *flags])
      object.exports.to_a
    end
  end

  # The header's AST, read for one target: its prefixed declarations.
  class AST
    def initialize(root, prefix, exclude)
      @nodes = {}
      @decls = []
      lower = prefix.downcase
      walk(root) { |node| @nodes[node['id']] = node }
      root.fetch('inner', []).each do |node|
        name = node['name'].to_s
        next unless name.downcase.start_with?(lower) && !exclude.include?(name)

        @decls << node
      end
    end

    def functions
      @functions ||= @decls.select { |node| node['kind'] == 'FunctionDecl' && api?(node) }.uniq { |node| node['name'] }
                           .map do |node|
        params = node.fetch('inner', []).select { |inner| inner['kind'] == 'ParmVarDecl' }
                     .map { |param| { 'name' => param['name'].to_s, 'type' => param.dig('type', 'qualType') } }
        { 'name' => node['name'], 'returns' => node.dig('type', 'qualType').sub(/\s*\(.*\z/m, ''),
          'params' => params, 'public' => true, 'static_inline' => false }
      end
    end

    def function?(name) = functions.any? { |function| function['name'] == name }

    def api?(node) = node.fetch('inner', []).any? { |inner| inner['kind'] == 'AnnotateAttr' }

    # Typedefs by name: a struct's or union's fields (none when opaque), an
    # enum's constants, or an alias's type.
    def types
      @decls.select { |node| node['kind'] == 'TypedefDecl' }.to_h do |node|
        record = record_of(node)
        entry = if record && record['kind'] == 'RecordDecl'
                  fields = record.fetch('inner', []).select { |inner| inner['kind'] == 'FieldDecl' }
                                 .map { |field| { 'name' => field['name'].to_s, 'type' => field.dig('type', 'qualType') } }
                  { 'kind' => record['completeDefinition'] ? record['tagUsed'] : 'opaque', 'fields' => fields }
                elsif record && record['kind'] == 'EnumDecl'
                  { 'kind' => 'enum', 'constants' => record.fetch('inner', []).map { |constant| constant['name'] } }
                else
                  { 'kind' => 'alias', 'type' => node.dig('type', 'qualType') }
                end
        [node['name'], entry]
      end
    end

    def all_enum_constants(prefix)
      @nodes.values.select { |node| node['kind'] == 'EnumConstantDecl' && node['name'].to_s.start_with?(prefix) }
            .map { |node| node['name'] }.uniq
    end

    private

    def record_of(node)
      walk(node) do |inner|
        decl = inner['decl'] || inner['ownedTagDecl']
        return @nodes[decl['id']] || decl if decl && %w[RecordDecl EnumDecl].include?(decl['kind'])
      end
      nil
    end

    def walk(node, &block)
      yield node
      node.fetch('inner', []).each { |inner| walk(inner, &block) }
    end
  end

  # Enum constants and integer macros with the prefix, and their values
  # (on the native target).
  def constants(header, ast, flags, prefix, exclude)
    upper = prefix.upcase
    names = ast.all_enum_constants(upper)
    macros = run(*flags.call(NATIVE), '-dM', '-E', header).lines.filter_map do |line|
      match = line.match(/\A#define (#{upper}\w+) (.+)\n?\z/) or next
      match[1] if match[2].match?(/\A[\w\s()|&<>+\-*~^]+\z/) && !match[2].include?('__attribute__')
    end
    candidates = (names + macros).uniq - exclude
    values = evaluate(header, candidates, flags.call(NATIVE))
    values.map { |name, value| { 'name' => name, 'value' => value } }.sort_by { |constant| constant['name'] }
  end

  # The values of `expressions` that are integer constants, by compiling them
  # as enum initializers; the ones that are not are left out.
  def evaluate(header, expressions, flags)
    candidates = expressions.dup
    3.times do
      source = +"#include \"#{File.basename(header)}\"\n#include <stddef.h>\n"
      candidates.each_with_index { |expression, index| source << "enum { c_api_probe_#{index} = (#{expression}) };\n" }
      output, errors, status = Toolchain::Tool[:clang].capture(*flags, "-I#{File.dirname(header)}", '-Wno-everything',
                                                             '-Xclang', '-ast-dump=json', '-fsyntax-only', '-',
                                                             stdin: source)
      unless status.success?
        bad = errors.scan(/<stdin>:(\d+):/).flatten.map { |line| line.to_i - 3 }.uniq
        raise Error, "cannot evaluate the constants:\n#{errors}" if bad.empty?

        candidates = candidates.reject.with_index { |_, index| bad.include?(index) }
        next
      end
      values = {}
      AST.new(JSON.parse(output), '', []).instance_variable_get(:@nodes).each_value do |node|
        index = node['name'].to_s[/\Ac_api_probe_(\d+)\z/, 1] or next
        constant = find(node) { |child| child['kind'] == 'ConstantExpr' } or next
        values[candidates[index.to_i]] = Integer(constant['value'])
      end
      return values
    end
    raise Error, 'cannot evaluate the constants'
  end

  # Each struct's and union's size, alignment and field offsets on LAYOUTS.
  def layouts(header, types, flags)
    records = types.select { |_, entry| %w[struct union].include?(entry['kind']) }
    expressions = records.flat_map do |name, entry|
      ["sizeof(#{name})", "_Alignof(#{name})",
       *entry['fields'].reject { |field| field['name'].empty? }.map { |field| "offsetof(#{name}, #{field['name']})" }]
    end
    per_target = LAYOUTS.transform_values { |target| evaluate(header, expressions, flags.call(target)) }
    types.to_h do |name, entry|
      next [name, entry] unless records.key?(name)

      layout = per_target.to_h do |arch, values|
        offsets = entry['fields'].reject { |field| field['name'].empty? }
                                 .to_h { |field| [field['name'], values["offsetof(#{name}, #{field['name']})"]] }
        [arch, { 'size' => values["sizeof(#{name})"], 'align' => values["_Alignof(#{name})"], 'offsets' => offsets }]
      end
      [name, entry.merge('layout' => layout)]
    end
  end

  def find(node, &block)
    return node if yield(node)

    node.fetch('inner', []).each do |inner|
      found = find(inner, &block)
      return found if found
    end
    nil
  end
end
