# frozen_string_literal: true

require_relative "../test_helper"

class RawMemoryTest < Minitest::Test
  Memory = TinyActors::Raw::Memory

  def test_allocate_is_private_pointer_free_and_aligned
    assert_raises(TypeError) { Memory.new }

    [1, 8, 16, 64].each do |alignment|
      memory = Memory.allocate(size: 16, alignment: alignment)
      assert_equal 16, memory.size
      refute_predicate memory, :closed?
      memory.release
      assert_predicate memory, :closed?
    end
  end

  def test_zero_sized_memory_supports_only_empty_boundary_access
    memory = Memory.allocate(size: 0, alignment: 8)

    assert_equal "".b, memory.read(0, 0)
    assert_nil memory.write(0, "".b)
    assert_raises(ArgumentError) { memory.read(1, 0) }
    assert_raises(ArgumentError) { memory.write(1, "".b) }
  ensure
    memory&.release unless memory&.closed?
  end

  def test_read_and_write_copy_binary_data_with_embedded_nul
    memory = Memory.allocate(size: 8, alignment: 8)
    bytes = "\x00\xffa\x00bc".b

    assert_nil memory.write(1, bytes)
    copy = memory.read(1, bytes.bytesize)
    assert_equal bytes, copy
    assert_equal Encoding::BINARY, copy.encoding
    copy.setbyte(0, 0x7f)
    assert_equal bytes, memory.read(1, bytes.bytesize)
  ensure
    memory&.release unless memory&.closed?
  end

  def test_rejects_negative_overflow_and_out_of_range_accesses
    memory = Memory.allocate(size: 16, alignment: 8)

    [-1, 16, (1 << 64)].each do |offset|
      length = offset == 0 ? (1 << 64) : 1
      assert_raises(ArgumentError, RangeError) { memory.read(offset, length) }
      assert_raises(ArgumentError, RangeError) { memory.write(offset, "x") }
    end
    assert_raises(RangeError) { memory.read(0, 1 << 64) }
    assert_raises(RangeError) { memory.read(0, -1) }
    assert_raises(ArgumentError) { memory.read(15, 2) }
    assert_raises(ArgumentError) { memory.write(15, "xx") }
    assert_equal "".b, memory.read(16, 0)
  ensure
    memory&.release unless memory&.closed?
  end

  def test_rejects_bad_types_before_native_access
    assert_raises(TypeError) { Memory.allocate(size: "8", alignment: 8) }
    assert_raises(TypeError) { Memory.allocate(size: 8, alignment: 8.0) }
    assert_raises(ArgumentError) { Memory.allocate(size: 8, alignment: 3) }
    assert_raises(RangeError) { Memory.allocate(size: -1, alignment: 8) }
    assert_raises(RangeError) { Memory.allocate(size: 1 << 64, alignment: 1) }

    memory = Memory.allocate(size: 4, alignment: 1)
    assert_raises(TypeError) { memory.read("0", 1) }
    assert_raises(TypeError) { memory.read(0, "1") }
    assert_raises(TypeError) { memory.write(0, [:not_bytes]) }
    assert_raises(RangeError) { memory.write(-1, "x") }
  ensure
    memory&.release unless memory&.closed?
  end

  def test_release_invalidates_access_and_is_idempotent
    memory = Memory.allocate(size: 4, alignment: 8)

    assert_nil memory.release
    assert_predicate memory, :closed?
    assert_nil memory.release
    assert_raises(RuntimeError) { memory.size }
    assert_raises(RuntimeError) { memory.read(0, 0) }
    assert_raises(RuntimeError) { memory.write(0, "".b) }
  end

  def test_native_storage_survives_gc_and_compaction_until_release
    memory = Memory.allocate(size: 8, alignment: 8)
    memory.write(0, "native\x00".b)
    GC.start
    GC.compact if GC.respond_to?(:compact)

    assert_equal "native\x00".b, memory.read(0, 7)
  ensure
    memory&.release unless memory&.closed?
  end

  def test_arena_view_is_real_native_storage_but_not_caller_releasable
    memory = Memory.__send__(:__test_arena_view, 16, 8)
    memory.write(0, "arena-bytes".b)
    assert_equal "arena-bytes".b, memory.read(0, 11)
    assert_raises(RuntimeError) { memory.release }

    Memory.__send__(:__test_arena_close)
    assert_predicate memory, :closed?
    assert_raises(RuntimeError) { memory.read(0, 1) }
  ensure
    Memory.__send__(:__test_arena_close)
  end

  def test_context_bytes_fixture_writes_real_actor_state_and_expires_view
    memory = Memory.__send__(:__test_context_view)

    assert Memory.__send__(:__test_context_readonly_rejected)
    assert_equal "\x01\xff\x00abc\x00\x00".b,
                 Memory.__send__(:__test_context_state)
    assert_predicate memory, :closed?
    assert_raises(RuntimeError) { memory.read(0, 1) }
    assert_raises(RuntimeError) { memory.write(0, "x") }
  ensure
    Memory.__send__(:__test_context_close)
  end

  def test_release_rejects_an_outstanding_arena_loan
    memory = Memory.__send__(:__test_loaned)

    GC.start
    GC.compact if GC.respond_to?(:compact)
    assert_equal 8, memory.size
    assert_raises(RuntimeError) { memory.release }
    Memory.__send__(:__test_clear_loan, memory)
    assert_nil memory.release
    assert_predicate memory, :closed?
  end
end
