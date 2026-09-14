# frozen_string_literal: true

require_relative "test_helper"

class BootstrapTest < Minitest::Test
  def test_loader_is_idempotent_and_uses_the_new_extension
    require "tinyactors"
    require "tinyactors/raw"
    assert TinyActors::Raw.singleton_methods(false).include?(:ta_text_from_cstr)
    assert $LOADED_FEATURES.any? { |path| path.end_with?("/tinyactors_raw.bundle", ".so", ".dylib") }
  end

  def test_text_helper_preserves_presence_and_stops_at_nul
    assert_nil TinyActors::Raw.ta_text_from_cstr(nil)
    assert_equal "", TinyActors::Raw.ta_text_from_cstr("")
    assert_equal "a", TinyActors::Raw.ta_text_from_cstr("a\0b")
    assert_equal "λ", TinyActors::Raw.ta_text_from_cstr("λ")
  end

  def test_constants_match_the_public_header_contract
    assert_equal 3, TinyActors::Raw::TA_ABI_VERSION
    assert_equal 0, TinyActors::Raw::TA_OK
    assert_equal 14, TinyActors::Raw::TA_FAULTED
    assert_equal "http://www.w3.org/TR/scxml/", TinyActors::Raw::TA_SCXML_INVOKE_TYPE
  end
end
