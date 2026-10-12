# Temporal.PlainTime (docs/temporal.md). Every expected value was generated
# by temporal-polyfill 1.0.5 (the vendored tests/vendor/temporal-polyfill,
# which diff-node runs as Temporal) and is the same in @js-temporal/polyfill
# 0.5.1. A RangeError or a TypeError there is a fault here. A case named
# "deliberate" faults where JavaScript gives a value: a conversion of a
# string, an array or an object, identity, locale data, a calendar other
# than ISO 8601's, or what staticjs does not build yet. temporal_test sweeps
# the strings, round's units, increments and modes, toString's digits and
# the overflow options further (diff-node compares its lines too).

# --- The constructor

=== new Temporal.PlainTime
new Temporal.PlainTime(13, 5, 9, 1, 2, 3)
--- value
Temporal.PlainTime.from("13:05:09.001002003")

=== no arguments is midnight, and undefined is 0
[new Temporal.PlainTime(), new Temporal.PlainTime(1, undefined, 3)]
--- value
[Temporal.PlainTime.from("00:00:00"),Temporal.PlainTime.from("01:00:03")]

=== the writer writes Temporal.PlainTime.from of the ISO string, with as many digits as it needs
[new Temporal.PlainTime(0, 0), new Temporal.PlainTime(23, 59, 59, 999, 999, 999), new Temporal.PlainTime(1, 2, 3, 400), new Temporal.PlainTime(1, 2, 3, 0, 50), new Temporal.PlainTime(1, 2, 3, 0, 0, 7)]
--- value
[Temporal.PlainTime.from("00:00:00"),Temporal.PlainTime.from("23:59:59.999999999"),Temporal.PlainTime.from("01:02:03.4"),Temporal.PlainTime.from("01:02:03.00005"),Temporal.PlainTime.from("01:02:03.000000007")]

=== the fields are cut toward zero, and booleans and null convert
new Temporal.PlainTime(1.9, 2.9, true, null, -0.5, false)
--- value
Temporal.PlainTime.from("01:02:01")

=== an argument past the nanosecond is not read
new Temporal.PlainTime(1, 2, 3, 4, 5, 6, "iso8601", {})
--- value
Temporal.PlainTime.from("01:02:03.004005006")

=== hour 24
new Temporal.PlainTime(24)
--- fault outside its range

=== a negative minute
new Temporal.PlainTime(1, -1)
--- fault outside its range

=== second 60
new Temporal.PlainTime(1, 2, 60)
--- fault outside its range

=== millisecond 1000
new Temporal.PlainTime(1, 2, 3, 1000)
--- fault outside its range

=== a NaN field
new Temporal.PlainTime(NaN)
--- fault NaN, infinite or undefined

=== an infinite nanosecond
new Temporal.PlainTime(0, 0, 0, 0, 0, Infinity)
--- fault NaN, infinite or undefined

=== a string field, deliberate
new Temporal.PlainTime("12")
--- fault converts only numbers

=== a BigInt field
new Temporal.PlainTime(12n)
--- fault

=== a constructor called without new
Temporal.PlainTime(12, 30)
--- error unsupported used only as Math.name

# --- from

=== from an ISO time
[Temporal.PlainTime.from("12:30"), Temporal.PlainTime.from("12"), Temporal.PlainTime.from("12:30:45.5"), Temporal.PlainTime.from("123045,123456789")]
--- value
[Temporal.PlainTime.from("12:30:00"),Temporal.PlainTime.from("12:00:00"),Temporal.PlainTime.from("12:30:45.5"),Temporal.PlainTime.from("12:30:45.123456789")]

=== from a time with T or t before it
[Temporal.PlainTime.from("T12:30"), Temporal.PlainTime.from("t1230"), Temporal.PlainTime.from("T12")]
--- value
[Temporal.PlainTime.from("12:30:00"),Temporal.PlainTime.from("12:30:00"),Temporal.PlainTime.from("12:00:00")]

=== from a date and time, whose date and offset are left out
[Temporal.PlainTime.from("2024-01-31T12:30"), Temporal.PlainTime.from("2024-01-31 12:30:01+01:00[Europe/Paris]"), Temporal.PlainTime.from("20240131t1230-0530[u-ca=iso8601]")]
--- value
[Temporal.PlainTime.from("12:30:00"),Temporal.PlainTime.from("12:30:01"),Temporal.PlainTime.from("12:30:00")]

