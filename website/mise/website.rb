# frozen_string_literal: true

require 'digest'
require 'fileutils'
require 'json'
require 'net/http'
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
  # The published format of an audience file (/audiences/audience.xsd).
  AUDIENCE_SCHEMA = ROOT.join('schema/audience.xsd')
  # The published format of a voice file (/voices/voice.xsd).
  VOICE_SCHEMA = ROOT.join('schema/voice.xsd')
  # The needs report: which pages and sections answer each audience's needs.
  NEEDS = ROOT.join('render/needs.xq')
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

  # The C libraries an example's main.c links, in link order: the
  # ECMAScript datamodel and the runtime leave libtinyactors's API undefined.
  C_LIBRARIES = %w[tinyactors_datamodel_ecmascript tinyactors_runtime tinyactors].freeze

  # The C libraries of tinyactors-core's newest release, for this machine:
  # each tarball checked against the release's SHA256SUMS and unpacked once
  # into build/c/<version>/<target>/<library>/ (include/ and lib/), as a
  # reader who installs them has them.
  def c_release
    version = REPO.join('tinyactors-core/CHANGELOG.md').read[/^## \[([^\]]+)\] - /, 1]
    target = Toolchain::Host.current.target.name
    dir = BUILD.join('c', version, target)
    return dir if C_LIBRARIES.all? { |library| dir.join(library, 'lib', "lib#{library}.a").file? }

    base = "https://github.com/tinyactors-dev/tinyactors-core/releases/download/v#{version}/"
    sums = download("#{base}SHA256SUMS").lines.to_h { |line| line.split.reverse }
    C_LIBRARIES.each do |library|
      name = "#{library}-#{version}-#{target}.tar.gz"
      tarball = download(base + name)
      fail!("#{name}: its SHA-256 is not the release's") unless Digest::SHA256.hexdigest(tarball) == sums[name]
      FileUtils.mkdir_p(dir.join(library))
      Toolchain::Tool[:tar].run('-xz', '--strip-components=1', '-C', dir.join(library), stdin: tarball, binary: true)
    end
    dir
  end

  # An example's C program, its main.c and the other .c files beside it,
  # built against c_release with the pinned clang (warnings are errors)
  # into build/c/programs/<name>.
  def c_program(dir, name)
    release = c_release
    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wshadow',
             *C_LIBRARIES.map { |library| "-I#{release.join(library, 'include')}" }]
    libraries = C_LIBRARIES.map { |library| release.join(library, 'lib', "lib#{library}.a").to_s }
    into = BUILD.join('c', 'programs', name)
    sources = dir.glob('*.c').sort.map(&:to_s)
    Toolchain::Target.host.program([*sources, *libraries, '-lm'], into: into.to_s, flags: flags)
    into
  end

  # The body at `url`, following the redirects of a release asset.
  def download(url, redirects = 5)
    response = Net::HTTP.get_response(URI(url))
    return response.body if response.is_a?(Net::HTTPSuccess)
    return download(response['location'], redirects - 1) if response.is_a?(Net::HTTPRedirection) && redirects.positive?

    fail!("#{url}: #{response.code}")
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
  # Addresses that moved (2026-11-26: the CLI section by kind of page), each
  # to where its page is now, permanently: a link someone kept still lands.
  # The voices replaced on 2026-11-28 by one voice per kind of page: a link
  # to an old voice file lands on its successor or, for the voice that
  # covered three kinds, on the index.
  MOVED = {
    '/voices/literal.xml' => '/voices/index.xml',
    '/voices/deep-dive.xml' => '/voices/explanation.xml',
    '/cli/ci/' => '/cli/guides/ci/',
    '/cli/protocol/' => '/cli/guides/scripting/',
    '/cli/snapshots/' => '/cli/guides/snapshots/',
    '/cli/run.schema.json' => '/cli/commands/run.schema.json'
  }.freeze

  # _redirects (Cloudflare's static assets read it): MOVED, each
  # command's page from /cli/<command>/ to /cli/commands/<command>/, and
  # every page's path without its trailing slash to the path, permanently
  # (the assets' own slash handling answers it with a 307; Cloudflare
  # applies _redirects first, whether or not an asset matches).
  def redirects!(commands)
    moved = MOVED.merge(commands.to_h { |command| ["/cli/#{command}/", "/cli/commands/#{command}/"] })
    slashless = sitemap_paths.select { |path| path.end_with?('/') && path != '/' }.to_h { |path| [path.delete_suffix('/'), path] }
    SITE.join('_redirects').write(moved.merge(slashless) { |from| fail!("_redirects: #{from} is both moved and a page") }
                                       .map { |from, to| "#{from} #{to} 301\n" }.join)
  end

  # The paths sitemap.xml lists, in its order.
  def sitemap_paths = SITE.join('sitemap.xml').read.scan(%r{<loc>#{Regexp.escape(ORIGIN)}([^<]*)</loc>}).flatten

  # Every URL path the sitemap has ever held, one a line (# a comment):
  # append-only, so mise run test sees a published URL that stops working.
  PUBLISHED = ROOT.join('published-urls.txt')
  def published_paths = PUBLISHED.readlines(chomp: true).map(&:strip).reject { |line| line.empty? || line.start_with?('#') }

  # When each page's content last changed, for sitemap.xml's <lastmod>:
  # the date of the last commit that touched its content file (the
  # changelog page's also its packages' changelogs), or today when the
  # file has changes no commit holds; site.xml's for the pages it makes
  # (the namespaces'). Written to build/dates.xml for the stylesheet.
  DATES = BUILD.join('dates.xml')
  def dates!
    today = Time.now.utc.strftime('%Y-%m-%d')
    date = lambda do |files|
      files.map do |file|
        relative = file.expand_path.relative_path_from(REPO).to_s
        changed, = capture('git', 'status', '--porcelain', '--', relative, chdir: REPO)
        committed, = capture('git', 'log', '-1', '--format=%cs', '--', relative, chdir: REPO)
        changed.strip.empty? && !committed.strip.empty? ? committed.strip : today
      end.max
    end
    changelogs = CONTENT.join('site.xml').read.scan(/<package\b[^>]*\schangelog="([^"]+)"/).flatten.uniq.map { |file| CONTENT.join(file) }
    rows = documents.drop(1).filter_map do |document|
      text = document.read
      path = text[/<page\b[^>]*\spath="([^"]+)"/, 1] or next
      %(  <page path="#{path}" date="#{date.call([document, *(text.include?('<changelog') ? changelogs : [])])}"/>\n)
    end
    FileUtils.mkdir_p(BUILD)
    DATES.write(%(<dates site="#{date.call([CONTENT.join('site.xml')])}">\n#{rows.join}</dates>\n))
  end

  # The schemas the site's documents name, served where their names say:
  # the content's (schema/site.xsd) at /schemas/site.xsd, and the CLI's
  # JSON Schema at the path of its "$id" too (its page serves it beside
  # the commands).
  def schemas!
    FileUtils.mkdir_p(SITE.join('schemas'))
    FileUtils.cp(SCHEMA, SITE.join('schemas/site.xsd'))
    run_schema = SITE.glob('cli/**/run.schema.json').first or return
    id = JSON.parse(run_schema.read)['$id'] or fail!("#{run_schema.relative_path_from(SITE)}: no $id")
    target = SITE.join(URI(id).path.delete_prefix('/'))
    FileUtils.mkdir_p(target.dirname)
    FileUtils.cp(run_schema, target) unless target == run_schema
  end

  # /.well-known/security.txt (RFC 9116): where to report a vulnerability,
  # the repository's private security advisories, expiring a year after
  # the build.
  def security_txt!
    source = CONTENT.join('site.xml').read[/<packages\b[^>]*\ssource="([^"]+)"/, 1] or fail!('site.xml: no packages source')
    expires = (Time.now.utc + (365 * 24 * 60 * 60)).strftime('%Y-%m-%dT%H:%M:%SZ')
    FileUtils.mkdir_p(SITE.join('.well-known'))
    SITE.join('.well-known/security.txt').write(<<~SECURITY)
      Contact: #{source}/security/advisories/new
      Expires: #{expires}
      Canonical: #{ORIGIN}/.well-known/security.txt
      Preferred-Languages: en
    SECURITY
  end

  # XSLT, robots.txt, and assets/.
  def build!
    validate!
    FileUtils.rm_rf(SITE)
    FileUtils.mkdir_p(SITE)
    packages!
    # The CLI section's pages are made from the released command (mise/cli.rb).
    cli = CLI::PAGE.file? ? ["--param=cli=#{CLI.capture!.to_path}"] : []
    dates!
    run('sxml', 'transform', "--result-directory=#{SITE}", "--param=version=#{version}", "--param=dates=#{DATES}", *cli, STYLESHEET, CONTENT.join('site.xml'))
    CLI.serve!(SITE) unless cli.empty?
    redirects!(cli.empty? ? [] : CLI.commands)
    schemas!
    security_txt!
    FileUtils.cp(AUDIENCE_SCHEMA, SITE.join('audiences/audience.xsd'))
    FileUtils.cp(VOICE_SCHEMA, SITE.join('voices/voice.xsd'))
    needs!
    assets!
    vendor!
    live!
    copywriter!
    puts "website: built #{SITE.relative_path_from(ROOT)} (#{SITE.glob('**/*').count(&:file?)} files)"
  end

  # The needs report (render/needs.xq) into build/needs.json, not
  # published: for each audience's need, the pages and sections that answer
  # it, and the needs none does, which are the editors' work list. The
  # build prints the counts and never fails on an unanswered need.
  def needs!
    out, ok = capture('sxml', 'xquery', "--context-item=#{CONTENT.join('site.xml')}", '--method=json', NEEDS)
    fail!("render/needs.xq failed:\n#{out}") unless ok
    BUILD.join('needs.json').write(out)
    report = JSON.parse(out)
    report['audiences'].each do |audience|
      puts "website: #{audience['id']}: #{audience['answered']} of #{audience['needs'].size} needs answered"
    end
    puts "website: unanswered needs (build/needs.json): #{report['unanswered'].join(', ')}" unless report['unanswered'].empty?
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
    # The demos' videos and posters (mise run demo), served at /assets/videos/.
    FileUtils.cp_r(CONTENT.join('videos'), assets.join('videos')) if CONTENT.join('videos').directory?
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
    SITE.join('robots.txt').write("User-agent: *\nAllow: /\nDisallow: /copywriter/\n\nSitemap: #{ORIGIN}/sitemap.xml\n")
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
      /changelog/feed.xml
        Content-Type: application/atom+xml; charset=utf-8
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
    'highlight.js' => ['@highlightjs/cdn-assets', %w[LICENSE es/core.min.js es/languages/{xml,javascript,typescript,python,c,bash,yaml,ini,json,markdown}.min.js]],
    # The presence ants' flags, one fetched per country seen (presence/layer.mjs).
    'flag-icons' => ['flag-icons', %w[LICENSE flags/4x3/*.svg]],
    # The icons render/site.xsl inlines (f:lucide): only their licence.
    'lucide' => ['lucide-static', %w[LICENSE]],
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
    # The charts a page shows as a diagram, and the pictures a page shows
    # (<drawing src>: the tutorial's graph), are served beside their examples.
    EXAMPLES.glob('**/*.{scxml,svg}').reject { |file| file.to_s.include?('/node_modules/') }.each do |file|
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
      # its modules and charts, and those under a charts/ beside them (the live chat's),
      # and the data a page fetches beside its module (atlas.json, sample.snapshot)
      files = dir.glob('{*,charts/**/*}.{mjs,scxml}') + dir.glob('*.{json,snapshot}').reject { |file| file.basename.to_s.start_with?('package') }
      files.each do |file|
        target = SITE.join('examples', name, file.relative_path_from(dir))
        FileUtils.mkdir_p(target.dirname)
        FileUtils.cp(file, target)
      end
      package = dir.join('node_modules/@tinyactors/wasm')
      next unless package.directory? # a program that runs no actors (atlas, inspector)

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

  # The copywriter (copywriter/, an application of its own): its build's
  # site/ at /copywriter/: the page, the widget a page loads on a device
  # that turned it on (render/site.xsl), the extension's ZIP. Not indexed.
  COPYWRITER = REPO.join('copywriter')
  def copywriter!
    run('mise', 'run', 'build', chdir: COPYWRITER)
    FileUtils.cp_r(COPYWRITER.join('dist/site'), SITE.join('copywriter'))
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

require_relative 'cli'
