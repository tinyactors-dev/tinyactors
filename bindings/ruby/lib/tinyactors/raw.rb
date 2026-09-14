# frozen_string_literal: true

module TinyActors
  module Raw
  end
end

require_relative "raw/result"
require_relative "raw/value"

begin
  require "tinyactors/tinyactors_raw"
rescue LoadError => error
  # Development builds place the bundle beside extconf.rb.  Installed gems
  # use the namespaced load path above.
  raise unless error.path == "tinyactors/tinyactors_raw"

  require File.expand_path("../../ext/tinyactors_raw/tinyactors_raw", __dir__)
end
