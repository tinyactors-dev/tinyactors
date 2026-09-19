# frozen_string_literal: true

require 'json'
require_relative 'rpc/api'
require_relative 'rpc/policy'
require_relative 'rpc/plan'
require_relative 'rpc/emit'

# Generates the JSON-RPC bindings of the test host from
# tinyactors-core/include/tinyactors.api.json and the policy in bindings.rb.
# All stages run in memory:
#
#   API description + policy --Plan--> decisions --Emit--> ta_rpc.{c,h}
module Rpc
  Output = Struct.new(:header, :source, :coverage, keyword_init: true)

  module_function

  def generate(description, policy)
    api = Api.load(description)
    plan = Plan.new(api, Policy.new(policy))
    emit = Emit.new(api, plan)
    Output.new(header: emit.header, source: emit.source, coverage: coverage(plan))
  end

  def coverage(plan)
    report = plan.report
    counts = %i[functions_bound functions_unimplemented functions_overridden functions_skipped types_bound types_runtime
                types_overridden types_adapter types_skipped].to_h { |key| [key.to_s, report[key].size] }
    counts['methods'] = plan.methods.size
    counts.merge(
      'skipped' => (report[:functions_skipped] + report[:types_skipped]).to_h,
      'value_models' => report[:value_models].to_h
    )
  end
end
