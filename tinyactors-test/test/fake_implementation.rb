# frozen_string_literal: true

# A tiny implementation of docs/driver.md for the runner's own tests: it
# speaks the frames and answers from rules, with no host behind it.
#
# - host.create: status 0 for a config that is a map with "systemID", else
#   -3 with an error naming systemID; its `datamodel` is "ecmascript" for
#   quickjs, "python" for native, and the name asked for otherwise;
#   host.free: {}.
# - host.request: "echo" replies its params as the result, "wrong" replies
#   {"n": 2}, "big" replies 300 bytes (with refuse: TA_ERROR_NO_MEMORY),
#   anything else -32601 with a message to match by wildcard.
# - scenario.run: the outcome written in the first document as
#   "outcome: <word>"; "crash" exits with status 3, "hang" never answers.
# - image.add, actor.spawn (session 1), peer.add (peer 1), clock.advance:
#   status 0 and nothing done; envelope.deliver of an envelope whose data is
#   one CBOR item: route 0, and the next host.run answers one envelope to
#   its origin with that data, as the echo setup does; with data that is
#   not: route 3, and host.run answers the error envelope "malformed".
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

def reply_to(request, refuse)
  call = CBOR.load(request)
  id = call['id']
  reply = case call['method']
          when 'echo' then { 'id' => id, 'result' => call['params'] }
          when 'wrong' then { 'id' => id, 'result' => { 'n' => 2 } }
          when 'big' then { 'id' => id, 'result' => { 'text' => 'x' * 280 } }
          else { 'id' => id, 'error' => error(-32_601, 'unknown', "no method #{call['method']}") }
          end
  bytes = CBOR.encode(reply)
  return { 'status' => -4 } if refuse

  { 'status' => 0, 'reply' => CBOR::Bytes.new(bytes) }
end

$sent = []

# The echo setup's chart, without a host: its data back, or the error
# envelope for data that is not one item.
def deliver(bytes)
  pairs = CBOR.decode(bytes).children.each_slice(2).to_h { |key, value| [CBOR.value(key), value] }
  origin = CBOR.value(pairs['origin'])
  data = CBOR.payload(pairs['data'].children.first)
  begin
    CBOR.decode(data)
  rescue CBOR::Error
    $sent << CBOR::Map.new([['error', 'malformed'], ['sendid', CBOR.value(pairs['sendid'])], ['origin', origin],
                            ['msgid', CBOR.value(pairs['msgid'])]])
    return { 'status' => 0, 'route' => 3 }
  end
  $sent << CBOR::Map.new([['target', origin], ['name', 'back'], ['data', CBOR::Tag.new(24, CBOR::Bytes.new(data))]])
  { 'status' => 0, 'route' => 0 }
end

def answer(method, params)
  case method
  when 'host.create'
    config = CBOR.load(params['config'].data)
    if config.is_a?(Hash) && config.key?('systemID')
      name = params['datamodel'] || 'quickjs'
      { 'status' => 0, 'datamodel' => { 'quickjs' => 'ecmascript', 'native' => 'python' }.fetch(name, name) }
    else
      { 'status' => -3,
        'error' => CBOR::Bytes.new(CBOR.encode({ 'error' => error(-32_602, 'missing', 'systemID is missing', 'systemID') })) }
    end
  when 'host.free' then {}
  when 'host.request' then reply_to(params['request'].data, params['refuse'])
  when 'echo.frame' then params
  when 'image.add', 'clock.advance' then { 'status' => 0 }
  when 'actor.spawn' then { 'status' => 0, 'session' => 1 }
  when 'peer.add' then { 'status' => 0, 'peer' => 1 }
  when 'envelope.deliver' then deliver(params['cbor'].data)
  when 'host.run'
    envelopes = $sent.map { |envelope| CBOR::Bytes.new(CBOR.encode(envelope)) }
    $sent = []
    { 'status' => 0, 'envelopes' => envelopes }
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
