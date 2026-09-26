# frozen_string_literal: true

# Checks the debug adapter as a client does: it starts bin/dap over pipes,
# drives it with a small scripted DAP client against a workbench directory
# written for the test, and checks the responses and events of every
# message the adapter implements, and the errors of the ones it refuses.
#
#   ruby ruby/dap_selftest.rb
#   DAP_TRACE=1 ruby ruby/dap_selftest.rb    # and print every message

require 'json'
require 'pathname'
require 'timeout'
require 'tmpdir'

require_relative 'dap_client'

ROOT = Pathname.new(__dir__).join('..').expand_path
HOST = ROOT.join('build/tinyactors-test')
abort "tinyactors-test host not found at #{HOST}; run `mise run build`" unless HOST.executable?

FAILURES = []

def check(what)
  result = yield
  if result
    puts "  ok   #{what}"
  else
    FAILURES << what
    puts "  FAIL #{what}"
  end
rescue StandardError => error
  FAILURES << what
  puts "  FAIL #{what}: #{error.class}: #{error.message}"
  puts error.backtrace.first(4).map { |line| "         #{line}" }
end

COUNTER = <<~CHART
  TinyactorsTest.chart :counter do
    data count: 0
    data items: [1, 2]
    data last: nil
    initial :idle

    state :idle do
      transition on: 'bump', target: :busy do
        assign '.count', '.count + 1'
        assign '.last', '$_event.data'
      end
      transition on: 'ping', type: :internal do
        send_event 'pong', to_expr: '$_event.origin', type_expr: '$_event.origintype', data: '.count'
      end
      transition on: 'fail', type: :internal do
        assign '.count', 'error("boom")'
      end
      transition on: 'error.execution', type: :internal do
        assign '.last', '"failed"'
      end
      transition on: 'lost', type: :internal do
        send_event 'hello', to: '#_actor_counter/nobody'
      end
    end

    state :busy do
      on_entry do
        log 'busy', expr: '.count'
        foreach '.items', item: 'it' do
          assign '.count', '.count + .it'
        end
      end
      transition on: 'done', target: :idle
    end
  end
CHART

SUPERVISOR = <<~CHART
  TinyactorsTest.chart :supervisor do
    data ticks: 0
    initial :supervising
    state :supervising do
      on_entry { send_event 'tick', delay: '5s', id: :tick }
      invoke chart: :worker, id: :worker
      transition on: 'tick', type: :internal do
        assign '.ticks', '.ticks + 1'
      end
      transition on: 'poke', type: :internal do
        send_event 'bump', to: '#_actor_counter/acct1'
      end
    end
  end
CHART

# A macrostep that never ends: only a pause gets it to stop.
SPINNER = <<~CHART
  TinyactorsTest.chart :spinner do
    data n: 0
    initial :resting
    state :resting do
      transition on: 'spin', target: :spinning
    end
    state :spinning do
      transition cond: 'true', target: :spinning do
        assign '.n', '.n + 1'
      end
    end
  end
CHART

WORKER = <<~CHART
  TinyactorsTest.chart :worker do
    initial :working
    state :working do
      transition on: 'finish', target: :finished
    end
    final :finished
  end
CHART

ACTORS = <<~ACTORS
  Workbench.actor :acct1, chart: :counter
  Workbench.actor :boss, chart: :supervisor
  Workbench.actor :spinner, chart: :spinner
ACTORS

# The line of a chart file that holds `needle`, from 1.
def line_of(text, needle) = text.lines.index { |line| line.include?(needle) } + 1

