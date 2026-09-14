# frozen_string_literal: true

require "minitest/autorun"
ext_dir = File.expand_path("../ext/tinyactors_raw", __dir__)
$LOAD_PATH.unshift(ext_dir) unless $LOAD_PATH.include?(ext_dir)
require "tinyactors"
