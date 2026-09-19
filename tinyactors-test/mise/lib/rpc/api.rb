# frozen_string_literal: true

require 'json'

module Rpc
  Error = Class.new(StandardError)

  # Read access to tinyactors.api.json, the only input about the C API.
  class Api
    FORMAT = 'tinyactors-api/2'

    attr_reader :types, :functions, :flag_sets

    def self.load(path)
      description = JSON.parse(File.read(path))
      unless description['format'] == FORMAT
        raise Error, "#{path} is #{description['format'].inspect}; this generator reads #{FORMAT}"
      end

      new(description)
    end

    def initialize(description)
      @types = description.fetch('types')
      @functions = description.fetch('functions')
      @flag_sets = description.fetch('flag_sets')
    end

    def type(name) = @types.fetch(name) { raise Error, "unknown type #{name}" }
    def kind(name) = type(name)['kind']
    def semantic(name) = type(name)['semantic']

    # Follows aliases, keeping a const on the alias.
    def resolve(type)
      const = type['const']
      type = @types.fetch(type['type'])['type'] while type['type'] && @types.dig(type['type'], 'kind') == 'alias'
      const && !type['const'] ? type.merge('const' => true) : type
    end

    def pointer?(type) = resolve(type).key?('pointer')
    def pointee(type) = resolve(resolve(type)['pointer'])

    # The C spelling of a type, without const.
    def c_type(type)
      return "#{c_type(type['pointer'])} *" if type.key?('pointer')

      type['type'] || type['builtin'] || raise(Error, "no C spelling for #{type.inspect}")
    end

    def enum_constant(enum, name)
      type(enum)['constants'].find { |constant| constant['name'] == name } or raise Error, "#{enum} has no #{name}"
    end
  end
end
