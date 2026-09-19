# frozen_string_literal: true

require_relative 'driver'
require_relative 'tinyactors_test/chart'
require_relative 'tinyactors_test/instruments'
require_relative 'tinyactors_test/trace'
require_relative 'tinyactors_test/scenario'
require_relative 'tinyactors_test/runner'

# The scenario DSL for tinyactors-test. Charts live in ruby/charts and
# scenarios in ruby/scenarios; see README.md.
module TinyactorsTest
  @charts = {}
  @groups = []

  class << self
    attr_reader :charts, :groups

    def chart(name, **options, &block)
      name = name.to_sym
      raise ChartError, "chart #{name} is defined twice" if @charts.key?(name)

      @charts[name] = Chart.define(name, **options, &block)
    end

    def scenarios(name, chart: nil, &block)
      group = Group.new(name, chart: chart)
      group.instance_eval(&block)
      group.finish!
      @groups << group
      group
    end
  end
end
