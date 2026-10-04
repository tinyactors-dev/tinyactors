# frozen_string_literal: true

# Source conventions of the amalgamation task.
module CAPI
  # A top-level function declaration or definition: return type at column 0,
  # followed by the ta_ name and its parameter list.
  RETURN_TYPE = /(?!typedef\b|static\b|struct\b|return\b|TA_API\b|TA_PRIVATE\b)(?:const\s+)?[A-Za-z_]\w*(?:\s*\*+\s*|\s+)/
  PROTOTYPE = /^(#{RETURN_TYPE})(ta_\w+)\s*\([^;{]*\)\s*;/m
  DEFINITION = /^(#{RETURN_TYPE})(ta_\w+)(\s*\([^;{]*\)\s*\{)/m
  PUBLIC_PROTOTYPE = /^TA_API\b[^;{(]*?\b(ta_\w+)\s*\(/

  module_function

  # Modules in dependency order, as listed by src/internal.h, as paths
  # under src/ without the extension: `ring`, or `generated/ring_codec` for
  # a unit the protocol renderer writes.
  def modules(root)
    umbrella = root.join('src/internal.h')
    umbrella.read.scan(%r{^#include "((?:generated/)?\w+)\.h"$}).flatten
  end

  def public_functions(header_text)
    header_text.scan(PUBLIC_PROTOTYPE).flatten
  end
end
