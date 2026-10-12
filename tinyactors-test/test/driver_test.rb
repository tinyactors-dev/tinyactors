# frozen_string_literal: true

require 'minitest/autorun'
require 'driver'

# Framing and process handling (docs/driver.md, "Frames and messages"),
# against test/fake_implementation.rb.
class DriverTest < Minitest::Test
  FAKE = ['ruby', File.expand_path('fake_implementation.rb', __dir__)].freeze

  def setup
    @driver = Driver.new(FAKE, timeout: 10)
  end

  def teardown
    @driver.close
  end

  def test_calls_and_replies_in_order
    3.times do |n|
      assert_equal({ 'n' => n }, @driver.call('echo.frame', { 'n' => n }))
    end
  end

  def test_a_frame_longer_than_64_kibibytes
    data = CBOR::Bytes.new(Random.new(1).bytes(100_000))
    assert_equal data.data, @driver.call('echo.frame', { 'data' => data })['data'].data
  end

  def test_the_result_as_an_item_keeps_its_bytes
    item = @driver.call_item('echo.frame', { 'a' => [1, 2] })
    assert_equal CBOR.from_diagnostic('{"a":[1,2]}'), item.bytes
  end

  def test_an_error_reply_is_raised_with_its_code
    error = assert_raises(Driver::RPCError) { @driver.call('no.such.method') }
    assert_equal(-32_601, error.code)
    assert_equal 'unknown', error.reason
  end

  def test_a_command_that_is_not_there
    assert_raises(Driver::Crashed) { Driver.new(['/nonexistent/implementation']) }
  end

  def test_a_command_that_ends_without_replying
    driver = Driver.new(['ruby', '-e', 'print "xy"'], timeout: 10)
    error = assert_raises(Driver::Crashed) { driver.call('echo.frame') }
    assert_match(/the command ended/, error.message)
  ensure
    driver&.close
  end

  def test_a_reply_that_is_not_cbor
    driver = Driver.new(['ruby', '-e', '$stdin.read(4); $stdout.write([1].pack("N") + "\xff"); $stdout.flush; sleep 1'], timeout: 10)
    error = assert_raises(Driver::Crashed) { driver.call('echo.frame') }
    assert_match(/not a CBOR reply/, error.message)
  ensure
    driver&.close
  end

  def test_restart_after_a_crash
    @driver.call('scenario.run', { 'documents' => [{ 'name' => 'a', 'text' => 'outcome: crash' }] })
  rescue Driver::Crashed
    @driver.restart
    assert_equal({ 'n' => 1 }, @driver.call('echo.frame', { 'n' => 1 }))
  else
    flunk 'the crash was not noticed'
  end
end
