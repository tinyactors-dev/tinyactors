# frozen_string_literal: true

require 'minitest/autorun'
require 'fileutils'
require 'open3'
require 'tmpdir'
require 'w3c'

# W3C.publish: a conversion is written into a new directory and put in
# place by one rename, so concurrent runs never read half of one; and
# convert.py writes only into a fresh directory.
class PublishTest < Minitest::Test
  def setup
    @directory = Dir.mktmpdir('tinyactors-test-publish')
    @out = Pathname.new(@directory).join('build/w3c/python')
  end

  def teardown
    FileUtils.rm_rf(@directory)
  end

  def test_a_conversion_is_published_whole
    published = W3C.publish(@out, -> { true }) { |fresh| write(fresh, 'one') }
    assert_equal @out, published
    assert @out.symlink?, 'out is a link to the conversion'
    assert_equal 'one', @out.join('index.txt').read
    assert_equal 'one', @out.join('a/test.scxml').read
    W3C.publish(@out, -> { true }) { |fresh| write(fresh, 'two') }
    assert_equal 'two', @out.join('index.txt').read
    assert_equal [@out.readlink.to_s], versions, 'the conversion it replaced is deleted'
  end

  def test_a_reader_sees_the_previous_conversion_while_the_next_is_written
    W3C.publish(@out, -> { true }) { |fresh| write(fresh, 'one') }
    writing = Queue.new
    go_on = Queue.new
    writer = Thread.new do
      W3C.publish(@out, -> { true }) do |fresh|
        fresh.join('index.txt').write('two')
        writing << true
        go_on.pop
        write(fresh, 'two')
      end
    end
    writing.pop
    assert_equal 'one', @out.join('index.txt').read
    assert_equal 'one', @out.join('a/test.scxml').read
    go_on << true
    writer.join
    assert_equal %w[two two], [@out.join('index.txt').read, @out.join('a/test.scxml').read]
  end

  def test_a_run_that_waited_for_the_lock_finds_the_conversion_done
    converted = []
    stale = -> { !@out.join('index.txt').file? }
    started = Queue.new
    go_on = Queue.new
    first = Thread.new do
      W3C.publish(@out, stale) do |fresh|
        started << true
        go_on.pop
        converted << :first
        write(fresh, 'first')
      end
    end
    started.pop
    second = Thread.new { W3C.publish(@out, stale) { |fresh| (converted << :second) && write(fresh, 'second') } }
    sleep 0.2 # the second run is waiting for the lock
    go_on << true
    [first, second].each(&:join)
    assert_equal [:first], converted
    assert_equal 'first', @out.join('index.txt').read
  end

  def test_a_conversion_that_fails_leaves_the_previous_one
    W3C.publish(@out, -> { true }) { |fresh| write(fresh, 'one') }
    assert_raises(RuntimeError) do
      W3C.publish(@out, -> { true }) do |fresh|
        write(fresh, 'half')
        raise 'convert.py failed'
      end
    end
    assert_equal 'one', @out.join('index.txt').read
    assert_equal [@out.readlink.to_s], versions
  end

  def test_a_conversion_written_in_place_before_is_replaced
    write(@out, 'old')
    W3C.publish(@out, -> { true }) { |fresh| write(fresh, 'new') }
    assert @out.symlink?
    assert_equal 'new', @out.join('index.txt').read
  end

  def test_nothing_is_written_when_it_is_not_stale
    W3C.publish(@out, -> { true }) { |fresh| write(fresh, 'one') }
    W3C.publish(@out, -> { false }) { flunk 'converted although not stale' }
    assert_equal 'one', @out.join('index.txt').read
  end

  def test_convert_py_refuses_a_directory_that_holds_files
    write(@out, 'old')
    out, status = Open3.capture2e('python3', '-I', W3C::CONVERT.to_s, W3C.stylesheet('python').to_s, W3C::IRP.to_s,
                                  @out.to_s)
    refute status.success?
    assert_includes out, 'is not empty'
    assert_equal 'old', @out.join('index.txt').read
  end

  private

  def write(directory, text)
    FileUtils.mkdir_p(directory.join('a'))
    directory.join('index.txt').write(text)
    directory.join('a/test.scxml').write(text)
  end

  def versions = Dir[@out.dirname.join(".#{@out.basename}.v*").to_s].map { |path| File.basename(path) }
end
