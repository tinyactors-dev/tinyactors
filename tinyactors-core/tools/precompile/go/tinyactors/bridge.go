// Package tinyactors is the Go side of the precompiled datamodel (a spike):
// the charts' generated code registers here, and core calls it through one
// C function (include/tinyactors/datamodel_precompiled.h).
package tinyactors

/*
#cgo CFLAGS: -O2 -I${SRCDIR}/../../../../include -I${SRCDIR}/../../demo
#cgo LDFLAGS: -L${SRCDIR}/../../../../build/release/lib -Wl,--start-group -ltinyactors_datamodel_precompiled -ltinyactors_host -ltinyactors_runtime -ltinyactors_compile -ltinyactors_system -ltinyactors_core -ltinyactors_otlp -ltinyactors_trace -ltinyactors_image -ltinyactors_base -Wl,--end-group -lm
#include <stdlib.h>
#include <tinyactors/datamodel_precompiled.h>
#include "demo.h"
ta_status ta_go_datamodel(ta_datamodel_precompiled **out);
*/
import "C"

import (
	"bytes"
	"fmt"
	"unsafe"

	"github.com/fxamacker/cbor/v2"
)

// Session is what the generator writes for a chart: its variables and its
// expressions, each a case of a switch on the expression's index.
type Session interface {
	base() *Base
	InitData(declaration int)
	SetData(declaration int, value any)
	Condition(expression int) bool
	Value(expression int) any
	String(expression int) string
	Assign(location int, value any)
	Script(expression int)
	Variables() map[string]any
	SetVariables(variables map[string]cbor.RawMessage)
}

// Base holds what every session has besides its variables: the system
// variables, _event, and the request being answered (for In).
type Base struct {
	Event        Event
	SessionID    string
	Name         string
	IOProcessors map[string]string
	given        map[int]bool // declarations the spawn's params set
	request      *C.ta_precompiled_request
}

func (b *Base) base() *Base { return b }

// In is SCXML's In(): whether the state is active.
func (b *Base) In(state string) bool {
	if b.request == nil {
		return false
	}
	name := C.CString(state)
	defer C.free(unsafe.Pointer(name))
	return C.ta_datamodel_precompiled_in(b.request, C.ta_string_view{pointer: name, length: C.uint32_t(len(state))}) != 0
}

// Chart is one chart's generated code.
type Chart struct {
	Name        string
	Fingerprint uint64
	Data        []string // the <data> ids, in order
	New         func() Session
}

var (
	charts   []Chart
	sessions []Session // a session's handle is its index + 1
	free     []int
)

// Register makes a chart's code known; the generated file calls it.
func Register(chart Chart) Chart {
	charts = append(charts, chart)
	return chart
}

// NoExpression is what a switch falls to for an index it does not hold.
func NoExpression(x int) string { return fmt.Sprintf("no expression %d of this kind in the generated code", x) }

func output(r *C.ta_precompiled_request, b []byte) C.ta_status {
	if len(b) > int(r.output.capacity) {
		return C.TA_ERROR_NO_MEMORY
	}
	copy(unsafe.Slice((*byte)(unsafe.Pointer(r.output.pointer)), len(b)), b)
	r.output.length = C.size_t(len(b))
	return C.TA_OK
}

func input(r *C.ta_precompiled_request) []byte {
	return unsafe.Slice((*byte)(r.input.pointer), int(r.input.length))
}

func encode(x any) []byte {
	b, err := cbor.Marshal(x)
	if err != nil {
		panic(err.Error())
	}
	return b
}

// start reads CREATE's input: [sessionid, name, {type: location}, params].
func start(s Session, chart Chart, decoder *cbor.Decoder) {
	var fields []cbor.RawMessage
	if err := decoder.Decode(&fields); err != nil || len(fields) != 4 {
		panic("a malformed start")
	}
	b := s.base()
	cbor.Unmarshal(fields[0], &b.SessionID)
	cbor.Unmarshal(fields[1], &b.Name)
	cbor.Unmarshal(fields[2], &b.IOProcessors)
	var params map[string]cbor.RawMessage
	if cbor.Unmarshal(fields[3], &params) == nil {
		b.given = map[int]bool{}
		for i, id := range chart.Data {
			if v, ok := params[id]; ok {
				s.SetData(i, Value(v))
				b.given[i] = true
			}
		}
	}
}

