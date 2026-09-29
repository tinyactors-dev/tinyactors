# frozen_string_literal: true

require 'set'

# The direct call graph of LLVM IR, and its cycles: what `check-recursion`
# reads in core (ADR 0060) and in the host kernel (kernel ADR 0001, section
# 2). An edge is a `call` or `invoke` that names a function defined in the
# same IR; calls through function pointers leave the code being checked and
# are not edges. A cycle is a strongly connected component of more than one
# function, or one function that calls itself.
module CallGraph
  module_function

  # { function => Set of the defined functions it calls directly }.
  def calls(ir)
    calls = Hash.new { |hash, key| hash[key] = Set.new }
    current = nil
    ir.each_line do |line|
      if (match = line.match(/\Adefine\b[^@]*@("?)([\w.$]+)\1\(/))
        current = match[2]
        calls[current]
      elsif line.start_with?('}')
        current = nil
      elsif current && (match = line.match(/\b(?:call|invoke)\b[^@]*@("?)([\w.$]+)\1\(/))
        calls[current] << match[2]
      end
    end
    defined = calls.keys.to_set
    calls.each_value { |callees| callees.select! { |callee| defined.include?(callee) } }
    calls
  end

  # The cycles of `calls`, each a sorted list of function names, found with
  # Tarjan's strongly connected components, iteratively.
  def cycles(calls)
    index = {}
    low = {}
    on_stack = Set.new
    stack = []
    components = []
    counter = 0
    calls.each_key do |start|
      next if index.key?(start)

      work = [[start, calls[start].to_a, 0]]
      index[start] = low[start] = counter
      counter += 1
      stack << start
      on_stack << start
      until work.empty?
        node, callees, position = work.last
        if position < callees.length
          work.last[2] += 1
          callee = callees[position]
          if !index.key?(callee)
            index[callee] = low[callee] = counter
            counter += 1
            stack << callee
            on_stack << callee
            work << [callee, calls[callee].to_a, 0]
          elsif on_stack.include?(callee)
            low[node] = [low[node], index[callee]].min
          end
        else
          work.pop
          low[work.last[0]] = [low[work.last[0]], low[node]].min unless work.empty?
          next unless low[node] == index[node]

          component = []
          loop do
            member = stack.pop
            on_stack.delete(member)
            component << member
            break if member == node
          end
          components << component.sort
        end
      end
    end
    components.select { |component| component.length > 1 || calls[component.first].include?(component.first) }
  end
end
