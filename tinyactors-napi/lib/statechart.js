'use strict';

// The fluent statechart builder (docs/design.md, "Fluent authoring"). It
// emits the generated spec shape directly (ADR 0013 point 6): the same shape
// `System.define()` accepts as a plain object and core's definition export
// returns. There is no separate normalization step.
//
// Until the JavaScript data model lands (ROADMAP step 11), charts compile
// against core's null data model. Expressions are data-model source text,
// and the null data model accepts only `In(state)` conditions. Anything that
// needs JavaScript values or functions throws `unsupported` and names the
// step that adds it.

const { TinyactorsError } = require('./errors');

const literalBrand = Symbol('tinyactors.literal');

/** Force a callable JavaScript value to be data rather than an expression. */
function literal(value) {
  return Object.freeze({ [literalBrand]: true, value });
}

function needsDataModel(what) {
  return new TinyactorsError(
    `${what} needs the JavaScript data model, which is not implemented yet (ROADMAP step 11)`,
    'unsupported',
  );
}

function invalid(message) {
  return new TinyactorsError(message, 'invalid');
}

function text(value, what) {
  if (typeof value !== 'string' || value.length === 0) {
    throw invalid(`${what}: expected a non-empty string`);
  }
  return value;
}

// An expression is data-model source text. JavaScript functions are the
// JavaScript data model's form (step 11).
function expression(value, what) {
  if (typeof value === 'function') {
    throw needsDataModel(`${what} as a JavaScript function`);
  }
  return text(value, what);
}

function targets(value, what) {
  const list = typeof value === 'string' ? value.split(/\s+/).filter(Boolean) : value;
  if (!Array.isArray(list) || list.length === 0) {
    throw invalid(`${what}: expected a state ID or a non-empty list of state IDs`);
  }
  return list.map((target, index) => text(target, `${what}[${index}]`));
}

// A literal/expression attribute pair (`x` / `xexpr`).
function stringSpec(value, what) {
  if (typeof value === 'function') {
    throw needsDataModel(`${what} as a JavaScript function`);
  }
  return { kind: 'literal', literal: text(value, what) };
}

function delaySpec(value, what) {
  if (typeof value === 'number') {
    if (!Number.isFinite(value) || value < 0) {
      throw invalid(`${what}: expected a non-negative number of milliseconds`);
    }
    return { kind: 'literal', literal: `${value}ms` };
  }
  return stringSpec(value, what);
}

function noValue(value, what) {
  if (value !== undefined) {
    throw needsDataModel(what);
  }
}

// One builder tree shares one root: once build() or System.define() has
// consumed it, every builder in the tree refuses changes.
class Root {
  constructor() {
    this.consumed = false;
  }

  check() {
    if (this.consumed) {
      throw invalid('this statechart builder was already built; start a new one with statechart()');
    }
  }
}

function block(root, build, what) {
  if (typeof build !== 'function') {
    throw invalid(`${what}: expected a function that receives the builder`);
  }
  const actions = new ActionBuilder(root);
  build(actions);
  return actions.actions;
}

// Actions shared by transitions, handlers, initial transitions and
// <finalize>. Each method appends one action spec to `this.actions`.
class ActionSequenceBuilder {
  #root;

  constructor(root) {
    this.#root = root;
    this.actions = [];
  }

