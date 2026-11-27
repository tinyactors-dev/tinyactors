# frozen_string_literal: true

# A fixture program for the MCP runner's tests (test/mcp_test.rb), which
# answers as docs/mcp.md says: fixture/open and fixture/spawn, MCP's
# initialize, tools/list (a tool "echo") and tools/call: "echo" gives its
# arguments as its result, "refuse" an error, "die" ends the program.
require 'json'

$stdout.sync = true
sessions = 0
$stdin.each_line do |line|
  message = JSON.parse(line)
  next unless message.key?('id')

  result = case message['method']
           when 'fixture/open' then {}
           when 'fixture/spawn' then { 'session' => (sessions += 1) }
           when 'initialize' then { 'protocolVersion' => message['params']['protocolVersion'], 'capabilities' => {}, 'serverInfo' => { 'name' => 'fake', 'version' => '0' } }
           when 'tools/list' then { 'tools' => [{ 'name' => 'echo', 'description' => 'echo', 'inputSchema' => { 'type' => 'object' } }] }
           when 'tools/call'
             name = message['params']['name']
             exit(1) if name == 'die'
             if name == 'refuse'
               { 'isError' => true, 'content' => [{ 'type' => 'text', 'text' => 'refused: no' }] }
             else
               { 'content' => [{ 'type' => 'text', 'text' => 'echo' }], 'structuredContent' => message['params']['arguments'] }
             end
           end
  puts JSON.generate({ 'jsonrpc' => '2.0', 'id' => message['id'], 'result' => result })
end
