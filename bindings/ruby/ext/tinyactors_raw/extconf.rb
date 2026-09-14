# frozen_string_literal: true

require "mkmf"

repo_root = File.expand_path("../../../..", __dir__)
src_dir = File.join(repo_root, "src")

$INCFLAGS << " -I#{src_dir}"
$CFLAGS << " -std=c99 -Wall -Wextra"
$CFLAGS.gsub!(/(?:^|\s)-g(?:\s|$)/, " ")
$postlink = ""
RbConfig::CONFIG["POSTLINK"] = ":"
$VPATH = [src_dir]
$srcs = ["tinyactors_raw.c"] + Dir[File.join(src_dir, "*.c")].map { |path| File.basename(path) }.sort

create_makefile("tinyactors_raw")
