# frozen_string_literal: true

# The SCXML target URI grammar, asserted on the two functions that own it
# (ADR 0044, include/tinyactors.h): core parses every spelling of a `<send>`
# target with ta_scxml_target_parse and writes every URI it gives a chart —
# `$_event.origin`, `$_ioprocessors`, an inspected delayed send — with
# ta_scxml_target_format. `send_targets.rb` proves the two spellings of a
# target agree when a chart sends; this file proves the two functions are
# inverses, which is what makes that agreement possible.
module TargetURIs
  # [URI, the parsed target, the URI it formats back to, what the row proves]
  #
  # The parsed shape is the ta_scxml_target the codec encodes: `kind` and
  # the member its kind selects. Every spelling core accepts formats back to
  # itself, except the rows where parsing is deliberately not injective: an
  # unreadable session ID, and an address written other than core writes it.
  TABLE = [
    ['#_internal', { kind: 'internal' }, '#_internal',
     'the sender\'s own internal queue'],
    ['#_parent', { kind: 'parent' }, '#_parent',
     'the invoking parent session'],
    ['#_scxml_7', { kind: 'actor', actor: { kind: 'runtime', runtime: 7 } }, '#_scxml_7',
     'a session by its runtime ID'],
    ['#_actor_subscription/sub_1',
     { kind: 'actor', actor: { kind: 'named', family: { name: 'subscription' }, name: 'sub_1' } },
     '#_actor_subscription/sub_1',
     'a named actor by its logical address'],
    # Core ADR 0096: each part of an address is percent-encoded, so a family
    # or a name may hold '/' and '%'.
    ['#_actor_sub%2Fscription/a%2Fb%25c%20d',
     { kind: 'actor', actor: { kind: 'named', family: { name: 'sub/scription' }, name: 'a/b%c d' } },
     '#_actor_sub%2Fscription/a%2Fb%25c%20d',
     'an address holding "/", "%" and a space, encoded'],
    ['#_actor_subscription/caf%C3%A9',
     { kind: 'actor', actor: { kind: 'named', family: { name: 'subscription' }, name: 'café' } },
     '#_actor_subscription/caf%C3%A9',
     'a name that is not ASCII is encoded byte by byte'],
    ['#_actor_subscription/a%2fb c',
     { kind: 'actor', actor: { kind: 'named', family: { name: 'subscription' }, name: 'a/b c' } },
     '#_actor_subscription/a%2Fb%20c',
     'lower-case hex and a byte left unencoded are read, and written as core writes them'],
    # A session URI whose text is not a session ID names no session: it
    # parses as runtime actor 0, which never exists, so a send to it is
    # inaccessible (SCXML C.1, test496) rather than invalid. It is the one
    # spelling that does not format back to itself: every unreadable
    # session ID means the same absent session.
    ['#_scxml_4x', { kind: 'actor', actor: { kind: 'runtime', runtime: 0 } }, '#_scxml_0',
     'a malformed session URI is an inaccessible session, not an invalid target']
  ].freeze

  # Targets no grammar rule accepts: TA_ERR_INVALID, which a send reports
  # as error.execution (test194).
  INVALID = ['elsewhere', '', '#', '#_', '#_actor_', '#_actor_subscription/', '#_actor_/sub_1',
             '#_actor_subscription/a/b', '#_actor_subscription/a%', '#_actor_subscription/a%2',
             '#_actor_subscription/a%zz'].freeze
end

