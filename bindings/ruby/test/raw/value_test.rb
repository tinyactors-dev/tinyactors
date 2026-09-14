# frozen_string_literal: true

require_relative "../test_helper"

class RawValueTest < Minitest::Test
  class IdentitySnapshotValue
    class << self
      attr_accessor :returned

      def _load(_bytes)
        returned
      end
    end

    def _dump(_depth)
      "identity-snapshot"
    end
  end

  def value_fixture(method, *arguments)
    TinyActors::Raw::Value.__send__(method, *arguments)
  end

  def teardown
    value_fixture(:__test_release_admission)
  end

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

  def test_snapshot_isolated_from_mutable_source
    source = {nested: ["original"]}
    snapshot = value_fixture(:__test_snapshot, TinyActors::Raw::Value.new(source))

    assert_equal TinyActors::Raw::TA_OK, snapshot.fetch(:status)
    copy = snapshot.fetch(:value).object
    refute_same source, copy
    refute_same source.fetch(:nested), copy.fetch(:nested)
    source.fetch(:nested)[0] = "changed"
    assert_equal ["original"], copy.fetch(:nested)
  end

  def test_native_nil_is_distinct_from_an_absent_handle
    result = value_fixture(:__test_snapshot, TinyActors::Raw::Value.new(nil))

    assert_equal TinyActors::Raw::TA_OK, result.fetch(:status)
    assert_nil result.fetch(:value).object
    assert_raises(TypeError) { value_fixture(:__test_snapshot, nil) }
    assert_equal TinyActors::Raw::TA_INVALID,
                 value_fixture(:__test_foreign_snapshot).fetch(:status)
  end

  def test_uncopyable_snapshot_returns_failure_without_output
    result = value_fixture(:__test_snapshot, TinyActors::Raw::Value.new(proc {}))

    assert_equal TinyActors::Raw::TA_EXECUTION_ERROR, result.fetch(:status)
    assert_nil result.fetch(:value)
  end

  def test_snapshot_rejects_a_loader_that_returns_mutable_source
    source = IdentitySnapshotValue.new
    IdentitySnapshotValue.returned = source

    result = value_fixture(:__test_snapshot, TinyActors::Raw::Value.new(source))

    assert_equal TinyActors::Raw::TA_EXECUTION_ERROR, result.fetch(:status)
    assert_nil result.fetch(:value)
    assert_nil $!, "a handled snapshot exception must not escape through Ruby's error state"
  ensure
    IdentitySnapshotValue.returned = nil
  end

  def test_public_runtime_retains_value_after_wrapper_is_dropped
    require "weakref"
    payload = {kept: ["through core"]}
    weak = WeakRef.new(payload)
    value = TinyActors::Raw::Value.new(payload)
    before = value_fixture(:__test_counts, value)

    assert_equal TinyActors::Raw::TA_OK, value_fixture(:__test_admit, value)
    after_admit = value_fixture(:__test_counts, value)
    assert_equal before.fetch(:retain) + 1, after_admit.fetch(:retain)
    assert_equal 1, after_admit.fetch(:core_refs)
    value = nil
    payload = nil
    GC.start
    GC.compact if GC.respond_to?(:compact)
    assert weak.weakref_alive?, "the public C runtime did not retain the value"

    assert_equal TinyActors::Raw::TA_OK, value_fixture(:__test_release_admission)
    GC.start
    GC.compact if GC.respond_to?(:compact)
    refute weak.weakref_alive?, "the retained root was not released after teardown"
  end

  def test_failed_public_admission_releases_acquired_definition_root_once
    value = TinyActors::Raw::Value.new({valid: true})
    invalid = TinyActors::Raw::Value.new(proc {})
    before = value_fixture(:__test_counts, value)

    refute_equal TinyActors::Raw::TA_OK, value_fixture(:__test_failed_admit, value, invalid)
    after = value_fixture(:__test_counts, value)
    assert_equal before.fetch(:retain) + 1, after.fetch(:retain)
    assert_equal before.fetch(:release) + 1, after.fetch(:release)
    assert_equal 0, after.fetch(:core_refs)
  end

  def test_public_runtime_owns_native_payloads_through_queue_current_replacement_and_teardown
    require "weakref"
    payload = {nested: ["queued"]}
    weak = WeakRef.new(payload)
    value = TinyActors::Raw::Value.new(payload)

    assert_equal TinyActors::Raw::TA_OK, value_fixture(:__test_queue_start, value)
    value = nil
    payload = nil
    GC.start
    GC.compact if GC.respond_to?(:compact)
    queued = value_fixture(:__test_queue_counts)
    assert_equal 1, queued.fetch(:snapshot)
    assert_operator queued.fetch(:live_core_refs), :>=, 2
    refute weak.weakref_alive?, "the queued snapshot retained the mutable source"

    assert_equal TinyActors::Raw::TA_OK, value_fixture(:__test_queue_step)
    current = value_fixture(:__test_queue_counts)
    assert_operator current.fetch(:seen_events), :>=, 1
    assert_operator current.fetch(:live_core_refs), :>=, 2
    GC.start
    GC.compact if GC.respond_to?(:compact)
    assert_operator value_fixture(:__test_queue_counts).fetch(:live_core_refs), :>=, 2

    before_replace = value_fixture(:__test_queue_counts)
    assert_equal TinyActors::Raw::TA_OK, value_fixture(:__test_queue_replace)
    replaced = value_fixture(:__test_queue_counts)
    assert_equal 2, replaced.fetch(:snapshot)
    assert_equal before_replace.fetch(:release) + 1, replaced.fetch(:release)
    assert_operator replaced.fetch(:live_core_refs), :>=, 2

    assert_equal TinyActors::Raw::TA_OK, value_fixture(:__test_queue_release)
    finished = value_fixture(:__test_queue_counts)
    assert_equal 0, finished.fetch(:live_core_refs)
    assert_equal finished.fetch(:retain) + finished.fetch(:snapshot), finished.fetch(:release)
  end
end
