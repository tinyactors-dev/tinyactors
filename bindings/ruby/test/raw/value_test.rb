# frozen_string_literal: true

require_relative "../test_helper"

class RawValueTest < Minitest::Test
  def test_preserves_object_identity
    object = {name: "value"}
    value = TinyActors::Raw::Value.new(object)

    assert_same object, value.object
  end

  def test_roots_nil_false_zero_and_empty_values
    [nil, false, 0, ""].each do |object|
      assert_same object, TinyActors::Raw::Value.new(object).object
    end
  end

  def test_root_survives_gc_and_compaction
    object = ["λ", {payload: "kept"}]
    value = TinyActors::Raw::Value.new(object)

    GC.start
    GC.compact if GC.respond_to?(:compact)

    assert_same object, value.object
  end

  def test_wrapper_is_immutable
    value = TinyActors::Raw::Value.new(Object.new)

    assert_predicate value, :frozen?
    assert_raises(FrozenError) { value.instance_variable_set(:@object, nil) }
  end
end
