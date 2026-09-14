# frozen_string_literal: true

require "json"
require "minitest/autorun"
require "open3"
require "rbconfig"
require "tempfile"

class RubyCoverageAuditTest < Minitest::Test
  ROOT = File.expand_path("..", __dir__)
  CHECKER = File.join(ROOT, "tools/check_ruby_interface.rb")

  def ledger
    JSON.parse(File.read(File.join(ROOT, "bindings/ruby/api-coverage.json")))
  end

  def audit(input = ledger, *arguments)
    Tempfile.create(["ruby-coverage", ".json"]) do |file|
      file.write(JSON.generate(input))
      file.flush
      Open3.capture3(RbConfig.ruby, CHECKER, "--manifest", file.path, *arguments)
    end
  end

  def test_planned_coverage_is_reported_without_claiming_implementation
    output, errors, status = audit
    assert status.success?, errors
    entries = ledger.fetch("entries")
    implemented = entries.count { |entry| entry["status"] == "implemented" }
    assert_includes output, "#{implemented} implemented; #{entries.size - implemented} planned"
    assert_match(/not runtime behavior/, output)
  end

  def test_complete_mode_matches_actual_coverage
    input = ledger
    _, errors, status = audit(input, "--require-complete")
    if input["entries"].any? { |entry| entry["status"] == "planned" }
      refute status.success?
      assert_match(/remain planned/, errors)
    else
      assert status.success?, errors
    end
  end

  def test_missing_and_duplicate_assignments_fail
    input = ledger
    input["entries"][0] = input["entries"][1].dup
    _, errors, status = audit(input)
    refute status.success?
    assert_match(/Duplicate assignments/, errors)
    assert_match(/Missing C procedures/, errors)
  end

  def test_signature_changes_fail
    input = ledger
    input["entries"][0]["c_signature"] = "void (void)"
    _, errors, status = audit(input)
    refute status.success?
    assert_match(/C signature changed/, errors)
  end

  def test_missing_brief_fails
    input = ledger
    input["entries"][0]["brief"] = "bindings/ruby/briefs/nonexistent.md"
    _, errors, status = audit(input)
    refute status.success?
    assert_match(/Missing brief file/, errors)
  end

  def test_implemented_claim_requires_real_source_and_tests
    input = ledger
    input["entries"][0]["status"] = "implemented"
    input["entries"][0]["source"] = "bindings/ruby/ext/tinyactors_raw/nonexistent.c"
    input["entries"][0]["test"] = "bindings/ruby/test/nonexistent_test.rb"
    _, errors, status = audit(input)
    refute status.success?
    assert_match(/Missing source file/, errors)
    assert_match(/Missing test file/, errors)
  end

  def test_existing_files_cannot_substitute_for_a_callable_wrapper
    input = ledger
    entry = input["entries"].find { |item| item["status"] == "planned" }
    skip "Every procedure already has implementation evidence" unless entry
    entry["status"] = "implemented"
    entry["source"] = "bindings/ruby/lib/tinyactors/raw.rb"
    entry["test"] = "bindings/ruby/briefs/CONTRACT.md"
    _, errors, status = audit(input)
    refute status.success?
    assert_match(/without callable exports/, errors)
  end
end