Dir.mktmpdir('dap-selftest') do |tmp|
  dir = Pathname.new(tmp)
  dir.join('counter.rb').write(COUNTER)
  dir.join('supervisor.rb').write(SUPERVISOR)
  dir.join('worker.rb').write(WORKER)
  dir.join('spinner.rb').write(SPINNER)
  counter_path = dir.join('counter.rb').realpath.to_s
  dir.join('actors.rb').write(ACTORS)

  client = DAPClient.new([RbConfig.ruby, ROOT.join('bin/dap').to_s])
  begin
    puts 'initialize'
    response = client.request('initialize', 'adapterID' => 'tinyactors', 'linesStartAt1' => true,
                                            'columnsStartAt1' => true, 'supportsInvalidatedEvent' => true)
    capabilities = response['body']
    check('initialize succeeds') { response['success'] }
    check('declares what it implements') do
      %w[supportsConfigurationDoneRequest supportsFunctionBreakpoints supportsDataBreakpoints
         supportsSetVariable supportsSetExpression supportsCompletionsRequest supportsSteppingGranularity
         supportsModulesRequest supportsLoadedSourcesRequest supportsExceptionInfoRequest
         supportsTerminateRequest supportsEvaluateForHovers supportsClipboardContext
         supportsConditionalBreakpoints supportsHitConditionalBreakpoints supportsGotoTargetsRequest
         supportsBreakpointLocationsRequest supportsLogPoints].all? { |name| capabilities[name] == true }
    end
    check('declares nothing it does not offer') do
      %w[supportsInstructionBreakpoints supportsDisassembleRequest supportsReadMemoryRequest
         supportsStepInTargetsRequest supportsTerminateThreadsRequest supportsStepBack supportsRestartFrame
         supportsRestartRequest].none? { |name| capabilities[name] }
    end
    check('never terminates or suspends the debuggee') do
      capabilities['supportTerminateDebuggee'] == false && capabilities['supportSuspendDebuggee'] == false
    end
    check('the exception filters') do
      capabilities['exceptionBreakpointFilters'].map { |filter| filter['filter'] } ==
        %w[error.execution error.communication error.platform ignored stopped]
    end
    client.event('initialized')
    check('initialized follows') { true }

    puts 'attach and launch'
    attach = client.request('attach', 'address' => '#_actor_counter/acct1')
    check('attach is refused, pointing at launch') { !attach['success'] && attach['message'].include?('launch') }
    bad = client.request('launch', 'workbench' => dir.join('missing').to_s)
    check('launch of a missing directory is refused') { !bad['success'] && bad['message'].include?('not a directory') }
    launch = client.request('launch', 'workbench' => dir.to_s, 'stopOnEntry' => true)
    check('launch succeeds') { launch['success'] }
    process = client.event('process')
    check('a process event names the workbench') do
      process['name'] == dir.to_s && process['startMethod'] == 'launch' && process['systemProcessId'].positive?
    end

    puts 'configuration'
    functions = client.request('setFunctionBreakpoints',
                               'breakpoints' => [{ 'name' => 'busy' }, { 'name' => 'exit:busy' }])
    check('function breakpoints are verified') do
      functions['success'] && functions['body']['breakpoints'].size == 2 &&
        functions['body']['breakpoints'].all? { |breakpoint| breakpoint['verified'] }
    end
    # A condition that never holds, so it never stops the flows below.
    early = client.request('setBreakpoints', 'source' => { 'path' => counter_path },
                                             'breakpoints' => [{ 'line' => line_of(COUNTER, "on: 'done'"),
                                                                 'condition' => 'false' }])
    check('a line breakpoint set before the actors start is not verified yet') do
      early['success'] && early['body']['breakpoints'].first['verified'] == false
    end
    unknown = client.request('setExceptionBreakpoints', 'filters' => ['error.nonsense'])
    check('an unknown exception filter is refused') { !unknown['success'] }
    check('exception filters are accepted') do
      client.request('setExceptionBreakpoints', 'filters' => ['error.execution'])['success']
    end
    done = client.request('configurationDone')
    check('configurationDone succeeds') { done['success'] }
    entry = client.event('stopped')
    check('stopOnEntry stops with reason entry') { entry['reason'] == 'entry' && entry['allThreadsStopped'] }
    verified = client.event('breakpoint')
    check('it is verified once its chart is loaded: a breakpoint event') do
      verified['reason'] == 'changed' && verified.dig('breakpoint', 'verified') &&
        verified.dig('breakpoint', 'line') == line_of(COUNTER, "on: 'done'") &&
        verified.dig('breakpoint', 'id') == early['body']['breakpoints'].first['id']
    end

    puts 'threads'
    threads = client.request('threads')['body']['threads']
    by_name = ->(prefix) { threads.find { |thread| thread['name'].start_with?(prefix) } }
    acct1 = by_name.call('counter/acct1')
    check('a thread per named actor, named by address and leaf states') do
      acct1 && acct1['name'] == 'counter/acct1 · idle' && by_name.call('supervisor/boss · supervising')
    end
    check('an invoked child is a thread too, named by its parent and invocation') do
      by_name.call('supervisor/boss invocation worker · working')
    end
    started = client.take('thread')
    check('thread events announced them') do
      started.map { |body| body['threadId'] }.sort == threads.map { |thread| thread['id'] }.sort &&
        started.all? { |body| body['reason'] == 'started' }
    end
    tid = acct1['id']

    puts 'modules and sources'
    modules = client.request('modules')['body']
    check('a module per definition revision') do
      names = modules['modules'].map { |mod| mod['name'] }.sort
      names == %w[counter spinner supervisor worker] && modules['totalModules'] == 4 &&
        modules['modules'].all? { |mod| mod['version'].to_s.size == 16 }
    end
    check('module events announced them') { client.take('module').size == 4 }
    paged = client.request('modules', 'startModule' => 1, 'moduleCount' => 1)['body']
    check('modules page') { paged['modules'].size == 1 && paged['totalModules'] == 4 }
    sources = client.request('loadedSources')['body']['sources']
    counter_source = sources.find { |source| source['path'] == dir.join('counter.rb').to_s }
    check('a loaded source per revision, with its file') { sources.size == 4 && counter_source }
    check('loadedSource events announced them') { client.take('loadedSource').size == 4 }
    text = client.request('source', 'sourceReference' => counter_source['sourceReference'],
                                    'source' => counter_source)['body']
    check('source answers the chart file text') { text['content'] == COUNTER }
    check('an unknown source is refused') { !client.request('source', 'sourceReference' => 999)['success'] }

    puts 'stack and scopes at rest'
    stack = client.request('stackTrace', 'threadId' => tid)['body']
    check('an idle actor has one frame, which says what holds it') do
      stack['totalFrames'] == 1 && stack['stackFrames'].first['name'] == 'idle in idle (held; 0 messages waiting)'
    end
    check('at the line its state was written on (core ADR 0086)') do
      top = stack['stackFrames'].first
      top.dig('source', 'path') == counter_path && top['line'] == line_of(COUNTER, 'state :idle do')
    end
    frame = stack['stackFrames'].first['id']
    scopes = client.request('scopes', 'frameId' => frame)['body']['scopes']
    check('seven scopes') do
      scopes.map { |scope| scope['name'] } == ['Data model', 'Event', 'Configuration', 'Queues', 'Timers',
                                               'Invocations', 'System']
    end
    scope = ->(name) { scopes.find { |candidate| candidate['name'] == name }['variablesReference'] }
    vars = ->(ref, **args) { client.request('variables', { 'variablesReference' => ref }.merge(args.transform_keys(&:to_s)))['body']['variables'] }
    data = vars.call(scope.call('Data model'))
    check('the data model') do
      data.map { |variable| [variable['name'], variable['value']] } ==
        [['count', '0'], ['items', '[1,2]'], ['last', 'null']]
    end
    items = data.find { |variable| variable['name'] == 'items' }
    check('a container expands, with its evaluateName') do
      children = vars.call(items['variablesReference'])
      items['indexedVariables'] == 2 && children.map { |variable| [variable['name'], variable['evaluateName']] } ==
        [['[0]', '.items[0]'], ['[1]', '.items[1]']]
    end
    check('variables page') do
      vars.call(items['variablesReference'], start: 1, count: 1).map { |variable| variable['value'] } == ['2']
    end
    configuration = vars.call(scope.call('Configuration'))
    check('the configuration') { configuration.first['name'] == 'active' && configuration.first['value'] == '{"idle":{}}' }
    system = vars.call(scope.call('System')).to_h { |variable| [variable['name'], variable] }
    check('the system scope, named as napi names it, with _ioprocessors besides') do
      processors = vars.call(system['_ioprocessors']['variablesReference']).map { |variable| variable['name'] }
      system.keys == %w[_sessionid _name _ioprocessors now actor definition residency scheduling published
                        allocatedBytes] &&
        system['_name']['value'] == '"counter"' && system['now']['value'] == '0' && processors.include?('debug') &&
        system['actor']['value'].match?(/\A"\d+:\d+"\z/) && system['scheduling']['value'] == '"held"'
    end
    boss = by_name.call('supervisor/boss')['id']
    boss_frame = client.request('stackTrace', 'threadId' => boss)['body']['stackFrames'].first['id']
    boss_scopes = client.request('scopes', 'frameId' => boss_frame)['body']['scopes']
    invocations = vars.call(boss_scopes.find { |candidate| candidate['name'] == 'Invocations' }['variablesReference'])
    check('the invocations, each with its thread') do
      invoke = vars.call(invocations.first['variablesReference']).to_h { |variable| [variable['name'], variable['value']] }
      invocations.size == 1 && invocations.first['name'] == 'worker' &&
        invoke['thread'].include?('supervisor/boss invocation worker') && invoke['active'] == 'true'
    end
    check('an unknown reference is refused') { !client.request('variables', 'variablesReference' => 9999)['success'] }

    puts 'a chart without spans: its encoded SCXML'
    worker_thread = threads.find { |thread| thread['name'].start_with?('supervisor/boss invocation worker') }['id']
    worker_top = client.request('stackTrace', 'threadId' => worker_thread)['body']['stackFrames'].first
    reference = worker_top.dig('source', 'sourceReference').to_i
    encoded = client.request('source', 'sourceReference' => reference)['body']
    check('an inline child chart has no spans; its frame is in the SCXML the codec encodes from it (ADR 0086)') do
      worker_top['name'] == 'idle in working (held; 0 messages waiting)' && reference.positive? && worker_top.dig('source', 'name') == 'worker.scxml' &&
        encoded['mimeType'] == 'application/scxml+xml' &&
        encoded['content'].lines[worker_top['line'] - 1].include?('<state id="working">')
    end
    check('and it is a loaded source of its own, by reference') do
      client.request('loadedSources')['body']['sources'].any? do |source|
        source['sourceReference'] == reference && source['path'].nil? && source['name'].start_with?('worker.scxml@')
      end
    end

    puts 'evaluate'
    evaluate = ->(expression, context, frame_id = frame) do
      arguments = { 'expression' => expression, 'context' => context }
      arguments['frameId'] = frame_id if frame_id
      client.request('evaluate', arguments)
    end
    check('watch evaluates jq over the data model') { evaluate.call('.count + 1', 'watch')['body']['result'] == '1' }
    hover = evaluate.call('.items', 'hover')['body']
    check('hover answers a container that expands') do
      hover['result'] == '[1,2]' && hover['indexedVariables'] == 2 &&
        vars.call(hover['variablesReference']).map { |variable| variable['value'] } == %w[1 2]
    end
    check('clipboard answers the whole value') do
      evaluate.call('.items', 'clipboard')['body']['result'] == JSON.pretty_generate([1, 2])
    end
    check('the variables context') { evaluate.call('.last', 'variables')['body']['result'] == 'null' }
    check('the system variables and In()') do
      evaluate.call('[$_name, In("idle"), In("busy"), ($_sessionid | length > 0)]', 'repl')['body']['result'] ==
        '["counter",true,false,true]'
    end
    broken = evaluate.call('.count +', 'watch')
    check('an expression jq refuses is an error') { !broken['success'] && !broken['message'].to_s.empty? }
    check('several values come back as a list') { evaluate.call('.items[]', 'repl')['body']['result'] == '[1,2]' }
    { 'replace counter.rb' => 'save the chart file',
      'goto nowhere' => 'not a state', 'goto idle busy' => 'cannot be entered together',
      'abandon' => 'no macrostep to abandon', 'cancel 12345' => 'no pending send 12345',
      'cancel dunning' => 'no pending send dunning' }.each do |command, why|
      answer = evaluate.call(command, 'repl')
      check("#{command} is refused, saying why") { !answer['success'] && answer['message'].include?(why) }
    end
    check('clock needs a +duration') { !evaluate.call('clock 5s', 'repl')['success'] }

    puts 'completions'
    complete = ->(text, column = text.size + 1) do
      client.request('completions', 'text' => text, 'column' => column, 'frameId' => frame)['body']['targets']
    end
    check('after send: the events the actor listens for') do
      labels = complete.call('send ').map { |target| target['label'] }
      %w[bump done fail ping].all? { |name| labels.include?(name) }
    end
    check('a partial event name, replacing what was typed') do
      targets = complete.call('send bu')
      targets.map { |target| [target['label'], target['start'], target['length']] } == [['bump', 6, 2]]
    end
    check('another actor\'s events after send @name') do
      complete.call('send @boss ').map { |target| target['label'] } == %w[poke tick] &&
        complete.call('send @acct1 d').map { |target| target['label'] } == ['done']
    end
    check('data locations') { complete.call('.it').map { |target| target['label'] } == ['items'] }
    check('console commands') { complete.call('cl').map { |target| target['label'] } == ['clock'] }
    check('state IDs after goto, and after from') do
      complete.call('goto ').map { |target| target['label'] } == %w[idle busy] &&
        complete.call('goto busy from i').map { |target| target['label'] } == ['idle'] &&
        complete.call('goto @boss s').map { |target| target['label'] } == ['supervising']
    end

    puts 'send, continue and function breakpoints'
    busy_id, exit_id = functions['body']['breakpoints'].map { |breakpoint| breakpoint['id'] }
    sent = evaluate.call('send bump {"n": 1}', 'repl')['body']
    check('send puts the event in the frame\'s actor\'s mailbox, where it waits while the system is stopped') do
      sent['result'] == 'sent bump to counter/acct1; it waits in the mailbox (1 message): continue or step to deliver it'
    end
    stack = client.request('stackTrace', 'threadId' => tid)['body']['stackFrames']
    check('the actor rests, holding nothing yet') do
      stack.map { |f| f['name'] } == ['idle in idle (held; 1 message waiting)']
    end
    frame_of = ->(thread) { client.request('stackTrace', 'threadId' => thread)['body']['stackFrames'] }
    # The frames down to the macrostep; the causal chain after them has
    # checks of its own.
    names_of = ->(thread) { frame_of.call(thread).take_while { |f| f['presentationHint'] != 'label' }.map { |f| f['name'] } }
    chain_of = ->(thread) { frame_of.call(thread).drop_while { |f| f['presentationHint'] != 'label' } }
    scope_ref = ->(frame_id, name) do
      client.request('scopes', 'frameId' => frame_id)['body']['scopes'].find { |scope| scope['name'] == name }['variablesReference']
    end
    queues = vars.call(scope_ref.call(stack.first['id'], 'Queues')).to_h { |variable| [variable['name'], variable] }
    check('the Queues scope shows it in the mailbox, as the console\'s') do
      mail = vars.call(queues['mailbox']['variablesReference'])
      message = vars.call(mail.first['variablesReference']).to_h { |variable| [variable['name'], variable['value']] }
      mail.size == 1 && message == { 'event' => '"bump"', 'data' => '{"n":1}', 'from' => '"this console"' }
    end
    continued = client.request('continue', 'threadId' => tid)
    check('continue answers for every thread') { continued['success'] && continued['body']['allThreadsContinued'] }
    stopped = client.event('stopped')
    check('an entry breakpoint stops before the state\'s onentry') do
      stopped['reason'] == 'function breakpoint' && stopped['hitBreakpointIds'] == [busy_id] &&
        stopped['description'] == 'entered busy' && stopped['threadId'] == tid
    end
    names = names_of.call(tid)
    check('frames: the next unit, its block, the microstep, the macrostep') do
      names == ['log 1', 'onentry of busy', 'microstep 2.1: idle → busy on bump', 'macrostep 2 on bump']
    end
    check('each at the line its element was written on') do
      frame_of.call(tid).take_while { |f| f['presentationHint'] != 'label' }.map { |f| f['line'] } ==
        [line_of(COUNTER, "log 'busy'"), line_of(COUNTER, 'state :busy do'), line_of(COUNTER, "on: 'bump'"), 0]
    end
    top = frame_of.call(tid).first['id']
    event = vars.call(scope_ref.call(top, 'Event')).to_h { |variable| [variable['name'], variable['value']] }
    check('the Event scope is _event, with SCXML\'s names') do
      event['name'] == '"bump"' && event['type'] == '"external"' && event['origin'] == '"debug://console"' &&
        event['origintype'] == '"debug"'
    end
    check('expressions see $_event') { evaluate.call('$_event.data.n', 'watch', top)['body']['result'] == '1' }
    check('the causal chain of an event the console sent') do
      chain_of.call(tid).map { |f| f['name'] } == ['caused by', 'the debug console sent bump']
    end
    check('the transition\'s content ran') { evaluate.call('[.count, .last]', 'watch', top)['body']['result'] == '[1,{"n":1}]' }
    check('the macrostep summary is not written before the macrostep ends') do
      client.outputs.none? { |text| text.include?('macrostep 2') }
    end

    puts 'stepping'
    step = ->(command, **args) do
      answer = client.request(command, { 'threadId' => tid }.merge(args.transform_keys(&:to_s)))
      [answer, client.event('stopped')]
    end
    answer, stopped = step.call('stepIn')
    check('stepIn runs one unit and stops with reason step') { answer['success'] && stopped['reason'] == 'step' }
    check('the unit was the <log>, whose record is stdout output, as `label: value`') do
      client.take('output').any? { |body| body['category'] == 'stdout' && body['output'] == "busy: 1\n" }
    end
    top_names = -> { names_of.call(tid) }
    check('the top frame is the next unit: a <foreach> that has not run') do
      top_names.call.first(2) == ['foreach 2 iteration 1', 'onentry of busy']
    end
    step.call('stepIn', granularity: 'instruction')
    check('instruction granularity stops at every unit, even where the position repeats (ADR 0083)') do
      top_names.call.first(2) == ['foreach 2 iteration 1', 'onentry of busy']
    end
    step.call('stepIn')
    check('stepIn goes into the body: its first unit, in the first iteration') do
      top_names.call.first(3) == ['assign 1', 'foreach 2 iteration 1', 'onentry of busy']
    end
    step.call('stepIn')
    check('then back to the loop, before its second iteration') do
      top_names.call.first(2) == ['foreach 2 iteration 2', 'onentry of busy'] &&
        evaluate.call('.count', 'watch', frame_of.call(tid).first['id'])['body']['result'] == '2'
    end
    check('the macrostep summary is not written before the macrostep ends') do
      client.outputs.none? { |text| text.include?('macrostep 2') }
    end
    answer, stopped = step.call('stepOut')
    check('stepOut leaves the block; its microstep was the last, so the macrostep ends') do
      stopped['reason'] == 'step' && top_names.call == ['idle in busy (held; 0 messages waiting)']
    end
    check('the macrostep summary is written when it ends, in one output') do
      summary = client.outputs.find { |text| text.start_with?('counter/acct1 macrostep 2: bump') }
      summary == <<~SUMMARY
        counter/acct1 macrostep 2: bump {"n":1}
          idle → busy on bump
            assign .count = 1
            assign .last = {"n":1}
            assign .count = 2
            assign .count = 4
          now in busy
      SUMMARY
    end
    check('at rest, the Event scope shows the event of the last macrostep, marked as such') do
      rest = vars.call(scope_ref.call(frame_of.call(tid).first['id'], 'Event')).to_h { |variable| [variable['name'], variable['value']] }
      rest['name'] == '"bump"' && rest['taken'] == '"in its last macrostep"'
    end
    check('the thread is renamed by its new state') do
      client.request('threads')['body']['threads'].find { |thread| thread['id'] == tid }['name'] == 'counter/acct1 · busy'
    end
    check('invalidated followed the macrostep') do
      client.take('invalidated').any? { |body| body['areas'].include?('variables') }
    end

    summary = ->(text) { client.event('output') { |body| body['output'].include?(text) }['output'] }
    # Waits for the summary of the macrostep an actor ran on an event.
    macrostep = ->(who, event) do
      client.event('output') do |body|
        body['output'].match?(/\A#{Regexp.escape(who)} macrostep \d+: #{Regexp.escape(event)}[ \n]/)
      end
    end

    puts 'an exit breakpoint, and a step of an idle actor'
    evaluate.call('send done', 'repl', nil)
    client.request('continue', 'threadId' => tid)
    stopped = client.event('stopped')
    check('an exit breakpoint in a microstep without content stops at its start, before any of it ran (ADR 0087)') do
      stopped['reason'] == 'function breakpoint' && stopped['hitBreakpointIds'] == [exit_id] &&
        stopped['description'] == 'exited busy' &&
        names_of.call(tid) ==
          ['transition busy → idle on done', 'microstep 3.1: busy → idle on done', 'macrostep 3 on done'] &&
        client.request('evaluate', 'expression' => 'In("busy")', 'context' => 'watch')['body']['result'] == 'true'
    end
    step.call('next')
    check('next from the start of the last microstep finishes the macrostep') do
      names_of.call(tid) == ['idle in idle (held; 0 messages waiting)']
    end
    answer = client.request('next', 'threadId' => tid)
    continued = client.event('continued')
    check('next on an idle actor answers at once, and everything runs') do
      answer['success'] && continued['threadId'] == tid && continued['allThreadsContinued']
    end
    check('the console says what the actor waits for') do
      summary.call('counter/acct1 waits for its next message').include?('`send` it one')
    end
    client.request('evaluate', 'expression' => 'send ping', 'context' => 'repl')
    stopped = client.event('stopped')
    check('the step stops when the actor takes its next event') do
      stopped['reason'] == 'step' && stopped['description'] == 'about to take ping' && stopped['threadId'] == tid
    end
    check('before transition selection') do
      names_of.call(tid) == ['about to take ping (before transition selection)']
    end
    step.call('stepIn', granularity: 'line')
    check('a step by microsteps lands at the microstep\'s start: its first unit has not run') do
      names_of.call(tid) == ['send 1', 'transition idle on ping (internal)', 'microstep 4.1: idle on ping (internal)',
                             'macrostep 4 on ping'] &&
        client.outputs.none? { |text| text.include?('replied pong') }
    end
    step.call('stepIn', granularity: 'line')
    check('the actor answers the console: its reply arrives as output') do
      summary.call('counter/acct1 replied pong').include?('counter/acct1 replied pong 4')
    end
    client.request('continue', 'threadId' => tid)
    macrostep.call('counter/acct1', 'ping')

    puts 'exceptions'
    client.request('evaluate', 'expression' => 'send fail', 'context' => 'repl')
    stopped = client.event('stopped')
    check('error.execution stops with reason exception') do
      stopped['reason'] == 'exception' && stopped['threadId'] == tid && stopped['text'].include?('error.execution')
    end
    info = client.request('exceptionInfo', 'threadId' => tid)['body']
    check('exceptionInfo names the error and why') do
      info['exceptionId'] == 'error.execution' && info['breakMode'] == 'always' &&
        info['description'] ==
          'error.execution: <assign> in idle on fail (internal): JSON expression produced no value'
    end
    check('exceptionInfo of a thread that did not fail is an error') do
      !client.request('exceptionInfo', 'threadId' => boss)['success']
    end
    client.request('setExceptionBreakpoints', 'filters' => %w[ignored error.communication])
    client.request('continue', 'threadId' => tid)
    macrostep.call('counter/acct1', 'fail')
    client.request('evaluate', 'expression' => 'send lost', 'context' => 'repl')
    stopped = client.event('stopped')
    check('error.communication: a send nobody can take') do
      stopped['reason'] == 'exception' && stopped['text'].start_with?('error.communication')
    end
    # The chart has no transition for the error.communication either; it
    # was ignored in the same resume, and one stop reports the first.
    client.request('continue', 'threadId' => tid)
    macrostep.call('counter/acct1', 'lost')
    client.request('evaluate', 'expression' => 'send nothing.here', 'context' => 'repl')
    stopped = client.event('stopped')
    check('ignored: an external event, too') do
      stopped['reason'] == 'exception' && stopped['text'] == 'nothing.here enabled no transition' &&
        client.request('exceptionInfo', 'threadId' => tid)['body']['exceptionId'] == 'nothing.here'
    end
    client.request('setExceptionBreakpoints', 'filters' => [])
    client.request('continue', 'threadId' => tid)
    macrostep.call('counter/acct1', 'nothing.here')
    check('the chart handled its error.execution') do
      client.request('evaluate', 'expression' => '.last', 'context' => 'watch')['body']['result'] == '"failed"'
    end

    puts 'pause at rest, data breakpoints'
    client.request('pause', 'threadId' => tid)
    stopped = client.event('stopped')
    check('pausing a system at rest stops at once') do
      stopped['reason'] == 'pause' && stopped['threadId'] == tid && stopped['description'] == 'paused'
    end
    top = frame_of.call(tid).first['id']
    data_ref = scope_ref.call(top, 'Data model')
    info = client.request('dataBreakpointInfo', 'variablesReference' => data_ref, 'name' => 'count')['body']
    check('a data-model location can be watched for writes') do
      info['dataId'] == 'counter/acct1 ["count"]' && info['accessTypes'] == ['write'] &&
        info['description'] == 'counter/acct1 .count'
    end
    by_expression = client.request('dataBreakpointInfo', 'name' => '.items[1]', 'frameId' => top)['body']
    check('so can one named by an expression') { by_expression['dataId'] == 'counter/acct1 ["items",1]' }
    event_ref = scope_ref.call(top, 'Event')
    check('other scopes cannot') do
      client.request('dataBreakpointInfo', 'variablesReference' => event_ref, 'name' => 'name')['body']['dataId'].nil?
    end
    read = client.request('setDataBreakpoints', 'breakpoints' => [{ 'dataId' => info['dataId'], 'accessType' => 'read' }])
    check('reads cannot be watched') { read['body']['breakpoints'].first['verified'] == false }
    set = client.request('setDataBreakpoints', 'breakpoints' => [{ 'dataId' => info['dataId'] }])
    check('writes can') { set['body']['breakpoints'].first['verified'] }
    client.request('setFunctionBreakpoints', 'breakpoints' => [])
    evaluate.call('send bump', 'repl', top)
    client.request('continue', 'threadId' => tid)
    stopped = client.event('stopped')
    check('an assignment to the location stops after it') do
      stopped['reason'] == 'data breakpoint' && stopped['description'] == 'count = 5' &&
        stopped['hitBreakpointIds'] == set['body']['breakpoints'].map { |breakpoint| breakpoint['id'] }
    end
    client.request('setDataBreakpoints', 'breakpoints' => [])
    client.request('continue', 'threadId' => tid)
    macrostep.call('counter/acct1', 'bump')

    puts 'a breakpoint on an event'
    on_ping = client.request('setFunctionBreakpoints', 'breakpoints' => [{ 'name' => 'on:ping' }])['body']['breakpoints']
    client.request('evaluate', 'expression' => 'send ping', 'context' => 'repl')
    stopped = client.event('stopped')
    check('on: stops before the macrostep that takes the event') do
      stopped['reason'] == 'function breakpoint' && stopped['hitBreakpointIds'] == [on_ping.first['id']] &&
        stopped['description'] == 'about to take ping' &&
        names_of.call(tid) == ['about to take ping (before transition selection)']
    end
    client.request('continue', 'threadId' => tid)
    macrostep.call('counter/acct1', 'ping')
    check('and does not stop again when the macrostep begins') { client.take('stopped').empty? }

    puts 'conditions and hit conditions'
    conditional = client.request('setFunctionBreakpoints', 'breakpoints' => [
                                   { 'name' => 'on:done', 'condition' => '.count > 1000' },
                                   { 'name' => 'idle -> busy', 'hitCondition' => '2' }
                                 ])['body']['breakpoints']
    client.request('evaluate', 'expression' => 'send done', 'context' => 'repl')
    macrostep.call('counter/acct1', 'done')
    check('a breakpoint whose condition is false does not stop') { client.take('stopped').empty? }
    client.request('evaluate', 'expression' => 'send bump', 'context' => 'repl')
    macrostep.call('counter/acct1', 'bump')
    client.request('evaluate', 'expression' => 'send done', 'context' => 'repl')
    macrostep.call('counter/acct1', 'done')
    check('the first hit of a hit condition of 2 does not stop') { client.take('stopped').empty? }
    client.request('evaluate', 'expression' => 'send bump', 'context' => 'repl')
    stopped = client.event('stopped')
    check('the second does, before the transition\'s content: at the microstep\'s start (ADR 0087)') do
      stopped['reason'] == 'function breakpoint' && stopped['hitBreakpointIds'] == [conditional.last['id']] &&
        names_of.call(tid).first(2) == ['assign 1', 'transition idle → busy on bump']
    end

    puts 'writing data'
    data_ref = scope_ref.call(frame_of.call(tid).first['id'], 'Data model')
    written = client.request('setVariable', 'variablesReference' => data_ref, 'name' => 'count', 'value' => '42')
    check('setVariable writes a data-model location') do
      written['success'] && written['body']['value'] == '42' &&
        client.request('evaluate', 'expression' => '.count', 'context' => 'watch')['body']['result'] == '42'
    end
    check('the actor stays where it was stopped, and keeps its thread') do
      names_of.call(tid).first(2) == ['assign 1', 'transition idle → busy on bump']
    end
    data_ref = scope_ref.call(frame_of.call(tid).first['id'], 'Data model')
    check('a value may be an expression') do
      client.request('setVariable', 'variablesReference' => data_ref, 'name' => 'count', 'value' => '.count + 1')['body']['value'] == '43'
    end
    expression = client.request('setExpression', 'expression' => '.last', 'value' => '{"x": 1}',
                                                 'frameId' => frame_of.call(tid).first['id'])
    check('setExpression writes the location an expression names') do
      expression['success'] && client.request('evaluate', 'expression' => '.last.x', 'context' => 'watch')['body']['result'] == '1'
    end
    configuration_ref = scope_ref.call(frame_of.call(tid).first['id'], 'Configuration')
    check('the other scopes are read-only') do
      answer = client.request('setVariable', 'variablesReference' => configuration_ref, 'name' => 'active', 'value' => '{}')
      !answer['success'] && answer['message'].include?('read-only')
    end
    boss_ref = scope_ref.call(frame_of.call(boss).first['id'], 'Data model')
    check('an actor with an active invocation is refused, saying why') do
      answer = client.request('setVariable', 'variablesReference' => boss_ref, 'name' => 'ticks', 'value' => '1')
      !answer['success'] && answer['message'].include?('active invocation')
    end
    worker = client.request('threads')['body']['threads']
                   .find { |thread| thread['name'].start_with?('supervisor/boss invocation worker') }['id']
    worker_ref = scope_ref.call(frame_of.call(worker).first['id'], 'Data model')
    check('an invoked child is refused, saying why') do
      answer = client.request('setVariable', 'variablesReference' => worker_ref, 'name' => 'x', 'value' => '1')
      !answer['success'] && answer['message'].include?('invoked child')
    end
    client.request('setFunctionBreakpoints', 'breakpoints' => [])
    client.request('continue', 'threadId' => tid)
    macrostep.call('counter/acct1', 'bump')
    check('the macrostep finished on the written data') do
      client.request('evaluate', 'expression' => '.count', 'context' => 'watch')['body']['result'] == '47'
    end

    puts 'timers and the clock'
    boss_top = frame_of.call(boss).first['id']
    timers = vars.call(scope_ref.call(boss_top, 'Timers'))
    check('the Timers scope lists pending delayed sends, by the ID the chart gave each') do
      timer = vars.call(timers.first['variablesReference']).to_h { |variable| [variable['name'], variable['value']] }
      timers.size == 1 && timers.first['name'] == 'tick' && timer['event'] == '"tick"' && timer['due'] == '5000'
    end
    clock = client.request('evaluate', 'expression' => 'clock +5s', 'context' => 'repl')['body']
    check('clock advances logical time') { clock['result'] == 't=5000ms' }
    macrostep.call('supervisor/boss', 'tick')
    check('and what falls due runs') do
      client.request('evaluate', 'expression' => '.ticks', 'context' => 'watch',
                                 'frameId' => frame_of.call(boss).first['id'])['body']['result'] == '1'
    end

    puts 'hot reload'
    client.take('module')
    client.take('loadedSource')
    client.request('pause', 'threadId' => tid)
    client.event('stopped') { |body| body['reason'] == 'pause' }
    old_worker = worker
    dir.join('supervisor.rb').write(SUPERVISOR.sub('data ticks: 0', "data ticks: 0\n  data revision: 2"))
    changed = client.event('module') { |body| body['reason'] == 'changed' }
    check('the retired revision is marked') { changed['module']['name'] == 'supervisor (retired)' }
    added = client.event('module') { |body| body['reason'] == 'new' }
    check('saving a chart announces its new revision') do
      added['module']['name'] == 'supervisor' && added['module']['id'] != changed['module']['id']
    end
    check('and its source') do
      source = client.event('loadedSource') { |body| body['reason'] == 'new' }['source']
      client.request('source', 'sourceReference' => source['sourceReference'])['body']['content'].include?('revision: 2')
    end
    check('the reaped child\'s thread exited') do
      client.event('thread') { |body| body['reason'] == 'exited' }['threadId'] == old_worker
    end
    check('the replaced actor keeps its thread') do
      client.request('threads')['body']['threads'].any? { |thread| thread['id'] == boss }
    end
    client.request('pause', 'threadId' => boss)
    stopped = client.event('stopped') { |body| body['reason'] == 'pause' }
    check('pause names the thread asked for when all rest') { stopped['threadId'] == boss }
    dir.join('supervisor.rb').write(SUPERVISOR.sub('data ticks: 0', "data ticks: 0\n  data revision: 3"))
    check('what a reload loses is important output') do
      client.event('output') { |body| body['category'] == 'important' }['output'].include?('delayed send(s) dropped')
    end
    continued = client.event('continued')
    check('when a reload replaces the stopped actor, everything runs again') do
      continued['threadId'] == boss && continued['allThreadsContinued']
    end

    puts 'pausing a macrostep that never ends'
    spinner = client.request('threads')['body']['threads'].find { |thread| thread['name'].start_with?('spinner/') }['id']
    client.request('evaluate', 'expression' => 'send @spinner spin', 'context' => 'repl')
    client.request('pause', 'threadId' => tid)
    stopped = client.event('stopped')
    check('pause stops the actor that is running') { stopped['reason'] == 'pause' && stopped['threadId'] == spinner }
    names = names_of.call(spinner)
    check('inside its macrostep') do
      names.last == 'macrostep 2 on spin' && names.any? { |name| name.start_with?('microstep 2.') }
    end
    microstep = ->(name) { name[/\Amicrostep 2\.(\d+):/, 1].to_i }
    spin_microstep = -> { microstep.call(names_of.call(spinner).find { |name| name.start_with?('microstep') }) }
    was = spin_microstep.call
    client.request('stepIn', 'threadId' => spinner, 'granularity' => 'line')
    client.event('stopped')
    spin_n = -> { client.request('evaluate', 'expression' => '.n', 'context' => 'watch',
                                             'frameId' => frame_of.call(spinner).first['id'])['body']['result'].to_i }
    at_start = spin_microstep.call
    n_at_start = spin_n.call
    check('a step by microsteps stops at the next microstep\'s start, before its first unit (ADR 0087)') do
      at_start > was && names_of.call(spinner).first(2) == ['assign 1', 'transition spinning → spinning']
    end
    client.request('stepIn', 'threadId' => spinner)
    client.event('stopped')
    check('stepIn runs that unit, the microstep\'s last, and stops at the next microstep\'s start') do
      spin_microstep.call == at_start + 1 && names_of.call(spinner).first == 'assign 1' && spin_n.call == n_at_start + 1
    end
    puts 'unwedging: abandon, goto, cancel'
    spin_frame = -> { frame_of.call(spinner).first['id'] }
    console = ->(text, frame_id = spin_frame.call) do
      client.request('evaluate', 'expression' => text, 'context' => 'repl', 'frameId' => frame_id)
    end
    jump = console.call('goto resting')['body']
    check('a jump of an actor inside a macrostep waits in its mailbox') do
      jump['result'] == 'spinner/spinner: goto resting queued; it waits in the mailbox (1 message): continue or ' \
                        'step to deliver it'
    end
    check('so does an event, behind the mail before it') do
      answer = client.request('evaluate', 'expression' => 'send @spinner poke', 'context' => 'repl')['body']
      mail = vars.call(scope_ref.call(frame_of.call(spinner).first['id'], 'Queues'))
                 .find { |variable| variable['name'] == 'mailbox' }
      answer['result'] == 'sent poke to spinner/spinner; it waits in the mailbox (2 messages): continue or step to ' \
                          'deliver it' && vars.call(mail['variablesReference']).size == 2
    end
    abandoned = console.call('abandon')['body']
    stopped = client.event('stopped')
    check('abandon ends the macrostep that never ends, and the system stops again where the actor rests') do
      abandoned['result'].match?(/\Aspinner\/spinner: abandoned macrostep \d+; it keeps the configuration after its last /) &&
        stopped['reason'] == 'pause' && stopped['description'] == 'its macrostep was abandoned' &&
        stopped['threadId'] == spinner && names_of.call(spinner) == ['idle in spinning (held; 2 messages waiting)']
    end
    check('its summary says so') do
      macrostep.call('spinner/spinner', 'spin')['output'].include?('abandoned by an operator')
    end
    client.request('continue', 'threadId' => spinner)
    check('on continue the actor takes its mail in order: the jump, then the event') do
      macrostep.call('spinner/spinner', 'operator.goto')['output'].include?('goto: exited spinning, entered resting') &&
        macrostep.call('spinner/spinner', 'poke')['output'].include?('poke enabled no transition')
    end
    client.request('pause', 'threadId' => spinner)
    client.event('stopped') { |body| body['reason'] == 'pause' }
    jump = console.call('goto resting')['body']
    stopped = client.event('stopped')
    check('goto of an actor at rest with nothing waiting, while stopped, runs now and stops with reason goto') do
      jump['result'] == 'spinner/spinner: goto resting' && stopped['reason'] == 'goto' &&
        stopped['threadId'] == spinner && stopped['description'] == 'the jump\'s macrostep ended' &&
        names_of.call(spinner) == ['idle in resting (held; 0 messages waiting)']
    end
    check('the console shows its macrostep, taken by the operator\'s event') do
      macrostep.call('spinner/spinner', 'operator.goto')['output'].include?('goto: exited resting, entered resting')
    end
    check('a jump from a state that is not active is refused') do
      !console.call('goto spinning from spinning')['success']
    end

    boss_frame = frame_of.call(boss).first['id']
    timers = vars.call(scope_ref.call(boss_frame, 'Timers'))
    send_id = timers.first['name']
    withdrawn = console.call("cancel #{send_id}", boss_frame)['body']
    check('cancel withdraws a pending delayed send by the ID the Timers scope shows') do
      withdrawn['result'] == "supervisor/boss: withdrew send #{send_id}" &&
        vars.call(scope_ref.call(frame_of.call(boss).first['id'], 'Timers')).empty?
    end
    check('a send withdrawn once is not found again') do
      again = console.call("cancel @boss #{send_id}")
      !again['success'] && again['message'] == "supervisor/boss has no pending send #{send_id}"
    end

    puts 'goto and gotoTargets'
    at_line = ->(needle) do
      client.request('gotoTargets', 'source' => counter_source, 'line' => line_of(COUNTER, needle))['body']['targets']
    end
    targets = at_line.call('state :busy do')
    check('gotoTargets at a state\'s line: that state, at its line') do
      targets.map { |target| [target['label'], target['line']] } == [['busy', line_of(COUNTER, 'state :busy do')]]
    end
    check('at an action\'s or a transition\'s line: the state it belongs to') do
      at_line.call("assign '.count', '.count + 1'").map { |target| target['label'] } == ['idle'] &&
        at_line.call("log 'busy'").map { |target| target['label'] } == ['busy']
    end
    check('at a line with nothing on it: none') { at_line.call('data count: 0').empty? }
    targets = client.request('gotoTargets', 'source' => { 'path' => counter_path })['body']['targets']
    check('without a line, every state of the file\'s chart') do
      targets.map { |target| target['label'] } == %w[idle busy]
    end
    check('without a source, those of the stopped thread\'s chart') do
      client.request('gotoTargets', 'source' => {}, 'line' => 1)['body']['targets']
            .map { |target| target['label'] }.sort == %w[resting spinning]
    end
    idle_target = targets.find { |target| target['label'] == 'idle' }['id']
    wrong = client.request('goto', 'threadId' => spinner, 'targetId' => idle_target)
    check('goto of a target in another chart is refused') { !wrong['success'] }
    before = frame_of.call(tid).first['name']
    went = client.request('goto', 'threadId' => tid, 'targetId' => idle_target)
    stopped = client.event('stopped')
    check('goto moves the thread and stops with reason goto') do
      before == 'idle in busy (held; 0 messages waiting)' && went['success'] && stopped['reason'] == 'goto' &&
        stopped['threadId'] == tid && names_of.call(tid) == ['idle in idle (held; 0 messages waiting)'] &&
        client.event('output') { |body| body['output'] == "counter/acct1: goto idle\n" }
    end
    check('the console shows the jump\'s macrostep') do
      macrostep.call('counter/acct1', 'operator.goto')['output'].include?('goto: exited busy, entered idle')
    end
    client.request('setFunctionBreakpoints', 'breakpoints' => [{ 'name' => 'busy' }])
    console.call('send @boss poke')
    client.request('continue', 'threadId' => tid)
    client.event('stopped')
    chain = chain_of.call(tid)
    check('the causal chain: the step of another actor that sent the event, then what sent that one') do
      chain.map { |f| f['name'] }.first == 'caused by' && chain.first['presentationHint'] == 'label' &&
        chain[1]['name'].match?(%r{\Asupervisor/boss: sent bump in step \d+\.1\z}) &&
        # The hot reload above added a line (`data revision: 3`) near the top.
        chain[1]['line'] == line_of(SUPERVISOR, "send_event 'bump'") + 1 &&
        chain[1].dig('source', 'name') == 'supervisor.rb' &&
        chain[2]['name'] == 'the debug console sent poke' && chain.size == 3
    end
    inside = names_of.call(tid).first(2)
    abandoned = console.call('abandon @acct1')['body']
    check('abandon inside a microstep finishes the microstep first, then abandons') do
      inside == ['log 1', 'onentry of busy'] &&
        abandoned['result'].start_with?('counter/acct1: abandoned macrostep ') &&
        client.event('stopped')['description'] == 'its macrostep was abandoned' &&
        names_of.call(tid) == ['idle in busy (held; 0 messages waiting)'] &&
        client.outputs.any? { |text| text == "busy: 48\n" }
    end
    client.request('setFunctionBreakpoints', 'breakpoints' => [])

    puts 'line breakpoints'
    client.request('continue', 'threadId' => tid)
    counter_source_path = { 'path' => counter_path }
    locations = client.request('breakpointLocations', 'source' => counter_source_path, 'line' => 1,
                                                      'endLine' => COUNTER.lines.size)['body']['breakpoints']
                      .map { |location| location['line'] }
    check('breakpointLocations: the lines that hold a state, a transition or an action') do
      [line_of(COUNTER, 'state :idle do'), line_of(COUNTER, "on: 'bump'"), line_of(COUNTER, "log 'busy'")]
        .all? { |line| locations.include?(line) } && !locations.include?(line_of(COUNTER, 'data count: 0'))
    end
    set = client.request('setBreakpoints', 'source' => counter_source_path, 'breakpoints' => [
                           { 'line' => line_of(COUNTER, "assign '.count', '.count + 1'") },
                           { 'line' => line_of(COUNTER, "log 'busy'") },
                           { 'line' => line_of(COUNTER, 'data items: [1, 2]') },
                           { 'line' => line_of(COUNTER, "foreach '.items'"), 'logMessage' => 'count is {.count}' },
                           { 'line' => line_of(COUNTER, "on: 'done'"), 'condition' => '.count > 1000' }
                         ])['body']['breakpoints']
    check('each line is verified where it snaps to: a line without an element to the nearest') do
      set.all? { |breakpoint| breakpoint['verified'] } &&
        set.map { |breakpoint| breakpoint['line'] }.first(3) ==
          [line_of(COUNTER, "assign '.count', '.count + 1'"), line_of(COUNTER, "log 'busy'"),
           line_of(COUNTER, 'initial :idle')]
    end
    console.call('send @acct1 done', nil)
    macrostep.call('counter/acct1', 'done')
    check('a line breakpoint whose condition is false does not stop') { client.take('stopped').empty? }
    console.call('send @acct1 bump', nil)
    stopped = client.event('stopped')
    check('a breakpoint on a microstep\'s first unit stops before it, at the microstep\'s start (ADR 0087)') do
      stopped['reason'] == 'breakpoint' && stopped['hitBreakpointIds'] == [set[0]['id']] &&
        stopped['description'].start_with?('before line') &&
        frame_of.call(tid).first(2).map { |f| [f['name'], f['line']] } ==
          [['assign 1', line_of(COUNTER, "assign '.count', '.count + 1'")],
           ['transition idle → busy on bump', line_of(COUNTER, "on: 'bump'")]]
    end
    client.request('continue', 'threadId' => tid)
    stopped = client.event('stopped')
    check('any other stops before its unit, with reason breakpoint') do
      stopped['reason'] == 'breakpoint' && stopped['hitBreakpointIds'] == [set[1]['id']] &&
        frame_of.call(tid).first['name'] == 'log 1' && frame_of.call(tid).first['line'] == line_of(COUNTER, "log 'busy'")
    end
    client.request('continue', 'threadId' => tid)
    macrostep.call('counter/acct1', 'bump')
    check('a log point writes its message, with expressions, once per iteration of its <foreach>, and does not stop') do
      counts = client.outputs.grep(/\Acount is \d+\n\z/)
      counts.size == 2 && counts.uniq.size == 2 && client.take('stopped').empty?
    end

    client.request('pause', 'threadId' => tid)
    client.event('stopped') { |body| body['reason'] == 'pause' }
    console.call('goto @acct1 idle', nil)
    client.event('stopped') { |body| body['reason'] == 'goto' }
    client.outputs
    console.call('goto @acct1 busy', nil)
    stopped = client.event('stopped')
    check('a breakpoint on a <log> that is its microstep\'s first unit stops before it, though <log> writes no trace record') do
      stopped['reason'] == 'breakpoint' && stopped['hitBreakpointIds'] == [set[1]['id']] &&
        names_of.call(tid).first(2) == ['log 1', 'onentry of busy'] && names_of.call(tid)[2].end_with?('.1: goto busy') &&
        client.outputs.none? { |text| text.start_with?('busy:') }
    end
    client.request('continue', 'threadId' => tid)
    macrostep.call('counter/acct1', 'operator.goto')

    moved = "# a line above everything\n#{COUNTER}"
    dir.join('counter.rb').write(moved)
    changed = client.event('breakpoint') { |body| body.dig('breakpoint', 'id') == set[1]['id'] }
    check('a reload that moves the lines moves the breakpoints: a breakpoint event') do
      changed['reason'] == 'changed' && changed.dig('breakpoint', 'line') == line_of(moved, "log 'busy'")
    end
    client.request('setBreakpoints', 'source' => counter_source_path, 'breakpoints' => [])

    puts 'going back'
    client.request('pause', 'threadId' => tid)
    client.event('stopped') { |body| body['reason'] == 'pause' }
    tid_frame = frame_of.call(tid).first['id']
    { 'stepBack' => { 'threadId' => tid }, 'reverseContinue' => { 'threadId' => tid },
      'restartFrame' => { 'frameId' => tid_frame }, 'restart' => {} }.each do |command, arguments|
      answer = client.request(command, arguments)
      check("#{command} answers an error that says why") { !answer['success'] && answer['message'].size > 10 }
    end
    client.request('continue', 'threadId' => tid)

    puts 'stepping an invoked child'
    child_id = client.request('threads')['body']['threads']
                     .find { |thread| thread['name'].start_with?('supervisor/boss invocation worker') }['id']
    client.request('pause', 'threadId' => child_id)
    client.event('stopped') { |body| body['reason'] == 'pause' }
    answer = client.request('stepIn', 'threadId' => child_id)
    client.event('continued') { |body| body['threadId'] == child_id }
    console.call('send @boss.worker finish', nil)
    stopped = client.event('stopped')
    check('a step of an idle child stops when it takes its next event') do
      answer['success'] && stopped['reason'] == 'step' && stopped['threadId'] == child_id &&
        names_of.call(child_id) == ['about to take finish (before transition selection)']
    end
    client.request('stepIn', 'threadId' => child_id)
    client.event('stopped')
    top = frame_of.call(child_id).first
    check('the next stepIn stands at its microstep\'s start, in the encoded SCXML of its chart') do
      top['name'] == 'transition working → finished on finish' && top.dig('source', 'name') == 'worker.scxml' &&
        client.request('source', 'sourceReference' => top.dig('source', 'sourceReference'))['body']['content']
              .lines[top['line'] - 1].include?('<transition event="finish" target="finished"')
    end
    client.request('stepIn', 'threadId' => child_id)
    client.event('stopped')
    check('and the one after it finishes the child') { names_of.call(child_id) == ['done in finished'] }
    client.request('continue', 'threadId' => child_id)

    puts 'refused requests'
    %w[cancel stepInTargets terminateThreads setInstructionBreakpoints locations readMemory writeMemory disassemble].each do |command|
      answer = client.request(command, 'threadId' => tid)
      check("#{command} answers an error that says why") { !answer['success'] && answer['message'].size > 10 }
    end
    check('an unknown request is an error') { !client.request('fooBar')['success'] }

    puts 'terminate'
    terminate = client.request('terminate')
    check('terminate succeeds') { terminate['success'] }
    check('and the session ends, with the actors left as they are') { client.event('terminated') }
  ensure
    client.request('disconnect') rescue nil
    status = client.close
    check('the adapter exits cleanly') { status.success? }
  end
end

puts 'a session over TCP, as VS Code\'s debugServer connects'
begin
  server = IO.popen([RbConfig.ruby, ROOT.join('bin/dap').to_s, '--port', '0'], err: %i[child out])
  port = Timeout.timeout(DAPClient::TIMEOUT) { server.gets.to_s[/listening on 127\.0\.0\.1:(\d+)/, 1] }
  require 'socket'
  socket = TCPSocket.new('127.0.0.1', Integer(port))
  json = JSON.generate('seq' => 1, 'type' => 'request', 'command' => 'initialize', 'arguments' => {})
  socket.write("Content-Length: #{json.bytesize}\r\n\r\n#{json}")
  header = Timeout.timeout(DAPClient::TIMEOUT) { socket.gets("\r\n\r\n") }
  answer = JSON.parse(socket.read(Integer(header[/Content-Length: (\d+)/, 1])))
  check('the adapter answers on its port') { answer['success'] && answer['body']['supportsConfigurationDoneRequest'] }
  socket.close
ensure
  Process.kill('TERM', server.pid) if server
  server&.close
end

puts
if FAILURES.empty?
  puts 'dap self-test passed'
else
  puts "dap self-test failed: #{FAILURES.size} check(s)"
  FAILURES.each { |failure| puts "  - #{failure}" }
  exit 1
end