=== a time with an offset and annotations
[Temporal.PlainTime.from("12:30+01:00"), Temporal.PlainTime.from("12:30-05[UTC]"), Temporal.PlainTime.from("12:30[foo=bar]")]
--- value
[Temporal.PlainTime.from("12:30:00"),Temporal.PlainTime.from("12:30:00"),Temporal.PlainTime.from("12:30:00")]

=== a leap second is the second before it
[Temporal.PlainTime.from("23:59:60"), Temporal.PlainTime.from("12:30:60.5")]
--- value
[Temporal.PlainTime.from("23:59:59"),Temporal.PlainTime.from("12:30:59.5")]

=== a date and time outside Temporal's limits still has its time
[Temporal.PlainTime.from("-271821-04-19T00:00"), Temporal.PlainTime.from("+275760-09-14T12:00")]
--- value
[Temporal.PlainTime.from("00:00:00"),Temporal.PlainTime.from("12:00:00")]

=== a time that reads as a year and month needs its T
Temporal.PlainTime.from("2021-12")
--- fault not one

=== a time that reads as a month and day needs its T
Temporal.PlainTime.from("1214")
--- fault not one

=== a time that reads as a month and day with an annotation
Temporal.PlainTime.from("12-14[UTC]")
--- fault not one

=== the same times with T
[Temporal.PlainTime.from("T2021-12"), Temporal.PlainTime.from("T1214"), Temporal.PlainTime.from("T12-14")]
--- value
[Temporal.PlainTime.from("20:21:00"),Temporal.PlainTime.from("12:14:00"),Temporal.PlainTime.from("12:00:00")]

=== a time that is no date: February 31st, month 13, day 0
[Temporal.PlainTime.from("0231"), Temporal.PlainTime.from("1300"), Temporal.PlainTime.from("12-00"), Temporal.PlainTime.from("2021-13")]
--- value
[Temporal.PlainTime.from("02:31:00"),Temporal.PlainTime.from("13:00:00"),Temporal.PlainTime.from("12:00:00"),Temporal.PlainTime.from("20:21:00")]

=== February 29th is a month and day
Temporal.PlainTime.from("0229")
--- fault not one

=== hour 24 in a string
Temporal.PlainTime.from("24:00")
--- fault not one

=== a date without a time
Temporal.PlainTime.from("2024-01-31")
--- fault not one

=== a time with Z
Temporal.PlainTime.from("12:30Z")
--- fault not one

=== a date and time with Z
Temporal.PlainTime.from("2024-01-31T12:30Z")
--- fault not one

=== a fraction of ten digits
Temporal.PlainTime.from("12:30:00.1234567890")
--- fault not one

=== a fraction of a minute
Temporal.PlainTime.from("12:30.5")
--- fault not one

=== white space around a time
Temporal.PlainTime.from(" 12:30")
--- fault not one

=== a space before a time alone
Temporal.PlainTime.from("12:30 ")
--- fault not one

=== a critical annotation of an unknown key
Temporal.PlainTime.from("12:30[!foo=bar]")
--- fault not one

=== a time with another calendar, deliberate
Temporal.PlainTime.from("12:30[u-ca=gregory]")
--- fault not one

=== from a property bag
[Temporal.PlainTime.from({ hour: 13 }), Temporal.PlainTime.from({ minute: 5, nanosecond: 1 }), Temporal.PlainTime.from({ hour: 1, minute: 2, second: 3, millisecond: 4, microsecond: 5, nanosecond: 6 })]
--- value
[Temporal.PlainTime.from("13:00:00"),Temporal.PlainTime.from("00:05:00.000000001"),Temporal.PlainTime.from("01:02:03.004005006")]

=== a bag's fields are constrained by default
[Temporal.PlainTime.from({ hour: 25, minute: -1 }), Temporal.PlainTime.from({ second: 60 }), Temporal.PlainTime.from({ millisecond: 1000, microsecond: -5, nanosecond: 2000 }), Temporal.PlainTime.from({ hour: 1e10 }, { overflow: "constrain" })]
--- value
[Temporal.PlainTime.from("23:00:00"),Temporal.PlainTime.from("00:00:59"),Temporal.PlainTime.from("00:00:00.999000999"),Temporal.PlainTime.from("23:00:00")]

