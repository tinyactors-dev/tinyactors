# frozen_string_literal: true

# The test binaries and their arguments, shared by the tasks that run them.
module Suite
  # name => arguments, in the order they run.
  BINARIES = {
    'header_test' => [],
    'arena_test' => [],
    'text_test' => [],
    'cursor_test' => [],
    'doc_test' => [],
    'incremental_test' => [],
    'writer_test' => [],
    'stream_test' => ['tests/spec'],
    'spec_test' => ['tests/spec']
  }.freeze

  # Tests that include private src/ headers, and so can only be linked against
  # the modules; every other test uses the public API alone and is also linked
  # against the amalgamation (mise run amalgamate-test).
  PRIVATE = %w[doc_test].freeze
  # Never linked: header_test needs no library, and its binary is built by build.
  UNLINKED = %w[header_test].freeze

  def self.public_api_tests = BINARIES.keys - PRIVATE - UNLINKED
end
