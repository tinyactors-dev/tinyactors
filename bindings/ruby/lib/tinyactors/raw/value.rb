# frozen_string_literal: true

module TinyActors
  module Raw
    # A rooted Ruby value used wherever the native runtime needs a
    # ta_value_t.  The extension can keep this wrapper alive without exposing
    # a Ruby object address or treating nil as an absent handle.
    class Value
      attr_reader :object

      def initialize(object)
        @object = object
        freeze
      end
    end
  end
end