=== a bag's hour past 23 with overflow reject
Temporal.PlainTime.from({ hour: 24 }, { overflow: "reject" })
--- fault outside its range

=== a bag's negative minute with overflow reject
Temporal.PlainTime.from({ minute: -1 }, { overflow: "reject" })
--- fault outside its range

=== a bag's second 60 with overflow reject
Temporal.PlainTime.from({ second: 60 }, { overflow: "reject" })
--- fault outside its range

=== a bag's fields are cut toward zero, and booleans and null convert
[Temporal.PlainTime.from({ hour: 1.5, minute: null, second: true }), Temporal.PlainTime.from({ hour: -0.5 }, { overflow: "reject" })]
--- value
[Temporal.PlainTime.from("01:00:01"),Temporal.PlainTime.from("00:00:00")]

=== a bag without a time field
Temporal.PlainTime.from({ hours: 1, day: 1 })
--- fault without an hour, minute

=== an empty bag
Temporal.PlainTime.from({})
--- fault without an hour, minute

=== a bag's undefined field is missing
Temporal.PlainTime.from({ hour: undefined, minute: 5 })
--- value
Temporal.PlainTime.from("00:05:00")

=== a bag's NaN field
Temporal.PlainTime.from({ hour: NaN })
--- fault NaN, infinite or undefined

=== a bag's calendar and time zone are not read
Temporal.PlainTime.from({ minute: 5, calendar: "gregory", timeZone: "UTC" })
--- value
Temporal.PlainTime.from("00:05:00")

=== a string field of a bag, deliberate
Temporal.PlainTime.from({ hour: "1" })
--- fault converts only numbers

=== from a PlainTime
(t => [Temporal.PlainTime.from(t), Temporal.PlainTime.from(t, { overflow: "reject" })])(new Temporal.PlainTime(12, 30))
--- value
[Temporal.PlainTime.from("12:30:00"),Temporal.PlainTime.from("12:30:00")]

=== from a PlainDateTime, its time
Temporal.PlainTime.from(new Temporal.PlainDateTime(2024, 1, 31, 1, 2, 3, 4, 5, 6))
--- value
Temporal.PlainTime.from("01:02:03.004005006")

=== from a PlainDate, which has no time fields
Temporal.PlainTime.from(new Temporal.PlainDate(2024, 1, 31))
--- fault

=== from a PlainTime checks the options
Temporal.PlainTime.from(new Temporal.PlainTime(12, 30), { overflow: "x" })
--- fault value of a Temporal option

=== from a string checks the options
Temporal.PlainTime.from("12:30", { overflow: "x" })
--- fault value of a Temporal option

=== options null
Temporal.PlainTime.from("12:30", null)
--- fault not an object

=== from a number
Temporal.PlainTime.from(12)
--- fault is made from

=== from null
Temporal.PlainTime.from(null)
--- fault is made from

=== from an array
Temporal.PlainTime.from([12, 30])
--- fault is made from

=== typeof the functions
[typeof Temporal.PlainTime.from, typeof Temporal.PlainTime.compare]
--- value
["function","function"]

# --- compare and equals

=== compare
[Temporal.PlainTime.compare("12:30", "12:31"), Temporal.PlainTime.compare("12:30", "12:30:00"), Temporal.PlainTime.compare("12:30:00.000000001", "12:30")]
--- value
[-1,0,1]

=== compare takes bags and date-times
[Temporal.PlainTime.compare({ hour: 1 }, "01:00"), Temporal.PlainTime.compare(new Temporal.PlainDateTime(2020, 1, 1, 1), new Temporal.PlainTime(1))]
--- value
[0,0]

=== compare of a date
Temporal.PlainTime.compare("12:30", "2024-01-31")
--- fault not one

=== equals
(t => [t.equals("12:30"), t.equals({ hour: 12, minute: 30 }), t.equals(new Temporal.PlainTime(12, 30, 0, 0, 0, 1)), t.equals(new Temporal.PlainDateTime(2000, 1, 1, 12, 30))])(new Temporal.PlainTime(12, 30))
--- value
[true,true,false,true]

