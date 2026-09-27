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

  # The description of `header`; `library` is the amalgamation whose exported
  # symbols decide which functions are implemented (none without one).
  def describe(header, library:, schema:)
    header = Pathname.new(header)
    ast = Clang.ast(header)
    builder = Builder.new(ast, implemented: library ? Clang.exported_functions(library) : [])
    model = builder.build
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

  # The header's version, "major.minor.patch", from its TA_VERSION_*
  # macros; nil when it defines none (the extractor's test headers).
  def version(macros)
    parts = %w[MAJOR MINOR PATCH].map { |part| macros["TA_VERSION_#{part}"] }
    return nil if parts.all?(&:nil?)
    raise Error, "TA_VERSION_MAJOR, _MINOR and _PATCH must all be whole numbers" unless parts.all? { _1&.match?(/\A\d+\z/) }

    parts.join('.')
  end

  def render(description) = "#{JSON.pretty_generate(description)}\n"
end