  #push(action) {
    this.#root.check();
    this.actions.push(action);
    return this;
  }

  raise(event, data) {
    noValue(data, 'raise() with data');
    return this.#push({ kind: 'raise', raise: { event: text(event, 'raise event') } });
  }

  send(event, build) {
    this.#root.check();
    if (typeof event === 'function') {
      throw needsDataModel('a send event as a JavaScript function');
    }
    const send = { event: { kind: 'name', name: text(event, 'send event') } };
    if (build !== undefined) {
      if (typeof build !== 'function') {
        throw invalid('send(): expected a function that receives the send builder');
      }
      build(new SendBuilder(this.#root, send));
    }
    return this.#push({ kind: 'send', send });
  }

  cancel(sendId) {
    return this.#push({ kind: 'cancel', cancel: { sendid: stringSpec(sendId, 'cancel send ID') } });
  }

  log(label, value) {
    const log = {};
    if (label !== undefined) {
      log.label = text(label, 'log label');
    }
    if (value !== undefined) {
      log.expr = expression(value, 'log value');
    }
    return this.#push({ kind: 'log', log });
  }

  choose(condition, then, otherwise) {
    this.#root.check();
    const conditional = {
      condition: expression(condition, 'choose() condition'),
      then_actions: block(this.#root, then, 'choose() then'),
    };
    if (otherwise !== undefined) {
      conditional.else_actions = block(this.#root, otherwise, 'choose() otherwise');
    }
    return this.#push({ kind: 'if', conditional });
  }

  assign() {
    throw needsDataModel('assign()');
  }

  assignAt() {
    throw needsDataModel('assignAt()');
  }

  script() {
    throw needsDataModel('script()');
  }

  forEach() {
    throw needsDataModel('forEach()');
  }
}

class ActionBuilder extends ActionSequenceBuilder {}

class SendBuilder {
  #root;
  #send;

  constructor(root, send) {
    this.#root = root;
    this.#send = send;
  }

  #set(member, value) {
    this.#root.check();
    this.#send[member] = value;
    return this;
  }

  to(target) {
    return this.#set('target', stringSpec(target, 'send target'));
  }

  via(ioType) {
    return this.#set('type', stringSpec(ioType, 'send I/O processor type'));
  }

  after(delay) {
    return this.#set('delay', delaySpec(delay, 'send delay'));
  }

  id(id) {
    return this.#set('id', { kind: 'literal', literal: text(id, 'send ID') });
  }

  idLocation() {
    throw needsDataModel('send idLocation()');
  }

  data() {
    throw needsDataModel('send data()');
  }

  param() {
    throw needsDataModel('send param()');
  }
}

class TransitionBuilder extends ActionSequenceBuilder {
  #root;

  constructor(root, transition) {
    super(root);
    this.#root = root;
    this.transition = transition;
  }

  when(condition) {
    this.#root.check();
    this.transition.condition = expression(condition, 'transition condition');
    return this;
  }

  target(target) {
    this.#root.check();
    this.transition.targets = targets(target, 'transition target');
    return this;
  }

  internal() {
    this.#root.check();
    this.transition.kind = 'internal';
    return this;
  }

  external() {
    this.#root.check();
    this.transition.kind = 'external';
    return this;
  }
}

class InvokeBuilder {
  #root;
  #invoke;

  constructor(root, invoke) {
    this.#root = root;
    this.#invoke = invoke;
  }

  #set(member, value) {
    this.#root.check();
    this.#invoke[member] = value;
    return this;
  }

  id(id) {
    return this.#set('id', { kind: 'literal', literal: text(id, 'invoke ID') });
  }

  idLocation() {
    throw needsDataModel('invoke idLocation()');
  }

  type(type) {
    return this.#set('type', stringSpec(type, 'invoke type'));
  }

  source(source) {
    return this.#set('src', stringSpec(source, 'invoke source'));
  }

  content() {
    throw needsDataModel('invoke content()');
  }

  param() {
    throw needsDataModel('invoke param()');
  }

  autoforward(enabled = true) {
    if (typeof enabled !== 'boolean') {
      throw invalid('autoforward(): expected a boolean');
    }
    return this.#set('autoforward', enabled);
  }

  finalize(build) {
    return this.#set('finalize', block(this.#root, build, 'invoke finalize()'));
  }
}

function initialSpec(root, target, actions) {
  const initial = { targets: targets(target, 'initial target') };
  if (actions !== undefined) {
    initial.actions = block(root, actions, 'initial actions');
  }
  return initial;
}

// Methods that add child states. `this.states` holds the child specs.
class StateContainerBuilder {
  #root;

  constructor(root) {
    this.#root = root;
    this.states = [];
  }

  #child(kind, id, build, what) {
    this.#root.check();
    const state = new StateBuilder(this.#root, kind, text(id, `${what} ID`));
    if (build !== undefined) {
      if (typeof build !== 'function') {
        throw invalid(`${what}(): expected a function that receives the state builder`);
      }
      build(state);
    }
    this.states.push(state.finish());
    return this;
  }

  state(id, build) {
    return this.#child(undefined, id, build, 'state');
  }

  parallel(id, build) {
    return this.#child('parallel', id, build, 'parallel');
  }

  final(id, build) {
    return this.#child('final', id, build, 'final');
  }

  history(id, depth, build) {
    if (depth !== 'shallow' && depth !== 'deep') {
      throw invalid('history(): depth must be "shallow" or "deep"');
    }
    return this.#child(`history_${depth}`, id, build, 'history');
  }
}

const stateKinds = new Set(['atomic', 'compound', 'parallel', 'final', 'history_shallow', 'history_deep']);

class StateBuilder extends StateContainerBuilder {
  #root;
  #kind;
  #spec;

  constructor(root, kind, id) {
    super(root);
    this.#root = root;
    this.#kind = kind;
    this.#spec = { kind: undefined, id };
  }

  #list(member, value) {
    this.#root.check();
    (this.#spec[member] ??= []).push(value);
    return this;
  }

  /** Override the kind normally inferred from the builder method and children. */
  kind(kind) {
    this.#root.check();
    if (!stateKinds.has(kind)) {
      throw invalid(`kind(): expected one of ${[...stateKinds].join(', ')}`);
    }
    this.#kind = kind;
    return this;
  }

  initial(target, actions) {
    this.#root.check();
    this.#spec.initial = initialSpec(this.#root, target, actions);
    return this;
  }

  data() {
    throw needsDataModel('data()');
  }

  dataExpression() {
    throw needsDataModel('dataExpression()');
  }

  entry(build) {
    return this.#list('onentry', { actions: block(this.#root, build, 'entry()') });
  }

  exit(build) {
    return this.#list('onexit', { actions: block(this.#root, build, 'exit()') });
  }

  on(event, build) {
    return this.#transition({ kind: 'external', event: text(event, 'transition event') }, build, 'on()');
  }

  always(build) {
    if (typeof build !== 'function') {
      throw invalid('always(): expected a function that receives the transition builder');
    }
    return this.#transition({ kind: 'external' }, build, 'always()');
  }

  #transition(transition, build, what) {
    this.#root.check();
    if (build !== undefined) {
      if (typeof build !== 'function') {
        throw invalid(`${what}: expected a function that receives the transition builder`);
      }
      const builder = new TransitionBuilder(this.#root, transition);
      build(builder);
      if (builder.actions.length > 0) {
        transition.actions = builder.actions;
      }
    }
    return this.#list('transitions', transition);
  }

  invoke(build) {
    this.#root.check();
    if (typeof build !== 'function') {
      throw invalid('invoke(): expected a function that receives the invoke builder');
    }
    const invoke = { kind: 'invoke' };
    build(new InvokeBuilder(this.#root, invoke));
    return this.#list('invokes', invoke);
  }

  doneData() {
    throw needsDataModel('doneData()');
  }

  // The finished spec. An unspecified kind is compound with children, and
  // atomic without.
  finish() {
    const spec = this.#spec;
    spec.kind = this.#kind ?? (this.states.length > 0 ? 'compound' : 'atomic');
    if (this.states.length > 0) {
      spec.states = this.states;
    }
    return spec;
  }
}

class DefinitionBuilder extends StateContainerBuilder {
  #root;
  #spec;

  constructor(options) {
    const root = new Root();
    super(root);
    this.#root = root;
    if (options === null || typeof options !== 'object') {
      throw invalid('statechart(): expected an options object with family and revision');
    }
    const { family, revision, name, binding } = options;
    if (typeof revision !== 'string' && !(revision instanceof Uint8Array)) {
      throw invalid('statechart(): revision must be a string or a Uint8Array');
    }
    this.#spec = { identity: { family: { name: text(family, 'statechart() family') }, revision } };
    if (name !== undefined) {
      this.#spec.name = text(name, 'statechart() name');
    }
    if (binding !== undefined) {
      if (binding !== 'early' && binding !== 'late') {
        throw invalid('statechart(): binding must be "early" or "late"');
      }
      this.#spec.binding = binding;
    }
  }

  data() {
    throw needsDataModel('data()');
  }

  dataExpression() {
    throw needsDataModel('dataExpression()');
  }

  initial(target, actions) {
    this.#root.check();
    this.#spec.initial = initialSpec(this.#root, target, actions);
    return this;
  }

  /** The spec in the generated shape. Consumes the builder. */
  build() {
    this.#root.check();
    this.#root.consumed = true;
    return { ...this.#spec, states: this.states };
  }
}

/**
 * Begin an ordered statechart definition. The builder is mutable until
 * build() or System.define() consumes it.
 */
function statechart(options) {
  return new DefinitionBuilder(options);
}

module.exports = { statechart, literal, DefinitionBuilder, literalBrand };
