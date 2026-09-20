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
module Api
  FORMAT = 'txml-api/1'

  module_function

  # The description of `header`; `library` is the amalgamation whose exported
  # symbols decide which functions are implemented (none without one).
  def describe(header, library:, schema:)
    header = Pathname.new(header)
    ast = Clang.ast(header)
    builder = Builder.new(ast, implemented: library ? Clang.exported_functions(library) : [])
    model = builder.build
    constants = Layout.constants(header, Clang.macros(header), known: builder.method(:known?))
    description = {
      'format' => FORMAT,
      'header' => "include/#{header.basename}",
      'targets' => nil,
      'constants' => constants,
      'flag_sets' => model['flag_sets'],
      'types' => model['types'],
      'functions' => model['functions']
    }
    description['targets'] = Layout.apply(header, description)
    description = JSON.parse(JSON.generate(description))
    errors = Schema.load(schema).errors(description)
    raise Error, "the description does not match #{File.basename(schema)}:\n#{errors.first(20).join("\n")}" unless errors.empty?

    Check.run(header, description)
    description
  end

  def render(description) = "#{JSON.pretty_generate(description)}\n"
end
