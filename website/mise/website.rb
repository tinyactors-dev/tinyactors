# frozen_string_literal: true

require 'digest'
require 'fileutils'
require 'open3'
require 'pathname'
require 'toolchain'

# What the website's tasks share: where things are, validating the content,
# building the site into build/site, and packing it for a release.
module Website
  ROOT = Pathname.new(__dir__).join('..').expand_path
  REPO = ROOT.parent
  CONTENT = ROOT.join('content')
  SCHEMA = ROOT.join('schema/site.xsd')
  STYLESHEET = ROOT.join('render/site.xsl')
  BUILD = ROOT.join('build')
  SITE = BUILD.join('site')
  EXAMPLES = ROOT.join('examples')
  # The design system, one copy for the repository: its tokens and
  # components are put ahead of style/site.css, its fonts and brand files
  # copied, so the site follows it without a copy of its own in git.
  DESIGN = REPO.join('.agents/skills/tinyactors-design')
  # The origin the site is served from; content/site.xml says it too.
  ORIGIN = 'https://www.tinyactors.dev'
  # wrangler, as staticjs's site deploys with it.
  WRANGLER = 'wrangler@4.146.0'

  module_function

  def fail!(message)
    abort "website: #{message}"
  end

  # Runs `command`, streaming its output; fails the task when it fails.
  def run(*command, chdir: ROOT, env: {})
    puts "$ #{command.join(' ')}"
    system(env, *command.map(&:to_s), chdir: chdir.to_s) or fail!("#{command.first} failed")
  end

  # `command`'s output and whether it succeeded, without failing.
  def capture(*command, chdir: ROOT)
    out, status = Open3.capture2e(*command.map(&:to_s), chdir: chdir.to_s)
    [out, status.success?]
  end

  # The content's documents: the site, then its pages.
  def documents = [CONTENT.join('site.xml'), *CONTENT.glob('pages/*.xml').sort]

  def validate!
    run('sxml', 'validate', "--schema=#{SCHEMA}", *documents)
    puts "website: #{documents.size} documents valid against #{SCHEMA.relative_path_from(ROOT)}"
  end

  # Validates the content, then writes the site into build/site: the pages,
  # sitemap.xml, llms.txt and robots.txt from the XSLT, and assets/.
  def build!
    validate!
    FileUtils.rm_rf(SITE)
    FileUtils.mkdir_p(SITE)
    run('sxml', 'transform', "--result-directory=#{SITE}", STYLESHEET, CONTENT.join('site.xml'))
    assets!
    puts "website: built #{SITE.relative_path_from(ROOT)} (#{SITE.glob('**/*').count(&:file?)} files)"
  end

  # One stylesheet: the design system's (styles.css's imports, in order)
  # with its font URLs made absolute, then the site's own. And the fonts and
  # the brand files the pages name.
  def assets!
    assets = SITE.join('assets')
    FileUtils.mkdir_p(assets.join('brand'))
    imports = DESIGN.join('styles.css').read.scan(/@import url\('([^']+)'\);/).flatten
    fail!("#{DESIGN.join('styles.css')} imports nothing") if imports.empty?
    design = imports.map do |path|
      css = DESIGN.join(path).read.gsub(%r{url\('\.\./assets/fonts/([^']+)'\)}, "url('/assets/fonts/\\1')")
      "/* #{path}, from the design system */\n#{css.strip}\n"
    end
    assets.join('site.css').write([*design, ROOT.join('style/site.css').read].join("\n"))
    FileUtils.cp_r(DESIGN.join('assets/fonts'), assets.join('fonts'))
    {
      'favicon.svg' => 'assets/brand/favicon/favicon.svg',
      'favicon.ico' => 'assets/brand/favicon/favicon.ico',
      'apple-touch-icon.png' => 'assets/brand/favicon/apple-touch-icon.png',
      'logo-mark.svg' => 'assets/logo-mark.svg',
      'logo-mark-dark.svg' => 'assets/brand/svg/tinyactors-symbol-parchment.svg'
    }.each { |name, source| FileUtils.cp(DESIGN.join(source), assets.join('brand', name)) }
    SITE.join('robots.txt').write("User-agent: *\nAllow: /\n\nSitemap: #{ORIGIN}/sitemap.xml\n")
    SITE.join('_headers').write(<<~HEADERS)
      /assets/*
        Cache-Control: public, max-age=86400
      /assets/fonts/*
        Cache-Control: public, max-age=31536000, immutable
    HEADERS
  end

  # A deterministic .tar.gz of build/site as `website-<version>/`: sorted
  # entries, one mtime (the release commit's), owner and group 0, no name or
  # time in the gzip header, so the same commit packs the same bytes.
  def tarball!(version, out_dir, epoch)
    staging = BUILD.join('release', 'staging')
    FileUtils.rm_rf(staging)
    FileUtils.mkdir_p(staging)
    name = "website-#{version}"
    FileUtils.cp_r(SITE, staging.join(name))
    tar = Toolchain::Tool[:tar].run('--sort=name', "--mtime=@#{epoch}", '--owner=0', '--group=0', '--numeric-owner',
                                    '--mode=u=rwX,go=rX', '--format=gnu', '-C', staging, '-cf', '-', name, binary: true)
    out = out_dir.join("#{name}.tar.gz")
    out.binwrite(Toolchain::Tool[:gzip].run('-n', '-9', stdin: tar, binary: true))
    FileUtils.rm_rf(staging)
    out
  end
end
