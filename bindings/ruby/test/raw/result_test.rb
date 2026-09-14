# frozen_string_literal: true

require_relative "../test_helper"

class RawResultTest < Minitest::Test
  def test_preserves_status_and_output_identity
    value = {diagnostic: "kept"}
    result = TinyActors::Raw::Result.new(status: TinyActors::Raw::TA_BUSY, value: value)

    assert_equal TinyActors::Raw::TA_BUSY, result.status
    assert_same value, result.value
    refute result.ok?
  end

  def test_only_ta_ok_is_success
    assert TinyActors::Raw::Result.new(status: TinyActors::Raw::TA_OK).ok?
    refute TinyActors::Raw::Result.new(status: TinyActors::Raw::TA_EMPTY).ok?
    refute TinyActors::Raw::Result.new(status: TinyActors::Raw::TA_AGAIN).ok?
    refute TinyActors::Raw::Result.new(status: TinyActors::Raw::TA_STALE).ok?
  end

  def test_result_is_immutable_but_value_is_not_copied
    result = TinyActors::Raw::Result.new(status: TinyActors::Raw::TA_OK, value: {})

    assert_predicate result, :frozen?
    assert_raises(FrozenError) { result.instance_variable_set(:@status, 99) }
    result.value[:diagnostic] = "available"
    assert_equal "available", result.value[:diagnostic]
  end
end
