# frozen_string_literal: true

require 'base64'
require 'fileutils'
require 'json'
require 'net/http'
require 'pathname'
require 'shellwords'
require 'tmpdir'
require 'toolchain'

# What the support tasks share: the exe VM ta-mcp that runs the server,
# reached through exe's HTTP API (POST https://exe.dev/exec, the body an
# exe command), as the website's presence server is (website/mise/presence.rb).
# The API key is EXE_API_KEY and the GitHub App's fields are GITHUB_APP_*,
# which fnox reads from 1Password (the root fnox.toml); a task re-runs
# itself under fnox when the key is not set.
module Support
  ROOT = Pathname.new(__dir__).join('..').expand_path
  REPO = ROOT.parent
  VM = 'ta-mcp'
  HOME = '/home/exedev'
  PORT = 8000 # exe's proxy forwards to it (the VM's default proxy port)
  # The URL clients reach while the proxy is private; mcp.tinyactors.dev
  # once the domain is added (a later, separate step).
  PUBLIC_URL = "https://mcp.tinyactors.dev" # a CNAME to ta-mcp.exe.xyz, DNS only; exe issues the certificate
  # What the VM runs; node_modules is installed there from the lockfile.
  FILES = %w[server.mjs package.json package-lock.json lib charts skills tools].freeze
  # The documentation the server searches: the packages' guides and READMEs.
  DOCS = {
    'tinyactors-napi/docs/guide' => 'tinyactors-napi/docs/guide',
    'tinyactors-napi/README.md' => 'tinyactors-napi/README.md',
    'tinyactors-cpython/README.md' => 'tinyactors-cpython/README.md',
    'tinyactors-cpython/docs/guide' => 'tinyactors-cpython/docs/guide',
    'tinyactors-core/docs/cli.md' => 'tinyactors-core/docs/cli.md',
  }.freeze

  # What the backups need in the VM's environment (lib/backup.mjs); fnox
  # reads them from the 1Password item "tinyactors-support backups".
  BACKUP_FIELDS = %w[BACKUP_S3_ENDPOINT BACKUP_S3_BUCKET BACKUP_S3_ACCESS_KEY_ID BACKUP_S3_SECRET_ACCESS_KEY BACKUP_KEY].freeze

  module_function

  def fail!(message)
    abort "support: #{message}"
  end

  # Re-runs the task under fnox when EXE_API_KEY is missing.
  def secrets!
    return if ENV['EXE_API_KEY'].to_s != ''
    if ENV['SUPPORT_UNDER_FNOX'].nil? && system('sh', '-c', 'command -v fnox', out: File::NULL)
      exec({ 'SUPPORT_UNDER_FNOX' => '1' }, 'fnox', 'exec', '--', $PROGRAM_NAME, *ARGV)
    end
    fail!('EXE_API_KEY is not set (fnox exec -- mise run ...)')
  end

  # An exe command, as `ssh exe.dev <command>` would run it; answers its output.
  def exe(command, timeout: 300)
    uri = URI('https://exe.dev/exec')
    request = Net::HTTP::Post.new(uri, 'Authorization' => "Bearer #{ENV.fetch('EXE_API_KEY')}")
    request.body = command
    response = Net::HTTP.start(uri.host, uri.port, use_ssl: true, read_timeout: timeout) { |http| http.request(request) }
    fail!("exe: #{response.code} #{response.body[0, 300]}") unless response.is_a?(Net::HTTPSuccess)
    response.body
  end

  # A bash script run on the VM as exedev (sudo needs no password there).
  # It goes as base64 in the command, since the API has no stdin.
  def vm(script, timeout: 300)
    exe("ssh #{VM} echo #{Base64.strict_encode64(script)} | base64 -d | bash", timeout: timeout)
  end

  # Bytes to a file on the VM, in pieces: exe's API takes a body of at most
  # 64 KB, and a bundle is larger.
  def upload(base64, path)
    vm("rm -f #{path}.b64")
    base64.scan(/.{1,48000}/).each { |piece| exe("ssh #{VM} echo #{piece} '>>' #{path}.b64") }
    vm("base64 -d #{path}.b64 > #{path} && rm #{path}.b64 && echo uploaded $(stat -c %s #{path}) bytes")
  end

  # The server's files and the documentation, as a .tar.gz in base64, made
  # with the repository's pinned GNU tar and gzip.
  def bundle
    Dir.mktmpdir('support-bundle') do |dir|
      FILES.each { |file| FileUtils.cp_r(ROOT.join(file), dir) }
      DOCS.each do |from, to|
        source = REPO.join(from)
        next unless source.exist?
        target = File.join(dir, 'content', to)
        FileUtils.mkdir_p(source.directory? ? target : File.dirname(target))
        source.directory? ? FileUtils.cp_r(Dir[source.join('*.md').to_s], target) : FileUtils.cp(source, target)
      end
      tar, error, status = Toolchain::Tool[:tar].capture('cf', '-', '.', chdir: dir, binary: true)
      fail!("tar failed: #{error}") unless status.success?
      gzipped, error, status = Toolchain::Tool[:gzip].capture('-n', stdin: tar, binary: true)
      fail!("gzip failed: #{error}") unless status.success?
      Base64.strict_encode64(gzipped)
    end
  end

  # Node.js 22, the release unpacked, its packages installed, the systemd
  # unit written, the service restarted, and its health answered.
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
      dir=#{HOME}/support/releases/#{release}
      mkdir -p $dir #{HOME}/support/data
      tar xzf /tmp/support-bundle.tar.gz -C $dir && rm /tmp/support-bundle.tar.gz
      (cd $dir && npm ci --omit=dev --no-audit --no-fund --loglevel=error)
      ln -sfn $dir #{HOME}/support/current
      touch #{HOME}/support/env && chmod 600 #{HOME}/support/env
      sudo tee /etc/systemd/system/tinyactors-support.service >/dev/null <<'UNIT'
      [Unit]
      Description=tinyactors-support: the support channel for agents (mcp.tinyactors.dev)
      After=network-online.target

      [Service]
      User=exedev
      WorkingDirectory=#{HOME}/support/current
      EnvironmentFile=#{HOME}/support/env
      Environment=PORT=#{PORT}
      Environment=DATA_DIR=#{HOME}/support/data
      Environment=NODE_NO_WARNINGS=1
      ExecStart=/opt/node/bin/node server.mjs
      KillSignal=SIGTERM
      TimeoutStopSec=30
      Restart=always
      RestartSec=2

      [Install]
      WantedBy=multi-user.target
      UNIT
      sudo systemctl daemon-reload
      sudo systemctl enable --quiet tinyactors-support
      sudo systemctl restart tinyactors-support
      for i in $(seq 1 40); do curl -fsS localhost:#{PORT}/healthz && break; sleep 0.5; done
      echo
      sudo journalctl -u tinyactors-support -n 3 --no-pager -o cat
      ls -1dt #{HOME}/support/releases/* | tail -n +6 | xargs -r rm -rf
    BASH
  end

  # The environment file on the VM: PUBLIC_URL, and the GitHub App's fields
  # and TypeSafe's key for Jev when fnox has them (else the server runs on the
  # stand-ins). Values go into the file and nowhere else: nothing prints them.
  def env_script(fields)
    # single-quoted: systemd's EnvironmentFile and bash read the value as it is
    # (a PEM's spaces included); its newlines go as \n, which server.mjs turns back
    lines = fields.map do |key, value|
      fail!("#{key} holds a single quote") if value.include?("'")
      "#{key}='#{value.gsub("\n", '\\n')}'"
    end.join("\n")
    <<~BASH
      set -euo pipefail
      mkdir -p #{HOME}/support
      umask 077
      echo '#{Base64.strict_encode64(lines + "\n")}' | base64 -d > #{HOME}/support/env
      echo "support: env holds #{fields.keys.join(', ')}"
      if systemctl is-enabled --quiet tinyactors-support 2>/dev/null; then sudo systemctl restart tinyactors-support; fi
    BASH
  end

  # The daily backup: a systemd timer (03:17 UTC, a random delay of up to
  # 20 minutes) running tools/backup.mjs as a oneshot service of its own,
  # beside the server, never inside it. Enabled only when the environment
  # has the bucket and the key.
  def backup_timer_script(enable)
    <<~BASH
      sudo tee /etc/systemd/system/tinyactors-support-backup.service >/dev/null <<'UNIT'
      [Unit]
      Description=tinyactors-support: a backup of its data to the bucket, then the runs past 14 pruned
      After=network-online.target

      [Service]
      Type=oneshot
      User=exedev
      WorkingDirectory=#{HOME}/support/current
      EnvironmentFile=#{HOME}/support/env
      Environment=DATA_DIR=#{HOME}/support/data
      Environment=NODE_NO_WARNINGS=1
      ExecStart=/opt/node/bin/node tools/backup.mjs backup
      ExecStartPost=/opt/node/bin/node tools/backup.mjs drill
      UNIT
      sudo tee /etc/systemd/system/tinyactors-support-backup.timer >/dev/null <<'UNIT'
      [Unit]
      Description=tinyactors-support: the daily backup

      [Timer]
      OnCalendar=*-*-* 03:17:00 UTC
      RandomizedDelaySec=1200
      Persistent=true

      [Install]
      WantedBy=timers.target
      UNIT
      sudo systemctl daemon-reload
      #{enable ? 'sudo systemctl enable --now --quiet tinyactors-support-backup.timer; echo "support: the daily backup is on"' :
                 'sudo systemctl disable --now --quiet tinyactors-support-backup.timer 2>/dev/null || true; echo "support: the daily backup is off (no bucket or key)"'}
    BASH
  end

  def release_name
    sha = `git -C #{ROOT.to_s.shellescape} rev-parse --short HEAD`.strip
    dirty = `git -C #{ROOT.to_s.shellescape} status --porcelain -- .`.strip.empty? ? '' : '-dirty'
    "#{Time.now.utc.strftime('%Y%m%dT%H%M%S')}-#{sha}#{dirty}"
  end
end
