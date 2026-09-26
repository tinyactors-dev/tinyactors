# frozen_string_literal: true

# Checks the debug adapter as a client does: it starts bin/dap over pipes,
# drives it with a small scripted DAP client against a workbench directory
# written for the test, and checks the responses and events of every
# message the adapter implements, and the errors of the ones it refuses.
#
#   ruby ruby/dap_selftest.rb
#   DAP_TRACE=1 ruby ruby/dap_selftest.rb    # and print every message

require 'json'
require 'open3'
require 'pathname'
require 'timeout'
require 'tmpdir'

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

# A DAP client over the adapter's stdin and stdout. Every message is kept;
# `request` answers the response to its request, and `event` waits for the
# next event of a name that matches, among those not taken yet.
class Client
  TIMEOUT = 15

  attr_reader :stderr_text

  def initialize(command)
    @stdin, @stdout, @stderr, @wait = Open3.popen3(*command)
    @stdin.binmode
    @stdout.binmode
    @seq = 0
    @events = []
    @responses = {}
    @stderr_text = +''
    @stderr_reader = Thread.new { @stderr.each_line { |line| @stderr_text << line } }
  end

  def request(command, arguments = nil)
    @seq += 1
    message = { 'seq' => @seq, 'type' => 'request', 'command' => command }
    message['arguments'] = arguments if arguments
    json = JSON.generate(message)
    warn "-> #{json}" if ENV['DAP_TRACE']
    @stdin.write("Content-Length: #{json.bytesize}\r\n\r\n#{json}")
    @stdin.flush
    seq = @seq
    read_until { @responses.key?(seq) }
    @responses.delete(seq)
  end

  # The first event not yet taken named `name` whose body the block
  # accepts; waits for it.
  def event(name, &accept)
    found = nil
    read_until do
      found = @events.find { |event| event['event'] == name && (accept.nil? || accept.call(event['body'] || {})) }
    end
    @events.delete(found)
    found['body'] || {}
  end

  # Events of a name already received, taken.
  def take(name)
    taken = @events.select { |event| event['event'] == name }
    @events -= taken
    taken.map { |event| event['body'] || {} }
  end

  # Reads what the adapter sent so far without waiting for anything.
  def drain
    read_message while @stdout.wait_readable(0.2)
  end

  def outputs = take('output').map { |body| body['output'] }

  def close
    @stdin.close unless @stdin.closed?
    @wait.join(TIMEOUT) || Process.kill('KILL', @wait.pid)
    @stderr_reader.join(TIMEOUT)
    @wait.value
  end

  private

  def read_until
    Timeout.timeout(TIMEOUT) do
      read_message until yield
    end
  rescue Timeout::Error
    raise "timed out; received events #{@events.map { |event| event['event'] }.inspect}; stderr: #{@stderr_text}"
  end

  def read_message
    length = nil
    while (line = @stdout.gets("\r\n"))
      line = line.chomp("\r\n")
      break if line.empty?

      name, value = line.split(':', 2)
      length = Integer(value) if name == 'Content-Length'
    end
    raise "the adapter closed its output; stderr: #{@stderr_text}" if length.nil?

    message = JSON.parse(@stdout.read(length))
    warn "<- #{JSON.generate(message)}" if ENV['DAP_TRACE']
    case message['type']
    when 'response' then @responses[message['request_seq']] = message
    when 'event' then @events << message
    end
  end
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

