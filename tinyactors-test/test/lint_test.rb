# frozen_string_literal: true

require 'minitest/autorun'
require 'open3'
require 'yaml'

# The adapters' lint allows the Python standard library by a list
# (lint/utils/python-standard-library.yml): it must be the pinned
# interpreter's, so an adapter can import every module of it and nothing
# else.
class LintTest < Minitest::Test
  UTIL = File.expand_path('../lint/utils/python-standard-library.yml', __dir__)

  def test_the_standard_library_list_is_the_pinned_interpreters
    regex = YAML.load_file(UTIL).fetch('rule').fetch('regex')
    listed = regex[/\A\^\((.*)\)\(\\\.\|\$\)\z/m, 1].to_s.split('|')
    script = 'import sys; print("\n".join(sorted(m for m in sys.stdlib_module_names if not m.startswith("_"))))'
    out, status = Open3.capture2('python', '-I', '-c', script, chdir: File.expand_path('..', __dir__))
    assert status.success?, 'python (tinyactors-test/mise.toml) did not run'
    assert_equal ['__future__', *out.split("\n")], listed,
                 "#{UTIL}: not the pinned interpreter's sys.stdlib_module_names; write it again (its header says how)"
  end
end