func newSession(chart Chart) (Session, C.uint64_t) {
	s := chart.New()
	if n := len(free); n > 0 {
		i := free[n-1]
		free = free[:n-1]
		sessions[i] = s
		return s, C.uint64_t(i + 1)
	}
	sessions = append(sessions, s)
	return s, C.uint64_t(len(sessions))
}

//export taGoCall
func taGoCall(r *C.ta_precompiled_request) (status C.ta_status) {
	defer func() {
		if p := recover(); p != nil {
			message := []byte(fmt.Sprint(p))
			if len(message) > int(r.message.capacity) {
				message = message[:r.message.capacity]
			}
			copy(unsafe.Slice((*byte)(unsafe.Pointer(r.message.pointer)), len(message)), message)
			r.message.length = C.size_t(len(message))
			status = C.TA_ERROR_EXECUTION
		}
	}()
	switch r.operation {
	case C.TA_PRECOMPILED_ATTACH:
		name := string(input(r))
		for i, chart := range charts {
			if chart.Name == name {
				if chart.Fingerprint != uint64(r.session) {
					panic(fmt.Sprintf("the chart %q changed since its code was generated: run go generate", name))
				}
				r.number = C.uint32_t(i)
				return C.TA_OK
			}
		}
		panic(fmt.Sprintf("no generated code for the chart %q", name))
	case C.TA_PRECOMPILED_CREATE, C.TA_PRECOMPILED_DESERIALIZE:
		chart := charts[r.chart]
		s, handle := newSession(chart)
		decoder := cbor.NewDecoder(bytes.NewReader(input(r)))
		start(s, chart, decoder)
		if r.operation == C.TA_PRECOMPILED_DESERIALIZE {
			var variables map[string]cbor.RawMessage
			if err := decoder.Decode(&variables); err != nil {
				panic("a malformed blob")
			}
			s.SetVariables(variables)
		}
		r.session = handle
		return C.TA_OK
	}
	s := sessions[r.session-1]
	b := s.base()
	b.request = r
	defer func() { b.request = nil }()
	x := int(r.expression)
	switch r.operation {
	case C.TA_PRECOMPILED_DESTROY:
		sessions[r.session-1] = nil
		free = append(free, int(r.session-1))
	case C.TA_PRECOMPILED_INIT_DATA:
		if !b.given[int(r.number)] {
			s.InitData(int(r.number))
		}
	case C.TA_PRECOMPILED_BIND_EVENT:
		b.Event = Event{}
		if err := cbor.Unmarshal(input(r), &b.Event); err != nil {
			panic("a malformed event")
		}
	case C.TA_PRECOMPILED_CONDITION:
		r.number = 0
		if s.Condition(x) {
			r.number = 1
		}
	case C.TA_PRECOMPILED_VALUE:
		return output(r, encode(s.Value(x)))
	case C.TA_PRECOMPILED_STRING:
		return output(r, []byte(s.String(x)))
	case C.TA_PRECOMPILED_ASSIGN:
		s.Assign(x, s.Value(int(r.number)))
	case C.TA_PRECOMPILED_ASSIGN_VALUE:
		s.Assign(x, Value(bytes.Clone(input(r))))
	case C.TA_PRECOMPILED_SCRIPT:
		s.Script(x)
	case C.TA_PRECOMPILED_SERIALIZE:
		return output(r, encode(s.Variables()))
	case C.TA_PRECOMPILED_VARIABLE:
		v, ok := s.Variables()[string(input(r))]
		if !ok {
			return C.TA_ERROR_INVALID
		}
		return output(r, encode(v))
	default:
		return C.TA_ERROR_UNSUPPORTED
	}
	return C.TA_OK
}

// Decode sets what out points to from a blob's variable, when the blob has it.
func Decode(variables map[string]cbor.RawMessage, name string, out any) {
	if v, ok := variables[name]; ok {
		if err := cbor.Unmarshal(v, out); err != nil {
			panic(fmt.Sprintf("the variable %s: %v", name, err))
		}
	}
}

// RunOrderDemo runs the scripted order (tools/precompile/demo) with the
// charts registered here.
func RunOrderDemo(chartPath string) int {
	var datamodel *C.ta_datamodel_precompiled
	if C.ta_go_datamodel(&datamodel) != C.TA_OK {
		return 1
	}
	defer C.ta_datamodel_precompiled_free(datamodel)
	path := C.CString(chartPath)
	defer C.free(unsafe.Pointer(path))
	return int(C.ta_demo_order(path, C.ta_datamodel_precompiled_binding(datamodel)))
}
