# frozen_string_literal: true

require 'base64'
require 'json'
require 'net/http'
require 'securerandom'
require 'shellwords'
require 'open3'
require_relative 'website'

# What the presence tasks share: the exe VM that runs the presence server
# (website/docs/presence.md), reached through exe's HTTP API
# (POST https://exe.dev/exec, the body an exe command), and how the server
# is put on it. The API key is EXE_API_KEY, which fnox reads from 1Password
# (the root fnox.toml); a task re-runs itself under fnox when it is not set.
module Presence
  VM = 'ta-presence'
  DIR = Website::ROOT.join('presence')
  # What the VM runs: the server and what it imports. node_modules is
  # installed there from the lockfile.
  FILES = %w[server.mjs world.mjs ticket.mjs visitor.scxml package.json package-lock.json].freeze
  ORIGINS = Website::ORIGIN
  HOME = '/home/exedev'
  PORT = 8000 # exe's proxy forwards to it (the VM's default proxy port)

  module_function

  # Re-runs the task under fnox when EXE_API_KEY is missing.
  def secrets!
    return if ENV['EXE_API_KEY'].to_s != ''
    if ENV['PRESENCE_UNDER_FNOX'].nil? && system('sh', '-c', 'command -v fnox', out: File::NULL)
      exec({ 'PRESENCE_UNDER_FNOX' => '1' }, 'fnox', 'exec', '--', $PROGRAM_NAME, *ARGV)
    end
    Website.fail!('EXE_API_KEY is not set (fnox exec -- mise run ...)')
  end

  # An exe command, as `ssh exe.dev <command>` would run it; answers its output.
  def exe(command, timeout: 300)
    uri = URI('https://exe.dev/exec')
    request = Net::HTTP::Post.new(uri, 'Authorization' => "Bearer #{ENV.fetch('EXE_API_KEY')}")
    request.body = command
    response = Net::HTTP.start(uri.host, uri.port, use_ssl: true, read_timeout: timeout) { |http| http.request(request) }
    Website.fail!("exe: #{response.code} #{response.body[0, 300]}") unless response.is_a?(Net::HTTPSuccess)
    response.body
  end

  # A bash script run on the VM as exedev (sudo needs no password there).
  # It goes as base64 in the command, since the API has no stdin.
  def vm(script, timeout: 300)
    exe("ssh #{VM} echo #{Base64.strict_encode64(script)} | base64 -d | bash", timeout: timeout)
  end

  # The server's files as a .tar.gz, base64, for the VM to unpack.
  def bundle
    out, status = Open3.capture2('tar', 'czf', '-', *FILES, chdir: DIR.to_s, binmode: true)
    Website.fail!('tar failed') unless status.success?
    Base64.strict_encode64(out)
  end

  # Node.js 22 (the repository's pin), the release unpacked, its packages
  # installed, the systemd unit written, the service restarted, and its
  # health answered.
  def deploy_script(release)
    <<~BASH
      set -euo pipefail
      if ! /opt/node/bin/node --version 2>/dev/null | grep -q '^v22\\.'; then
        base=https://nodejs.org/dist/latest-v22.x
        name=$(curl -fsSL $base/SHASUMS256.txt | awk '/linux-x64.tar.xz$/ {print $2}')
        sum=$(curl -fsSL $base/SHASUMS256.txt | awk '/linux-x64.tar.xz$/ {print $1}')
        curl -fsSL -o /tmp/node.tar.xz $base/$name
        echo "$sum  /tmp/node.tar.xz" | sha256sum -c - >/dev/null
        sudo rm -rf /opt/node && sudo mkdir -p /opt/node
        sudo tar xJf /tmp/node.tar.xz -C /opt/node --strip-components=1 && rm /tmp/node.tar.xz
      fi
      export PATH=/opt/node/bin:$PATH
      dir=#{HOME}/presence/releases/#{release}
      mkdir -p $dir
      echo '#{bundle}' | base64 -d | tar xzf - -C $dir
      (cd $dir && npm ci --omit=dev --no-audit --no-fund --loglevel=error)
      ln -sfn $dir #{HOME}/presence/current
      test -f #{HOME}/presence/env || { echo 'presence: no #{HOME}/presence/env: mise run presence:key first' >&2; exit 1; }
      sudo tee /etc/systemd/system/presence.service >/dev/null <<'UNIT'
      [Unit]
      Description=presence: the website's ants (tinyactors)
      After=network-online.target

      [Service]
      User=exedev
      WorkingDirectory=#{HOME}/presence/current
      EnvironmentFile=#{HOME}/presence/env
      Environment=PORT=#{PORT}
      ExecStart=/opt/node/bin/node server.mjs
      Restart=always
      RestartSec=2

      [Install]
      WantedBy=multi-user.target
      UNIT
      sudo systemctl daemon-reload
      sudo systemctl enable --quiet presence
      sudo systemctl restart presence
      for i in $(seq 1 20); do curl -fsS localhost:#{PORT}/health && break; sleep 0.5; done
      echo
      ls -1dt #{HOME}/presence/releases/* | tail -n +6 | xargs -r rm -rf
    BASH
  end

  # TICKET_KEY and ORIGINS on the VM; the service restarted when it runs.
  def key_script(key)
    <<~BASH
      set -euo pipefail
      mkdir -p #{HOME}/presence
      umask 077
      printf 'TICKET_KEY=%s\\nORIGINS=%s\\n' '#{key}' '#{ORIGINS}' > #{HOME}/presence/env
      if systemctl is-enabled --quiet presence 2>/dev/null; then sudo systemctl restart presence; fi
      echo presence: key set
    BASH
  end

  def release_name
    sha = `git -C #{Website::ROOT.to_s.shellescape} rev-parse --short HEAD`.strip
    dirty = `git -C #{Website::ROOT.to_s.shellescape} status --porcelain -- presence`.strip.empty? ? '' : '-dirty'
    "#{Time.now.utc.strftime('%Y%m%dT%H%M%S')}-#{sha}#{dirty}"
  end
end
