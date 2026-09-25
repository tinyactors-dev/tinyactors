# frozen_string_literal: true

require 'json'

module API
  # Validates JSON against a JSON Schema (draft 2020-12), limited to the
  # keywords include/tinyactors.api.schema.json uses: type, const, enum,
  # properties, required, additionalProperties, items, $ref to $defs, and
  # oneOf. An unsupported keyword is an error, so the schema cannot silently
  # say more than this checks.
  class Schema
    KEYWORDS = %w[$schema $id title description type const enum properties required additionalProperties items $ref oneOf
                  $defs].freeze

    def self.load(path) = new(JSON.parse(File.read(path)))

    def initialize(root)
      @root = root
    end

    # Returns a list of "path: problem" strings; empty when valid.
    def errors(value)
      validate(@root, value, '$')
    end

    private

    def validate(schema, value, path)
      unknown = schema.keys - KEYWORDS
      raise Error, "schema uses unsupported keywords #{unknown.join(', ')}" unless unknown.empty?
      return validate(resolve(schema['$ref']), value, path) if schema['$ref']

      errors = []
      errors << "#{path}: expected #{Array(schema['type']).join(' or ')}" if schema['type'] && !type?(schema['type'], value)
      errors << "#{path}: expected #{schema['const'].to_json}" if schema.key?('const') && value != schema['const']
      errors << "#{path}: expected one of #{schema['enum'].to_json}" if schema['enum'] && !schema['enum'].include?(value)
      return errors unless errors.empty?

      errors.concat(one_of(schema['oneOf'], value, path)) if schema['oneOf']
      errors.concat(object(schema, value, path)) if value.is_a?(Hash)
      if value.is_a?(Array) && schema['items']
        value.each_with_index { |item, index| errors.concat(validate(schema['items'], item, "#{path}[#{index}]")) }
      end
      errors
    end

    def object(schema, value, path)
      errors = (schema['required'] || []).reject { |key| value.key?(key) }.map { |key| "#{path}: missing #{key}" }
      properties = schema['properties'] || {}
      value.each do |key, member|
        if properties.key?(key)
          errors.concat(validate(properties[key], member, "#{path}.#{key}"))
        elsif schema['additionalProperties'] == false
          errors << "#{path}: unexpected member #{key}"
        elsif schema['additionalProperties'].is_a?(Hash)
          errors.concat(validate(schema['additionalProperties'], member, "#{path}.#{key}"))
        end
      end
      errors
    end

    # Exactly one branch matches; otherwise report the closest branch.
    def one_of(branches, value, path)
      results = branches.map { |branch| validate(branch, value, path) }
      matching = results.count(&:empty?)
      return [] if matching == 1
      return ["#{path}: matches #{matching} alternatives"] if matching > 1

      results.min_by(&:size)
    end

    def type?(types, value)
      Array(types).any? do |type|
        case type
        when 'object' then value.is_a?(Hash)
        when 'array' then value.is_a?(Array)
        when 'string' then value.is_a?(String)
        when 'integer' then value.is_a?(Integer)
        when 'boolean' then [true, false].include?(value)
        when 'null' then value.nil?
        else raise Error, "unsupported schema type #{type}"
        end
      end
    end

    def resolve(reference)
      name = reference[%r{\A#/\$defs/(\w+)\z}, 1] or raise Error, "unsupported $ref #{reference}"
      @root.fetch('$defs').fetch(name)
    end
  end
end
