# frozen_string_literal: true

require 'json'
require 'open3'
require 'shellwords'
require 'tmpdir'

module Api
  # Runs clang on the public header and answers questions about its AST.
  # Extraction is pinned to explicit targets in freestanding mode, so the
  # result never depends on the machine that runs it.
  module Clang
    Error = Class.new(StandardError)

    # The first target is where types are read; every target gets a layout.
    TARGETS = %w[x86_64-unknown-linux-gnu wasm32-unknown-unknown].freeze
    DEFINES = ['-DTA_API_EXTRACT', '-DTA_API=__attribute__((annotate("tinyactors")))'].freeze

    module_function

    def compiler
      Shellwords.split(ENV.fetch('CLANG', 'clang'))
    end

    def flags(target)
      ["--target=#{target}", '-ffreestanding', '-std=c11', '-xc', *DEFINES]
    end

    def run(*arguments, input: nil)
      output, errors, status = Open3.capture3(*compiler, *arguments, stdin_data: input)
      raise Error, "clang #{arguments.join(' ')} failed:\n#{errors}" unless status.success?

      output
    end

    def ast(header, target: TARGETS.first)
      json = run(*flags(target), '-fparse-all-comments', '-Wno-pragma-once-outside-header',
                 '-Xclang', '-ast-dump=json', '-fsyntax-only', header.to_s)
      Ast.new(JSON.parse(json), header)
    end

    # Object-like macros the header defines, as {name => body}.
    def macros(header, target: TARGETS.first)
      output = run(*flags(target), '-Wno-pragma-once-outside-header', '-dM', '-E', header.to_s)
      output.lines.each_with_object({}) do |line, macros|
        match = line.match(/\A#define (\w+)(\()?[^ ]* ?(.*)\n?\z/) or next
        macros[match[1]] = match[3] unless match[2]
      end
    end

    # Names of the functions an object file compiled from `source` defines
    # and exports.
    def exported_functions(source, target: TARGETS.first)
      Dir.mktmpdir('tinyactors-api') do |directory|
        object = File.join(directory, 'library.o')
        run(*flags(target), "-I#{File.dirname(source)}", '-c', source.to_s, '-o', object)
        output, errors, status = Open3.capture3(*Shellwords.split(ENV.fetch('NM', 'nm')), '-P', '-g', object)
        raise Error, "nm failed:\n#{errors}" unless status.success?

        output.lines.filter_map { |line| line.split[0] if line.split[1] == 'T' }
      end
    end

    # Evaluates integer constant expressions for one target by compiling them
    # as enum initializers: clang folds each one and reports its value and
    # type. Returns {key => {value:, type:}}.
    def evaluate(header, expressions, target:)
      source = +"#include \"#{File.basename(header)}\"\n#include <stddef.h>\n"
      expressions.each_with_index do |(_, expression), index|
        source << "enum { tinyactors_probe_#{index} = (#{expression}) };\n"
      end
      json = run(*flags(target), "-I#{File.dirname(header)}", '-Wno-everything',
                 '-Xclang', '-ast-dump=json', '-fsyntax-only', '-', input: source)
      values = {}
      keys = expressions.keys
      each_node(JSON.parse(json)) do |node|
        next unless node['kind'] == 'EnumConstantDecl' && (index = node['name'][/\Atinyactors_probe_(\d+)\z/, 1])

        constant = find_node(node) { |child| child['kind'] == 'ConstantExpr' } or
          raise Error, "#{expressions[keys[index.to_i]]} is not an integer constant for #{target}"
        values[keys[index.to_i]] = { value: Integer(constant['value']), type: expression_type(constant) }
      end
      missing = keys - values.keys
      raise Error, "could not evaluate #{missing.join(', ')} for #{target}" unless missing.empty?

      values
    end

    # The type an expression has before implicit conversions, e.g. the type
    # named by a cast.
    def expression_type(node)
      node = node['inner'].first while %w[ConstantExpr ImplicitCastExpr ParenExpr].include?(node['kind']) && node['inner']
      node.dig('type', 'qualType')
    end

    # Compiles C source against the header for one target; returns the
    # diagnostics, empty when it compiles cleanly.
    def check(header, source, target:)
      _output, errors, status = Open3.capture3(*compiler, *flags(target), "-I#{File.dirname(header)}", '-Wall', '-Wextra',
                                               '-Werror', '-pedantic', '-fsyntax-only', '-', stdin_data: source)
      status.success? ? '' : errors
    end

    def each_node(node, &block)
      return unless node.is_a?(Hash)

      block.call(node)
      (node['inner'] || []).each { |child| each_node(child, &block) }
    end

    def find_node(node, &block)
      each_node(node) { |child| return child if block.call(child) }
      nil
    end

    # The JSON AST of one translation unit.
    class Ast
      attr_reader :header, :source

      def initialize(json, header)
        @json = json
        @header = header.to_s
        @source = File.binread(@header)
        @nodes = {}
        Clang.each_node(json) { |node| @nodes[node['id']] = node if node['id'] }
      end

      def node(id) = @nodes.fetch(id)

      # Top-level declarations written in the header, in order. clang records a
      # location's file only when it changes, so the current file is tracked
      # through every location in document order.
      def declarations
        file = nil
        @json['inner'].filter_map do |node|
          file = track(node['loc'], file)
          own = same_file?(file)
          file = track_all(node, file)
          node if own
        end
      end

      # Annotation macros on a declaration, read from the header at each
      # attribute's expansion location.
      def annotations(node)
        (node['inner'] || []).select { |child| child['kind'] == 'AnnotateAttr' }.map do |attribute|
          offset = expansion(attribute.dig('range', 'begin'))['offset']
          CType.parse_annotation(@source.byteslice(offset, 800).force_encoding('UTF-8'))
        end
      end

      # The source text of a declaration, from the start of its range to the
      # end of its last token.
      def text(node)
        range = node['range']
        first = expansion(range['begin'])
        last = expansion(range['end'])
        @source.byteslice(first['offset'], last['offset'] + last['tokLen'] - first['offset']).force_encoding('UTF-8')
      end

      # The comment clang attached to a declaration, as plain text: its
      # paragraphs, without code blocks.
      def doc(node)
        comment = (node['inner'] || []).find { |child| child['kind'] == 'FullComment' } or return nil
        paragraphs = (comment['inner'] || []).select { |child| child['kind'] == 'ParagraphComment' }.map do |paragraph|
          texts = []
          Clang.each_node(paragraph) { |child| texts << child['text'] if child['kind'] == 'TextComment' }
          texts.map(&:strip).reject(&:empty?).join(' ')
        end
        text = paragraphs.reject(&:empty?).join("\n\n")
        text.empty? ? nil : text
      end

      private

      def expansion(location) = location['expansionLoc'] || location

      def same_file?(file)
        file && File.expand_path(file) == File.expand_path(@header)
      end

      def track(location, file)
        return file unless location.is_a?(Hash)

        %w[spellingLoc expansionLoc].each { |key| file = track(location[key], file) }
        location['file'] || file
      end

      def track_all(node, file)
        file = track(node['loc'], file)
        if (range = node['range'])
          file = track(range['begin'], file)
          file = track(range['end'], file)
        end
        (node['inner'] || []).each { |child| file = track_all(child, file) if child.is_a?(Hash) }
        file
      end
    end
  end
end
