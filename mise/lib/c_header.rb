# frozen_string_literal: true

require 'json'
require 'pathname'
require 'toolchain'

# A C header, read with ast-grep's C parser rather than a regular
# expression: a declaration's name is found in its syntax tree, whatever
# its return type, pointer or line breaks.
class CHeader
  def initialize(path)
    @path = Pathname.new(path)
  end

  # The functions it declares with `macro` (TA_API, TXML_API): the name of
  # each function declarator of a declaration that carries the macro.
  def functions_declared_with(macro)
    rule = <<~YAML
      id: functions-declared-with
      language: c
      rule:
        kind: identifier
        inside:
          kind: function_declarator
          field: declarator
          inside:
            kind: declaration
            has:
              kind: type_identifier
              regex: ^#{macro}$
              stopBy: end
            stopBy: end
    YAML
    matches = Toolchain::Tool[:ast_grep].run('scan', '--inline-rules', rule, '--json=compact', @path)
    JSON.parse(matches).map { |match| match.fetch('text') }.uniq.sort
  end
end
