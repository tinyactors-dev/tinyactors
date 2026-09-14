# frozen_string_literal: true

module TinyActors
  module Raw
    class Result
      attr_reader :status, :value

      def initialize(status:, value: nil)
        @status = Integer(status)
        @value = value
        freeze
      end

      def ok?
        status == Raw::TA_OK
      end
    end
  end
end
