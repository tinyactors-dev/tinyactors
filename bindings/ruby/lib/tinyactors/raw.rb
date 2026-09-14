# frozen_string_literal: true

module TinyActors
  module Raw
  end
end

require_relative "raw/result"
require_relative "raw/value"

if (RUBY_VERSION.split('.').map(&:to_i).first(2) <=> [3, 3]) >= 0
  require_relative "tinyactors_raw"
end
