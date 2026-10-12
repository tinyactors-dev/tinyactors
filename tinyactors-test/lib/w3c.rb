# frozen_string_literal: true

require 'fileutils'
require 'pathname'

# The W3C SCXML IRP suite: downloaded once into build/w3c/irp and its
# templates instantiated for ECMAScript into build/w3c/ecma, with an
# index.txt the runners read. The W3C stylesheets are XSLT 2.0, which only
# SaxonC implements among what we can pin, so that one step is
# suites/w3c/convert.py, run with the pinned uv (mise.toml).
module W3C
  ROOT = Pathname.new(__dir__).join('..').expand_path
  BASE = 'https://www.w3.org/Voice/2013/scxml-irp/'
  IRP = ROOT.join('build/w3c/irp')
  CONVERTED = ROOT.join('build/w3c/ecma')
  SKIP = ROOT.join('suites/w3c/skip.txt')

  module_function

  def suite
    return CONVERTED if CONVERTED.join('index.txt').file?

    FileUtils.mkdir_p(IRP)
    manifest = IRP.join('manifest.xml')
    manifest.write(get('manifest.xml')) unless manifest.file?
    files = manifest.read.scan(/<(?:start|dep) uri="([^"]+)"/).flatten.uniq.sort + %w[confEcma.xsl confXPath.xsl]
    missing = files.reject { |file| IRP.join(file).file? }
    missing.each do |file|
      FileUtils.mkdir_p(IRP.join(file).dirname)
      IRP.join(file).binwrite(get(file))
    end
    puts "w3c: #{files.size} files in #{IRP.relative_path_from(ROOT)} (#{missing.size} fetched)"
    pid = Process.spawn('uv', 'run', '--quiet', '--with', 'saxonche', 'python', ROOT.join('suites/w3c/convert.py').to_s,
                        IRP.to_s, CONVERTED.to_s, chdir: ROOT.to_s)
    _pid, status = Process.wait2(pid)
    raise "w3c: convert.py failed (#{status.exitstatus})" unless status.success?

    CONVERTED
  end

  # w3.org throttles bursts (HTTP 429): one request at a time, waiting as
  # long as it asks.
  def get(path, attempts: 8)
    require 'net/http'
    attempts.times do |attempt|
      response = Net::HTTP.get_response(URI(BASE + path), { 'User-Agent' => 'curl/8.5.0' })
      return response.body if response.is_a?(Net::HTTPSuccess)
      raise "#{path}: HTTP #{response.code}" unless response.code == '429' && attempt < attempts - 1

      after = response['Retry-After'].to_s
      sleep(after.match?(/\A\d+\z/) ? [after.to_i, 120].min : [2**(attempt + 1), 60].min)
    end
  end

  # One test of index.txt: its id, conformance, start document and
  # dependencies.
  Test = Struct.new(:id, :conformance, :start, :dependencies)

  def tests(directory = suite)
    directory.join('index.txt').readlines(chomp: true).map do |line|
      id, conformance, start, dependencies = line.split(' ')
      Test.new(id, conformance, start, dependencies == '-' ? [] : dependencies.split(','))
    end
  end

  # The ids skip.txt lists, with their reasons.
  def skipped
    SKIP.readlines(chomp: true).reject { |line| line.start_with?('#') || line.strip.empty? }
        .to_h { |line| line.split(' ', 2) }
  end
end
