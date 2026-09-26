# frozen_string_literal: true

require 'json'
require 'open3'
require 'timeout'

# A DAP client over the adapter's stdin and stdout, for the adapter's
# self-test and the shared transcripts (dap_transcripts.rb). Every message
# is kept; `request` answers the response to its request, and `event` waits
# for the next event of a name that matches, among those not taken yet.
class DAPClient
  TIMEOUT = 15

  attr_reader :stderr_text

  def initialize(command)
    @stdin, @stdout, @stderr, @wait = Open3.popen3(*command)
    @stdin.binmode
    @stdout.binmode
    @seq = 0
    @events = []
    @responses = {}
    @stderr_text = +''
    @stderr_reader = Thread.new { @stderr.each_line { |line| @stderr_text << line } }
  end

  def request(command, arguments = nil)
    @seq += 1
    message = { 'seq' => @seq, 'type' => 'request', 'command' => command }
    message['arguments'] = arguments if arguments
    json = JSON.generate(message)
    warn "-> #{json}" if ENV['DAP_TRACE']
    @stdin.write("Content-Length: #{json.bytesize}\r\n\r\n#{json}")
    @stdin.flush
    seq = @seq
    read_until { @responses.key?(seq) }
    @responses.delete(seq)
  end

  # The first event not yet taken named `name` whose body the block
  # accepts; waits for it.
  def event(name, &accept)
    found = nil
    read_until do
      found = @events.find { |event| event['event'] == name && (accept.nil? || accept.call(event['body'] || {})) }
    end
    @events.delete(found)
    found['body'] || {}
  end

  # Events of a name already received, taken.
  def take(name)
    taken = @events.select { |event| event['event'] == name }
    @events.delete_if { |event| event['event'] == name }
    taken.map { |event| event['body'] || {} }
  end

  # The events received and not taken yet, oldest first, as messages. A
  # caller that takes events itself deletes them from this list.
  def pending = @events

  # Reads what the adapter sent so far without waiting for anything.
  def drain
    read_message while @stdout.wait_readable(0.2)
  end

  def outputs = take('output').map { |body| body['output'] }

  def close
    @stdin.close unless @stdin.closed?
    @wait.join(TIMEOUT) || Process.kill('KILL', @wait.pid)
    @stderr_reader.join(TIMEOUT)
    @wait.value
  end

  # Reads messages until the block answers true; raises after TIMEOUT.
  def read_until
    Timeout.timeout(TIMEOUT) do
      read_message until yield
    end
  rescue Timeout::Error
    raise "timed out; received events #{@events.map { |event| event['event'] }.inspect}; stderr: #{@stderr_text}"
  end

  private

  def read_message
    length = nil
    while (line = @stdout.gets("\r\n"))
      line = line.chomp("\r\n")
      break if line.empty?

      name, value = line.split(':', 2)
      length = Integer(value) if name == 'Content-Length'
    end
    raise "the adapter closed its output; stderr: #{@stderr_text}" if length.nil?

    message = JSON.parse(@stdout.read(length))
    warn "<- #{JSON.generate(message)}" if ENV['DAP_TRACE']
    case message['type']
    when 'response' then @responses[message['request_seq']] = message
    when 'event' then @events << message
    end
  end
end