=== equals of a number
new Temporal.PlainTime(12, 30).equals(5)
--- fault is made from

=== === on two times compares identity, deliberate
new Temporal.PlainTime(12, 30) === new Temporal.PlainTime(12, 30)
--- fault

=== < on two times calls valueOf
new Temporal.PlainTime(12, 30) < new Temporal.PlainTime(12, 31)
--- fault

# --- Getters

=== every getter
(t => [t.hour, t.minute, t.second, t.millisecond, t.microsecond, t.nanosecond])(new Temporal.PlainTime(13, 5, 9, 1, 2, 3))
--- value
[13,5,9,1,2,3]

=== a date's getters are undefined
(t => [t.year, t.day, t.calendarId, t.hours, t.dayOfWeek])(new Temporal.PlainTime(13, 5))
--- value
[undefined,undefined,undefined,undefined,undefined]

=== destructuring reads the getters
(({ hour, minute, nanosecond }) => [hour, minute, nanosecond])(Temporal.PlainTime.from("01:02:03.000000004"))
--- value
[1,2,4]

=== a method read as a value, deliberate
new Temporal.PlainTime(12).round
--- fault does not read them as values

=== a getter called
new Temporal.PlainTime(12).hour()
--- fault does not have this method

=== a method of another Temporal type
new Temporal.PlainTime(12).toPlainDateTime(new Temporal.PlainDate(2024, 1, 31))
--- fault does not have this method

# --- with

=== with replaces fields, and constrains by default
(t => [t.with({ hour: 1 }), t.with({ minute: 70 }), t.with({ second: 5, nanosecond: 9 }), t.with({ hour: -1 })])(new Temporal.PlainTime(12, 30, 15))
--- value
[Temporal.PlainTime.from("01:30:15"),Temporal.PlainTime.from("12:59:15"),Temporal.PlainTime.from("12:30:05.000000009"),Temporal.PlainTime.from("00:30:15")]

=== with overflow reject
new Temporal.PlainTime(12, 30).with({ hour: 24 }, { overflow: "reject" })
--- fault outside its range

=== with overflow reject of fields within range
new Temporal.PlainTime(12, 30).with({ hour: 23 }, { overflow: "reject" })
--- value
Temporal.PlainTime.from("23:30:00")

=== with no fields
new Temporal.PlainTime(12, 30).with({})
--- fault without an hour, minute

=== with only a date's fields
new Temporal.PlainTime(12, 30).with({ year: 2024 })
--- fault without an hour, minute

=== with a calendar
new Temporal.PlainTime(12, 30).with({ hour: 1, calendar: "iso8601" })
--- fault calendar or a timeZone

=== with a time zone
new Temporal.PlainTime(12, 30).with({ hour: 1, timeZone: "UTC" })
--- fault calendar or a timeZone

=== with an undefined calendar and time zone
new Temporal.PlainTime(12, 30).with({ hour: 1, calendar: undefined, timeZone: undefined })
--- value
Temporal.PlainTime.from("01:30:00")

=== with a string
new Temporal.PlainTime(12, 30).with("13:00")
--- fault object of the fields

=== with a PlainTime
new Temporal.PlainTime(12, 30).with(new Temporal.PlainTime(13))
--- fault object of the fields

# --- round

=== round to a unit, rounding half away from zero by default
(t => [t.round("hour"), t.round("minute"), t.round("second"), t.round("millisecond"), t.round("microsecond"), t.round("nanosecond")])(new Temporal.PlainTime(12, 29, 30, 500, 500, 500))
--- value
[Temporal.PlainTime.from("12:00:00"),Temporal.PlainTime.from("12:30:00"),Temporal.PlainTime.from("12:29:31"),Temporal.PlainTime.from("12:29:30.501"),Temporal.PlainTime.from("12:29:30.500501"),Temporal.PlainTime.from("12:29:30.5005005")]

=== round's units in the plural
(t => [t.round("hours"), t.round({ smallestUnit: "minutes" })])(new Temporal.PlainTime(12, 29, 30))
--- value
[Temporal.PlainTime.from("12:00:00"),Temporal.PlainTime.from("12:30:00")]

