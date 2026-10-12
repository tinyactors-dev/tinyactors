# frozen_string_literal: true

require 'open3'
require_relative 'cbor'

# One implementation, started as a command, spoken to as docs/driver.md
# says: length-prefixed CBOR frames on its standard input and output.
class Driver
  # The command ended, or wrote what is not a frame, while a call waited.
  class Crashed < StandardError; end

  # A call answered with an error (the protocol's, not core's).
  class RPCError < StandardError
    attr_reader :code, :reason, :path

    def initialize(error)
      data = error['data'].is_a?(Hash) ? error['data'] : {}
      @code = error['code']
      @reason = data['reason']
      @path = data['path']
      super("#{error['code']} #{@reason} #{@path}: #{error['message']}".squeeze(' '))
    end
  end

  attr_reader :command

  # `timeout`: seconds a reply may take before the command counts as hung;
  # `chdir`: the directory it runs in.
  def initialize(command, timeout: 60, chdir: Dir.pwd)
    @command = command
    @timeout = timeout
    @chdir = chdir
    @next_id = 0
    start
  end

  # The result of `method` with `params` (a Hash, or nil for none), as a
  # CBOR::Item, so a caller can compare it byte for byte.
  def call_item(method, params = nil)
    request = { 'method' => method }
    request['params'] = params if params
    request['id'] = (@next_id += 1)
    write(CBOR.encode(request))
    reply = CBOR.decode(read)
    raise Crashed, "reply #{CBOR.diagnostic(reply)} is not a map" unless reply.major == 5

    pairs =reply.children.each_slice(2).to_h { |key, value| [CBOR.value(key), value] }
    raise Crashed, "reply #{CBOR.diagnostic(reply)} is not to call #{@next_id}" unless CBOR.value(pairs['id']) == @next_id
    raise RPCError, CBOR.value(pairs['error']) if pairs.key?('error')

    pairs.fetch('result') { raise Crashed, "reply #{CBOR.diagnostic(reply)} has no result" }
  rescue CBOR::Error => e
    raise Crashed, "not a CBOR reply: #{e.message}"
  end

  # The same result as Ruby values (CBOR.value).
  def call(method, params = nil) = CBOR.value(call_item(method, params))

  # How many lines of standard error there are so far, and the lines after
  # such a count: what the command said during one case.
  def stderr_count = @stderr_lines.size

  def stderr_since(count, lines = 20)
    sleep(0.05) # the reader thread may not have the last lines yet
    @stderr_lines.drop(count).last(lines).join
  end

  def close
    @stdin.close unless @stdin.closed?
    @stdout.close unless @stdout.closed?
    @wait.join(2)
    @stderr_thread.join(1)
    Process.kill('KILL', @wait.pid) if @wait.alive?
  rescue Errno::ESRCH, IOError
    nil
  end

  # A new process, after a crash: the next case starts clean.
  def restart
    close
    start
  end

  private

  def start
    @stdin, @stdout, stderr, @wait = Open3.popen3(*@command, chdir: @chdir)
    @stdin.binmode
    @stdout.binmode
    @stderr_lines = []
    @stderr_thread = Thread.new { stderr.each_line { |line| @stderr_lines << line } }
  rescue SystemCallError => e
    raise Crashed, "cannot start #{@command.join(' ')}: #{e.message}"
  end

  def write(bytes)
    @stdin.write([bytes.bytesize].pack('N') + bytes)
    @stdin.flush
  rescue Errno::EPIPE, IOError
    raise Crashed, ended
  end

  def read
    length = read_exactly(4).unpack1('N')
    read_exactly(length)
  end

  def read_exactly(count)
    out = +''.b
    while out.bytesize < count
      raise Crashed, "no reply in #{@timeout} s" unless @stdout.wait_readable(@timeout)

      chunk = @stdout.read_nonblock(count - out.bytesize, exception: false)
      raise Crashed, ended if chunk.nil?

      out << chunk unless chunk == :wait_readable
    end
    out
  end

  def ended
    @wait.join(5)
    status = @wait.value if !@wait.alive?
    "the command ended#{status ? " (#{status.exitstatus ? "exit #{status.exitstatus}" : "signal #{status.termsig}"})" : ''}"
  end
end
