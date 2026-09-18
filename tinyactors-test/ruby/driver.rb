# frozen_string_literal: true

require 'json'
require 'open3'

module TinyactorsTest
  class ProtocolError < StandardError; end

  # The host answered a request with a JSON-RPC error object.
  class RpcError < ProtocolError
    attr_reader :rpc_method, :code, :rpc_message, :data

    def initialize(rpc_method, error)
      @rpc_method = rpc_method
      @code = error[:code]
      @rpc_message = error[:message]
      @data = error[:data]
      super("#{rpc_method}: #{code} #{rpc_message}")
    end
  end

  class Driver
    DEFAULT_TIMEOUT = 10

    attr_reader :notifications

    def initialize(executable, timeout: DEFAULT_TIMEOUT)
      @stdin, @stdout, @stderr, @wait_thread = Open3.popen3(executable.to_s)
      @timeout = timeout
      @next_id = 0
      @notifications = []
      @diagnostics = +''
      # Drain stderr continuously so a chatty host never blocks on a full pipe.
      @stderr_reader = Thread.new { @stderr.each_line { |line| @diagnostics << line } }
    end

    def call(method, params = nil)
      @next_id += 1
      request = { jsonrpc: '2.0', id: @next_id, method: method }
      request[:params] = params unless params.nil?
      write(request)
      response = read_response(method)
      raise RpcError.new(method, response[:error]) if response[:error]
      raise ProtocolError, "response ID mismatch: #{response.inspect}" unless response[:id] == @next_id

      response[:result]
    end

    def notify(method, params = nil)
      request = { jsonrpc: '2.0', method: method }
      request[:params] = params unless params.nil?
      write(request)
    end

    def diagnostics
      @diagnostics.dup
    end

    def close
      call('host.shutdown') if @wait_thread.alive?
    ensure
      terminate
    end

    # Closes the host without a shutdown request; used after a failure, when
    # the host may no longer answer.
    def terminate
      @stdin.close unless @stdin.closed?
      status = @wait_thread.join(@timeout) ? @wait_thread.value : kill_host
      @stderr_reader.join(@timeout)
      @stdout.close unless @stdout.closed?
      @stderr.close unless @stderr.closed?
      raise ProtocolError, "host exited #{status.inspect}: #{@diagnostics}" unless status&.success?
    end

    private

    def write(request)
      @stdin.puts(JSON.generate(request))
      @stdin.flush
    rescue Errno::EPIPE, IOError
      raise ProtocolError, "host is gone: #{@diagnostics}"
    end

    def read_response(method)
      loop do
        unless @stdout.wait_readable(@timeout)
          raise ProtocolError, "#{method}: no response within #{@timeout}s"
        end

        line = @stdout.gets
        raise ProtocolError, "#{method}: host closed stdout: #{@diagnostics}" if line.nil?

        message = JSON.parse(line, symbolize_names: true)
        if message[:jsonrpc] == '2.0' && message.key?(:method) && !message.key?(:id)
          @notifications << message
          next
        end
        return message
      end
    end

    def kill_host
      Process.kill('KILL', @wait_thread.pid)
      @wait_thread.value
    rescue Errno::ESRCH
      @wait_thread.value
    end
  end
end
