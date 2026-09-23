# frozen_string_literal: true

module TinyactorsTest
  # A statechart given as an SCXML document. The host imports it with
  # ta_definition_import() through the codec.scxml adapter (ADR 0053), so the
  # document is read by the SCXML codec that ships (tinyactors-scxml), not by
  # anything in Ruby.
  #
  # `datamodel:` picks the data-model adapter the host compiles with; the
  # document's own `datamodel` attribute, when it has one, must name the same.
  # `files:` are the resources a `<data src>` or `<script src>` may name: the
  # host's resolver.map serves them, since the core performs no I/O.
  class ScxmlChart
    attr_reader :name, :datamodel, :source, :files

    MEDIA_TYPE = 'application/scxml+xml'

    def initialize(name, source, datamodel: 'jq', files: {})
      @name = name.to_s
      @source = source.to_s
      @datamodel = datamodel&.to_s
      @files = files.transform_keys(&:to_s).transform_values(&:to_s)
    end

    # What the SCXML invoker is given for a `file:` src naming this chart.
    def invoker_entry = source

    def to_s = "#{name} (SCXML)"
  end

  # One line saying why ta_definition_import refused a document: where in
  # it, when the error is located, and what. A compile error is written by
  # core's ta_compile_error_format (ADR 0058), so a broken chart reads
  #   3:36: states[0].transitions[0].targets[0]: unknown state "idel"; nearest is "idle"
  # `ta` is the driver's proxy; `error` is the response's out.error.
  def self.describe_import_error(ta, error)
    return 'no error detail' unless error.is_a?(Hash)

    where = error[:located] ? "#{error.dig(:at, :line)}:#{error.dig(:at, :column)}: " : ''
    case error[:stage]
    when 'decode'
      decode = error[:decode] || {}
      construct = decode[:construct].to_s.empty? ? '' : " <#{decode[:construct]}>"
      attribute = decode[:attribute].to_s.empty? ? '' : " #{decode[:attribute]}="
      "#{where}#{decode[:code]}#{construct}#{attribute}: #{decode[:message]}"
    when 'compile'
      formatted = ta.compile.error_format(error: error[:compile])
      text = formatted[:result] == 'ok' ? formatted.dig(:out, :out) : error[:compile].inspect
      "#{where}#{text}"
    else
      error.inspect
    end
  end
end
