# frozen_string_literal: true

module SCXML
  module IRP
    # The dialect of a `datamodel="null"` document (SCXML B.1). The profile's
    # whole expression language is the `In(state)` predicate in a condition, so
    # this dialect writes that and refuses everything else: a test needing any
    # other construct is skipped with the reason, as the importer's strict
    # conversion requires.
    module NullExpressions
      DATAMODEL = 'null'

      module_function

      def datamodel = DATAMODEL

      # The null data model can record nothing, so a test's outcome cannot be
      # written into data the way the jq and ECMAScript dialects do. The
      # importer asks before emitting the outcome.
      def records_outcome? = false

      def conf_attribute(name, value)
        case name
        when 'targetpass' then ['target', 'pass']
        when 'targetfail' then ['target', 'fail']
        when 'datamodel' then ['datamodel', DATAMODEL]
        when 'inState' then ['cond', "In('#{value}')"]
        else unsupported("conf:#{name}")
        end
      end

      def conf_content(name) = unsupported("conf:#{name} content")

      def conf_assignment(name, _attributes) = unsupported("conf:#{name} assignment")

      def conf_script = unsupported('conf:script')

      def string(text) = text.to_s

      def plain_expression(_source) = unsupported('an expression')

      def plain_location(_source) = unsupported('a location')

      def script_body(_text) = unsupported('a script')

      def unsupported(what)
        raise Unsupported, "the null data model has no #{what}"
      end
    end
  end
end
