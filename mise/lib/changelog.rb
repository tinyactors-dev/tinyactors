# frozen_string_literal: true

# A changelog per component (docs/adr/0002-changelogs.md). The evidence for
# a release is computed from git: the commits since the last release
# (Changelog::Ledger), the change in the public API (Changelog::APISurface),
# and the notes of the ADRs the commits name (Changelog::ADRs). An agent
# reads that evidence (`mise run changelog:draft`) and writes the summary;
# `changelog:write` adds the generated parts and `changelog:check` rejects a
# section whose prose the evidence does not support. The release tasks take
# their release notes from the checked section (Release.notes).
#
# Everything here takes a Changelog::Repo, so the tests run it against a
# repository they build.
module Changelog
  class Error < StandardError; end
end

require 'changelog/components'
require 'changelog/repo'
require 'changelog/ledger'
require 'changelog/api_surface'
require 'changelog/adrs'
require 'changelog/evidence'
require 'changelog/document'
require 'changelog/check'
