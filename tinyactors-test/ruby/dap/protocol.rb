# frozen_string_literal: true

require 'json'

module DAP
  class ProtocolError < StandardError; end

  # A request the adapter refuses. The message is what the client shows.
  class RequestError < StandardError; end

  # The base protocol of the Debug Adapter Protocol: each message is a JSON
  # object after a `Content-Length` header and a blank line. Messages are
  # read on a thread of their own (Server), and written only by the thread
  # that runs the adapter, so the output needs no lock beyond the one here,
  # which keeps a message whole if that ever changes.
  class Protocol
    def initialize(input, output)
      @input = input
      @output = output
      @seq = 0
      @lock = Mutex.new
    end

    # The next message as a Hash with string keys, or nil at end of input.
    def read
      length = nil
      loop do
        line = @input.gets("\r\n")
        return nil if line.nil?

        line = line.chomp("\r\n")
        break if line.empty?

        name, value = line.split(':', 2)
        length = Integer(value.strip) if name.strip.casecmp?('Content-Length')
      end
      raise ProtocolError, 'a message has no Content-Length header' if length.nil?

      body = @input.read(length)
      raise ProtocolError, 'the input ended inside a message' if body.nil? || body.bytesize < length

      JSON.parse(body.force_encoding(Encoding::UTF_8))
    end

    def respond(request, body = nil)
      write('type' => 'response', 'request_seq' => request['seq'], 'success' => true,
            'command' => request['command'], 'body' => body || {})
    end

    # A failed request. `message` is short; the error body repeats it so a
    # client that shows `body.error.format` shows it too.
    def refuse(request, message)
      write('type' => 'response', 'request_seq' => request['seq'], 'success' => false,
            'command' => request['command'], 'message' => message,
            'body' => { 'error' => { 'id' => 1, 'format' => message, 'showUser' => true } })
    end

    def event(name, body = nil)
      message = { 'type' => 'event', 'event' => name }
      message['body'] = body unless body.nil?
      write(message)
    end

    private

    def write(message)
      @lock.synchronize do
        @seq += 1
        json = JSON.generate(message.merge('seq' => @seq))
        @output.write("Content-Length: #{json.bytesize}\r\n\r\n#{json}")
        @output.flush
      end
    end
  end
end
