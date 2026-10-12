# frozen_string_literal: true

require 'json'
require 'pathname'

# A binding's mapping table, `docs/api-map.md` (tinyactors-core/docs/bindings.md,
# section 2; ROADMAP.md, step 8, "Gates"): each item of the binding's API next
# to what core has behind it. The table is the one whose header row begins
# `| API item | Core |`; every row's Core cell names, in backticks, at least
# one of
#
#   - a function of core's API descriptions (include/*.api.json): `ta_host_send`;
#   - a constant of them, an effect or a run status: `TA_EFFECT_LOG`,
#     `TA_HOST_EFFECT_PEER`, `TA_RUN_IDLE`;
#   - a request method of docs/requests.md: `actor.context`;
#
# and nothing else in backticks. An item with nothing in core behind it does
# not belong in the API. Each binding's `mise run lint` checks its table;
# `mise run api-map-test` at the root tests the check.
module APIMap
  ROOT = Pathname.new(__dir__).join('../..').expand_path
  CORE = ROOT.join('tinyactors-core')
  HEADER = /\A\|\s*API item\s*\|\s*Core\s*\|/

  Row = Data.define(:line, :item, :core)

  module_function

  # The rows of the mapping table in `text`, or nil when it has none.
  def rows(text)
    lines = text.lines
    start = lines.index { |line| line.match?(HEADER) } or return nil

    lines[(start + 2)..].each_with_index.take_while { |line, _| line.start_with?('|') }.map do |line, index|
      cells = line.strip.delete_prefix('|').delete_suffix('|').split('|').map(&:strip)
      Row.new(line: start + 3 + index, item: cells[0].to_s, core: cells[1].to_s)
    end
  end

  # Every name core offers a binding: functions and constants of the API
  # descriptions, and the request methods.
  def known(core = CORE)
    names = Dir[core.join('include/*.api.json').to_s].flat_map do |file|
      api = JSON.parse(File.read(file))
      api.fetch('functions', []).map { |entry| entry.fetch('name') } +
        api.fetch('constants', []).map { |entry| entry.fetch('name') }
    end
    names + methods(core.join('docs/requests.md').read)
  end

  # The methods of requests.md: the backticked names of the headings under
  # "## Methods".
  def methods(text)
    section = text[/^## Methods\s*\n(.*?)(?=^## |\z)/m, 1].to_s
    section.scan(/^### (.+)$/).flatten.flat_map { |heading| heading.scan(/`([^`]+)`/).flatten }
  end

  # What is wrong with the table in `text`, one message per problem.
  def problems(text, known)
    rows = rows(text) or return ['no mapping table: a table whose header row begins "| API item | Core |"']

    known = known.to_set
    rows.flat_map do |row|
      names = row.core.scan(/`([^`]+)`/).flatten
      found = []
      found << "line #{row.line}: the row has no API item" if row.item.empty?
      found << "line #{row.line}: #{row.item} names nothing in core in backticks" if names.empty?
      names.reject { |name| known.include?(name) }.each do |name|
        found << "line #{row.line}: #{row.item}: `#{name}` is no function, constant or request method of core"
      end
      found
    end
  end

  # Checks a binding's table; answers the problems, none when it passes.
  def check(path, core = CORE) = problems(Pathname.new(path).read, known(core))
end