=== round past midnight wraps to midnight
[new Temporal.PlainTime(23, 30).round("hour"), new Temporal.PlainTime(23, 59, 59, 999).round({ smallestUnit: "second", roundingMode: "ceil" })]
--- value
[Temporal.PlainTime.from("00:00:00"),Temporal.PlainTime.from("00:00:00")]

=== round with an increment and a mode
(t => [t.round({ smallestUnit: "minute", roundingIncrement: 15 }), t.round({ smallestUnit: "hour", roundingIncrement: 6, roundingMode: "floor" }), t.round({ smallestUnit: "minute", roundingIncrement: 30, roundingMode: "halfEven" })])(new Temporal.PlainTime(12, 45))
--- value
[Temporal.PlainTime.from("12:45:00"),Temporal.PlainTime.from("12:00:00"),Temporal.PlainTime.from("13:00:00")]

=== every rounding mode on an exact half
(t => ["ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor", "halfExpand", "halfTrunc", "halfEven"].map(roundingMode => t.round({ smallestUnit: "hour", roundingMode })))(new Temporal.PlainTime(12, 30))
--- value
[Temporal.PlainTime.from("13:00:00"),Temporal.PlainTime.from("12:00:00"),Temporal.PlainTime.from("13:00:00"),Temporal.PlainTime.from("12:00:00"),Temporal.PlainTime.from("13:00:00"),Temporal.PlainTime.from("12:00:00"),Temporal.PlainTime.from("13:00:00"),Temporal.PlainTime.from("12:00:00"),Temporal.PlainTime.from("12:00:00")]

=== round's increment is cut toward zero; true is 1
(t => [t.round({ smallestUnit: "hour", roundingIncrement: 12.9 }), t.round({ smallestUnit: "hour", roundingIncrement: true })])(new Temporal.PlainTime(12, 30))
--- value
[Temporal.PlainTime.from("12:00:00"),Temporal.PlainTime.from("13:00:00")]

=== round's increment must divide the next unit
new Temporal.PlainTime(12, 30).round({ smallestUnit: "hour", roundingIncrement: 5 })
--- fault does not divide

=== round's increment must be less than the next unit
new Temporal.PlainTime(12, 30).round({ smallestUnit: "minute", roundingIncrement: 60 })
--- fault does not divide

=== round's increment 0
new Temporal.PlainTime(12, 30).round({ smallestUnit: "minute", roundingIncrement: 0 })
--- fault below 1

=== round's increment null is 0
new Temporal.PlainTime(12, 30).round({ smallestUnit: "minute", roundingIncrement: null })
--- fault below 1

=== round's increment past 10^9
new Temporal.PlainTime(12, 30).round({ smallestUnit: "nanosecond", roundingIncrement: 1e10 })
--- fault above 10^9

=== round's increment NaN
new Temporal.PlainTime(12, 30).round({ smallestUnit: "minute", roundingIncrement: NaN })
--- fault NaN

=== round's increment as a string, deliberate
new Temporal.PlainTime(12, 30).round({ smallestUnit: "hour", roundingIncrement: "2" })
--- fault converts only numbers

=== round to a day
new Temporal.PlainTime(12, 30).round("day")
--- fault does not round to

=== round to a unit that is not one
new Temporal.PlainTime(12, 30).round("Hour")
--- fault value of a Temporal option

=== round without options
new Temporal.PlainTime(12, 30).round()
--- fault without a smallestUnit

=== round without a smallestUnit
new Temporal.PlainTime(12, 30).round({ roundingMode: "floor" })
--- fault without a smallestUnit

=== round's smallestUnit auto
new Temporal.PlainTime(12, 30).round({ smallestUnit: "auto" })
--- fault value of a Temporal option

=== round of an array, which has no options
new Temporal.PlainTime(12, 30).round([])
--- fault without a smallestUnit

=== round of null
new Temporal.PlainTime(12, 30).round(null)
--- fault not an object

=== a rounding mode that is not one
new Temporal.PlainTime(12, 30).round({ smallestUnit: "hour", roundingMode: "halfeven" })
--- fault value of a Temporal option

# --- toString, toJSON and the rest

