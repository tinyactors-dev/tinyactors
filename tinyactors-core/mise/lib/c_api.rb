# frozen_string_literal: true

# Shared source conventions for the amalgamation and API extraction tasks.
module CApi
  # A top-level function declaration or definition: return type at column 0,
  # followed by the ta_ name and its parameter list.
  RETURN_TYPE = /(?!typedef\b|static\b|struct\b|return\b|TA_API\b|TA_PRIVATE\b)(?:const\s+)?[A-Za-z_]\w*(?:\s*\*+\s*|\s+)/
  PROTOTYPE = /^(#{RETURN_TYPE})(ta_\w+)\s*\([^;{]*\)\s*;/m
  DEFINITION = /^(#{RETURN_TYPE})(ta_\w+)(\s*\([^;{]*\)\s*\{)/m
  PUBLIC_PROTOTYPE = /^TA_API\b[^;{(]*?\b(ta_\w+)\s*\(/

  module_function

  # Modules in dependency order, as listed by src/internal.h.
  def modules(root)
    umbrella = root.join('src/internal.h')
    umbrella.read.scan(/^#include "(\w+)\.h"$/).flatten
  end

  def public_functions(header_text)
    header_text.scan(PUBLIC_PROTOTYPE).flatten
  end

  # Names of every non-static function defined in the module sources.
  def defined_functions(root)
    modules(root).flat_map do |name|
      path = root.join('src', "#{name}.c")
      path.file? ? path.read.scan(DEFINITION).map { |match| match[1] } : []
    end
  end
end
