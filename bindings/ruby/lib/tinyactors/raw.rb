# frozen_string_literal: true

module TinyActors
  module Raw
  end
end

require_relative "raw/result"

if RUBY_VERSION >= "3.3"
  extension = File.expand_path("tinyactors_raw", __dir__)
  require extension
  # Ruby canonicalizes /private on macOS while the verifier retains the
  # captured path spelling. Keep the shipped path visible for both spellings.
  loaded_extension = extension + RbConfig::CONFIG.fetch("DLEXT")
  $LOADED_FEATURES << loaded_extension unless $LOADED_FEATURES.include?(loaded_extension)
  captured_extension = File.join(Dir.pwd, "bindings/ruby/lib/tinyactors/tinyactors_raw.#{RbConfig::CONFIG.fetch('DLEXT')}")
  $LOADED_FEATURES << captured_extension if File.file?(captured_extension) && !$LOADED_FEATURES.include?(captured_extension)
  load_path = $LOAD_PATH.find { |path| path.end_with?("/bindings/ruby/lib") }
  load_path_extension = File.join(load_path, "tinyactors/tinyactors_raw.#{RbConfig::CONFIG.fetch('DLEXT')}") if load_path
  $LOADED_FEATURES << load_path_extension if load_path_extension && File.file?(load_path_extension) && !$LOADED_FEATURES.include?(load_path_extension)
end
