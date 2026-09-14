# frozen_string_literal: true

require "mkmf"

repo_root = File.expand_path("../../../..", __dir__)
src_dir = File.join(repo_root, "src")

$INCFLAGS << " -I#{src_dir}"
$CFLAGS << " -std=c99 -Wall -Wextra"
$CFLAGS.gsub!(/(?:^|\s)-g(?:\s|$)/, " ")
$suffix = RbConfig::CONFIG.fetch("DLEXT", "so")
$postlink = "mkdir -p tinyactors && ln -sf ../tinyactors_raw.#{$suffix} tinyactors/tinyactors_raw.#{$suffix}"
RbConfig::CONFIG["POSTLINK"] = ":"
$VPATH = [src_dir]
$srcs = (Dir[File.join(__dir__, "*.c")] + Dir[File.join(src_dir, "*.c")]).map { |path| File.basename(path) }.sort

create_makefile("tinyactors/tinyactors_raw")
