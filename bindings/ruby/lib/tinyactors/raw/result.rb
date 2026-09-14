# frozen_string_literal: true

module TinyActors
  module Raw
    class Result
      attr_reader :status, :value

      def initialize(status:, value: nil)
        raise TypeError, "status must be an Integer" unless status.is_a?(Integer)
        @status = status
        @value = value
        freeze
      end

      def ok?
        status == Raw::TA_OK
      end
    end
  end
end
