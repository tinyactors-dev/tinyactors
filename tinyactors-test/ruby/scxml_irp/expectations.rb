# frozen_string_literal: true

require 'json'
require 'pathname'

# Expected outcomes of the imported W3C SCXML IRP scenarios. The generated
# scenario files call SCXMLIRP.expect_failure before each scenario.
#
# Every scenario is expected to pass unless baseline.json lists it. The
# baseline records each known failure with its exact error message, so a
# scenario that starts failing differently is a FAIL, and one that starts
# passing is an XPASS until `mise run scxml-irp-baseline` records the
# progress.
module SCXMLIRP
  BASELINE = Pathname.new(__dir__).join('baseline.json')
  BASELINE_REASON = 'known failure recorded in ruby/scxml_irp/baseline.json'

  # Set by the baseline task, which runs every scenario without expectations.
  IGNORE_BASELINE = ENV['SCXML_IRP_IGNORE_BASELINE'] == '1'

  def self.baseline
    @baseline ||= BASELINE.file? ? JSON.parse(BASELINE.read) : {}
  end

  def self.expect_failure(group, scenario)
    return if IGNORE_BASELINE

    message = baseline[scenario]
    return unless message

    group.expect_failure(BASELINE_REASON, message: /\A#{Regexp.escape(message)}\z/)
  end
end
