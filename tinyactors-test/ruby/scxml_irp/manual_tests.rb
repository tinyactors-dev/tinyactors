# frozen_string_literal: true

module SCXML
  module IRP
    # The W3C manifest marks a test `manual` when its pass criterion is a
    # person reading the log output. With a log sink (ADR 0045) most of them
    # have an objective criterion after all, stated here: `body` replaces the
    # generated scenario body, `outcome: true` keeps the ordinary "reached
    # pass" check, and `skip` says why a test stays manual.
    #
    # Chart names are the ones the importer derives from the test ID.
    module ManualTests
      TESTS = {
        # "The tester must look at the log output and verify that both keys
        # are there." Two things stop that from being objective here: SCXML
        # leaves the format of _event.raw to the processor, and this data
        # model keeps one value per parameter name, which ADR 0034 leaves to
        # the plugin. There is nothing to assert that the standard requires.
        '178' => {
          skip: 'the standard leaves _event.raw unspecified, and this data model keeps ' \
                'one value per <param> name (ADR 0034)'
        },

        # "Both the parent and child process print out the contents of the
        # event. The tester must check if they are the same." Each logs the
        # same seven fields, so the check is that the two sets are equal.
        '230' => {
          why: 'the two logged field sets must be equal',
          body: <<~RUBY
            log_sink = recording_log
            machine = spawn :irp_test230
            run_to_completion machine
            sets = log_sink.entries('log').group_by { |entry| entry[:source] }.values
            assert_equal 2, sets.size, 'the parent and the child each logged the event'
            original, forwarded = sets.map { |set| set.map { |entry| [entry[:label], entry[:value]] } }
            assert_equal original, forwarded, 'an autoforwarded event carries the original fields'
          RUBY
        },

        # "Tester must examine log output from child process to determine
        # success": the cancelled child's onexit handlers must run, and it
        # must not reach the final state that says it was not cancelled.
        '250' => {
          why: 'the cancelled child logs its onexit handlers and not its final state',
          body: <<~RUBY
            log_sink = recording_log
            machine = spawn :irp_test250
            run_to_completion machine
            logged = log_sink.logged.map(&:last)
            assert_equal ['Exiting sub01', 'Exiting sub0'], logged,
                         'the cancelled child ran its onexit handlers, innermost first'
          RUBY
        },

        # Its criterion is that the processor rejects the document, which is
        # not a log question; the importer cannot convert it anyway.
        '301' => {
          skip: 'its criterion is that the processor rejects the document, and conf:scriptBadSrc ' \
                'needs <script src>, which the importer does not convert'
        },

        # Its criterion is that two log outputs match, but the document logs
        # an empty expression, which the importer does not convert.
        '307' => {
          skip: 'its criterion is that two log outputs match, and conf:expr="" is an empty ' \
                'expression, which the importer does not convert'
        },

        # "The processor is allowed to reject this doc, but if it executes it
        # with its illegal expression, it must raise an error." This processor
        # executes it, so the document's own pass state decides.
        '313' => { why: 'this processor executes the document, so its pass state decides', outcome: true },

        # Same shape: if executed, the error must come from s03 and not
        # earlier, which the document's own fail transition checks.
        '314' => { why: 'this processor executes the document, so its pass state decides', outcome: true },

        # "There is no platform-independent way to test that event1 is not
        # processed." Inspection is one: the machine is done, and the event
        # raised in the final state's onentry left nothing queued behind it.
        '415' => {
          why: 'inspection shows the machine halted with nothing left to process',
          body: <<~RUBY
            machine = spawn :irp_test415
            run_to_completion machine
            assert machine.done?, 'the machine halts when it enters a top-level final state'
            assert_equal [], machine.observe[:internal_events], 'nothing is left queued'
          RUBY
        },

        # A fully manual test: 513/test513.txt is prose, not a document. It
        # also needs the optional BasicHTTP Event I/O Processor.
        '513' => { skip: 'prose, not a document: it asks a person to send an event to a URL (SCXML C.2)' }
      }.freeze

      module_function

      def entry(id) = TESTS[id.to_s]

      # Why a manual test stays skipped, or nil when it can be automated.
      def skip_reason(id)
        found = entry(id)
        return 'manual test: a person judges its log output' if found.nil?

        found[:skip]
      end

      # The scenario body that replaces the generated one, or nil.
      def body(id) = entry(id)&.[](:body)
    end
  end
end
