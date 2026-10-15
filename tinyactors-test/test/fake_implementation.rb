# frozen_string_literal: true

# A tiny implementation of docs/driver.md for the runner's own tests: it
# speaks the frames and answers from rules, with no host behind it.
#
# - host.create: status 0 for a config that is a map with "systemID", else
#   -3 with an error naming systemID; its `datamodel` is "ecmascript" for
#   quickjs, "python" for native, and the name asked for otherwise;
#   host.free: {}.
# - host.post: "echo" replies its params as the result, "wrong" replies
#   {"n": 2}, "big" replies 300 bytes (with refuse: TA_ERROR_NO_MEMORY),
#   "subscribe" replies {} and subscribes its params' session, anything else
#   -32601 with a message to match by wildcard; a message without "id" gets
#   no reply.
# - actor.send: status 0; the next host.run answers one notification,
#   actor.changed with value "t", for each subscribed session sent an event.
#   With `cbor` and `origin` it is the echo setup's chart: data that is one
#   CBOR item is sent back to the origin as "back" (the next host.run's
#   `sends`), and data that is not is refused with -3.
# - scenario.run: the outcome written in the first document as
#   "outcome: <word>"; "crash" exits with status 3, "hang" never answers.
# - image.add, actor.spawn (session 1), clock.advance: status 0 and
#   nothing done.
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

$subscribed = []
$changed = []

def reply_to(request, refuse)
  call = CBOR.load(request)
  id = call['id']
  return { 'status' => -4 } if refuse

  reply = case call['method']
          when 'echo' then { 'id' => id, 'result' => call['params'] }
          when 'subscribe'
            $subscribed << call['params']['session']
            { 'id' => id, 'result' => {} }
          when 'wrong' then { 'id' => id, 'result' => { 'n' => 2 } }
          when 'big' then { 'id' => id, 'result' => { 'text' => 'x' * 280 } }
          else { 'id' => id, 'error' => error(-32_601, 'unknown', "no method #{call['method']}") }
          end
  return { 'status' => 0, 'notifications' => [] } unless call.key?('id')

  { 'status' => 0, 'reply' => CBOR::Bytes.new(CBOR.encode(reply)), 'notifications' => [] }
end

def notifications
  changed = $changed.uniq.map do |session|
    CBOR::Bytes.new(CBOR.encode({ 'method' => 'actor.changed', 'params' => { 'session' => session, 'value' => 't' } }))
  end
  $changed = []
  changed
end

$sent = []

# The echo setup's chart, without a host: its data back to the origin, or
# a refusal for data that is not one item.
def echo(params)
  data = params['cbor'].data
  begin
    CBOR.decode(data)
  rescue CBOR::Error
    return { 'status' => -3 }
  end
  $sent << { 'target' => params['origin'], 'event' => 'back', 'data' => CBOR::Bytes.new(data) }
  { 'status' => 0 }
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
  when 'host.post' then reply_to(params['message'].data, params['refuse'])
  when 'actor.send'
    return echo(params) if params.key?('cbor')

    $changed << params['session'] if $subscribed.include?(params['session'])
    { 'status' => 0 }
  when 'echo.frame' then params
  when 'image.add', 'clock.advance' then { 'status' => 0 }
  when 'actor.spawn' then { 'status' => 0, 'session' => 1 }
  when 'host.run'
    sends = $sent
    $sent = []
    { 'status' => 0, 'sends' => sends, 'notifications' => notifications }
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
