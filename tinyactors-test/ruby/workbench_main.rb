# frozen_string_literal: true

# Entry point of the interactive workbench; see ruby/workbench/README.md.
require_relative 'workbench'

Workbench::App.main(ARGV)