TinyactorsTest.scenarios 'target URIs' do
  # Catches: a parser and a writer that drift apart — the URI core hands a
  # chart as `$_event.origin` would then not parse back to the address it
  # came from, and a reply to that origin would go somewhere else.
  scenario 'every URI parses to its target and formats back to itself' do
    TargetURIs::TABLE.each do |text, expected, formatted, claim|
      result, target = parse_target(text)
      assert_equal 'ok', result, "#{text}: #{claim}"
      assert_equal expected, target, "#{text}: #{claim}"
      assert_equal ['ok', formatted, formatted.bytesize + 1], format_target(target),
                   "#{text} formats to #{formatted} (#{claim})"
      assert_equal ['ok', target], parse_target(formatted),
                   "#{formatted} parses to the same target again"
    end
  end

  # Catches: an invalid target core accepts, which a chart would then send
  # to with no error at all (test194 requires error.execution).
  scenario 'a target no rule accepts is invalid' do
    TargetURIs::INVALID.each do |text|
      assert_equal ['err_invalid', nil], parse_target(text), "#{text.inspect} is not a target"
    end
  end

  # `#_<invokeid>` names an invocation of the sending session. The ID is a
  # value of the system, so the same text always parses to the same ID and
  # different texts to different ones; SCXML does not define `#_self`, so
  # it reads as the invocation named "self" (see send_targets.rb).
  #
  # Catches: an invoke target parsed by position or by a counter, so that
  # `#_worker` in two charts of one system named two different invocations.
  scenario 'an invocation target is an ID of the system' do
    # A target is looked up and never interned (core ADR 0100): text that
    # names no invocation ID of the system names none.
    _, unknown = parse_target('#_worker')
    assert_equal 'invoke', unknown[:kind]
    assert_equal (2**64 - 1).to_s, unknown[:invoke].to_s, 'an ID no chart or invocation made is TA_INVOKE_ID_NONE'
    # Authors' IDs are interned with their charts.
    %w[worker helper self].each do |id|
      core('ta_system_intern_atom', ta.system.intern_atom(system: system_handle, text: id))
    end
    result, worker = parse_target('#_worker')
    assert_equal 'ok', result
    assert_equal 'invoke', worker[:kind]

    assert_equal ['ok', worker], parse_target('#_worker'), 'the same text is the same invocation'
    _, other = parse_target('#_helper')
    assert worker[:invoke] != other[:invoke], 'a different invoke ID for a different name'
    assert_equal ['ok', '#_worker', '#_worker'.bytesize + 1], format_target(worker)
    assert_equal ['ok', '#_helper', '#_helper'.bytesize + 1], format_target(other)

    _, self_target = parse_target('#_self')
    assert_equal 'invoke', self_target[:kind], 'SCXML defines no #_self, so it is an invocation name'
    assert_equal ['ok', '#_self', '#_self'.bytesize + 1], format_target(self_target)
  end

  # The list/buffer convention of the public API: a too-small buffer answers
  # err_nomem and writes nothing, always reporting what it needs, so a zero
  # capacity is a size query (include/tinyactors.h).
  #
  # Catches: a formatter that truncates into a short buffer, or that reports
  # a size a later call cannot use.
  scenario 'formatting reports the capacity it needs' do
    _, target = parse_target('#_actor_subscription/sub_1')
    needed = '#_actor_subscription/sub_1'.bytesize + 1

    assert_equal ['err_nomem', nil, needed], format_target(target, capacity: 0), 'a size query'
    assert_equal ['err_nomem', nil, needed], format_target(target, capacity: needed - 1), 'one byte short'
    assert_equal ['ok', '#_actor_subscription/sub_1', needed], format_target(target, capacity: needed),
                 'exactly the reported capacity is enough'
  end

  # Catches: a formatter that invents a URI for a target that has none,
  # which would put text no parser accepts into `$_event.origin`.
  scenario 'a target with no URI has none' do
    assert_equal ['err_not_found', nil, 0], format_target({ kind: 'invalid' })
  end

  # The URI core writes for a chart parses back to that chart's own
  # session: `$_ioprocessors.scxml.location` (SCXML C.1) is written with
  # ta_scxml_target_format, and reading it back is what a chart does when it
  # sends a reply address around.
  #
  # Catches: a location core writes in a spelling its own parser rejects, or
  # that parses to another session.
  scenario 'the location core gives a chart parses back to that session' do
    chart = TinyactorsTest::Chart.define(:uri_reader) do
      data location: nil
      state :ready do
        on_entry { assign '.location', '$_ioprocessors.scxml.location' }
      end
    end
    actor = spawn(chart)

    location = actor.data('.location')
    assert_equal actor.session_uri, location, 'the session URI of this actor'
    assert_equal ['ok', { kind: 'actor', actor: { kind: 'runtime', runtime: actor.handle } }],
                 parse_target(location), 'and it parses back to this session'
  end
end
