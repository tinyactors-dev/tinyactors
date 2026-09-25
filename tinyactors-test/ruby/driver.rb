# frozen_string_literal: true

require 'json'
require 'open3'

module TinyactorsTest
  class ProtocolError < StandardError; end

  # The host answered a request with a JSON-RPC error object.
  class RPCError < ProtocolError
    attr_reader :rpc_method, :code, :rpc_message, :data

    def initialize(rpc_method, error)
      @rpc_method = rpc_method
      @code = error[:code]
      @rpc_message = error[:message]
      @data = error[:data]
      super("#{rpc_method}: #{code} #{rpc_message}")
    end
  end

  # Dotted RPC method names as nested Ruby calls, built from rpc.discover.
  class Proxy
    def initialize(driver, discovery, prefix = [])
      @driver = driver
      @discovery = discovery
      @prefix = prefix
      @schemas = discovery.fetch(:schemas, {}).transform_keys(&:to_s)
      # Define every child segment eagerly: method_missing never sees names
      # that Object already has, such as `ta.actor.inspect`.
      children.each do |child|
        define_singleton_method(child) { |*args, **params, &block| method_missing(child.to_sym, *args, **params, &block) }
      end
    end

    def to_s = "#<#{self.class.name} #{@prefix.empty? ? '(root)' : @prefix.join('.')}>"
    alias inspect to_s

    def namespace?(name)
      path = (@prefix + [name.to_s]).join('.')
      @schemas.each_key.any? { |method| method == path || method.start_with?("#{path}.") }
    end

    def schema(name) = @schemas[(@prefix + [name.to_s]).join('.')]

    def respond_to_missing?(name, include_private = false)
      namespace?(name) || super
    end

    def method_missing(name, *args, **params, &block)
      path = (@prefix + [name.to_s]).join('.')
      return super unless namespace?(name)

      schema = @schemas[path]
      return Proxy.new(@driver, @discovery, @prefix + [name.to_s]) if schema.nil?
      raise ArgumentError, "#{path} takes keyword arguments only" unless args.empty?

      check(path, schema, params)
      @driver.call(path, params)
    end

    private

    def children
      depth = @prefix.size
      @schemas.each_key.filter_map do |method|
        segments = method.split('.')
        segments[depth] if segments.size > depth && segments.first(depth) == @prefix
      end.uniq
    end

    def check(path, schema, params)
      declared = schema[:params]
      return if declared.nil?

      unknown = params.keys.map(&:to_s) - declared.keys.map(&:to_s)
      raise ArgumentError, "#{path}: unknown argument(s) #{unknown.join(', ')}" unless unknown.empty?

      missing = declared.select { |_, spec| spec[:required] }.keys.map(&:to_s) - params.keys.map(&:to_s)
      raise ArgumentError, "#{path}: missing argument(s) #{missing.join(', ')}" unless missing.empty?
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
      raise RPCError.new(method, response[:error]) if response[:error]
      raise ProtocolError, "response ID mismatch: #{response.inspect}" unless response[:id] == @next_id

      response[:result]
    end

    def notify(method, params = nil)
      request = { jsonrpc: '2.0', method: method }
      request[:params] = params unless params.nil?
      write(request)
    end

    # A proxy over the methods the host advertises in rpc.discover, e.g.
    # `driver.api.ta.actor.spawn(system: handle, definition: 1)` or, through
    # method_missing, `driver.ta.actor.spawn(...)`. Argument names are checked
    # against the advertised schemas before anything is sent.
    def api
      @api ||= Proxy.new(self, call('rpc.discover'))
    end

    def respond_to_missing?(name, include_private = false)
      (@api && @api.namespace?(name)) || super
    end

    def method_missing(name, *args, **kwargs, &block)
      return api.public_send(name, *args, **kwargs, &block) if api.namespace?(name)

      super
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