=== toString writes the seconds, and the fraction it needs
[new Temporal.PlainTime(12, 30).toString(), new Temporal.PlainTime(12, 30, 0, 500).toString(), new Temporal.PlainTime(12, 30, 0, 0, 0, 1).toString()]
--- value
["12:30:00","12:30:00.5","12:30:00.000000001"]

=== toString's fractionalSecondDigits
(t => [0, 1, 2.7, 3, 6, 9, "auto"].map(fractionalSecondDigits => t.toString({ fractionalSecondDigits })))(new Temporal.PlainTime(12, 30, 45, 678, 900))
--- value
["12:30:45","12:30:45.6","12:30:45.67","12:30:45.678","12:30:45.678900","12:30:45.678900000","12:30:45.6789"]

=== toString's smallestUnit, which overrides the digits
(t => ["minute", "second", "millisecond", "microseconds", "nanosecond"].map(smallestUnit => t.toString({ smallestUnit, fractionalSecondDigits: 1 })))(new Temporal.PlainTime(12, 30, 45, 678, 900))
--- value
["12:30","12:30:45","12:30:45.678","12:30:45.678900","12:30:45.678900000"]

=== toString truncates by default, and rounds by its roundingMode
(t => [t.toString({ smallestUnit: "minute" }), t.toString({ smallestUnit: "minute", roundingMode: "halfExpand" }), t.toString({ fractionalSecondDigits: 0, roundingMode: "ceil" })])(new Temporal.PlainTime(12, 30, 45, 678))
--- value
["12:30","12:31","12:30:46"]

=== toString rounds past midnight to midnight
new Temporal.PlainTime(23, 59, 59, 999).toString({ smallestUnit: "second", roundingMode: "ceil" })
--- value
"00:00:00"

=== toString does not read a calendarName or an increment
new Temporal.PlainTime(12, 30).toString({ calendarName: "x", roundingIncrement: 5 })
--- value
"12:30:00"

=== fractionalSecondDigits 10
new Temporal.PlainTime(12, 30).toString({ fractionalSecondDigits: 10 })
--- fault fractionalSecondDigits

=== fractionalSecondDigits -1
new Temporal.PlainTime(12, 30).toString({ fractionalSecondDigits: -0.5 })
--- fault fractionalSecondDigits

=== fractionalSecondDigits as a string of a number
new Temporal.PlainTime(12, 30).toString({ fractionalSecondDigits: "2" })
--- fault fractionalSecondDigits

=== fractionalSecondDigits null
new Temporal.PlainTime(12, 30).toString({ fractionalSecondDigits: null })
--- fault fractionalSecondDigits

=== fractionalSecondDigits NaN
new Temporal.PlainTime(12, 30).toString({ fractionalSecondDigits: NaN })
--- fault fractionalSecondDigits

=== toString's smallestUnit hour
new Temporal.PlainTime(12, 30).toString({ smallestUnit: "hour" })
--- fault does not round to

=== toString's smallestUnit auto, which the polyfills read differently, deliberate
new Temporal.PlainTime(12, 30).toString({ smallestUnit: "auto" })
--- fault polyfills read differently

=== toString of null options
new Temporal.PlainTime(12, 30).toString(null)
--- fault not an object

=== toJSON, whose argument is not read
new Temporal.PlainTime(12, 30).toJSON({ smallestUnit: "minute" })
--- value
"12:30:00"

=== toLocaleString needs locale data, deliberate
new Temporal.PlainTime(12, 30).toLocaleString()
--- fault locale data

=== valueOf
new Temporal.PlainTime(12, 30).valueOf()
--- fault valueOf

=== add
[new Temporal.PlainTime(12, 30).add({ hours: 1 }), new Temporal.PlainTime(12, 30).add("PT12H"), new Temporal.PlainTime(12, 30).add("-PT13H"), new Temporal.PlainTime(12, 30).add("PT0.000000001S")]
--- value
[Temporal.PlainTime.from("13:30:00"),Temporal.PlainTime.from("00:30:00"),Temporal.PlainTime.from("23:30:00"),Temporal.PlainTime.from("12:30:00.000000001")]

=== add wraps around midnight, and leaves the date units out
[new Temporal.PlainTime(23, 59, 59, 999, 999, 999).add("PT0.000000001S"), new Temporal.PlainTime(12).add("P1Y2M3W"), new Temporal.PlainTime(12).add("P1DT1H")]
--- value
[Temporal.PlainTime.from("00:00:00"),Temporal.PlainTime.from("12:00:00"),Temporal.PlainTime.from("13:00:00")]

