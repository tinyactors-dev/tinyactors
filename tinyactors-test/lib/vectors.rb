# frozen_string_literal: true

# The protocol vectors' files (suites/protocol/README.md, "The format"): a
# case is a line "<kind> <name>" and the lines after it, up to a blank line;
# each is a key and a value. Lines starting with # are comments.
module Vectors
  # The keys whose value is CBOR in hex, each followed by a diag line.
  HEX_KEYS = %w[send reply cbor back error envelope].freeze

  Case = Struct.new(:file, :kind, :name, :lines) do
    # The value of the nth line with `key`, or nil.
    def get(key, nth = 0) = all(key)[nth]

    def all(key) = lines.select { |each, _| each == key }.map(&:last)

    def key?(key) = lines.any? { |each, _| each == key }

    def to_s = "#{file} \"#{name}\""
  end

  module_function

  def read(path)
    file = File.basename(path)
    cases = []
    current = nil
    File.readlines(path, chomp: true).each do |line|
      next if line.start_with?('#')

      if line.strip.empty?
        current = nil
        next
      end
      key, value = line.split(' ', 2)
      if current
        current.lines << [key, value.to_s]
      else
        current = Case.new(file, key, value.to_s, [])
        cases << current
      end
    end
    cases
  end
end
