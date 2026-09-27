# frozen_string_literal: true

require 'json'
require 'pathname'
require_relative 'api/c_type'
require_relative 'api/clang'
require_relative 'api/builder'
require_relative 'api/layout'
require_relative 'api/check'
require_relative 'api/schema'

# Describes the public API in include/tinyactors.h as
# include/tinyactors.api.json (docs/internal/api-ir.md). Every stage works in
# memory; the description is the only file it writes.
#
#   header --clang--> AST --Builder--> model --Layout--> + layouts, constants
#          --Schema--> validated --Check--> compiled against the header
module API
  FORMAT = 'tinyactors-api/2'

  module_function

  # The platforms whose functions the header declares only for them (root
  # ADR 0001, docs/adr/0001-packaging-and-releases.md, decision 9): the
  # clang target the header is read for, and the preprocessor condition it
  # uses.
  PLATFORMS = { 'wasm' => { target: 'wasm32-unknown-unknown', guard: 'defined(__wasm__)' } }.freeze

  def platform_guard(platform) = PLATFORMS.fetch(platform).fetch(:guard)

  # The description of `header`; `library` is the amalgamation whose exported
  # symbols decide which functions are implemented (none without one).
  #
  # A library that builds on core (the SCXML codec) is described with the
  # same extractor: `prefix` is its macro prefix (its constants and
  # <prefix>_VERSION_*), `includes` the directories its header's #includes
  # need, and `imports` the API descriptions of the libraries it uses,
  # whose types its functions may name. The description lists only its
  # own types, and records each import's header and version.
  def describe(header, library:, schema:, prefix: 'TA', includes: [], imports: [])
    header = Pathname.new(header)
    Clang.includes = includes
    imported = imports.each_with_object({}) { |description, types| types.merge!(description.fetch('types')) }
    ast = Clang.ast(header)
    builder = Builder.new(ast, implemented: library ? Clang.exported_functions(library) : [], imports: imported)
    model = builder.build
    model['functions'] += platform_functions(header, library, model['functions'].map { |function| function['name'] },
                                             imported)
    macros = Clang.macros(header)
    constants = Layout.constants(header, macros, known: builder.method(:known?), prefix: prefix)
    description = { 'format' => FORMAT, 'header' => "include/#{header.basename}" }
    version = version(macros, prefix)
    description['version'] = version if version
    unless imports.empty?
      description['imports'] = imports.map { |each| { 'header' => each.fetch('header'), 'version' => each['version'] }.compact }
    end
    description.merge!(
      'targets' => nil,
      'constants' => constants,
      'flag_sets' => model['flag_sets'],
      'types' => model['types'],
      'functions' => model['functions']
    )
    description['targets'] = Layout.apply(header, description)
    description = JSON.parse(JSON.generate(description))
    errors = Schema.load(schema).errors(description)
    raise Error, "the description does not match #{File.basename(schema)}:\n#{errors.first(20).join("\n")}" unless errors.empty?

    Check.run(header, description)
    description
  end

  # The functions the header declares only for a platform, marked with it.
  # A platform function is implemented when the platform's amalgamation
  # (include/<name>_<platform>.c, beside `library`) defines it.
  def platform_functions(header, library, portable, imported = {})
    PLATFORMS.flat_map do |platform, options|
      source = library && Pathname.new(library).sub_ext('').then { |base| Pathname.new("#{base}_#{platform}.c") }
      implemented = source&.file? ? Clang.exported_functions(source, target: options[:target]) : []
      Builder.new(Clang.ast(header, target: options[:target]), implemented: implemented, imports: imported).build['functions']
             .reject { |function| portable.include?(function['name']) }
             .map { |function| function.merge('platform' => platform) }
    end
  end

  # The header's version, "major.minor.patch", from its <prefix>_VERSION_*
  # macros; nil when it defines none (the extractor's test headers).
  def version(macros, prefix = 'TA')
    parts = %w[MAJOR MINOR PATCH].map { |part| macros["#{prefix}_VERSION_#{part}"] }
    return nil if parts.all?(&:nil?)
    raise Error, "#{prefix}_VERSION_MAJOR, _MINOR and _PATCH must all be whole numbers" unless parts.all? { _1&.match?(/\A\d+\z/) }

    parts.join('.')
  end

  def render(description) = "#{JSON.pretty_generate(description)}\n"
end
