# frozen_string_literal: true

# A refused post leaves nothing behind, and consecutive posts each answer
# their own reply.
require_relative '../../../tinyactors-test/lib/driver'

def bytes(value) = CBOR::Bytes.new(CBOR.encode(value))
def check(condition, message) = (raise message unless condition)

driver = Driver.new([ARGV.fetch(0)])
begin
  created = driver.call('host.create', 'config' => bytes('systemID' => 'h'), 'datamodel' => 'quickjs')
  check(created['status'] == 0, 'create refused')
  refused = driver.call('host.post', 'message' => bytes('method' => 'host.config', 'id' => 90), 'refuse' => true)
  check(refused['status'] == -4 && !refused.key?('reply'), 'refused post returned a reply')
  2.times do |index|
    posted = driver.call('host.post', 'message' => bytes('method' => 'host.config', 'id' => index))
    check(posted['status'] == 0 && posted.key?('reply'), 'post did not return its reply')
    reply = CBOR.value(CBOR.decode(posted.fetch('reply').data))
    check(reply['id'] == index, 'post returned another reply')
  end
  check(driver.call('host.run').fetch('sends').empty?, 'a run after the posts sent something')
  driver.call('host.free')
  puts 'ok   conform batch: refused post, consecutive posts'
ensure
  driver.close
end
