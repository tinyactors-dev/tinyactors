# frozen_string_literal: true

# Driver-owned envelopes must survive the end of a host.post response.
require_relative '../../../tinyactors-test/lib/driver'

def bytes(value) = CBOR::Bytes.new(CBOR.encode(value))
def check(condition, message) = (raise message unless condition)

driver = Driver.new([ARGV.fetch(0)])
begin
  created = driver.call('host.create', 'config' => bytes('systemID' => 'h'), 'datamodel' => 'quickjs')
  check(created['status'] == 0, 'create refused')
  peer = driver.call('peer.add', 'id' => 'p', 'trusted' => true).fetch('peer')
  2.times do |index|
    delivered = driver.call('envelope.deliver', 'peer' => peer,
                           'cbor' => bytes('target' => 'tinyactors://h/@missing', 'name' => 'ping',
                                           'origin' => 'tinyactors://p/#_scxml_9', 'msgid' => "m#{index}"))
    check(delivered['status'] == 0, 'deliver refused')
    if index.zero?
      refused = driver.call('host.post', 'message' => bytes('method' => 'host.config', 'id' => 90), 'refuse' => true)
      check(refused['status'] == -4 && !refused.key?('reply'), 'refused post returned a reply')
    end
    posted = driver.call('host.post', 'message' => bytes('method' => 'host.config', 'id' => index))
    check(posted['status'] == 0 && posted.key?('reply'), 'post did not return its reply')
    reply = CBOR.value(CBOR.decode(posted.fetch('reply').data))
    check(reply['id'] == index, 'post returned another reply')
  end
  run = driver.call('host.run')
  envelopes = run.fetch('envelopes').map { |item| CBOR.value(CBOR.decode(item.data)) }
  check(envelopes.map { |item| item['msgid'] } == %w[m0 m1], 'posts lost or reordered peer errors')
  check(envelopes.all? { |item| item['error'] == 'unknown-target' }, 'wrong peer error')
  check(driver.call('host.run').fetch('envelopes').empty?, 'peer errors returned twice')
  driver.call('host.free')
  puts 'ok   conform batch: refused post, consecutive mixed posts, retained envelopes'
ensure
  driver.close
end
