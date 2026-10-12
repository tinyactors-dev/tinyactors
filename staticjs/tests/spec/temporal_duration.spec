# Temporal.Duration (docs/temporal.md). Every expected value was generated
# by temporal-polyfill 1.0.5 (the vendored tests/vendor/temporal-polyfill,
# which diff-node runs as Temporal) and is the same in @js-temporal/polyfill
# 0.5.1. A RangeError or a TypeError there is a fault here. A case named
# "deliberate" faults where JavaScript gives a value: a conversion of a
# string, an array or an object, identity, locale data, a calendar other
# than ISO 8601's, or a ZonedDateTime, which staticjs does not have.
# temporal_test sweeps the strings, round's and total's units, modes and
# relativeTo dates, and toString's digits further (diff-node compares its
# lines too).

# --- The constructor

=== new Temporal.Duration
new Temporal.Duration(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
--- value
Temporal.Duration.from("P1Y2M3W4DT5H6M7.00800901S")

=== no arguments is zero, and undefined is 0
[new Temporal.Duration(), new Temporal.Duration(undefined, 1)]
--- value
[Temporal.Duration.from("PT0S"),Temporal.Duration.from("P1M")]

=== negative fields
new Temporal.Duration(-1, -2, -3, -4, -5, -6, -7, -8, -9, -10)
--- value
Temporal.Duration.from("-P1Y2M3W4DT5H6M7.00800901S")

=== booleans and null convert
new Temporal.Duration(true, null, false, 1)
--- value
Temporal.Duration.from("P1Y1D")

=== -0 is 0
[new Temporal.Duration(-0, 0, -0).years === 0, new Temporal.Duration(-0).sign]
--- value
[true,0]

=== an argument past the nanoseconds is not read
new Temporal.Duration(0, 0, 0, 0, 0, 0, 0, 0, 0, 1, "x", {})
--- value
Temporal.Duration.from("PT0.000000001S")

=== fields of both signs
new Temporal.Duration(1, -1)
--- fault different signs

=== a fraction
new Temporal.Duration(0, 0, 0, 1.5)
--- fault not an integer

=== NaN
new Temporal.Duration(NaN)
--- fault not an integer

=== an infinity
new Temporal.Duration(0, 0, 0, 0, 0, 0, 0, 0, 0, -Infinity)
--- fault not an integer

=== a string field, deliberate
new Temporal.Duration("1")
--- fault converts only numbers

=== an array field, deliberate
new Temporal.Duration([])
--- fault converts only numbers

=== an object field
new Temporal.Duration({})
--- fault converts only numbers

=== a BigInt field
new Temporal.Duration(1n)
--- fault BigInt

=== years, months and weeks below 2^32
[new Temporal.Duration(4294967295, 4294967295, 4294967295).toString(), new Temporal.Duration(-4294967295).years]
--- value
["P4294967295Y4294967295M4294967295W",-4294967295]

=== years of 2^32
new Temporal.Duration(4294967296)
--- fault 2^32 or more

=== weeks of -2^32
new Temporal.Duration(0, 0, -4294967296)
--- fault 2^32 or more

=== the days and time just below 2^53 seconds
new Temporal.Duration(0, 0, 0, 104249991374, 7, 36, 31, 999, 999, 999)
--- value
Temporal.Duration.from("P104249991374DT7H36M31.999999999S")

=== the days and time at 2^53 seconds
new Temporal.Duration(0, 0, 0, 104249991374, 7, 36, 32)
--- fault reach 2^53 seconds

=== negative, at 2^53 seconds
new Temporal.Duration(0, 0, 0, 0, 0, 0, -9007199254740991, 0, 0, -1000000000)
--- fault reach 2^53 seconds

=== milliseconds past 2^53 are a double
new Temporal.Duration(0, 0, 0, 0, 0, 0, 0, 9007199254740990000).milliseconds
--- value
9007199254740990000

=== nanoseconds near the limit, as their double
new Temporal.Duration(0, 0, 0, 0, 0, 0, 0, 0, 0, 9007199254740990999999999)
--- value
Temporal.Duration.from("PT9007199254740990.926258176S")

=== nanoseconds whose double reaches the limit
new Temporal.Duration(0, 0, 0, 0, 0, 0, 0, 0, 0, 9007199254740991999999999)
--- fault reach 2^53 seconds

=== hours far past the limit
new Temporal.Duration(0, 0, 0, 0, 1e300)
--- fault reach 2^53 seconds

=== the writer writes Temporal.Duration.from of the ISO string
[new Temporal.Duration(), new Temporal.Duration(0, 0, 0, 0, 0, 0, 0, 1), new Temporal.Duration(1, 0, 0, 0, 0, 1), new Temporal.Duration(0, 0, 0, 0, 0, 0, 0, 0, 0, -1)]
--- value
[Temporal.Duration.from("PT0S"),Temporal.Duration.from("PT0.001S"),Temporal.Duration.from("P1YT1M"),Temporal.Duration.from("-PT0.000000001S")]

# --- from

=== from a string
Temporal.Duration.from("P1Y2M3W4DT5H6M7.008009010S")
--- value
Temporal.Duration.from("P1Y2M3W4DT5H6M7.00800901S")

=== from a negative string, lowercase, with a comma
Temporal.Duration.from("-p1dt2h3m4,5s")
--- value
Temporal.Duration.from("-P1DT2H3M4.5S")

=== a plus sign
Temporal.Duration.from("+PT1M")
--- value
Temporal.Duration.from("PT1M")

=== fractional hours and minutes spread over the smaller fields
[Temporal.Duration.from("PT1.5H"), Temporal.Duration.from("PT0.999999999H"), Temporal.Duration.from("-PT1.000000001M")]
--- value
[Temporal.Duration.from("PT1H30M"),Temporal.Duration.from("PT59M59.9999964S"),Temporal.Duration.from("-PT1M0.00000006S")]

=== P alone
Temporal.Duration.from("P")
--- fault ISO 8601 duration

=== T with nothing after it
Temporal.Duration.from("P1DT")
--- fault ISO 8601 duration

=== a fraction of a date unit
Temporal.Duration.from("P1.5D")
--- fault ISO 8601 duration

=== a fraction before another unit
Temporal.Duration.from("PT1.5H1M")
--- fault ISO 8601 duration

=== ten fraction digits
Temporal.Duration.from("PT1.0000000001S")
--- fault ISO 8601 duration

=== units out of order
Temporal.Duration.from("P1D1Y")
--- fault ISO 8601 duration

=== a minus sign that is not ASCII
Temporal.Duration.from("−P1D")
--- fault ISO 8601 duration

=== white space
Temporal.Duration.from(" P1D")
--- fault ISO 8601 duration

=== a string past the limits
Temporal.Duration.from("PT9007199254740992S")
--- fault reach 2^53 seconds

=== many leading zeros
Temporal.Duration.from("P00000000000000000000001D")
--- value
Temporal.Duration.from("P1D")

=== digits past 2^64
Temporal.Duration.from("P18446744073709551616D")
--- fault reach 2^53 seconds

=== from a bag
Temporal.Duration.from({ days: 1, hours: 2, nanoseconds: -0 })
--- value
Temporal.Duration.from("P1DT2H")

=== a bag's undefined field is not there
Temporal.Duration.from({ days: undefined, minutes: 5 })
--- value
Temporal.Duration.from("PT5M")

=== a bag with none of the fields
Temporal.Duration.from({ day: 1 })
--- fault without years, months

=== a bag's fraction
Temporal.Duration.from({ hours: 1.5 })
--- fault not an integer

=== a bag's fields of both signs
Temporal.Duration.from({ hours: 1, minutes: -1 })
--- fault different signs

=== a bag's string field, deliberate
Temporal.Duration.from({ hours: "1" })
--- fault converts only numbers

=== an array
Temporal.Duration.from([])
--- fault an object of years

=== a number
Temporal.Duration.from(1)
--- fault an object of years

=== a PlainDate
Temporal.Duration.from(Temporal.PlainDate.from("2024-01-31"))
--- fault an object of years

=== a Duration
Temporal.Duration.from(Temporal.Duration.from("PT1H"))
--- value
Temporal.Duration.from("PT1H")

# --- Getters

=== every field
(() => {
  const d = Temporal.Duration.from("-P1Y2M3W4DT5H6M7.008009010S");
  return [d.years, d.months, d.weeks, d.days, d.hours, d.minutes, d.seconds, d.milliseconds, d.microseconds, d.nanoseconds, d.sign, d.blank];
})()
--- value
[-1,-2,-3,-4,-5,-6,-7,-8,-9,-10,-1,false]

=== sign and blank of zero
[new Temporal.Duration().sign, new Temporal.Duration().blank, new Temporal.Duration(0, 0, 0, 0, 0, 0, 0, 0, 0, 1).sign]
--- value
[0,true,1]

=== a plain value's getters are undefined
(() => {
  const d = Temporal.Duration.from("P1D");
  return [d.day, d.year, d.hour, d.calendarId, d.epochNanoseconds];
})()
--- value
[undefined,undefined,undefined,undefined,undefined]

=== a method read as a value, deliberate
Temporal.Duration.from("P1D").negated
--- fault does not read them as values

=== a method a Duration does not have
Temporal.Duration.from("P1D").equals("P1D")
--- fault does not have this method

=== no own properties
[Object.keys({ ...Temporal.Duration.from("P1D") }).length, Object.hasOwn(Temporal.Duration.from("P1D"), "days")]
--- value
[0,false]

=== typeof
typeof Temporal.Duration.from("P1D")
--- value
"object"

# --- with, negated, abs

=== with
Temporal.Duration.from("P1Y2M").with({ months: 5, days: 1 })
--- value
Temporal.Duration.from("P1Y5M1D")

=== with a Duration takes all its fields
Temporal.Duration.from("P1Y2M").with(Temporal.Duration.from("PT1H"))
--- value
Temporal.Duration.from("PT1H")

=== with a sign that mixes
Temporal.Duration.from("P1Y").with({ months: -1 })
--- fault different signs

=== with nothing to change
Temporal.Duration.from("P1Y").with({})
--- fault without years, months

=== with a string
Temporal.Duration.from("P1Y").with("P1D")
--- fault an object of years

=== negated and abs
(() => {
  const d = Temporal.Duration.from("-P1DT2H");
  return [d.negated(), d.abs(), d.negated().negated(), new Temporal.Duration().negated()];
})()
--- value
[Temporal.Duration.from("P1DT2H"),Temporal.Duration.from("P1DT2H"),Temporal.Duration.from("-P1DT2H"),Temporal.Duration.from("PT0S")]

# --- add and subtract

=== add
Temporal.Duration.from("P1DT12H").add("PT13H")
--- value
Temporal.Duration.from("P2DT1H")

=== add balances up to the larger largest unit
[Temporal.Duration.from("PT59M").add({ seconds: 61 }), Temporal.Duration.from("PT1H").add("P1D"), Temporal.Duration.from("PT1000000H").add("PT1S")]
--- value
[Temporal.Duration.from("PT60M1S"),Temporal.Duration.from("P1DT1H"),Temporal.Duration.from("PT1000000H1S")]

=== add of the opposite sign
Temporal.Duration.from("P1D").add("-PT1H")
--- value
Temporal.Duration.from("PT23H")

=== subtract
[Temporal.Duration.from("PT1H").subtract("PT1H"), Temporal.Duration.from("PT1H").subtract("PT2H30M")]
--- value
[Temporal.Duration.from("PT0S"),Temporal.Duration.from("-PT1H30M")]

=== add with months
Temporal.Duration.from("P1M").add("P1D")
--- fault adding Durations with years

=== add with weeks in the argument
Temporal.Duration.from("P1D").subtract("P1W")
--- fault adding Durations with years

=== add past the limit
Temporal.Duration.from("PT9007199254740991S").add("PT1S")
--- fault reach 2^53 seconds

=== add of a bag and of a Duration
[Temporal.Duration.from("PT1S").add({ milliseconds: 500 }), Temporal.Duration.from("PT1S").add(Temporal.Duration.from("PT1S"))]
--- value
[Temporal.Duration.from("PT1.5S"),Temporal.Duration.from("PT2S")]

=== add of nothing
Temporal.Duration.from("PT1S").add()
--- fault an object of years

# --- compare

=== compare
[Temporal.Duration.compare("PT1H", "PT60M"), Temporal.Duration.compare("PT1H", "PT59M"), Temporal.Duration.compare("-PT1H", "PT1S"), Temporal.Duration.compare("P1D", "PT24H")]
--- value
[0,1,-1,0]

=== compare with calendar units relative to a date
[Temporal.Duration.compare("P1M", "P30D", { relativeTo: "2024-01-31" }), Temporal.Duration.compare("P1M", "P29D", { relativeTo: "2024-01-31" }), Temporal.Duration.compare("P1Y", "P365D", { relativeTo: Temporal.PlainDate.from("2024-01-01") }), Temporal.Duration.compare("P1Y", "P365D", { relativeTo: Temporal.PlainDateTime.from("2023-01-01T23:00") })]
--- value
[-1,0,1,0]

=== compare with calendar units without relativeTo
Temporal.Duration.compare("P1M", "P30D")
--- fault without relativeTo

=== equal fields need no relativeTo
Temporal.Duration.compare("P1M", "P1M")
--- value
0

=== compare reads relativeTo when the fields are equal
Temporal.Duration.compare("P1M", "P1M", { relativeTo: "junk" })
--- fault PlainDate from an ISO 8601 date

=== compare's relativeTo with a time zone, deliberate
Temporal.Duration.compare("P1M", "P30D", { relativeTo: "2024-01-31T00:00[UTC]" })
--- fault no Temporal.ZonedDateTime

=== compare's relativeTo past the limits once the months are added
Temporal.Duration.compare("P1M", "P30D", { relativeTo: "+275760-09-01" })
--- fault outside Temporal's limits

=== compare's options not an object
Temporal.Duration.compare("P1D", "P2D", "2024-01-01")
--- fault not an object

# --- round

=== round to a unit
[Temporal.Duration.from("PT1H29M30S").round("hour"), Temporal.Duration.from("PT1H30M").round("hours"), Temporal.Duration.from("-PT1H30M").round("hour")]
--- value
[Temporal.Duration.from("PT1H"),Temporal.Duration.from("PT2H"),Temporal.Duration.from("-PT2H")]

=== round balances up to the largest unit
[Temporal.Duration.from("PT130M").round({ largestUnit: "hour" }), Temporal.Duration.from("PT49H").round({ largestUnit: "day" }), Temporal.Duration.from("P2D").round({ largestUnit: "hour" })]
--- value
[Temporal.Duration.from("PT2H10M"),Temporal.Duration.from("P2DT1H"),Temporal.Duration.from("PT48H")]

=== round with an increment and a mode
[Temporal.Duration.from("PT7M").round({ smallestUnit: "minute", roundingIncrement: 5, roundingMode: "ceil" }), Temporal.Duration.from("-PT7M").round({ smallestUnit: "minute", roundingIncrement: 5, roundingMode: "ceil" }), Temporal.Duration.from("PT7M30S").round({ smallestUnit: "minute", roundingIncrement: 15, roundingMode: "halfEven" })]
--- value
[Temporal.Duration.from("PT10M"),Temporal.Duration.from("-PT5M"),Temporal.Duration.from("PT0S")]

=== round to days without relativeTo, in 24-hour days
[Temporal.Duration.from("PT36H").round({ smallestUnit: "day" }), Temporal.Duration.from("P3DT12H").round({ smallestUnit: "day", roundingIncrement: 2, roundingMode: "floor" })]
--- value
[Temporal.Duration.from("P2D"),Temporal.Duration.from("P2D")]

=== round to months relative to a month's end
[Temporal.Duration.from("P1M15D").round({ smallestUnit: "month", relativeTo: "2024-01-31" }), Temporal.Duration.from("P1M15D").round({ smallestUnit: "month", relativeTo: "2024-02-29" }), Temporal.Duration.from("P1M14D").round({ smallestUnit: "month", relativeTo: "2024-02-29" })]
--- value
[Temporal.Duration.from("P1M"),Temporal.Duration.from("P1M"),Temporal.Duration.from("P1M")]

=== round balances days into months relative to a date
[Temporal.Duration.from("P45D").round({ largestUnit: "month", relativeTo: "2024-01-31" }), Temporal.Duration.from("-P45D").round({ largestUnit: "month", relativeTo: "2024-03-31" }), Temporal.Duration.from("P400D").round({ largestUnit: "year", relativeTo: "2023-02-28" })]
--- value
[Temporal.Duration.from("P1M16D"),Temporal.Duration.from("-P1M14D"),Temporal.Duration.from("P1Y1M6D")]

=== round to weeks
[Temporal.Duration.from("P20D").round({ smallestUnit: "week", relativeTo: "2024-01-01" }), Temporal.Duration.from("P1M").round({ largestUnit: "week", relativeTo: "2024-02-01" })]
--- value
[Temporal.Duration.from("P3W"),Temporal.Duration.from("P4W1D")]

=== round a carry up into the year
Temporal.Duration.from("P11M30DT23H").round({ smallestUnit: "day", largestUnit: "year", relativeTo: "2023-01-01", roundingMode: "ceil" })
--- value
Temporal.Duration.from("P1Y")

=== round to years by an increment, as the largest unit
Temporal.Duration.from("P7Y").round({ smallestUnit: "year", roundingIncrement: 5, relativeTo: "2024-01-01" })
--- value
Temporal.Duration.from("P5Y")

=== round relative to a bag and a PlainDateTime
[Temporal.Duration.from("P1M").round({ largestUnit: "day", relativeTo: { year: 2024, month: 2, day: 10 } }), Temporal.Duration.from("P1M").round({ largestUnit: "day", relativeTo: Temporal.PlainDateTime.from("2024-03-10T23:00") })]
--- value
[Temporal.Duration.from("P29D"),Temporal.Duration.from("P31D")]

=== round with months and no relativeTo
Temporal.Duration.from("P1M").round({ largestUnit: "day" })
--- fault without relativeTo

=== round to months with no relativeTo
Temporal.Duration.from("P40D").round({ largestUnit: "month" })
--- fault without relativeTo

=== round without options
Temporal.Duration.from("P1D").round()
--- fault round without options

=== round without either unit
Temporal.Duration.from("P1D").round({ roundingMode: "ceil" })
--- fault without a smallestUnit or a largestUnit

=== round with the largest unit below the smallest
Temporal.Duration.from("P1D").round({ largestUnit: "hour", smallestUnit: "day" })
--- fault largestUnit smaller

=== round to days by an increment with a larger largest unit
Temporal.Duration.from("P1D").round({ smallestUnit: "day", largestUnit: "month", roundingIncrement: 2, relativeTo: "2024-01-01" })
--- fault roundingIncrement above 1

=== round by an increment that does not divide the hour
Temporal.Duration.from("PT1H").round({ smallestUnit: "minute", roundingIncrement: 7 })
--- fault does not divide

=== round to an unknown unit
Temporal.Duration.from("PT1H").round("fortnight")
--- fault this value of a Temporal option

=== round with "auto" as the smallest unit, deliberate: the polyfills differ
Temporal.Duration.from("PT1H").round({ smallestUnit: "auto" })
--- fault this value of a Temporal option

=== round with "auto" as the largest unit
Temporal.Duration.from("PT90M").round({ largestUnit: "auto", smallestUnit: "minute" })
--- value
Temporal.Duration.from("PT90M")

=== round past the limit
Temporal.Duration.from("PT9007199254740991S").round({ smallestUnit: "minute", roundingMode: "ceil" })
--- fault reach 2^53 seconds

=== round relative to a date past the limits
Temporal.Duration.from("P1Y").round({ smallestUnit: "day", relativeTo: "+275760-01-01" })
--- fault outside Temporal's limits

=== round relative to a date with a time zone, deliberate
Temporal.Duration.from("P1D").round({ smallestUnit: "hour", relativeTo: { year: 2024, month: 1, day: 1, timeZone: "UTC" } })
--- fault no Temporal.ZonedDateTime

=== round relative to a date with another calendar, deliberate
Temporal.Duration.from("P1D").round({ smallestUnit: "hour", relativeTo: "2024-01-01[u-ca=gregory]" })
--- fault PlainDate from an ISO 8601 date

=== round relative to a string with Z
Temporal.Duration.from("P1D").round({ smallestUnit: "hour", relativeTo: "2024-01-01T00:00Z" })
--- fault PlainDate from an ISO 8601 date

=== round relative to a PlainTime
Temporal.Duration.from("P1D").round({ smallestUnit: "hour", relativeTo: Temporal.PlainTime.from("12:00") })
--- fault a PlainDate is made from

# --- total

=== total of time units
[Temporal.Duration.from("PT1H30M").total("hour"), Temporal.Duration.from("PT1H30M").total({ unit: "minutes" }), Temporal.Duration.from("P1DT1H").total("hours"), Temporal.Duration.from("PT1S").total("day"), Temporal.Duration.from("-PT1.5S").total("millisecond")]
--- value
[1.5,90,25,0.000011574074074074073,-1500]

=== total relative to a date
[Temporal.Duration.from("P1Y2M").total({ unit: "day", relativeTo: "2024-01-31" }), Temporal.Duration.from("P1M").total({ unit: "day", relativeTo: "2024-02-29" }), Temporal.Duration.from("P45D").total({ unit: "month", relativeTo: "2024-01-31" }), Temporal.Duration.from("-P1M15D").total({ unit: "month", relativeTo: "2024-03-31" })]
--- value
[425,29,1.5161290322580645,-1.5172413793103448]

=== total in weeks and years
[Temporal.Duration.from("P1M").total({ unit: "week", relativeTo: "2024-02-01" }), Temporal.Duration.from("P18M").total({ unit: "year", relativeTo: "2024-01-01" })]
--- value
[4.142857142857143,1.4958904109589042]

=== total of zero relative to a date
Temporal.Duration.from("PT0S").total({ unit: "month", relativeTo: "2024-01-31" })
--- value
0

=== total of months without relativeTo
Temporal.Duration.from("P1M").total("day")
--- fault without relativeTo

=== total in months without relativeTo
Temporal.Duration.from("P40D").total("month")
--- fault without relativeTo

=== total without options
Temporal.Duration.from("P1D").total()
--- fault total without options

=== total without a unit
Temporal.Duration.from("P1D").total({ relativeTo: "2024-01-01" })
--- fault total without a unit

=== total relative to a date with a time zone, deliberate
Temporal.Duration.from("P1M").total({ unit: "day", relativeTo: "2024-01-31[Europe/Paris]" })
--- fault no Temporal.ZonedDateTime

# --- toString and toJSON

=== toString
[Temporal.Duration.from("P1Y2M3W4DT5H6M7.008009010S").toString(), new Temporal.Duration().toString(), Temporal.Duration.from("-PT0.5S").toString(), Temporal.Duration.from("P1D").toString(), Temporal.Duration.from("PT1H").toString()]
--- value
["P1Y2M3W4DT5H6M7.00800901S","PT0S","-PT0.5S","P1D","PT1H"]

=== toString's fractionalSecondDigits
[Temporal.Duration.from("PT1.987654321S").toString({ fractionalSecondDigits: 0 }), Temporal.Duration.from("PT1.987654321S").toString({ fractionalSecondDigits: 3 }), Temporal.Duration.from("P1D").toString({ fractionalSecondDigits: 2 }), Temporal.Duration.from("PT1S").toString({ fractionalSecondDigits: "auto" })]
--- value
["PT1S","PT1.987S","P1DT0.00S","PT1S"]

=== toString rounds and carries into the larger units
[Temporal.Duration.from("PT59M59.9S").toString({ fractionalSecondDigits: 0, roundingMode: "halfExpand" }), Temporal.Duration.from("-PT1.5S").toString({ smallestUnit: "second", roundingMode: "floor" }), Temporal.Duration.from("PT1.999999999S").toString({ smallestUnit: "millisecond", roundingMode: "ceil" })]
--- value
["PT60M0S","-PT2S","PT2.000S"]

=== toString with the smallest unit an hour
Temporal.Duration.from("PT1H").toString({ smallestUnit: "hour" })
--- fault does not round to

=== toString with the smallest unit a minute
Temporal.Duration.from("PT1H").toString({ smallestUnit: "minute" })
--- fault does not round to

=== toString past the limit after rounding
Temporal.Duration.from("PT9007199254740991.5S").toString({ smallestUnit: "second", roundingMode: "ceil" })
--- fault reach 2^53 seconds

=== toJSON and JSON.stringify
[Temporal.Duration.from("PT1M").toJSON(), JSON.stringify({ d: Temporal.Duration.from("-P2W") })]
--- value
["PT1M","{\"d\":\"-P2W\"}"]

=== toLocaleString needs locale data, deliberate
Temporal.Duration.from("P1D").toLocaleString()
--- fault locale data

=== valueOf
Temporal.Duration.from("P1D").valueOf()
--- fault valueOf

=== === on two Durations compares identity, deliberate
Temporal.Duration.from("P1D") === Temporal.Duration.from("P1D")
--- fault compares identity

=== a Duration in a template, deliberate
`${Temporal.Duration.from("P1D")}`
--- fault

=== Temporal.Duration alone
Temporal.Duration
--- error unsupported

# --- Reviewer's cases: rounding without relativeTo on ties, and the limits
# (values from temporal-polyfill 1.0.5, the same in @js-temporal/polyfill
# 0.5.1 and the proposal's reference polyfill)

=== halfEven of a negative half day to days
Temporal.Duration.from("-PT12H").round({ smallestUnit: "day", roundingMode: "halfEven" })
--- value
Temporal.Duration.from("PT0S")

=== halfCeil of a negative half day to days
Temporal.Duration.from("-PT12H").round({ smallestUnit: "day", roundingMode: "halfCeil" })
--- value
Temporal.Duration.from("PT0S")

=== halfTrunc of a negative hour and a half
Temporal.Duration.from("-PT90M").round({ smallestUnit: "hour", roundingMode: "halfTrunc" })
--- value
Temporal.Duration.from("-PT1H")

=== halfEven of a negative two and a half seconds in toString
Temporal.Duration.from("-PT2.5S").toString({ fractionalSecondDigits: 0, roundingMode: "halfEven" })
--- value
"-PT2S"

=== ceil in toString carries into days
Temporal.Duration.from("P1DT23H59M59.9S").toString({ fractionalSecondDigits: 0, roundingMode: "ceil" })
--- value
"P2DT0S"

=== the greatest seconds with every fraction, total in seconds
Temporal.Duration.from("PT9007199254740991.999999999S").total("second")
--- value
9007199254740992

=== the greatest seconds rounded up past the limit
Temporal.Duration.from("PT9007199254740991.999999999S").round({ smallestUnit: "second", roundingMode: "ceil" })
--- fault

=== the greatest seconds balanced to days
Temporal.Duration.from("PT9007199254740991.999999999S").round({ largestUnit: "day" })
--- value
Temporal.Duration.from("P104249991374DT7H36M31.999999999S")

=== the greatest days and time
new Temporal.Duration(0, 0, 0, 104249991374, 7, 36, 31, 999, 999, 999)
--- value
Temporal.Duration.from("P104249991374DT7H36M31.999999999S")

=== a second more than the greatest days and time
new Temporal.Duration(0, 0, 0, 104249991374, 7, 36, 32)
--- fault

=== a day by an increment of 10^9 relative to a date
Temporal.Duration.from("P3D").round({ smallestUnit: "day", roundingIncrement: 1e9, relativeTo: "2024-01-01" })
--- value
Temporal.Duration.from("PT0S")

=== a year relative to the last year a date has
Temporal.Duration.from("P1Y").round({ largestUnit: "day", relativeTo: "+275760-01-01" })
--- fault

=== three weeks in months from February
Temporal.Duration.from("P3W").total({ unit: "month", relativeTo: "2024-02-01" })
--- value
0.7241379310344828

# The specification moves the rounding window on here (ComputeNudgeWindow's
# additionalShift: P1M); @js-temporal/polyfill gives PT0S. staticjs faults.
=== round to months from a month's end into a shorter month, deliberate
Temporal.Duration.from("P1MT12H").round({ smallestUnit: "month", roundingMode: "trunc", relativeTo: "2024-01-31" })
--- fault the specification moves the window on

=== total in months from a month's end into a shorter month, deliberate
Temporal.Duration.from("P1MT12H").total({ unit: "month", relativeTo: "2024-01-31" })
--- fault the specification moves the window on
