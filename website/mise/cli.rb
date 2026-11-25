# frozen_string_literal: true

require 'base64'
require 'fileutils'
require 'json'
require 'open3'

# The CLI section (/cli/), made from the released `tinyactors` command's
# own description of itself: its help as XML (`help -f xml`, `help
# <command> -f xml`, the tables its argument parser reads), each example
# of its help run on the charts of examples/cli/ with what it printed, and
# the files it writes that the pages serve (`run --schema`). The site
# documents the latest release, as its examples use the published
# packages: the binary is the one website/mise.toml pins
# ("github:tinyactors-dev/tinyactors-core"), installed by mise's github
# backend from the release's archive.
#
# For development, before a release exists or to preview the working tree's
# command, TINYACTORS_CLI names another binary (tinyactors-core's
# build/dev/bin/tinyactors), which the build and the tests then use
# instead of the pin's; the pages then say that binary's version. Until the
# pinned release is published, mise cannot install it and stops every task
# here; run them with MISE_TASK_RUN_AUTO_INSTALL=false and TINYACTORS_CLI
# set.
module Website
  module CLI
    TOOL = 'github:tinyactors-dev/tinyactors-core'
    OVERRIDE = 'TINYACTORS_CLI'
    # What the build captures: cli.xml (read by render/site.xsl), the
    # fixture the examples run in, and site/, copied to the site's /cli/.
    DIR = BUILD.join('cli')
    DOCUMENT = DIR.join('cli.xml')
    SERVED = DIR.join('site')
    # The charts and inputs the help's examples name (order.scxml,
    # system.xml, shop.json, requests.jsonl, ...).
    FIXTURE = EXAMPLES.join('cli')
    # The page that lists the commands (its <commands>), whose path the
    # command pages are under.
    PAGE = CONTENT.join('pages/cli.xml')
    # What the help's examples read besides the fixture's own files, made
    # by the released command itself before they run: images, copies a
    # `fmt -w` may rewrite, an include directory, and a snapshot and a
    # hibernation blob of a host (`run` with shop.jsonl).
    SETUP = [
      %w[compile order.scxml payment.scxml -o order.tai],
      %w[compile system.xml -o system.tai],
      %w[compile system.xml -o shop.tai],
      %w[compile order-v1.scxml -o order-v1.tai],
      %w[compile order-v1.scxml -o deployed.tai],
      %w[compile order-v1.scxml -o old.tai],
      %w[compile order.scxml -o new.tai]
    ].freeze
    # Sessions a page shows with what they print: the output, committed
    # beside them in examples/cli/, and the command line that prints it,
    # which the page shows too. `mise run test` runs each in the fixture
    # and compares.
    TRANSCRIPTS = { 'requests.txt' => 'tinyactors run --io-processor payments --system shop order.scxml < requests.jsonl' }.freeze
    SNAPSHOT = { arguments: %w[run --config shop.json --system shop system.xml], input: 'shop.jsonl', blob: 17,
                 blob_file: 'order-17.blob' }.freeze

    module_function

    # The version website/mise.toml pins.
    def pin = ROOT.join('mise.toml').read[/^"#{Regexp.escape(TOOL)}"\s*=\s*"([^"]+)"/, 1] || Website.fail!("mise.toml pins no #{TOOL}")

    def override = ENV.fetch(OVERRIDE, '').then { |path| path.empty? ? nil : Pathname.new(path).expand_path }

    # The binary: TINYACTORS_CLI's, or the pinned release's (mise where).
    def binary = @binary ||= resolve

    def resolve
      if (path = override)
        Website.fail!("#{OVERRIDE}=#{path}: not an executable file") unless path.file? && path.executable?
        return path
      end
      out, ok = Website.capture('mise', 'where', "#{TOOL}@#{pin}")
      Website.fail!("#{TOOL}@#{pin} is not installed (mise install; before its release, set #{OVERRIDE}):\n#{out}") unless ok
      path = Pathname.new(out.strip).join('bin/tinyactors')
      Website.fail!("#{path}: the release has no bin/tinyactors") unless path.file?
      path
    end

    # A directory whose `tinyactors` is the binary, for the examples' PATH.
    def bin
      dir = DIR.join('bin')
      FileUtils.mkdir_p(dir)
      link = dir.join('tinyactors')
      FileUtils.rm_f(link)
      File.symlink(binary.to_s, link.to_s)
      dir
    end

    def run!(*arguments, chdir: ROOT, stdin: nil)
      out, err, status = Open3.capture3(binary.to_s, *arguments, chdir: chdir.to_s, stdin_data: stdin.to_s, binmode: true)
      Website.fail!("tinyactors #{arguments.join(' ')} exited with #{status.exitstatus}:\n#{err}") unless status.success?
      out
    end

    # The program's help and each command's, as JSON (for Ruby) and XML
    # (for the stylesheet), the same tables either way.
    def help(*command, format: 'json') = (@help ||= {})[[command, format]] ||= run!('help', *command, '-f', format)
    def commands = JSON.parse(help).fetch('commands').map { |command| command['command'] }

    # Each command's options, every name: "-c, --chart NAME" is -c and --chart.
    def options
      program = JSON.parse(help).fetch('programOptions').flat_map { |option| option_names(option) }
      commands.to_h do |command|
        [command, (JSON.parse(help(command))['options'] || []).flat_map { |option| option_names(option) } + program]
      end
    end

    def option_names(option) = option['option'].scan(/(?<![\w-])--?[A-Za-z][\w-]*/)

    # The fixture: examples/cli/'s files, then SETUP and the snapshot
    # session, run by the binary in it.
    def fixture!
      dir = DIR.join('fixture')
      FileUtils.rm_rf(dir)
      FileUtils.mkdir_p(dir)
      FIXTURE.children.select(&:file?).each { |file| FileUtils.cp(file, dir) }
      FileUtils.mkdir_p(dir.join('charts'))
      %w[order.scxml payment.scxml].each { |name| FileUtils.cp(dir.join(name), dir.join('charts')) }
      FileUtils.mkdir_p(dir.join('shared'))
      SETUP.each { |arguments| run!(*arguments, chdir: dir) }
      out = run!(*SNAPSHOT[:arguments], chdir: dir, stdin: dir.join(SNAPSHOT[:input]).read)
      blob = out.lines.flat_map { |line| [JSON.parse(line)].flatten }
                .find { |message| message.dig('result', 'blob') && message['id'] }&.dig('result', 'blob')
      Website.fail!("#{SNAPSHOT[:input]}: no run.hibernate answered a blob") unless blob
      dir.join(SNAPSHOT[:blob_file]).binwrite(Base64.urlsafe_decode64(blob + ('=' * ((4 - (blob.size % 4)) % 4))))
      Website.fail!("#{SNAPSHOT[:input]}: wrote no shop.snapshot") unless dir.join('shop.snapshot').file?
      dir
    end

    # One example of the help, run by sh in a copy of the fixture with
    # standard input empty (where the example gives none): what it printed
    # (standard output, then standard error), its exit status, and the SVG
    # files it wrote, served under /cli/examples/.
    def example!(fixture, bin, command, index, line)
      dir = DIR.join('runs', "#{command}-#{index}")
      FileUtils.rm_rf(dir)
      FileUtils.mkdir_p(dir.dirname)
      FileUtils.cp_r(fixture, dir)
      before = dir.glob('**/*').select(&:file?).to_h { |file| [file, file.mtime] }
      env = { 'PATH' => "#{bin}:#{ENV.fetch('PATH')}", 'LC_ALL' => 'C' }
      out, err, status = Open3.capture3(env, 'sh', '-c', line, chdir: dir.to_s, stdin_data: '', binmode: true)
      svgs = dir.glob('*.svg').select { |file| before[file] != file.mtime }.sort.map do |file|
        name = "#{command}-#{index}-#{file.basename}"
        FileUtils.mkdir_p(SERVED.join('examples'))
        FileUtils.cp(file, SERVED.join('examples', name))
        [file.basename.to_s, "examples/#{name}"]
      end
      { line: line, status: status.exitstatus, output: (out + err).force_encoding('UTF-8').scrub, svgs: svgs }
    end

    # The files the CLI page serves from the binary (`<artifact arguments
    # file>`: `run --schema` as run.schema.json), into site/.
    def artifacts!
      return unless PAGE.file?

      PAGE.read.scan(/<artifact\s([^>]*)>/).flatten.each do |attributes|
        attribute = ->(name) { attributes[/\s?#{name}="([^"]*)"/, 1] or Website.fail!("#{PAGE}: an <artifact> with no #{name}") }
        FileUtils.mkdir_p(SERVED)
        SERVED.join(attribute.call('file')).binwrite(run!(*attribute.call('arguments').split))
      end
    end

    def xml_text(text) = text.gsub(/[^\t\n\r -퟿-�\u{10000}-\u{10FFFF}]/, '').encode(xml: :text)
    def xml_attribute(text) = xml_text(text).gsub('"', '&quot;')
    def document_element(xml) = xml.sub(/\A<\?xml[^>]*\?>\s*/, '')

    # build/cli/cli.xml: the binary's version and help, and for each
    # command its help and each example's run. Returns its path.
    def capture!
      FileUtils.rm_rf(SERVED)
      FileUtils.mkdir_p(SERVED)
      fixture = fixture!
      path = bin
      names = commands
      runs = 0
      parts = names.map do |command|
        examples = JSON.parse(help(command))['examples'] || []
        results = examples.each_with_index.map do |line, i|
          runs += 1
          result = example!(fixture, path, command, i + 1, line)
          files = result[:svgs].map { |name, href| %(<svg file="#{xml_attribute(name)}" href="#{xml_attribute(href)}"/>) }.join
          %(<example line="#{xml_attribute(line)}" status="#{result[:status]}"><output>#{xml_text(result[:output])}</output>#{files}</example>)
        end
        %(<command name="#{command}">#{document_element(help(command, format: 'xml'))}<runs>#{results.join("\n")}</runs></command>)
      end
      artifacts!
      version = JSON.parse(run!('version', '-f', 'json'))
      FileUtils.mkdir_p(DIR)
      DOCUMENT.write(<<~XML)
        <?xml version="1.0" encoding="UTF-8"?>
        <cli version="#{xml_attribute(version.fetch('version'))}" binary="#{xml_attribute(override ? 'override' : 'release')}">
        #{document_element(help(format: 'xml'))}
        #{parts.join("\n")}
        </cli>
      XML
      puts "website: tinyactors #{version['version']} (#{override ? OVERRIDE : "#{TOOL}@#{pin}"}): " \
           "#{names.size} commands, #{runs} examples run"
      DOCUMENT
    end

    # A command line run as an example is: in a copy of the fixture, the
    # binary first on PATH. What it printed and its exit status.
    def shell(line)
      fixture = DIR.join('fixture')
      dir = DIR.join('runs', 'transcript')
      FileUtils.rm_rf(dir)
      FileUtils.mkdir_p(dir.dirname)
      FileUtils.cp_r(fixture, dir)
      env = { 'PATH' => "#{DIR.join('bin')}:#{ENV.fetch('PATH')}", 'LC_ALL' => 'C' }
      out, err, status = Open3.capture3(env, 'sh', '-c', line, chdir: dir.to_s, binmode: true)
      [(out + err).force_encoding('UTF-8').scrub, status.exitstatus]
    end

    # The served files, into the built site's /cli/.
    def serve!(site) = FileUtils.cp_r("#{SERVED}/.", site.join('cli'))
  end
end
