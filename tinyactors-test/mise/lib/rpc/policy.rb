# frozen_string_literal: true

module RPC
  # The binding decisions in bindings.rb that the API description cannot
  # make: what the RPC host leaves out, what it implements by hand, and how
  # its JSON is laid out. Every pattern must match something, so the policy
  # cannot go stale unnoticed.
  class Policy
    def initialize(mod)
      @mod = mod
      @used = Hash.new { |hash, key| hash[key] = {} }
    end

    def skip_function(name) = lookup(:SKIP_FUNCTIONS, name)
    def override_function(name) = lookup(:OVERRIDE_FUNCTIONS, name)
    def skip_type(name) = lookup(:SKIP_TYPES, name)
    def override_type(name) = lookup(:OVERRIDE_TYPES, name)
    def flatten_member?(struct, member) = !lookup(:FLATTEN_UNION_MEMBERS, "#{struct}.#{member}").nil?
    def flatten_parameter = @mod::FLATTEN_PARAMETER
    def max_buffer_elements = @mod::MAX_BUFFER_ELEMENTS

    # Raises when an entry matched nothing during planning.
    def check_used!
      stale = %i[SKIP_FUNCTIONS OVERRIDE_FUNCTIONS SKIP_TYPES OVERRIDE_TYPES FLATTEN_UNION_MEMBERS]
              .flat_map do |table|
        @mod.const_get(table).keys.reject { |pattern| @used[table][pattern] }.map { |pattern| "#{table} #{pattern}" }
      end
      raise Error, "bindings.rb entries match nothing: #{stale.join(', ')}" unless stale.empty?
    end

    private

    def lookup(table, name)
      pattern, reason = @mod.const_get(table).find { |each, _| File.fnmatch(each, name, File::FNM_EXTGLOB) }
      return nil unless pattern

      @used[table][pattern] = true
      reason
    end
  end
end