=== subtract
[new Temporal.PlainTime(0).subtract("PT1S"), new Temporal.PlainTime(12, 30).subtract({ minutes: 45 })]
--- value
[Temporal.PlainTime.from("23:59:59"),Temporal.PlainTime.from("11:45:00")]

=== add takes no options it reads
new Temporal.PlainTime(12).add("PT1H", { overflow: "reject" })
--- value
Temporal.PlainTime.from("13:00:00")

=== until in hours by default
[new Temporal.PlainTime(12, 30).until("13:00"), new Temporal.PlainTime(12, 30).until("01:15:30.5"), new Temporal.PlainTime(12, 30).until(new Temporal.PlainTime(12, 30))]
--- value
[Temporal.Duration.from("PT30M"),Temporal.Duration.from("-PT11H14M29.5S"),Temporal.Duration.from("PT0S")]

=== until with a largest unit and rounding
[new Temporal.PlainTime(0).until("23:59:59.999999999", { largestUnit: "second" }), new Temporal.PlainTime(0).until("23:59:59.5", { smallestUnit: "second", roundingMode: "halfEven" }), new Temporal.PlainTime(0).until("23:59:58.5", { smallestUnit: "second", roundingMode: "halfEven" }), new Temporal.PlainTime(0).until("01:07:30", { smallestUnit: "minute", roundingIncrement: 15 })]
--- value
[Temporal.Duration.from("PT86399.999999999S"),Temporal.Duration.from("PT24H"),Temporal.Duration.from("PT23H59M58S"),Temporal.Duration.from("PT1H")]

=== since
[new Temporal.PlainTime(13).since("12:30"), new Temporal.PlainTime(13).since("12:30:30", { smallestUnit: "minute", roundingMode: "floor" })]
--- value
[Temporal.Duration.from("PT30M"),Temporal.Duration.from("PT29M")]

=== until with a date unit
new Temporal.PlainTime(13).until("12:30", { largestUnit: "day" })
--- fault this method does not round to

=== until with an increment of an hour that does not divide a day
new Temporal.PlainTime(13).until("12:30", { smallestUnit: "hour", roundingIncrement: 5 })
--- fault does not divide

=== until of a date
new Temporal.PlainTime(13).until("2024-01-01")
--- fault PlainTime from an ISO 8601 time

# --- A PlainTime among other values

=== JSON.stringify writes its ISO string
JSON.stringify({ at: new Temporal.PlainTime(12, 30, 0, 5), list: [Temporal.PlainTime.from("01:02")] })
--- value
"{\"at\":\"12:30:00.005\",\"list\":[\"01:02:00\"]}"

=== typeof, truthiness and ??
(t => [typeof t, !t, t ?? 1])(new Temporal.PlainTime(12, 30))
--- value
["object",false,Temporal.PlainTime.from("12:30:00")]

=== in an array, an object and a map
(t => [[t], { t }, new Map([["t", t]])])(new Temporal.PlainTime(12, 30))
--- value
[[Temporal.PlainTime.from("12:30:00")],{"t":Temporal.PlainTime.from("12:30:00")},new Map([["t",Temporal.PlainTime.from("12:30:00")]])]

=== a spread into an object has no properties
({ ...new Temporal.PlainTime(12, 30) })
--- value
{}

=== a template converts with toString, deliberate
`${new Temporal.PlainTime(12, 30)}`
--- fault only primitives into strings

=== a set of times compares identity, deliberate
new Set([new Temporal.PlainTime(12, 30)]).size
--- fault primitives only

=== a module exports a time
@module times
export const t = Temporal.PlainTime.from("12:30");
export const later = t.with({ hour: 13 });
--- value
{"later":Temporal.PlainTime.from("13:30:00"),"t":Temporal.PlainTime.from("12:30:00")}

# --- The namespace

=== Temporal.PlainTime is a namespace, not a value
[Temporal.PlainTime]
--- error unsupported used only as Math.name

=== a static PlainTime does not have
Temporal.PlainTime.prototype
--- error unsupported does not have this built-in
