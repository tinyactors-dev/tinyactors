# frozen_string_literal: true

require 'digest'
require 'fileutils'
require 'json'
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

  # The content's documents: the site, then its pages and its guides.
  def documents = [CONTENT.join('site.xml'), *CONTENT.glob('pages/*.xml').sort, *CONTENT.glob('guides/*.xml').sort]

  def validate!
    run('sxml', 'validate', "--schema=#{SCHEMA}", *documents)
    puts "website: #{documents.size} documents valid against #{SCHEMA.relative_path_from(ROOT)}"
  end

  # Validates the content, then writes the site into build/site: the pages,
  # sitemap.xml, each page's Markdown, llms.txt and llms-full.txt from the
  # XSLT, robots.txt, and assets/.
  def build!
    validate!
    FileUtils.rm_rf(SITE)
    FileUtils.mkdir_p(SITE)
    packages!
    run('sxml', 'transform', "--result-directory=#{SITE}", "--param=version=#{version}", STYLESHEET, CONTENT.join('site.xml'))
    assets!
    vendor!
    live!
    puts "website: built #{SITE.relative_path_from(ROOT)} (#{SITE.glob('**/*').count(&:file?)} files)"
  end

  # What the pages append to the URLs of the stylesheet and the scripts
  # (`?v=`): a hash of what they are built from, so a deploy that changes
  # one is fetched anew whatever a cache kept.
  def version
    inputs = [*ROOT.glob('components/*.mjs'), *PRESENCE_BROWSER, ROOT.join('style/site.css'), DESIGN.join('styles.css'),
              *DESIGN.glob('tokens/*.css'), DESIGN.join('components/components.css')].sort
    Digest::SHA256.hexdigest(inputs.map { |file| "#{file.basename}\0#{file.read}" }.join("\0"))[0, 12]
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
    # The content's images (an author's portrait), served at /assets/images/.
    FileUtils.cp_r(CONTENT.join('images'), assets.join('images')) if CONTENT.join('images').directory?
    FileUtils.mkdir_p(assets.join('platforms'))
    ROOT.glob('icons/platforms/*.svg').each { |file| FileUtils.cp(file, assets.join('platforms')) }
    {
      'favicon.svg' => 'assets/brand/favicon/favicon.svg',
      'favicon.ico' => 'assets/brand/favicon/favicon.ico',
      'apple-touch-icon.png' => 'assets/brand/favicon/apple-touch-icon.png',
      'logo-mark.svg' => 'assets/logo-mark.svg',
      'ant.svg' => 'assets/ant-resting.svg', # the anthill's sprites: rigged, legs pivot
      'logo-mark-dark.svg' => 'assets/brand/svg/tinyactors-symbol-parchment.svg'
    }.each { |name, source| FileUtils.cp(DESIGN.join(source), assets.join('brand', name)) }
    SITE.join('robots.txt').write("User-agent: *\nAllow: /\n\nSitemap: #{ORIGIN}/sitemap.xml\n")
    # Only the fonts are cached long: their names never change content.
    # Everything else keeps Cloudflare's default (revalidated by its ETag),
    # so a deploy is seen at once, and a response from before it (a 404 of
    # a file the deploy adds) is never kept.
    # The site for agents (render/markdown.xsl) is UTF-8 text, and each
    # page's Markdown says so with its type.
    SITE.join('_headers').write(<<~HEADERS)
      /assets/fonts/*
        Cache-Control: public, max-age=31536000, immutable
      /*.md
        Content-Type: text/markdown; charset=utf-8
      /llms.txt
        Content-Type: text/plain; charset=utf-8
      /llms-full.txt
        Content-Type: text/plain; charset=utf-8
    HEADERS
  end

  # The browser dependencies the pages load, from package.json and its
  # lockfile, each copied to vendor/<name>/: only what a browser loads (no
  # source maps, no typings) and its licence. The pages' import map
  # (render/site.xsl) names their entry points.
  VENDOR = {
    'scxmljs' => ['@tinyactors/scxmljs', %w[LICENSE THIRD_PARTY_NOTICES dist/**/*.js dist/themes/*.css]],
    'quickjs-emscripten-core' => ['quickjs-emscripten-core', %w[LICENSE dist/*.mjs]],
    'quickjs-singlefile-browser-release-sync' => ['@jitl/quickjs-singlefile-browser-release-sync', %w[LICENSE dist/*.mjs]],
    'quickjs-ffi-types' => ['@jitl/quickjs-ffi-types', %w[LICENSE dist/*.mjs]],
    'highlight.js' => ['@highlightjs/cdn-assets', %w[LICENSE es/core.min.js es/languages/{xml,javascript,typescript,python,c,bash}.min.js]],
    # The presence ants' flags, one fetched per country seen (presence/layer.mjs).
    'flag-icons' => ['flag-icons', %w[LICENSE flags/4x3/*.svg]],
    # The live chat's typeface (examples/chat-live), Latin at 400, 700 and 900.
    'lato' => ['@fontsource/lato', %w[LICENSE files/lato-latin-{400,700,900}-normal.woff2]]
  }.freeze
  # Presence's browser side (docs/presence.md): its modules and chart, and
  # the ant's gait from the ant-animation skill, its one copy.
  PRESENCE = ROOT.join('presence')
  PRESENCE_BROWSER = [PRESENCE.join('client.mjs'), PRESENCE.join('layer.mjs'), PRESENCE.join('link.scxml'),
                      ROOT.join('.agents/skills/ant-animation/scripts/ant.mjs')].freeze

  # website/'s packages, installed before the transform and vendor!:
  # installed again whenever one is missing or at another version than
  # package.json pins, so a checkout with an older node_modules builds too.
  def packages!
    pinned = JSON.parse(ROOT.join('package.json').read).fetch('dependencies')
    current = pinned.all? do |name, version|
      manifest = ROOT.join('node_modules', name, 'package.json')
      manifest.file? && JSON.parse(manifest.read)['version'] == version
    end
    run('npm', 'ci', '--no-audit', '--no-fund') unless current
  end

  def vendor!
    VENDOR.each do |name, (package, globs)|
      source = ROOT.join('node_modules', package)
      fail!("#{package} is not installed (npm ci in website/)") unless source.directory?
      globs.flat_map { |glob| source.glob(glob) }.each do |file|
        target = SITE.join('vendor', name, file.relative_path_from(source))
        FileUtils.mkdir_p(target.dirname)
        FileUtils.cp(file, target)
      end
    end
    # The charts a page shows as a diagram are served beside their examples.
    EXAMPLES.glob('**/*.scxml').reject { |file| file.to_s.include?('/node_modules/') }.each do |file|
      target = SITE.join('examples', file.relative_path_from(EXAMPLES))
      FileUtils.mkdir_p(target.dirname)
      FileUtils.cp(file, target)
    end
  end

  # What the live examples need in the browser: the components, each
  # example a page runs (its modules and charts), and the package they
  # import, @tinyactors/wasm at the version the examples' lockfiles pin,
  # which the pages' import map names.
  def live!
    FileUtils.mkdir_p(SITE.join('assets/components'))
    ROOT.glob('components/*.mjs').each { |file| FileUtils.cp(file, SITE.join('assets/components')) }
    examples = documents.flat_map { |document| document.read.scan(/<(?:live|playground)\s[^>]*src="[^"]*examples\/([^\/"]+)\//).flatten }.uniq
    vendored = nil
    examples.each do |name|
      dir = EXAMPLES.join(name)
      run('npm', 'ci', '--no-audit', '--no-fund', chdir: dir) unless dir.join('node_modules/.package-lock.json').file?
      FileUtils.mkdir_p(SITE.join('examples', name))
      # its modules and charts, and those under a charts/ beside them (the live chat's)
      dir.glob('{*,charts/**/*}.{mjs,scxml}').each do |file|
        target = SITE.join('examples', name, file.relative_path_from(dir))
        FileUtils.mkdir_p(target.dirname)
        FileUtils.cp(file, target)
      end
      package = dir.join('node_modules/@tinyactors/wasm')
      version = package.join('package.json').read[/"version":\s*"([^"]+)"/, 1]
      fail!("examples/#{name} uses @tinyactors/wasm #{version}, another example #{vendored}") if vendored && vendored != version
      next if vendored

      vendored = version
      target = SITE.join('vendor/tinyactors-wasm')
      FileUtils.mkdir_p(target)
      %w[index.mjs lib tinyactors.wasm LICENSE].each { |entry| FileUtils.cp_r(package.join(entry), target) }
    end
    puts "website: #{examples.size} live examples, @tinyactors/wasm #{vendored}" unless examples.empty?
    presence!(vendored)
  end

  # Presence's browser side into assets/presence/, beside the
  # @tinyactors/wasm the live examples vendor, which its chart runs on.
  def presence!(vendored)
    return unless CONTENT.join('site.xml').read.include?('<presence ')
    fail!('presence needs @tinyactors/wasm, which only a live example vendors') unless vendored
    FileUtils.mkdir_p(SITE.join('assets/presence'))
    PRESENCE_BROWSER.each { |file| FileUtils.cp(file, SITE.join('assets/presence')) }
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
