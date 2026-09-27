# frozen_string_literal: true

require 'json'
require 'pathname'
require_relative 'api/c_type'
require_relative 'api/clang'
require_relative 'api/builder'
require_relative 'api/layout'
require_relative 'api/check'
require_relative 'api/schema'

# Describes the public API in include/txml.h as
# include/txml.api.json (docs/internal/api-ir.md). Every stage works in
# memory; the description is the only file it writes.
#
#   header --clang--> AST --Builder--> model --Layout--> + layouts, constants
#          --Schema--> validated --Check--> compiled against the header
module API
  FORMAT = 'txml-api/1'

  module_function

  # The description of `header`; `library` is the amalgamation whose exported
  # symbols decide which functions are implemented (none without one).
  # The platforms whose functions the header declares only for them
  # (docs/adr/0001-packaging-and-releases.md, decision 9): the clang target
  # the header is read for, and the preprocessor condition it uses.
  PLATFORMS = { 'wasm' => { target: 'wasm32-unknown-unknown', guard: 'defined(__wasm__)' } }.freeze

  def platform_guard(platform) = PLATFORMS.fetch(platform).fetch(:guard)

  def describe(header, library:, schema:)
    header = Pathname.new(header)
    ast = Clang.ast(header)
    builder = Builder.new(ast, implemented: library ? Clang.exported_functions(library) : [])
    model = builder.build
    model['functions'] += platform_functions(header, library, model['functions'].map { |function| function['name'] })
    macros = Clang.macros(header)
    constants = Layout.constants(header, macros, known: builder.method(:known?))
    description = { 'format' => FORMAT, 'header' => "include/#{header.basename}" }
    version = version(macros)
    description['version'] = version if version
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
  def platform_functions(header, library, portable)
    PLATFORMS.flat_map do |platform, options|
      source = library && Pathname.new(library).sub_ext('').then { |base| Pathname.new("#{base}_#{platform}.c") }
      implemented = source&.file? ? Clang.exported_functions(source, target: options[:target]) : []
      Builder.new(Clang.ast(header, target: options[:target]), implemented: implemented).build['functions']
             .reject { |function| portable.include?(function['name']) }
             .map { |function| function.merge('platform' => platform) }
    end
  end

  # The header's version, "major.minor.patch", from its TXML_VERSION_*
  # macros; nil when it defines none (the extractor's test headers).
  def version(macros)
    parts = %w[MAJOR MINOR PATCH].map { |part| macros["TXML_VERSION_#{part}"] }
    return nil if parts.all?(&:nil?)
    raise Error, "TXML_VERSION_MAJOR, _MINOR and _PATCH must all be whole numbers" unless parts.all? { _1&.match?(/\A\d+\z/) }

    parts.join('.')
  end

  def render(description) = "#{JSON.pretty_generate(description)}\n"
end
