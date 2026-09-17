# frozen_string_literal: true

require 'json'
require 'open3'

module TinyactorsTest
  class ProtocolError < StandardError; end

  class Driver
    attr_reader :notifications

    def initialize(executable)
      @stdin, @stdout, @stderr, @wait_thread = Open3.popen3(executable.to_s)
      @next_id = 0
      @notifications = []
    end

    def call(method, params = nil)
      @next_id += 1
      request = { jsonrpc: '2.0', id: @next_id, method: method }
      request[:params] = params unless params.nil?
      @stdin.puts(JSON.generate(request))
      response = read_response
      raise ProtocolError, response[:error].inspect if response[:error]
      raise ProtocolError, "response ID mismatch: #{response.inspect}" unless response[:id] == @next_id

      response[:result]
    end

    def notify(method, params = nil)
      request = { jsonrpc: '2.0', method: method }
      request[:params] = params unless params.nil?
      @stdin.puts(JSON.generate(request))
    end

    def close
      call('host.shutdown') if @wait_thread.alive?
      @stdin.close unless @stdin.closed?
      status = @wait_thread.value
      diagnostics = @stderr.read
      @stdout.close
      @stderr.close
      raise ProtocolError, "host exited #{status.exitstatus}: #{diagnostics}" unless status.success?
    end

    private

    def read_response
      loop do
        message = JSON.parse(@stdout.readline, symbolize_names: true)
        if message[:jsonrpc] == '2.0' && message.key?(:method) && !message.key?(:id)
          @notifications << message
          next
        end
        return message
      end
    end
  end
end
