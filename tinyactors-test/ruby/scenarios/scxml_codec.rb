# frozen_string_literal: true

# Charts written as SCXML documents and imported through the codec that
# ships (tinyactors-scxml, ADR 0053): ta_definition_import decodes, compiles
# and registers them in the host.
module ScxmlCodecCharts
  NS = 'xmlns="http://www.w3.org/2005/07/scxml"'

  TinyactorsTest.scxml_chart :scxml_counter, <<~SCXML, files: { 'file:limits.json' => '{"max": 2}' }
    <scxml #{NS} version="1.0" name="scxml_counter" initial="counting">
      <datamodel>
        <data id="count" expr="0"/>
        <data id="limits" src="file:limits.json"/>
      </datamodel>
      <state id="counting">
        <transition event="tick" cond=".count &lt; .limits.max" type="internal">
          <assign location=".count" expr=".count + 1"/>
        </transition>
        <transition event="tick" target="full"/>
      </state>
      <final id="full"/>
    </scxml>
  SCXML

  # A parent whose child machine is inline markup: the codec serializes it
  # with its namespace (SCXML G.6), core hands the bytes to the invoker, and
  # the invoker imports them through the same codec.
  TinyactorsTest.scxml_chart :scxml_parent, <<~SCXML
    <scxml #{NS} version="1.0" name="scxml_parent" initial="waiting">
      <datamodel><data id="reply"/></datamodel>
      <state id="waiting">
        <invoke type="scxml">
          <param name="greeting" expr='"hello"'/>
          <content>
            <scxml #{NS} version="1.0" name="scxml_child" initial="answer">
              <datamodel><data id="greeting"/></datamodel>
              <final id="answer">
                <onentry><send event="child.said" target="#_parent"><param name="greeting" location=".greeting"/></send></onentry>
              </final>
            </scxml>
          </content>
        </invoke>
        <transition event="child.said" type="internal">
          <assign location=".reply" expr="$_event.data.greeting"/>
        </transition>
        <transition event="done.invoke" target="done"/>
      </state>
      <final id="done"/>
    </scxml>
  SCXML

  # An inline child the invoker has no reader for: its content is labelled
  # text/plain, not SCXML (ADR 0053: the invoker, not core, decides).
  TinyactorsTest.chart :scxml_foreign_child do
    state :parent do
      invoke type: 'scxml', content: '<scxml/>', media_type: 'text/plain'
      transition on: 'error.execution', target: :refused
    end
    state :refused
  end
end

TinyactorsTest.scenarios 'scxml codec' do
  # Catches: a document the host cannot run end to end — the codec, the
  # resolver for <data src>, and escaped conditions all have to work.
  scenario 'an SCXML document runs, with data fetched through the resolver' do
    counter = spawn :scxml_counter
    3.times { counter.deliver 'tick' }
    assert counter.done?, 'the counter reached its final state'
    assert_equal 2, counter.data('.count')
  end

  # Catches: inline markup that loses its namespace, or an invoker that
  # cannot read what the codec produced.
  scenario 'an inline SCXML child is imported by the invoker through the codec' do
    parent = spawn :scxml_parent
    assert parent.done?, 'the parent saw done.invoke'
    assert_equal 'hello', parent.data('.reply')
  end

  # Catches: an invoker that runs content whose media type it cannot read.
  scenario 'an inline chart in a media type the invoker has no reader for is refused' do
    parent = spawn :scxml_foreign_child
    assert_active parent, :refused
  end

  # Catches: a decode error that loses where it happened.
  scenario 'a malformed document is refused with its position' do
    response = ta.definition.import(system: system_handle, codec: { '$adapter' => 'codec.scxml' },
                                    input: "<scxml #{ScxmlCodecCharts::NS}>\n  <state id=\"a\" bogus=\"1\"/>\n</scxml>",
                                    datamodel: { '$adapter' => 'datamodel.jq' },
                                    allocator: { '$adapter' => 'allocator.libc' })
    assert_equal 'err_invalid', response[:result]
    error = response.dig(:out, :error)
    assert_equal ['decode', true, 2], [error[:stage], error[:located], error.dig(:at, :line)]
    assert_equal %w[unknown_attribute state bogus],
                 [error.dig(:as, :decode, :code) || error.dig(:decode, :code),
                  error.dig(:as, :decode, :construct) || error.dig(:decode, :construct),
                  error.dig(:as, :decode, :attribute) || error.dig(:decode, :attribute)]
  end

  # Catches: a semantic error in a document that says neither where nor what
  # (ADR 0058) — the compile error is located in the document and formatted.
  scenario 'a document with an unknown target is refused with its line, column and nearest state' do
    source = "<scxml #{ScxmlCodecCharts::NS} name=\"c\">\n  <state id=\"idle\">\n" \
             "    <transition event=\"go\" target=\"idel\"/>\n  </state>\n</scxml>"
    response = ta.definition.import(system: system_handle, codec: { '$adapter' => 'codec.scxml' }, input: source,
                                    datamodel: { '$adapter' => 'datamodel.jq' },
                                    allocator: { '$adapter' => 'allocator.libc' })
    assert_equal 'err_invalid', response[:result]
    error = response.dig(:out, :error)
    compile = error.dig(:as, :compile) || error[:compile]
    assert_equal ['compile', true, 3, 36], [error[:stage], error[:located], error.dig(:at, :line), error.dig(:at, :column)]
    assert_equal %w[unknown_state idel idle], [compile[:code], compile[:subject], compile[:related]]
    assert_equal '3:36: states[0].transitions[0].targets[0]: unknown state "idel"; nearest is "idle"',
                 TinyactorsTest.describe_import_error(ta, error)
  end

  # Catches: codec limits that do not reach the embedder, or a limit error
  # that does not say which bound.
  scenario 'a codec configured with limits refuses what exceeds them' do
    response = ta.definition.import(system: system_handle,
                                    codec: { '$adapter' => 'codec.scxml', 'config' => { 'max_depth' => 2 } },
                                    input: "<scxml #{ScxmlCodecCharts::NS}><state><final id=\"f\"/></state></scxml>",
                                    datamodel: { '$adapter' => 'datamodel.jq' },
                                    allocator: { '$adapter' => 'allocator.libc' })
    assert_equal 'err_invalid', response[:result]
    error = response.dig(:out, :error)
    decode = error.dig(:as, :decode) || error[:decode]
    assert_equal %w[limit max_depth], [decode[:code], decode[:construct]]
  end
end