Dir.mktmpdir('dap-selftest') do |tmp|
  dir = Pathname.new(tmp)
  dir.join('counter.rb').write(COUNTER)
  dir.join('supervisor.rb').write(SUPERVISOR)
  dir.join('worker.rb').write(WORKER)
  dir.join('spinner.rb').write(SPINNER)
  dir.join('actors.rb').write(ACTORS)

  client = Client.new([RbConfig.ruby, ROOT.join('bin/dap').to_s])
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
         supportsConditionalBreakpoints supportsHitConditionalBreakpoints].all? { |name| capabilities[name] == true }
    end
    check('declares nothing that needs pending core work') do
      %w[supportsStepBack supportsRestartFrame supportsGotoTargetsRequest supportsBreakpointLocationsRequest
         supportsRestartRequest supportsLogPoints].none? { |name| capabilities[name] }
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
    unknown = client.request('setExceptionBreakpoints', 'filters' => ['error.nonsense'])
    check('an unknown exception filter is refused') { !unknown['success'] }
    check('exception filters are accepted') do
      client.request('setExceptionBreakpoints', 'filters' => ['error.execution'])['success']
    end
    done = client.request('configurationDone')
    check('configurationDone succeeds') { done['success'] }
    entry = client.event('stopped')
    check('stopOnEntry stops with reason entry') { entry['reason'] == 'entry' && entry['allThreadsStopped'] }

    puts 'threads'
    threads = client.request('threads')['body']['threads']
    by_name = ->(prefix) { threads.find { |thread| thread['name'].start_with?(prefix) } }
    acct1 = by_name.call('counter/acct1')
    check('a thread per named actor, named by address and leaf states') do
      acct1 && acct1['name'] == 'counter/acct1 · idle' && by_name.call('supervisor/boss · supervising')
    end
    check('an invoked child is a thread too') { by_name.call('worker (actor') }
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
    check('an idle actor has one frame') do
      stack['totalFrames'] == 1 && stack['stackFrames'].first['name'] == 'idle in idle'
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
    check('the system scope') do
      processors = vars.call(system['_ioprocessors']['variablesReference']).map { |variable| variable['name'] }
      system['_name']['value'] == '"counter"' && system['now']['value'] == '0' && processors.include?('debug')
    end
    boss = by_name.call('supervisor/boss')['id']
    boss_frame = client.request('stackTrace', 'threadId' => boss)['body']['stackFrames'].first['id']
    boss_scopes = client.request('scopes', 'frameId' => boss_frame)['body']['scopes']
    invocations = vars.call(boss_scopes.find { |candidate| candidate['name'] == 'Invocations' }['variablesReference'])
    check('the invocations, each with its thread') do
      invoke = vars.call(invocations.first['variablesReference']).to_h { |variable| [variable['name'], variable['value']] }
      invocations.size == 1 && invoke['thread'].include?('worker (actor') && invoke['active'] == 'true'
    end
    check('an unknown reference is refused') { !client.request('variables', 'variablesReference' => 9999)['success'] }

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
    { 'goto busy' => '0082', 'abandon' => '0082', 'cancel dunning' => '0084', 'fork' => '0085',
      'replace counter.rb' => 'save the chart file' }.each do |command, why|
      answer = evaluate.call(command, 'repl')
      check("#{command.split.first} is refused, saying what it needs") { !answer['success'] && answer['message'].include?(why) }
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
      complete.call('send @boss ').map { |target| target['label'] } == ['tick'] &&
        complete.call('send @acct1 d').map { |target| target['label'] } == ['done']
    end
    check('data locations') { complete.call('.it').map { |target| target['label'] } == ['items'] }
    check('console commands') { complete.call('cl').map { |target| target['label'] } == ['clock'] }

    puts 'send, continue and function breakpoints'
    busy_id, exit_id = functions['body']['breakpoints'].map { |breakpoint| breakpoint['id'] }
    sent = evaluate.call('send bump {"n": 1}', 'repl')['body']
    check('send delivers to the frame\'s actor, which runs on continue') do
      sent['result'] == 'sent bump to acct1; it runs when you continue'
    end
    stack = client.request('stackTrace', 'threadId' => tid)['body']['stackFrames']
    check('an actor holding an event it has not begun') do
      stack.map { |f| f['name'] } == ['about to take bump (before transition selection)']
    end
    frame_of = ->(thread) { client.request('stackTrace', 'threadId' => thread)['body']['stackFrames'] }
    scope_ref = ->(frame_id, name) do
      client.request('scopes', 'frameId' => frame_id)['body']['scopes'].find { |scope| scope['name'] == name }['variablesReference']
    end
    queues = vars.call(scope_ref.call(stack.first['id'], 'Queues')).to_h { |variable| [variable['name'], variable] }
    check('the Queues scope shows the event about to be taken') do
      vars.call(queues['current']['variablesReference']).find { |variable| variable['name'] == 'name' }['value'] == '"bump"'
    end
    continued = client.request('continue', 'threadId' => tid)
    check('continue answers for every thread') { continued['success'] && continued['body']['allThreadsContinued'] }
    stopped = client.event('stopped')
    check('an entry breakpoint stops before the state\'s onentry') do
      stopped['reason'] == 'function breakpoint' && stopped['hitBreakpointIds'] == [busy_id] &&
        stopped['description'] == 'entered busy' && stopped['threadId'] == tid
    end
    names = frame_of.call(tid).map { |f| f['name'] }
    check('frames: the next unit, its block, the microstep, the macrostep') do
      names == ['log 1', 'onentry of busy', 'microstep 2.1: idle → busy on bump', 'macrostep 2 on bump']
    end
    top = frame_of.call(tid).first['id']
    event = vars.call(scope_ref.call(top, 'Event')).to_h { |variable| [variable['name'], variable['value']] }
    check('the Event scope is _event') do
      event['name'] == '"bump"' && event['type'] == '"external"' && event['origin'] == '"debug://console"' &&
        event['origintype'] == '"debug"'
    end
    check('expressions see $_event') { evaluate.call('$_event.data.n', 'watch', top)['body']['result'] == '1' }
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
    check('the unit was the <log>, whose record is stdout output') do
      client.take('output').any? { |body| body['category'] == 'stdout' && body['output'] == "<log> busy 1\n" }
    end
    top_names = -> { frame_of.call(tid).map { |f| f['name'] } }
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
    step.call('next')
    check('next steps over the rest of the loop; the microstep is over, selection is next') do
      top_names.call == ['selecting transitions', 'microstep 2.1: idle → busy on bump', 'macrostep 2 on bump'] &&
        evaluate.call('.count', 'watch', frame_of.call(tid).first['id'])['body']['result'] == '4'
    end
    check('the macrostep summary is not written before the macrostep ends') do
      client.outputs.none? { |text| text.include?('macrostep 2') }
    end
    answer, stopped = step.call('stepOut')
    check('stepOut between microsteps runs to the end of the macrostep') do
      stopped['reason'] == 'step' && top_names.call == ['idle in busy']
    end
    check('the macrostep summary is written when it ends') do
      summary = client.outputs.find { |text| text.start_with?('acct1: macrostep 2 on bump') }
      summary && summary.include?('idle → busy on bump') && summary.include?('assign .count = 4')
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
      client.event('output') { |body| body['output'].match?(/\A#{who}: macrostep \d+ on #{Regexp.escape(event)} /) }
    end

    puts 'an exit breakpoint, and a step of an idle actor'
    evaluate.call('send done', 'repl', nil)
    client.request('continue', 'threadId' => tid)
    stopped = client.event('stopped')
    check('an exit breakpoint of a state without onexit hits where the actor next stands') do
      stopped['reason'] == 'function breakpoint' && stopped['hitBreakpointIds'] == [exit_id] &&
        stopped['description'] == 'exited busy' &&
        frame_of.call(tid).map { |f| f['name'] } ==
          ['selecting transitions', 'microstep 3.1: busy → idle on done', 'macrostep 3 on done']
    end
    step.call('next')
    check('next between microsteps finishes the macrostep') { frame_of.call(tid).map { |f| f['name'] } == ['idle in idle'] }
    answer = client.request('next', 'threadId' => tid)
    continued = client.event('continued')
    check('next on an idle actor answers at once, and everything runs') do
      answer['success'] && continued['threadId'] == tid && continued['allThreadsContinued']
    end
    check('the console says what the actor waits for') do
      summary.call('acct1 waits for its next event').include?('`send` it one')
    end
    client.request('evaluate', 'expression' => 'send ping', 'context' => 'repl')
    stopped = client.event('stopped')
    check('the step stops when the actor takes its next event') do
      stopped['reason'] == 'step' && stopped['description'] == 'took ping' && stopped['threadId'] == tid
    end
    check('before transition selection') do
      frame_of.call(tid).map { |f| f['name'] } == ['about to take ping (before transition selection)']
    end
    step.call('stepIn', granularity: 'line')
    check('the actor answers the console: its reply arrives as output') do
      summary.call('acct1 replied pong').include?('acct1 replied pong 4')
    end
    client.request('continue', 'threadId' => tid)
    macrostep.call('acct1', 'ping')

    puts 'exceptions'
    client.request('evaluate', 'expression' => 'send fail', 'context' => 'repl')
    stopped = client.event('stopped')
    check('error.execution stops with reason exception') do
      stopped['reason'] == 'exception' && stopped['threadId'] == tid && stopped['text'].include?('error.execution')
    end
    info = client.request('exceptionInfo', 'threadId' => tid)['body']
    check('exceptionInfo names the error and why') do
      info['exceptionId'] == 'error.execution' && info['breakMode'] == 'always' &&
        info['description'] == 'error.execution: JSON expression produced no value'
    end
    check('exceptionInfo of a thread that did not fail is an error') do
      !client.request('exceptionInfo', 'threadId' => boss)['success']
    end
    client.request('setExceptionBreakpoints', 'filters' => %w[ignored error.communication])
    client.request('continue', 'threadId' => tid)
    macrostep.call('acct1', 'fail')
    client.request('evaluate', 'expression' => 'send lost', 'context' => 'repl')
    stopped = client.event('stopped')
    check('error.communication: a send nobody can take') do
      stopped['reason'] == 'exception' && stopped['text'].start_with?('error.communication')
    end
    client.request('continue', 'threadId' => tid)
    stopped = client.event('stopped')
    check('ignored: the chart has no transition for it') do
      stopped['reason'] == 'exception' && stopped['text'] == 'error.communication enabled no transition'
    end
    client.request('continue', 'threadId' => tid)
    macrostep.call('acct1', 'lost')
    client.request('evaluate', 'expression' => 'send nothing.here', 'context' => 'repl')
    stopped = client.event('stopped')
    check('ignored: an external event, too') do
      stopped['reason'] == 'exception' && stopped['text'] == 'nothing.here enabled no transition' &&
        client.request('exceptionInfo', 'threadId' => tid)['body']['exceptionId'] == 'nothing.here'
    end
    client.request('setExceptionBreakpoints', 'filters' => [])
    client.request('continue', 'threadId' => tid)
    macrostep.call('acct1', 'nothing.here')
    check('the chart handled its error.execution') do
      client.request('evaluate', 'expression' => '.last', 'context' => 'watch')['body']['result'] == '"failed"'
    end

    puts 'pause at rest, data breakpoints'
    client.request('pause', 'threadId' => tid)
    stopped = client.event('stopped')
    check('pausing a system at rest stops at once') { stopped['reason'] == 'pause' && stopped['threadId'] == tid }
    top = frame_of.call(tid).first['id']
    data_ref = scope_ref.call(top, 'Data model')
    info = client.request('dataBreakpointInfo', 'variablesReference' => data_ref, 'name' => 'count')['body']
    check('a data-model location can be watched for writes') do
      info['dataId'] == 'acct1 ["count"]' && info['accessTypes'] == ['write'] && info['description'] == 'acct1 .count'
    end
    by_expression = client.request('dataBreakpointInfo', 'name' => '.items[1]', 'frameId' => top)['body']
    check('so can one named by an expression') { by_expression['dataId'] == 'acct1 ["items",1]' }
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
      stopped['reason'] == 'data breakpoint' && stopped['description'] == 'acct1: .count = 5' &&
        stopped['hitBreakpointIds'] == set['body']['breakpoints'].map { |breakpoint| breakpoint['id'] }
    end
    client.request('setDataBreakpoints', 'breakpoints' => [])
    client.request('continue', 'threadId' => tid)
    macrostep.call('acct1', 'bump')

    puts 'a breakpoint on an event'
    on_ping = client.request('setFunctionBreakpoints', 'breakpoints' => [{ 'name' => 'on:ping' }])['body']['breakpoints']
    client.request('evaluate', 'expression' => 'send ping', 'context' => 'repl')
    stopped = client.event('stopped')
    check('on: stops before the macrostep that takes the event') do
      stopped['reason'] == 'function breakpoint' && stopped['hitBreakpointIds'] == [on_ping.first['id']] &&
        frame_of.call(tid).map { |f| f['name'] } == ['about to take ping (before transition selection)']
    end
    client.request('continue', 'threadId' => tid)
    macrostep.call('acct1', 'ping')
    check('and does not stop again when the macrostep begins') { client.take('stopped').empty? }

    puts 'conditions and hit conditions'
    conditional = client.request('setFunctionBreakpoints', 'breakpoints' => [
                                   { 'name' => 'on:done', 'condition' => '.count > 1000' },
                                   { 'name' => 'idle -> busy', 'hitCondition' => '2' }
                                 ])['body']['breakpoints']
    client.request('evaluate', 'expression' => 'send done', 'context' => 'repl')
    macrostep.call('acct1', 'done')
    check('a breakpoint whose condition is false does not stop') { client.take('stopped').empty? }
    client.request('evaluate', 'expression' => 'send bump', 'context' => 'repl')
    macrostep.call('acct1', 'bump')
    client.request('evaluate', 'expression' => 'send done', 'context' => 'repl')
    macrostep.call('acct1', 'done')
    check('the first hit of a hit condition of 2 does not stop') { client.take('stopped').empty? }
    client.request('evaluate', 'expression' => 'send bump', 'context' => 'repl')
    stopped = client.event('stopped')
    # A resume runs at least one unit after it selects a microstep, so a
    # block that is the microstep's first stops after its first unit.
    check('the second does, in the transition\'s content: after its first unit, the microstep\'s first') do
      stopped['reason'] == 'function breakpoint' && stopped['hitBreakpointIds'] == [conditional.last['id']] &&
        frame_of.call(tid).map { |f| f['name'] }.first(2) == ['assign 2', 'transition idle → busy on bump']
    end

    puts 'writing data'
    data_ref = scope_ref.call(frame_of.call(tid).first['id'], 'Data model')
    written = client.request('setVariable', 'variablesReference' => data_ref, 'name' => 'count', 'value' => '42')
    check('setVariable writes a data-model location') do
      written['success'] && written['body']['value'] == '42' &&
        client.request('evaluate', 'expression' => '.count', 'context' => 'watch')['body']['result'] == '42'
    end
    check('the actor stays where it was stopped, and keeps its thread') do
      frame_of.call(tid).map { |f| f['name'] }.first(2) == ['assign 2', 'transition idle → busy on bump']
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
    worker = client.request('threads')['body']['threads'].find { |thread| thread['name'].start_with?('worker') }['id']
    worker_ref = scope_ref.call(frame_of.call(worker).first['id'], 'Data model')
    check('an invoked child is refused, saying why') do
      answer = client.request('setVariable', 'variablesReference' => worker_ref, 'name' => 'x', 'value' => '1')
      !answer['success'] && answer['message'].include?('invoked child')
    end
    client.request('setFunctionBreakpoints', 'breakpoints' => [])
    client.request('continue', 'threadId' => tid)
    macrostep.call('acct1', 'bump')
    check('the macrostep finished on the written data') do
      client.request('evaluate', 'expression' => '.count', 'context' => 'watch')['body']['result'] == '46'
    end

    puts 'timers and the clock'
    boss_top = frame_of.call(boss).first['id']
    timers = vars.call(scope_ref.call(boss_top, 'Timers'))
    check('the Timers scope lists pending delayed sends') do
      timer = vars.call(timers.first['variablesReference']).to_h { |variable| [variable['name'], variable['value']] }
      timers.size == 1 && timer['event'] == '"tick"' && timer['due'] == '5000'
    end
    clock = client.request('evaluate', 'expression' => 'clock +5s', 'context' => 'repl')['body']
    check('clock advances logical time') { clock['result'] == 't=5000ms' }
    macrostep.call('boss', 'tick')
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
    names = frame_of.call(spinner).map { |f| f['name'] }
    check('inside its macrostep') do
      names.last == 'macrostep 2 on spin' && names[1].start_with?('microstep 2.')
    end
    before = client.request('evaluate', 'expression' => '.n', 'context' => 'watch',
                                        'frameId' => frame_of.call(spinner).first['id'])['body']['result'].to_i
    microstep = ->(name) { name[/\Amicrostep 2\.(\d+):/, 1].to_i }
    was = microstep.call(frame_of.call(spinner)[1]['name'])
    client.request('stepIn', 'threadId' => spinner, 'granularity' => 'instruction')
    client.event('stopped')
    check('and it can be stepped from there, one unit: this microstep\'s one assign') do
      microstep.call(frame_of.call(spinner)[1]['name']) == was + 1 &&
        client.request('evaluate', 'expression' => '.n', 'context' => 'watch',
                                   'frameId' => frame_of.call(spinner).first['id'])['body']['result'].to_i == before + 1
    end
    check('another actor cannot take a console message meanwhile; it waits') do
      answer = client.request('evaluate', 'expression' => 'send @spinner spin', 'context' => 'repl')['body']
      queue = vars.call(scope_ref.call(frame_of.call(spinner).first['id'], 'Queues'))
                .find { |variable| variable['name'] == 'debugger' }
      answer['result'].start_with?('queued spin for spinner') && queue['value'].include?('spin')
    end

    puts 'refused requests'
    %w[restart setBreakpoints breakpointLocations goto gotoTargets stepBack reverseContinue restartFrame cancel
       stepInTargets terminateThreads setInstructionBreakpoints locations readMemory writeMemory disassemble].each do |command|
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
  port = Timeout.timeout(Client::TIMEOUT) { server.gets.to_s[/listening on 127\.0\.0\.1:(\d+)/, 1] }
  require 'socket'
  socket = TCPSocket.new('127.0.0.1', Integer(port))
  json = JSON.generate('seq' => 1, 'type' => 'request', 'command' => 'initialize', 'arguments' => {})
  socket.write("Content-Length: #{json.bytesize}\r\n\r\n#{json}")
  header = Timeout.timeout(Client::TIMEOUT) { socket.gets("\r\n\r\n") }
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
