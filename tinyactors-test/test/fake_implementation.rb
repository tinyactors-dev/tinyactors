# frozen_string_literal: true

# A tiny implementation of docs/driver.md for the runner's own tests: it
# speaks the frames and answers from rules, with no host behind it.
#
# - host.create: status 0 for a config that is a map with "systemID", else
#   -3 with an error naming systemID; host.free: {}.
# - host.request: "echo" replies its params as the result, "wrong" replies
#   {"n": 2}, "big" replies 300 bytes (with capacity: TA_ERROR_NO_MEMORY
#   below that), anything else -32601 with a message to match by wildcard.
# - scenario.run: the outcome written in the first document as
#   "outcome: <word>"; "crash" exits with status 3, "hang" never answers.
# - echo.frame: its params back, for framing tests.
# - any other method: -32601.
require_relative '../lib/cbor'

$stdout.binmode
$stdin.binmode

def frame(item)
  bytes = CBOR.encode(item)
  $stdout.write([bytes.bytesize].pack('N') + bytes)
  $stdout.flush
end

def error(code, reason, message = 'no such method', path = '')
  { 'code' => code, 'message' => message, 'data' => { 'reason' => reason, 'path' => path } }
end

def reply_to(request, capacity)
  call = CBOR.load(request)
  id = call['id']
  reply = case call['method']
          when 'echo' then { 'id' => id, 'result' => call['params'] }
          when 'wrong' then { 'id' => id, 'result' => { 'n' => 2 } }
          when 'big' then { 'id' => id, 'result' => { 'text' => 'x' * 280 } }
          else { 'id' => id, 'error' => error(-32_601, 'unknown', "no method #{call['method']}") }
          end
  bytes = CBOR.encode(reply)
  return { 'status' => -4, 'length' => bytes.bytesize } if capacity && capacity < bytes.bytesize

  { 'status' => 0, 'length' => bytes.bytesize, 'reply' => CBOR::Bytes.new(bytes) }
end

def answer(method, params)
  case method
  when 'host.create'
    config = CBOR.load(params['config'].data)
    if config.is_a?(Hash) && config.key?('systemID')
      { 'status' => 0, 'datamodel' => 'ecmascript' }
    else
      { 'status' => -3,
        'error' => CBOR::Bytes.new(CBOR.encode({ 'error' => error(-32_602, 'missing', 'systemID is missing', 'systemID') })) }
    end
  when 'host.free' then {}
  when 'host.request' then reply_to(params['request'].data, params['capacity'])
  when 'echo.frame' then params
  when 'scenario.run'
    outcome = params['documents'].first['text'][/outcome: (\w+)/, 1]
    if outcome == 'crash'
      warn 'fake: crashing as asked'
      exit 3
    end
    sleep if outcome == 'hang'
    warn "fake: the outcome is #{outcome}" unless outcome == 'pass'
    { 'outcome' => outcome }
  end
end

while (head = $stdin.read(4))
  call = CBOR.load($stdin.read(head.unpack1('N')))
  result = answer(call['method'], call['params'] || {})
  frame(result ? { 'id' => call['id'], 'result' => result } : { 'id' => call['id'], 'error' => error(-32_601, 'unknown') })
end
